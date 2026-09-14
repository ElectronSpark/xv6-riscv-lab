//! String/memory primitives — Rust port of `kernel/string.c`.
//!
//! ## `memcpy`/`memset`/`memmove`/`memcmp` and compiler-builtins
//!
//! These four symbols are special: LLVM can recognize a hand-written
//! byte-copy/byte-fill/byte-compare loop as a "libcall idiom" and rewrite
//! it into a call to the corresponding C library function
//! (`LoopIdiomRecognize` / `SimplifyLibCalls`). Doing that *inside a
//! function that is itself named `memcpy`* would be a real,
//! stack-smashing infinite recursion. LLVM specifically guards against
//! this (it never rewrites a loop into a call to the libcall that names
//! the enclosing function), which is also how `compiler_builtins`'
//! optional `mem` feature and crates like `rlibc` get away with the same
//! pattern — but *only* as long as the loop body stays low-level pointer
//! arithmetic (`*p.add(i)`), never `core::ptr::copy`/`copy_nonoverlapping`/
//! `write_bytes` or slice methods that lower to those same intrinsics
//! (those really would recurse, since the intrinsic calls are inserted
//! unconditionally, not just recognized post-hoc). This file therefore
//! implements all four with raw-pointer byte loops only.
//!
//! Verified empirically for this exact target/profile (`riscv64gc-
//! unknown-none-elf`, `opt-level=z`, fat LTO, `codegen-units=1`) before
//! writing this file: a standalone byte-loop `memcpy`/`memset` compiles
//! to a plain loop with no outgoing call, and no undefined symbols.
//!
//! ## Why no duplicate-symbol conflict with `compiler_builtins`
//!
//! The prebuilt `compiler_builtins` rlib shipped with this target's
//! stable-Rust sysroot is built *without* its `mem` Cargo feature (that
//! feature is only reachable via `-Z build-std-features=compiler-
//! builtins-mem`, which needs nightly + `build-std`; this crate uses the
//! ordinary precompiled `core`/`compiler_builtins` from `rustup`).
//! Checked directly: `nm` on every object inside
//! `libcompiler_builtins-*.rlib` for this target shows no `memcpy`,
//! `memset`, `memmove`, or `memcmp` symbol at all. So, before this port,
//! *every* call to these four functions from Rust code (there are dozens,
//! e.g. `mm/vm.rs`, `mm/kalloc.rs`, `proc/thread.rs`) already resolved to
//! the C definitions in `kernel/string.c` at final link time — there was
//! never a competing Rust definition to conflict with. After this port,
//! they resolve to the definitions below instead; no other archive member
//! defines them, so exactly one copy of each ships in the kernel ELF
//! (verified post-build with `nm` on `kernel_with_symbols_elf`).
//!
//! The exported memory and string functions are compatibility boundaries for
//! compiler-generated libc calls and existing raw kernel callers. Prefer
//! [`copy_cstr`] and ordinary slice operations in Rust code. Host tests keep
//! these functions mangled so they cannot replace the host C library.

#![deny(unsafe_op_in_unsafe_fn)]

use core::ffi::{c_char, c_int, c_void, CStr};
use core::ptr;

/// Copy a C string into a bounded Rust buffer, reserving one byte for NUL.
/// Returns the number of payload bytes copied. An empty destination is a no-op;
/// excess source bytes are truncated, and bytes after the terminator are kept.
pub fn copy_cstr(dst: &mut [u8], src: &CStr) -> usize {
    let Some(capacity) = dst.len().checked_sub(1) else {
        return 0;
    };
    let copied = capacity.min(src.to_bytes().len());
    dst[..copied].copy_from_slice(&src.to_bytes()[..copied]);
    dst[copied] = 0;
    copied
}

// ---------------------------------------------------------------------------
// memset / memcmp / memmove / memcpy — see module doc for the no-recursion
// and no-compiler_builtins-conflict rationale. Raw pointer arithmetic only.
// ---------------------------------------------------------------------------

/// # Safety
/// `dst` must be valid for writes of `n` bytes.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void {
    unsafe {
        let d = dst as *mut u8;
        let byte = c as u8;
        let mut i = 0usize;
        while i < n {
            *d.add(i) = byte;
            i += 1;
        }
        dst
    }
}

