//! Block I/O request object -- Rust port of `kernel/dev/bio.c` (Phase 2
//! Wave 22; see `docs/rustify/phase2_plan.md` and its §0 "Two distinct
//! bio concepts" note).
//!
//! **Not to be confused with [`crate::bufcache`]** (`kernel/bio.c` ->
//! `kernel/bufcache.rs`), the classic xv6 `struct buf` disk-block cache
//! (`bread`/`bwrite`/`brelse`). This file is the *other*, unrelated
//! "bio": a Linux-style, ref-counted block-I/O request descriptor
//! (`struct bio` + `bio_vec` segments) that `bufcache.rs` and the
//! remaining C drivers (`virtio_disk.c`, `ramdisk.c`, `x1_sdhci.c`)
//! build, submit through [`super::blkdev::blkdev_submit_bio`], and wait
//! on. Two layers, same historical name, ported in the same wave per
//! the plan but kept in separate files with separate design notes, as
//! mandated.
//!
//! `struct bio`/`bio_vec` (`dev/bio_types.h`) are already real bindgen
//! types (`crate::bindings::{bio, bio_vec}`, reachable since Phase 2
//! Wave 19/21 via `dev/blkdev.h` -> `dev/bio.h` -> `dev/bio_types.h`,
//! see `kernel/dev/blkdev.rs`'s module doc) -- no opaque stand-ins.
//! `struct bio` embeds a `struct kobject` as its *first* field, so every
//! `*mut kobject` <-> `*mut bio` cast below is a plain offset-0
//! reinterpretation (mirrors `container_of(obj, struct bio, kobj)` on
//! this layout exactly), matching `kernel/dev/dev.rs`'s own
//! `device_t`/`kobject` reasoning.
//!
//! `dev/bio.h`'s several `static inline` helpers (`bio_iter_*`,
//! `bio_dir_write`, `bio_start_io_acct`/`bio_end_io_acct`, `bio_endio`,
//! `bio_complete`, `bio_await`) have no external linkage in C and are
//! **not** touched by this wave -- the header stays a live, unmodified
//! C translation unit, `#include`d by the remaining C drivers directly
//! and reimplemented natively (the one function it needs, `bio_await`)
//! by `kernel/bufcache.rs`. This file ports only `bio.c`'s five
//! externally-linked functions: [`bio_alloc`], [`bio_add_seg`],
//! [`bio_dup`], [`bio_release`], [`bio_validate`].
//!
//! # Preserved-as-is: `bio_release`'s page cleanup is a no-op
//!
//! The C original's kobject release callback had its per-`bio_vec` page
//! refcount teardown loop entirely commented out (dead code, `#if 0`-
//! style) -- only `kmm_free(bio)` ever ran. Ported faithfully: every
//! current caller (`kernel/bufcache.rs`'s `__buf_alloc_bio`,
//! `kernel/vfs/xv6fs/superblock.rs`'s bitmap/data I/O) hands `bio_add_seg`
//! a page it owns independently of the bio's lifetime (a statically
//! preallocated buffer-cache page, or a pcache page pinned by its own
//! refcount) and never expects `bio_release` to drop a page reference on
//! its behalf, so reproducing the commented-out C behavior (a silent
//! no-op) rather than "fixing" it to actually decrement page refcounts
//! is the faithful port -- flipping it live would double-drop every one
//! of those callers' pages.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_int, c_void};

use crate::bindings::{bio, bio_vec, blkdev_t, bool_, completion_t, kobject, page_t, PGSIZE};

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block, matching the
// established convention (`kernel/dev/blkdev.rs`, `kernel/dev/dev.rs`)
// of declaring cross-module C-ABI calls locally rather than reaching
// through a shared facade, even for symbols defined elsewhere in this
// same crate.
// ---------------------------------------------------------------------------

unsafe extern "C" {
    // kernel/kobject.rs.
    fn kobject_init(obj: *mut kobject);
    fn kobject_get(obj: *mut kobject);
    fn kobject_put(obj: *mut kobject);
    fn kobject_refcount(obj: *mut kobject) -> i64;

    // kernel/mm/kalloc.rs (kmm_alloc/kmm_free) + kernel/string.rs (memset).
    fn kmm_alloc(n: usize) -> *mut c_void;
    fn kmm_free(ptr: *mut c_void);
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // kernel/lock/completion.rs.
    fn completion_init(c: *mut completion_t);
}

// ---------------------------------------------------------------------------
// Local constants -- mirrors `dev/bio.h`'s `#define`s. `BIO_MAX_VECS`/
// `BIO_MAX_SIZE` are plain integer literals, not surfaced through
// bindgen (`allowlist_var` only covers `struct`/`static` symbols, not
// header macros with no corresponding C declaration). `E2BIG` is a real
// errno bound by `<errno.h>`, but `build.rs`'s `allowlist_var("E[A-Z]+")`
// regex requires a letter immediately after `E` and doesn't match
// `E2BIG` (digit next) -- every other errno this file needs (`EINVAL`,
// `ENOMEM`, `EIO`) *is* bindgen-captured, so only this one is hand-typed.
// ---------------------------------------------------------------------------

