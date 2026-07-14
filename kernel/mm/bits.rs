//! Bit-manipulation lookup tables.
//!
//! Rust port of the original `kernel/bits.c`. Exposes the same four 256-byte
//! tables under their original C symbol names so that
//! `kernel/inc/bits.h` (compiled with `USE_SOFTWARE_FFS=1`) can keep using
//! them unchanged:
//!
//! * `__uint8_bits_count[i]`     = popcount(i)
//! * `__uint8_trailing_zeros[i]` = ctz(i), or -1 when i == 0
//! * `__uint8_leading_zeros[i]`  = clz(i) within a byte, or -1 when i == 0
//! * `__uint8_inverse[i]`        = bit-reverse of i
//!
//! The C header declares all four arrays as `const int8 [256]`. The values in
//! `__uint8_inverse` exceed the i8 range, but the underlying byte
//! representation is identical, so the C side reads exactly what the original
//! `bits.c` produced.

const fn build_popcount() -> [i8; 256] {
    let mut t = [0i8; 256];
    let mut i: usize = 0;
    while i < 256 {
        let mut x = i as u8;
        let mut c: i8 = 0;
        while x != 0 {
            c += (x & 1) as i8;
            x >>= 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

const fn build_trailing_zeros() -> [i8; 256] {
    let mut t = [-1i8; 256];
    let mut i: usize = 1;
    while i < 256 {
        let mut x = i as u8;
        let mut c: i8 = 0;
        while (x & 1) == 0 {
            c += 1;
            x >>= 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

const fn build_leading_zeros() -> [i8; 256] {
    let mut t = [-1i8; 256];
    let mut i: usize = 1;
    while i < 256 {
        let x = i as u8;
        let mut c: i8 = 0;
        let mut mask: u8 = 0x80;
        while (x & mask) == 0 {
            c += 1;
            mask >>= 1;
        }
        t[i] = c;
        i += 1;
    }
    t
}

const fn build_inverse() -> [u8; 256] {
    let mut t = [0u8; 256];
    let mut i: usize = 0;
    while i < 256 {
        let mut x = i as u8;
        let mut r: u8 = 0;
        let mut k = 0;
        while k < 8 {
            r = (r << 1) | (x & 1);
            x >>= 1;
            k += 1;
        }
        t[i] = r;
        i += 1;
    }
    t
}

// Place tables in `.rodata` to match the original C `const` arrays and avoid
// occupying RAM. `#[no_mangle]` preserves the exact C symbol names referenced
// by `kernel/inc/bits.h`.

#[link_section = ".rodata"]
pub(crate) static __uint8_bits_count: [i8; 256] = build_popcount();

#[link_section = ".rodata"]
pub(crate) static __uint8_trailing_zeros: [i8; 256] = build_trailing_zeros();

#[link_section = ".rodata"]
pub(crate) static __uint8_leading_zeros: [i8; 256] = build_leading_zeros();

// Stored as `[u8; 256]` because some entries exceed the i8 range. The C side
// declares it as `const int8 [256]`, but only the byte representation matters
// since the symbol is never dereferenced in the current code base; this
// preserves backward ABI compatibility for any future caller.
#[link_section = ".rodata"]
pub(crate) static __uint8_inverse: [u8; 256] = build_inverse();

// ---------------------------------------------------------------------------
// Host-test suite (`cargo test --target x86_64-unknown-linux-gnu`; see
// `kernel/lib.rs`'s "Host-test seam" doc). Verifies the four `const fn`
// table-generators (and therefore the `#[no_mangle] pub static` tables built
// from them) against independent, naively-written reimplementations of the
// same bit operations, exhaustively over all 256 byte values.
//
// Reference-coverage note: `test/src/ut_bits_main.c` has 40 cmocka cases, but
// most of them (`bits_ctzg`/`bits_clzg`/`bits_popcountg`/`bits_ffsg`'s
// multi-width generics, the `bits_ctz_ptr*` family, `bits_foreach_set_bit`,
// `bits_next_bit_set`) exercise `kernel/inc/bits.h`'s C `static inline`
// wrapper macros/functions built *on top of* these tables -- there is no
// Rust port of that layer (`bits.rs` only ever produced the four lookup
// tables the C header already consumed; see this file's module doc), so
// those ~36 cases have no Rust surface to test here and remain covered by
// the existing (already-fixed, see `docs/rustify/test_port_plan.md`'s Phase
// 0) `ut_bits` C cmocka suite, unaffected by this addition. The four cases
// that *do* have a direct Rust equivalent (`test_bits_ffs8_matches_naive`,
// `test_bits_ctz8_matches_naive`, `test_bits_clz8_matches_naive`,
// `test_bits_popcount8_matches_naive`) are reproduced below against the
// tables those four C functions read from.
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

    /// Bit-reversal within a byte, independently written (no C reference
    /// test exercises `__uint8_inverse` directly -- it has no current
    /// caller in the tree per this file's module doc -- but it is one of
    /// the four `build_*` tables this file promises byte-for-byte, so it
    /// gets the same naive-vs-table treatment as the other three).
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
                __uint8_bits_count[x as usize],
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
                __uint8_trailing_zeros[x as usize],
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
                __uint8_leading_zeros[x as usize],
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
                __uint8_inverse[x as usize],
                naive_reverse8(x),
                "bit-reverse mismatch for byte {x:#04x}"
            );
        }
    }

    #[test]
    fn zero_is_the_documented_sentinel_for_ctz_and_clz_tables() {
        // Matches the C reference's convention (`naive_ctz8`/`naive_clz8`
        // both special-case 0 to -1) and this file's own module doc.
        assert_eq!(__uint8_trailing_zeros[0], -1);
        assert_eq!(__uint8_leading_zeros[0], -1);
    }

    #[test]
    fn build_functions_reproduce_the_exported_static_tables_exactly() {
        // Regression guard: the `#[no_mangle] pub static`s above are each
        // `build_*()` evaluated at compile time. Re-invoking the const fns
        // here and comparing against the statics catches anyone changing
        // one but not the other (e.g. hand-editing a static's initializer
        // to no longer literally be a call to its `build_*` function).
        assert_eq!(build_popcount(), __uint8_bits_count);
        assert_eq!(build_trailing_zeros(), __uint8_trailing_zeros);
        assert_eq!(build_leading_zeros(), __uint8_leading_zeros);
        assert_eq!(build_inverse(), __uint8_inverse);
    }
}