/// # Safety
/// `v1` and `v2` must be valid for reads of `n` bytes.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn memcmp(v1: *const c_void, v2: *const c_void, n: usize) -> c_int {
    unsafe {
        let mut s1 = v1 as *const u8;
        let mut s2 = v2 as *const u8;
        let mut cnt = n;
        while cnt > 0 {
            cnt -= 1;
            let b1 = *s1;
            let b2 = *s2;
            if b1 != b2 {
                return (b1 as c_int) - (b2 as c_int);
            }
            s1 = s1.add(1);
            s2 = s2.add(1);
        }
        0
    }
}

/// # Safety
/// `dst` must be valid for writes of `n` bytes and `src` valid for reads
/// of `n` bytes (they may overlap — that's the point of `memmove`).
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void {
    unsafe {
        if n == 0 {
            return dst;
        }
        let mut s = src as *const u8;
        let mut d = dst as *mut u8;
        let d_const = d as *const u8;
        if s < d_const && s.add(n) > d_const {
            // Overlapping, src before dst: copy backwards.
            s = s.add(n);
            d = d.add(n);
            let mut cnt = n;
            while cnt > 0 {
                cnt -= 1;
                s = s.sub(1);
                d = d.sub(1);
                *d = *s;
            }
        } else {
            let mut cnt = n;
            while cnt > 0 {
                *d = *s;
                d = d.add(1);
                s = s.add(1);
                cnt -= 1;
            }
        }
        dst
    }
}

/// `memcpy` exists to placate GCC (and, now, rustc/LLVM idiom-recognition
/// on the C side of the build). Use `memmove` — matches the original C
/// comment/implementation exactly; delegating to a *different* named
/// function is not the self-recursion hazard described in the module
/// doc (that's specifically about a loop lowering back into a call to
/// the same symbol).
///
/// # Safety
/// Same contract as `memmove`.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void {
    unsafe { memmove(dst, src, n) }
}

// ---------------------------------------------------------------------------
// String functions.
// ---------------------------------------------------------------------------

/// # Safety
/// `p` and `q` must point to valid NUL-terminated strings.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn strcmp(p: *const c_char, q: *const c_char) -> c_int {
    unsafe {
        let mut p = p;
        let mut q = q;
        while *p != 0 && *p == *q {
            p = p.add(1);
            q = q.add(1);
        }
        (*p as u8 as c_int) - (*q as u8 as c_int)
    }
}

/// # Safety
/// `p` and `q` must be valid for reads until a NUL byte or `n` bytes,
/// whichever comes first.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn strncmp(p: *const c_char, q: *const c_char, n: usize) -> c_int {
    unsafe {
        let mut p = p;
        let mut q = q;
        let mut n = n;
        while n > 0 && *p != 0 && *p == *q {
            n -= 1;
            p = p.add(1);
            q = q.add(1);
        }
        if n == 0 {
            return 0;
        }
        (*p as u8 as c_int) - (*q as u8 as c_int)
    }
}

/// # Safety
/// `dst` must be valid for writes of `n` bytes; `src` valid for reads up to
/// its NUL terminator or `n` bytes, whichever comes first. The buffers must
/// not overlap. Neither pointer is accessed when `n == 0`.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn strncpy(dst: *mut c_char, src: *const c_char, n: usize) -> *mut c_char {
    let mut ended = false;
    for i in 0..n {
        let byte = if ended {
            0
        } else {
            // SAFETY: i < n; we stop reading src as soon as its NUL is seen.
            unsafe { *src.add(i) }
        };
        ended |= byte == 0;
        // SAFETY: the caller supplies n writable bytes and i < n.
        unsafe { *dst.add(i) = byte };
    }
    dst
}

/// Like `strncpy` but guaranteed to NUL-terminate, without padding.
///
/// # Safety
/// `dst` must be valid for writes of `n` bytes; `src` valid for reads up to
/// its NUL terminator or `n - 1` bytes, whichever comes first. The buffers
/// must not overlap. Neither pointer is accessed when `n == 0`.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn safestrcpy(dst: *mut c_char, src: *const c_char, n: usize) -> *mut c_char {
    if n == 0 {
        return dst;
    }
    let mut copied = 0;
    while copied < n - 1 {
        // SAFETY: copied < n - 1 and no preceding source byte was NUL.
        let byte = unsafe { *src.add(copied) };
        if byte == 0 {
            break;
        }
        // SAFETY: copied < n, inside the caller's writable destination.
        unsafe { *dst.add(copied) = byte };
        copied += 1;
    }
    // SAFETY: copied <= n - 1, including the zero-payload case.
    unsafe { *dst.add(copied) = 0 };
    dst
}

