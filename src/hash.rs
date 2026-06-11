//! FNV-1a (64-bit) hashing, used to give every shader module a stable identity
//! derived from its SPIR-V code.

const OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
const PRIME: u64 = 0x0000_0100_0000_01b3;

/// Computes the 64-bit FNV-1a hash of `data`.
///
/// This is the material identity used throughout the layer: two shader modules
/// with byte-identical SPIR-V hash to the same value, and that value is what
/// `MIRROR_HIGHLIGHT` entries are matched against.
#[must_use]
pub fn fnv1a64(data: &[u8]) -> u64 {
    data.iter().fold(OFFSET_BASIS, |hash, &byte| {
        (hash ^ u64::from(byte)).wrapping_mul(PRIME)
    })
}

#[cfg(test)]
mod tests {
    use super::fnv1a64;

    #[test]
    fn known_vectors() {
        // Canonical FNV-1a/64 test vectors.
        assert_eq!(fnv1a64(b""), 0xcbf2_9ce4_8422_2325);
        assert_eq!(fnv1a64(b"a"), 0xaf63_dc4c_8601_ec8c);
        assert_eq!(fnv1a64(b"foobar"), 0x8594_4171_f739_67e8);
    }

    #[test]
    fn distinguishes_inputs() {
        assert_ne!(fnv1a64(b"foobar"), fnv1a64(b"foobaz"));
    }
}
