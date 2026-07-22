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
//! ## Signatures
//!
//! Every function below keeps the exact C name and signature from
//! `kernel/inc/string.h`, so the ~40 files that `#include "string.h"` and
//! the many existing Rust `unsafe extern "C" { pub safe fn memset(...);
//! }`-style re-declarations (see the crate-root doc comment on
//! `clashing_extern_declarations`) keep linking unchanged — this is a
//! pure symbol-provenance change, C source to Rust source, nothing else.
//!
//! `char` is unsigned on this RISC-V target/ABI (`__CHAR_UNSIGNED__`,
//! confirmed via the toolchain's predefined macros), and `core::ffi::
//! c_char` mirrors that on `riscv64gc-unknown-none-elf` (confirmed:
//! `c_char::MIN == 0`), so ordinary `as c_int` promotion below already
//! reproduces the C `unsigned char` subtraction semantics used by
//! `memcmp`/`strcmp`/`strncmp` — no explicit `as u8` massaging needed.
//!
//! `strtok`'s C `static char *saveptr` file-scope state (already
//! non-thread-safe in the original — not this port's job to fix) is
//! reproduced with a private `AtomicPtr` instead of `static mut`, which
//! is memory-safe to touch from `unsafe` code without extra ceremony but
//! preserves the exact (single last-caller-wins) semantics.
//!
//! One deliberate behavior fix versus the literal C source: `strstr`'s
//! C body computes `haystack_len - needle_len` as an unsigned subtraction
//! that underflows (wraps to a huge value) when `needle` is longer than
//! `haystack`, driving the search loop out of bounds. Zero callers of
//! `strstr` exist anywhere in this repository (checked via full-tree
//! grep), so this is invisible to every test in the verification matrix;
//! the Rust version adds the missing length guard rather than
//! reproducing the latent out-of-bounds read as real Rust-pointer UB.
//! The public signature also follows `string.h`'s prototype
//! (`const char *`) rather than the (unused, non-matching) `char *` the
//! `.c` definition actually used.

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicPtr, Ordering};

// `kmm_alloc` is now `impl Kmem` (kalloc.rs KERNEL-OO wave); called
// fully-qualified at its call sites below instead of `use`-imported.

// ---------------------------------------------------------------------------
// memset / memcmp / memmove / memcpy — see module doc for the no-recursion
// and no-compiler_builtins-conflict rationale. Raw pointer arithmetic only.
// ---------------------------------------------------------------------------

/// # Safety
/// `dst` must be valid for writes of `n` bytes.
#[no_mangle]
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
#[no_mangle]
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
#[no_mangle]
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
#[no_mangle]
pub unsafe extern "C" fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void {
    unsafe { memmove(dst, src, n) }
}

// ---------------------------------------------------------------------------
// String functions.
// ---------------------------------------------------------------------------

/// # Safety
/// `p` and `q` must point to valid NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn strcmp(p: *const c_char, q: *const c_char) -> c_int {
    unsafe {
        let mut p = p;
        let mut q = q;
        while *p != 0 && *p == *q {
            p = p.add(1);
            q = q.add(1);
        }
        (*p as c_int) - (*q as c_int)
    }
}

/// # Safety
/// `p` and `q` must be valid for reads until a NUL byte or `n` bytes,
/// whichever comes first.
#[no_mangle]
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
        (*p as c_int) - (*q as c_int)
    }
}

/// # Safety
/// `s` must be valid for writes of `n` bytes; `t` valid for reads up to
/// its NUL terminator or `n` bytes, whichever comes first.
#[no_mangle]
pub unsafe extern "C" fn strncpy(s: *mut c_char, t: *const c_char, n: usize) -> *mut c_char {
    unsafe {
        let os = s;
        let mut s = s;
        let mut t = t;
        let mut n = n;
        while n > 0 {
            let ch = *t;
            t = t.add(1);
            *s = ch;
            s = s.add(1);
            if ch == 0 {
                break;
            }
            n -= 1;
        }
        while n > 0 {
            n -= 1;
            *s = 0;
            s = s.add(1);
        }
        os
    }
}

