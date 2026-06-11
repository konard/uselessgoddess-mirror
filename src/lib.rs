//! The `mirror` Vulkan layer.
//!
//! `mirror` intercepts graphics-pipeline creation and replaces every fragment
//! shader with a tiny built-in one that renders window-space depth as grayscale,
//! turning any application into an X-ray-like depth view. Individual materials,
//! identified by the FNV-1a hash of their fragment SPIR-V, can instead be
//! painted a fixed highlight color. See [`config`] for the environment
//! variables that drive it.
//!
//! The crate builds both as a `cdylib` (the shared object the Vulkan loader
//! dlopen's) and as an `rlib`, so the test harness and examples can reuse the
//! hashing, configuration and SPIR-V helpers.

pub mod config;
pub mod hash;
pub mod spirv;

mod layer;
mod vk_layer;

pub use layer::{vkGetDeviceProcAddr, vkGetInstanceProcAddr};