const BIO_MAX_VECS: i16 = 128;
const BIO_MAX_SIZE: u16 = 1 << 15;
const E2BIG: u32 = 7;

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// `ERR_PTR`/`IS_ERR` (`kernel/inc/errno.h`), generic over the pointee
/// type. Reimplemented locally, matching this crate's established
/// per-file convention (see `kernel/dev/blkdev.rs`, `kernel/vfs/inode.rs`).
const MAX_ERRNO: isize = 4095;
#[inline(always)]
fn is_err_value(p: usize) -> bool {
    p >= (-(MAX_ERRNO)) as usize
}
#[inline(always)]
fn err_ptr<T>(errno: c_int) -> *mut T {
    errno as isize as *mut T
}

// ---------------------------------------------------------------------------
// kobject release callback.
// ---------------------------------------------------------------------------

/// Mirrors `__bio_relase_kobj_cb()`. See the module doc's "Preserved-as-
/// is" note: the C original's page-cleanup loop was entirely commented
/// out, so this is faithfully just a free.
extern "C" fn bio_release_kobj_cb(obj: *mut kobject) {
    // `kobj` is `struct bio`'s first field (offset 0) -- plain
    // reinterpret cast, not `container_of` arithmetic (see module doc).
    let bio_ptr = obj as *mut bio;
    // SAFETY: `kobject_put` (this callback's only caller) guarantees
    // `obj` was live and its refcount just reached zero, i.e. `bio_ptr`
    // is a valid, uniquely-owned `struct bio` allocation from
    // `kmm_alloc` in `bio_alloc` below -- freeing it here is exactly
    // the deallocation `bio_alloc` paired it with.
    unsafe { kmm_free(bio_ptr as *mut c_void) };
}

// ---------------------------------------------------------------------------
// Public C ABI -- exact symbol/signature parity with `kernel/inc/dev/bio.h`.
// ---------------------------------------------------------------------------

/// Allocate a `bio` with `vec_length` (uninitialised) segments. Returns
/// an `ERR_PTR` on invalid arguments or allocation failure.
// P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`)
// now import this via crate-path `use` instead of an `extern`
// redeclaration -- demoted.
pub(crate) extern "C" fn bio_alloc(
    bdev: *mut blkdev_t,
    vec_length: i16,
    rw: bool_,
    end_io: Option<unsafe extern "C" fn(bio: *mut bio)>,
    private_data: *mut c_void,
) -> *mut bio {
    if bdev.is_null() || vec_length <= 0 || vec_length > BIO_MAX_VECS {
        return err_ptr(neg(crate::bindings::EINVAL));
    }
    let bio_size = core::mem::size_of::<bio>() + (vec_length as usize) * core::mem::size_of::<bio_vec>();
    let bio_ptr = unsafe { kmm_alloc(bio_size) } as *mut bio;
    if bio_ptr.is_null() {
        return err_ptr(neg(crate::bindings::ENOMEM));
    }
    // SAFETY: `bio_ptr` is a freshly-allocated, uniquely-owned
    // `bio_size`-byte region (checked non-null above); zeroing it before
    // any typed field access is required (matches the C `memset(bio, 0,
    // bio_size)`) and is valid for a `u8`-representable region of this
    // size.
    unsafe {
        memset(bio_ptr as *mut c_void, 0, bio_size);
        (*bio_ptr).bdev = bdev;
        (*bio_ptr).block_shift = (*bdev).block_shift;
        (*bio_ptr).vec_length = vec_length;
        (*bio_ptr).__bindgen_anon_1.set_rw(rw as u64);
        (*bio_ptr).end_io = end_io;
        (*bio_ptr).private_data = private_data;
        (*bio_ptr).kobj.name = c"bio".as_ptr();
        (*bio_ptr).kobj.ops.release = Some(bio_release_kobj_cb);
        kobject_init(&raw mut (*bio_ptr).kobj);
        completion_init(&raw mut (*bio_ptr).io_completion);
    }
    bio_ptr
}