/// Like `strncpy` but guaranteed to NUL-terminate.
///
/// # Safety
/// `s` must be valid for writes of `n` bytes; `t` valid for reads up to
/// its NUL terminator or `n - 1` bytes, whichever comes first.
#[no_mangle]
pub unsafe extern "C" fn safestrcpy(s: *mut c_char, t: *const c_char, n: usize) -> *mut c_char {
    unsafe {
        let os = s;
        if n == 0 {
            return os;
        }
        let mut s = s;
        let mut t = t;
        let mut n = n;
        loop {
            n -= 1;
            if n == 0 {
                break;
            }
            let ch = *t;
            t = t.add(1);
            *s = ch;
            s = s.add(1);
            if ch == 0 {
                break;
            }
        }
        *s = 0;
        os
    }
}

/// # Safety
/// `s` must point to a valid NUL-terminated string.
#[no_mangle]
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
#[no_mangle]
pub unsafe extern "C" fn strnlen(s: *const c_char, maxlen: usize) -> usize {
    unsafe {
        let mut n = 0usize;
        while n < maxlen && *s.add(n) != 0 {
            n += 1;
        }
        n
    }
}

/// Zero-sized facade for the handful of `string.rs` helpers that are
/// *not* compiler/C-ABI floor (see the module doc's `memcpy`/`memset`/
/// `strdup`-class rationale for why the rest stay free `#[no_mangle]`
/// fns): `strcat`/`strtok`/`strstr` are plain `pub(crate)` Rust-only
/// entry points, callable from nowhere by symbol name, so they become
/// associated fns here (KERNEL-OO final wave).
pub(crate) struct Strings;

impl Strings {
    /// # Safety
    /// `dest` must point to a NUL-terminated string with enough trailing
    /// room to also hold `src`'s contents plus a NUL; `src` must point to a
    /// valid NUL-terminated string.
    pub(crate) unsafe fn strcat(dest: *mut c_char, src: *const c_char) -> *mut c_char {
        unsafe {
            let n = strlen(dest);
            let m = strlen(src);
            strncpy(dest.add(n), src, m);
            *dest.add(n + m) = 0;
            dest
        }
    }
}

/// # Safety
/// `str_` (when non-null) and `delim` must point to valid NUL-terminated
/// strings; `saveptr` must be a valid, writable `*mut *mut c_char`, and
/// when `str_` is null, `*saveptr` must be a value this function
/// previously wrote there (or null).
#[no_mangle]
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

/// File-scope `strtok` cursor. Mirrors the C `static char *saveptr` —
/// still not thread/hart-safe (same as the original), just not built on
/// `static mut`.
static STRTOK_SAVEPTR: AtomicPtr<c_char> = AtomicPtr::new(ptr::null_mut());

impl Strings {
    /// # Safety
    /// Same contract as `strtok_r`'s `str_`/`delim`, using the shared
    /// `STRTOK_SAVEPTR` cursor in place of an explicit `saveptr`.
    pub(crate) unsafe fn strtok(str_: *mut c_char, delim: *const c_char) -> *mut c_char {
        unsafe {
            let mut saveptr = STRTOK_SAVEPTR.load(Ordering::Relaxed);
            let result = strtok_r(str_, delim, &mut saveptr as *mut *mut c_char);
            STRTOK_SAVEPTR.store(saveptr, Ordering::Relaxed);
            result
        }
    }

    /// Bounded substring search. See the module doc for the one deliberate
    /// behavior fix versus the literal (unused, latently out-of-bounds) C
    /// body: `needle` longer than `haystack` now returns null instead of
    /// underflowing the loop bound.
    ///
    /// # Safety
    /// `haystack` and `needle` must point to valid NUL-terminated strings.
    pub(crate) unsafe fn strstr(
        haystack: *const c_char,
        needle: *const c_char,
    ) -> *const c_char {
        unsafe {
            let needle_len = strlen(needle);
            if needle_len == 0 {
                return haystack;
            }

            let haystack_len = strlen(haystack);
            if needle_len > haystack_len {
                return ptr::null();
            }

            let mut i = 0usize;
            while i <= haystack_len - needle_len {
                if strncmp(haystack.add(i), needle, needle_len) == 0 {
                    return haystack.add(i);
                }
                i += 1;
            }
            ptr::null()
        }
    }
}

/// # Safety
/// `s` must be valid for reads up to its NUL terminator or `n` bytes,
/// whichever comes first. Returns null on allocation failure.
#[no_mangle]
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
/// memcpy/memset/memmove/memcmp; keep `#[no_mangle] extern "C"` here too.
#[no_mangle]
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
