//! Historical byte lookup tables generated with Rust integer operations.
//!
//! These crate-private tables currently have no production consumers; they
//! retain the old byte-level results for exhaustive host regression tests.
//! They are Rust symbols, not exports for `kernel/inc/bits.h`'s optional C
//! software fallback. Kernel Rust code should use integer methods directly.
//!
//! The zero-count tables preserve the historical `-1` sentinel for zero,
//! rather than the `8` returned by `u8::{leading_zeros, trailing_zeros}`.
//! Reversed bytes use `u8` so every result is represented without sign loss.

const fn build_popcount() -> [i8; 256] {
    let mut table = [0; 256];
    let mut index = 0;
    while index < table.len() {
        table[index] = (index as u8).count_ones() as i8;
        index += 1;
    }
    table
}

const fn build_trailing_zeros() -> [i8; 256] {
    let mut table = [-1; 256];
    let mut index = 1;
    while index < table.len() {
        table[index] = (index as u8).trailing_zeros() as i8;
        index += 1;
    }
    table
}

const fn build_leading_zeros() -> [i8; 256] {
    let mut table = [-1; 256];
    let mut index = 1;
    while index < table.len() {
        table[index] = (index as u8).leading_zeros() as i8;
        index += 1;
    }
    table
}

const fn build_inverse() -> [u8; 256] {
    let mut table = [0; 256];
    let mut index = 0;
    while index < table.len() {
        table[index] = (index as u8).reverse_bits();
        index += 1;
    }
    table
}

// Immutable statics naturally live in read-only storage. Unused tables may
// be discarded; no symbol export or forced linker section is required.
pub(crate) static BYTE_POPCOUNTS: [i8; 256] = build_popcount();
pub(crate) static BYTE_TRAILING_ZEROS: [i8; 256] = build_trailing_zeros();
pub(crate) static BYTE_LEADING_ZEROS: [i8; 256] = build_leading_zeros();
pub(crate) static BYTE_REVERSED_BITS: [u8; 256] = build_inverse();

// Exercise every byte against independent bit-by-bit reference algorithms.
#[cfg(test)]
mod tests {
    use super::*;

    /// Rust port of the C reference's `naive_popcount8`.
    fn naive_popcount8(value: u8) -> i8 {
        let mut total = 0i8;
        let mut x = value;
        while x != 0 {
            total += (x & 1) as i8;
            x >>= 1;
        }
        total
    }

    /// Rust port of the C reference's `naive_ctz8` (trailing zeros, -1 for 0).
    fn naive_trailing_zeros8(value: u8) -> i8 {
        if value == 0 {
            return -1;
        }
        let mut count = 0i8;
        let mut x = value;
        while (x & 1) == 0 {
            count += 1;
            x >>= 1;
        }
        count
    }

    /// Rust port of the C reference's `naive_clz8` (leading zeros within a
    /// byte, -1 for 0).
    fn naive_leading_zeros8(value: u8) -> i8 {
        if value == 0 {
            return -1;
        }
        let mut count = 0i8;
        let mut mask: u8 = 0x80;
        while (value & mask) == 0 {
            count += 1;
            mask >>= 1;
        }
        count
    }

    /// Independent bit-by-bit reference for all 256 reversed byte values.
    fn naive_reverse8(value: u8) -> u8 {
        let mut x = value;
        let mut result: u8 = 0;
        for _ in 0..8 {
            result = (result << 1) | (x & 1);
            x >>= 1;
        }
        result
    }

    #[test]
    fn popcount_table_matches_naive_popcount_for_every_byte_value() {
        for value in 0u16..256 {
            let x = value as u8;
            assert_eq!(
                BYTE_POPCOUNTS[x as usize],
                naive_popcount8(x),
                "popcount mismatch for byte {x:#04x}"
            );
        }
    }

    #[test]
    fn trailing_zeros_table_matches_naive_ctz_for_every_byte_value() {
        for value in 0u16..256 {
            let x = value as u8;
            assert_eq!(
                BYTE_TRAILING_ZEROS[x as usize],
                naive_trailing_zeros8(x),
                "ctz mismatch for byte {x:#04x}"
            );
        }
    }

    #[test]
    fn leading_zeros_table_matches_naive_clz_for_every_byte_value() {
        for value in 0u16..256 {
            let x = value as u8;
            assert_eq!(
                BYTE_LEADING_ZEROS[x as usize],
                naive_leading_zeros8(x),
                "clz mismatch for byte {x:#04x}"
            );
        }
    }

    #[test]
    fn inverse_table_matches_naive_bit_reversal_for_every_byte_value() {
        for value in 0u16..256 {
            let x = value as u8;
            assert_eq!(
                BYTE_REVERSED_BITS[x as usize],
                naive_reverse8(x),
                "bit-reverse mismatch for byte {x:#04x}"
            );
        }
    }

    #[test]
    fn zero_is_the_documented_sentinel_for_ctz_and_clz_tables() {
        // Matches the C reference's convention (`naive_ctz8`/`naive_clz8`
        // both special-case 0 to -1) and this file's own module doc.
        assert_eq!(BYTE_TRAILING_ZEROS[0], -1);
        assert_eq!(BYTE_LEADING_ZEROS[0], -1);
    }

    #[test]
    fn build_functions_reproduce_the_static_tables_exactly() {
        // Compile-time and runtime evaluations must produce the same bytes.
        assert_eq!(build_popcount(), BYTE_POPCOUNTS);
        assert_eq!(build_trailing_zeros(), BYTE_TRAILING_ZEROS);
        assert_eq!(build_leading_zeros(), BYTE_LEADING_ZEROS);
        assert_eq!(build_inverse(), BYTE_REVERSED_BITS);
    }
}