/// Install a data segment (`page`/`len`/`offset`) at index `idx` of a
/// not-yet-submitted `bio`. Returns `0` on success, a negative errno
/// otherwise.
// P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`)
// now import this via crate-path `use` instead of an `extern`
// redeclaration -- demoted.
pub(crate) extern "C" fn bio_add_seg(bio_ptr: *mut bio, page: *mut page_t, idx: i16, len: u16, offset: u16) -> c_int {
    if bio_ptr.is_null() || page.is_null() || len == 0 {
        return neg(crate::bindings::EINVAL);
    }
    // SAFETY: `bio_ptr` is non-null, caller-owned (not yet submitted, so
    // exclusively accessed by this thread per the bio lifecycle
    // contract in the module doc); every field read/write below is a
    // plain, in-bounds access on that live allocation once `idx` is
    // validated against `vec_length` just below.
    unsafe {
        if (*bio_ptr).__bindgen_anon_1.valid() != 0 || (*bio_ptr).__bindgen_anon_1.done() != 0 {
            return neg(crate::bindings::EIO);
        }
        if (*bio_ptr).vec_length <= 0 || (*bio_ptr).vec_length > BIO_MAX_VECS {
            return neg(crate::bindings::EINVAL);
        }
        if idx < 0 || idx >= (*bio_ptr).vec_length {
            return neg(crate::bindings::EINVAL);
        }
        let bvec_ptr = (*bio_ptr).bvecs.as_mut_ptr().add(idx as usize);
        let mut total_size = (*bio_ptr).size.wrapping_sub((*bvec_ptr).len);
        total_size = total_size.wrapping_add(len);
        if total_size > BIO_MAX_SIZE {
            return neg(E2BIG);
        }
        (*bvec_ptr).bv_page = page;
        (*bvec_ptr).len = len;
        (*bvec_ptr).offset = offset;
        (*bio_ptr).size = total_size;
    }
    0
}

/// Increment a `bio`'s reference count.
// P3-1D mesh sweep: no live caller anywhere in the tree today (full-tree
// grep, matches the pre-existing RUST_FORCE_UNDEFINED comment) --
// demoted; `#[allow(dead_code)]` documents the gap, same precedent as
// `dev/cdev.rs`'s `cdev_dup`.
#[allow(dead_code)]
pub(crate) extern "C" fn bio_dup(bio_ptr: *mut bio) -> c_int {
    if bio_ptr.is_null() {
        return neg(crate::bindings::EINVAL);
    }
    // SAFETY: `bio_ptr` non-null, caller holds a reference (bio_dup's
    // documented precondition, `kernel/inc/dev/bio.h`).
    unsafe { kobject_get(&raw mut (*bio_ptr).kobj) };
    0
}

/// Decrement a `bio`'s reference count, freeing it via
/// [`bio_release_kobj_cb`] once it reaches zero.
// P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`)
// now import this via crate-path `use` instead of an `extern`
// redeclaration -- demoted.
pub(crate) extern "C" fn bio_release(bio_ptr: *mut bio) -> c_int {
    if bio_ptr.is_null() {
        return neg(crate::bindings::EINVAL);
    }
    // SAFETY: `bio_ptr` non-null, caller holds a reference being given up.
    unsafe { kobject_put(&raw mut (*bio_ptr).kobj) };
    0
}

/// Validate a `bio`'s fields against its target block device before
/// submission. Returns `0` if valid, `-EINVAL` otherwise.
// P3-1D mesh sweep: callers (`dev/blkdev.rs`, `dev/x1_sdhci.rs`) now
// import this via crate-path `use` instead of an `extern` redeclaration --
// demoted.
pub(crate) extern "C" fn bio_validate(bio_ptr: *mut bio, blkdev: *mut blkdev_t) -> c_int {
    if bio_ptr.is_null() || blkdev.is_null() {
        return neg(crate::bindings::EINVAL);
    }
    // SAFETY: both pointers non-null; `bio_ptr`'s `bvecs[0..vec_length)`
    // are read-only accessed below after `vec_length` itself is bounds-
    // checked, mirroring the C loop exactly.
    unsafe {
        if (*bio_ptr).bdev != blkdev {
            return neg(crate::bindings::EINVAL);
        }
        if (*bio_ptr).block_shift != (*blkdev).block_shift {
            return neg(crate::bindings::EINVAL);
        }
        if (*bio_ptr).vec_length <= 0 || (*bio_ptr).vec_length > BIO_MAX_VECS {
            return neg(crate::bindings::EINVAL);
        }
        if (*bio_ptr).size > BIO_MAX_SIZE {
            return neg(crate::bindings::EINVAL);
        }
        if kobject_refcount(&raw mut (*bio_ptr).kobj) <= 0 {
            return neg(crate::bindings::EINVAL);
        }
        if (*bio_ptr).error != 0 {
            return neg(crate::bindings::EINVAL);
        }
        if (*bio_ptr).__bindgen_anon_1.valid() != 0 || (*bio_ptr).__bindgen_anon_1.done() != 0 {
            return neg(crate::bindings::EINVAL);
        }

        let mut total_size: u32 = 0;
        for i in 0..(*bio_ptr).vec_length {
            let bvec_ptr = (*bio_ptr).bvecs.as_mut_ptr().add(i as usize);
            if (*bvec_ptr).bv_page.is_null() {
                return neg(crate::bindings::EINVAL);
            }
            if (*bvec_ptr).offset as u32 + (*bvec_ptr).len as u32 > PGSIZE {
                return neg(crate::bindings::EINVAL);
            }
            total_size += (*bvec_ptr).len as u32;
            if total_size > BIO_MAX_SIZE as u32 {
                return neg(crate::bindings::EINVAL);
            }
        }

        if total_size != (*bio_ptr).size as u32 {
            return neg(crate::bindings::EINVAL);
        }
    }
    0
}
