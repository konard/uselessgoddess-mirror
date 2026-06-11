//! Compile-time embedding of SPIR-V binaries produced by `build.rs`.
//!
//! `include_bytes!` only guarantees byte alignment, but Vulkan requires the
//! `pCode` pointer handed to `vkCreateShaderModule` to be 4-byte aligned. The
//! [`include_spv!`] macro therefore embeds the blob inside an over-aligned
//! wrapper and hands back a [`Spirv`] exposing both the `u8` view (for hashing)
//! and the `u32` view (for Vulkan).

/// Wrapper that forces 4-byte alignment on an embedded SPIR-V blob.
#[repr(C, align(4))]
pub struct Aligned<B: ?Sized>(pub B);

/// A SPIR-V binary embedded in the executable, aligned for Vulkan.
#[derive(Clone, Copy)]
pub struct Spirv {
    bytes: &'static [u8],
}

impl Spirv {
    /// Wraps an already 4-byte-aligned SPIR-V byte slice. Prefer [`include_spv!`].
    #[must_use]
    pub const fn new(bytes: &'static [u8]) -> Self {
        Self { bytes }
    }

    /// The raw bytes, as fed to [`crate::hash::fnv1a64`].
    #[must_use]
    pub const fn bytes(&self) -> &'static [u8] {
        self.bytes
    }

    /// The SPIR-V words, as required by `VkShaderModuleCreateInfo::pCode`.
    #[must_use]
    pub fn words(&self) -> &'static [u32] {
        debug_assert_eq!(
            self.bytes.len() % 4,
            0,
            "SPIR-V is a stream of 32-bit words"
        );
        debug_assert_eq!(
            self.bytes.as_ptr() as usize % 4,
            0,
            "SPIR-V must be 4-byte aligned"
        );
        // SAFETY: `include_spv!` guarantees 4-byte alignment and SPIR-V length is
        // always a whole number of 32-bit words.
        unsafe {
            std::slice::from_raw_parts(self.bytes.as_ptr().cast::<u32>(), self.bytes.len() / 4)
        }
    }
}

/// Embeds `<name>.spv` from the build script's `OUT_DIR` as a [`Spirv`].
#[macro_export]
macro_rules! include_spv {
    ($name:literal) => {{
        static ALIGNED: &$crate::spirv::Aligned<[u8]> = &$crate::spirv::Aligned(*include_bytes!(
            concat!(env!("OUT_DIR"), "/", $name, ".spv")
        ));
        $crate::spirv::Spirv::new(&ALIGNED.0)
    }};
}