/// # Safety
/// `s` must point to a valid NUL-terminated string.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn strlen(s: *const c_char) -> usize {
    unsafe {
        let mut n = 0usize;
        while *s.add(n) != 0 {
            n += 1;
        }
        n
    }
}

/// # Safety
/// `s` must be valid for reads up to its NUL terminator or `maxlen`
/// bytes, whichever comes first.
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn strnlen(s: *const c_char, maxlen: usize) -> usize {
    unsafe {
        let mut n = 0usize;
        while n < maxlen && *s.add(n) != 0 {
            n += 1;
        }
        n
    }
}

/// # Safety
/// `str_` (when non-null) and `delim` must point to valid NUL-terminated
/// strings; `saveptr` must be a valid, writable `*mut *mut c_char`, and
/// when `str_` is null, `*saveptr` must be a value this function
/// previously wrote there (or null).
#[cfg_attr(not(test), no_mangle)]
pub unsafe extern "C" fn strtok_r(
    str_: *mut c_char,
    delim: *const c_char,
    saveptr: *mut *mut c_char,
) -> *mut c_char {
    unsafe {
        let mut str_ = str_;
        if str_.is_null() {
            str_ = *saveptr;
        }
        if str_.is_null() {
            return ptr::null_mut();
        }

        // Skip leading delimiters.
        while *str_ != 0 {
            let mut d = delim;
            let mut is_delim = false;
            while *d != 0 {
                if *str_ == *d {
                    is_delim = true;
                    break;
                }
                d = d.add(1);
            }
            if !is_delim {
                break;
            }
            str_ = str_.add(1);
        }

        if *str_ == 0 {
            *saveptr = str_;
            return ptr::null_mut();
        }

        let token = str_;

        while *str_ != 0 {
            let mut d = delim;
            while *d != 0 {
                if *str_ == *d {
                    *str_ = 0;
                    *saveptr = str_.add(1);
                    return token;
                }
                d = d.add(1);
            }
            str_ = str_.add(1);
        }

        *saveptr = str_;
        token
    }
}

/// # Safety
/// `s` must be valid for reads up to its NUL terminator or `n` bytes,
/// whichever comes first. Returns null on allocation failure.
#[cfg_attr(not(test), no_mangle)]
#[cfg(not(test))]
pub unsafe extern "C" fn strndup(s: *const c_char, n: usize) -> *mut c_char {
    unsafe {
        let len = strnlen(s, n);
        let new_str = crate::mm::kalloc::Kmem::kmm_alloc(len + 1) as *mut c_char;
        if new_str.is_null() {
            return ptr::null_mut();
        }
        strncpy(new_str, s, len);
        *new_str.add(len) = 0;
        new_str
    }
}

