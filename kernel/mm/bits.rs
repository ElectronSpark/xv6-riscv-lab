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

#[no_mangle]
#[link_section = ".rodata"]
pub static __uint8_bits_count: [i8; 256] = build_popcount();

#[no_mangle]
#[link_section = ".rodata"]
pub static __uint8_trailing_zeros: [i8; 256] = build_trailing_zeros();

#[no_mangle]
#[link_section = ".rodata"]
pub static __uint8_leading_zeros: [i8; 256] = build_leading_zeros();

// Stored as `[u8; 256]` because some entries exceed the i8 range. The C side
// declares it as `const int8 [256]`, but only the byte representation matters
// since the symbol is never dereferenced in the current code base; this
// preserves backward ABI compatibility for any future caller.
#[no_mangle]
#[link_section = ".rodata"]
pub static __uint8_inverse: [u8; 256] = build_inverse();