/// # Safety
/// `s` must point to a valid NUL-terminated string. Returns null on
/// allocation failure.
///
/// NOT demoted, despite having zero textual Rust callers anywhere in the
/// tree: this function's body (measure length, allocate, copy) is exactly
/// the pattern LLVM's `TargetLibraryInfo` recognizes as equivalent to the
/// C `strdup()` libcall — several out-of-scope files (`vfs/inode.rs`'s
/// `make_iter_parent`, `vfs/tmpfs/inode.rs`'s `__tmpfs_dir_iter`,
/// `vfs/xv6fs/inode.rs`'s `__xv6fs_dir_iter`, plus `vfs_dir_iter`/
/// `vfs_ilookup`) implement the identical "strlen + alloc + copy" idiom
/// independently and get silently rewritten by the optimizer into a call
/// to the external symbol `strdup`, discovered as a real link failure
/// (`undefined reference to 'strdup'`) when this was first demoted. Same
/// class of hazard the wave's caution note already flags for
/// memcpy/memset/memmove/memcmp; keep `#[cfg_attr(not(test), no_mangle)] extern "C"` here too.
#[cfg_attr(not(test), no_mangle)]
#[cfg(not(test))]
pub unsafe extern "C" fn strdup(s: *const c_char) -> *mut c_char {
    unsafe {
        let len = strlen(s);
        let new_str = crate::mm::kalloc::Kmem::kmm_alloc(len + 1) as *mut c_char;
        if new_str.is_null() {
            return ptr::null_mut();
        }
        strncpy(new_str, s, len);
        *new_str.add(len) = 0;
        new_str
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn bounded_copy_never_overwrites_canaries() {
        for src in [c"", c"a", c"abcd", c"abcdef"] {
            for n in 0..=5 {
                let mut dst = [0x55u8; 7];
                // SAFETY: the destination has n bytes between the canaries;
                // the source is a disjoint valid C string.
                unsafe { strncpy(dst.as_mut_ptr().add(1).cast(), src.as_ptr(), n) };
                assert_eq!(dst[0], 0x55);
                assert!(dst[n + 1..].iter().all(|&b| b == 0x55));
                for (i, &byte) in dst[1..n + 1].iter().enumerate() {
                    assert_eq!(byte, src.to_bytes().get(i).copied().unwrap_or(0));
                }
            }
        }
    }

    #[test]
    fn terminating_copy_writes_exactly_one_terminator() {
        for src in [c"", c"a", c"abcd", c"abcdef"] {
            for n in 0..=5 {
                let mut dst = [0x55u8; 7];
                // SAFETY: source is a valid C string; n fits the destination.
                unsafe { safestrcpy(dst.as_mut_ptr().add(1).cast(), src.as_ptr(), n) };
                assert_eq!(dst[0], 0x55);
                let copied = src.to_bytes().len().min(n.saturating_sub(1));
                let end = if n == 0 { 1 } else { copied + 2 };
                assert!(dst[end..].iter().all(|&b| b == 0x55));
                if n > 0 {
                    assert_eq!(&dst[1..1 + copied], &src.to_bytes()[..copied]);
                    assert_eq!(dst[1 + copied], 0);
                }
            }
        }
    }

    #[test]
    fn safe_copy_handles_empty_full_and_truncated_buffers() {
        assert_eq!(copy_cstr(&mut [], c"abc"), 0);
        let mut dst = [0x55; 6];
        assert_eq!(copy_cstr(&mut dst[..1], c"abc"), 0);
        assert_eq!(dst, [0, 0x55, 0x55, 0x55, 0x55, 0x55]);
        assert_eq!(copy_cstr(&mut dst[..3], c"abcdef"), 2);
        assert_eq!(&dst[..3], b"ab\0");
        assert_eq!(copy_cstr(&mut dst, c"a"), 1);
        assert_eq!(&dst[..2], b"a\0");
        assert_eq!(dst[3], 0x55);
    }

    #[test]
    fn comparisons_use_unsigned_bytes_on_every_target() {
        let high = [0xffu8, 0];
        let low = [0x7fu8, 0];
        // SAFETY: both arrays contain valid terminated strings.
        unsafe {
            assert!(strcmp(high.as_ptr().cast(), low.as_ptr().cast()) > 0);
            assert!(strncmp(high.as_ptr().cast(), low.as_ptr().cast(), 1) > 0);
        }
    }

    #[test]
    fn tokenizer_handles_null_saved_cursor_and_repeated_exhaustion() {
        let mut saved = ptr::null_mut();
        let mut bytes = *b"//a///b/\0";
        // SAFETY: bytes is writable and terminated, the delimiter is a valid
        // C string, and saved is exclusively borrowed for each call.
        unsafe {
            assert!(strtok_r(ptr::null_mut(), c"/".as_ptr(), &mut saved).is_null());
            let a = strtok_r(bytes.as_mut_ptr().cast(), c"/".as_ptr(), &mut saved);
            assert_eq!(CStr::from_ptr(a), c"a");
            let b = strtok_r(ptr::null_mut(), c"/".as_ptr(), &mut saved);
            assert_eq!(CStr::from_ptr(b), c"b");
            assert!(strtok_r(ptr::null_mut(), c"/".as_ptr(), &mut saved).is_null());
            assert!(strtok_r(ptr::null_mut(), c"/".as_ptr(), &mut saved).is_null());
        }
    }

    #[test]
    fn memory_primitives_cover_overlap_and_zero_length() {
        let mut bytes = *b"abcdef";
        // SAFETY: the ranges are in bytes; overlap is supported by memmove.
        unsafe {
            memmove(bytes.as_mut_ptr().add(1).cast(), bytes.as_ptr().cast(), 5);
            assert_eq!(&bytes, b"aabcde");
            memmove(bytes.as_mut_ptr().cast(), bytes.as_ptr().add(1).cast(), 5);
            assert_eq!(&bytes, b"abcdee");
            memset(bytes.as_mut_ptr().cast(), 0x1ff, 3);
            assert_eq!(&bytes[..3], &[0xff; 3]);
            assert_eq!(memcmp(bytes.as_ptr().cast(), bytes.as_ptr().cast(), 6), 0);
            strncpy(ptr::null_mut(), ptr::null(), 0);
            safestrcpy(ptr::null_mut(), ptr::null(), 0);
        }
    }
}
