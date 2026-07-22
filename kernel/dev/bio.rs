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

use crate::bindings::{bio, bio_vec, blkdev_t, bool_, kobject, page_t, PGSIZE};
use crate::kobject::{HasKobject, Kobject};

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block, matching the
// established convention (`kernel/dev/blkdev.rs`, `kernel/dev/dev.rs`)
// of declaring cross-module C-ABI calls locally rather than reaching
// through a shared facade, even for symbols defined elsewhere in this
// same crate.
// ---------------------------------------------------------------------------

// P3-D3c: `kobject.rs`'s entry points are genuinely `unsafe fn`s now that
// their `#[no_mangle]` exports are gone; this file's original extern
// declarations were plain (non-`safe`) `fn`s, so every call site already
// sits in an `unsafe` context -- the plain `use` keeps them unchanged.
// KERNEL-OO: now reached as `Kobject::` assoc fns (`Kobject` already in
// scope via the `HasKobject, Kobject` import above).

unsafe extern "C" {
    // kernel/string.rs.
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
}

// P3-D3b: lock/completion.rs's `completion_init` is a plain safe Rust fn
// now that its `#[no_mangle]` export is gone; reached by crate path (the
// call site sits in an `unsafe` block that stays required for its raw
// field projections).
use crate::lock::completion::RawCompletion;

// P3-D3a: `kmm_alloc`/`kmm_free` are genuinely `unsafe fn` in
// `crate::mm::kalloc` now that their `#[no_mangle]` exports are gone;
// this file's original extern declarations were plain (non-`safe`) `fn`,
// so every call site already sits in an `unsafe` context — the plain
// `use` keeps them unchanged.
// `kmm_alloc`/`kmm_free` are now `impl Kmem` (kalloc.rs KERNEL-OO wave);
// called fully-qualified at their call sites below instead of `use`-imported.

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

// P3-CS14: `bio_alloc` now builds a `KResult<*mut bio>` internally and
// encodes it to the ABI-fixed `ERR_PTR` exactly once at the `extern "C"`
// boundary via `result_to_errptr`; only the error-return encoding changed
// (canonical home `crate::kstd`, P3-CS1 centralization).
use crate::kstd::{result_to_errptr, Errno, KResult};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N4 (bio family).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/dev/
// bio_types.h`'s `struct bio_vec`/`struct bio` now: `build.rs`
// blocklists the bindgen-generated forms and injects `pub use
// crate::dev::bio::... as ...;` facade re-exports (no `_t` typedefs
// exist for either). The anonymous C bitfield struct
// `struct { uint64 valid : 1; uint64 rw : 1; uint64 done : 1; }` became
// the named 8/8 `{bits,_pad}` holder `BioFlagBits` (N3 precedent;
// consumers' `__bindgen_anon_1` sites re-pointed to `.flags`). The C
// flexible array member `struct bio_vec bvecs[0]` keeps bindgen's own
// zero-sized `__IncompleteArrayField` representation (same helper type,
// same accessors — `bvecs.as_mut_ptr()` call sites unchanged). The
// explicit `_pad0` field reproduces bindgen's `__bindgen_padding_0:
// [u64; 2]` (`error`+4 → 8-aligned 112 → 128): the C `completion_t`
// *typedef* carries `__ALIGNED_CACHELINE`, and its native
// `RawCompletion` is genuinely 128/64, so `io_completion` lands at 128
// either way — the field is kept for byte-for-byte form fidelity with
// the pre-nativization output.
//
// Copy fidelity: `bio_vec` derived Copy/Clone in the pre-nativization
// bindgen output; `bio` derived NEITHER (kobject-embedder derive
// pattern, same as `device_instance`), so `Bio` deliberately has no
// derives.
// ---------------------------------------------------------------------------

/// `struct bio_vec` (`kernel/inc/dev/bio_types.h`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct BioVec {
    pub bv_page: *mut page_t,
    pub len: crate::bindings::uint16,
    pub offset: crate::bindings::uint16,
}

/// Native replacement for the anonymous C bitfield struct
/// `struct { uint64 valid : 1; uint64 rw : 1; uint64 done : 1; }`
/// inside `struct bio` (bindgen's `bio__bindgen_ty_1`: a 1-byte
/// `__BindgenBitfieldUnit` + 7 pad bytes, `repr(C, align(8))`, 8/8).
/// riscv64 is little-endian, so C allocates `valid` at bit 0, `rw` at
/// bit 1 and `done` at bit 2 of the (8-byte) unit's byte 0 — identical
/// to bindgen's `get(0,1)`/`get(1,1)`/`get(2,1)` accessors reproduced
/// below.
#[repr(C, align(8))]
#[derive(Copy, Clone)]
pub struct BioFlagBits {
    bits: u8,
    _pad: [u8; 7],
}

impl BioFlagBits {
    #[inline]
    pub(crate) fn valid(&self) -> u64 {
        (self.bits & 0b001) as u64
    }
    #[inline]
    pub(crate) fn set_valid(&mut self, val: u64) {
        self.bits = (self.bits & !0b001) | (((val as u8) & 0b01) << 0);
    }
    #[inline]
    pub(crate) fn rw(&self) -> u64 {
        ((self.bits & 0b010) >> 1) as u64
    }
    #[inline]
    pub(crate) fn set_rw(&mut self, val: u64) {
        self.bits = (self.bits & !0b010) | (((val as u8) & 0b01) << 1);
    }
    #[inline]
    pub(crate) fn done(&self) -> u64 {
        ((self.bits & 0b100) >> 2) as u64
    }
    #[inline]
    pub(crate) fn set_done(&mut self, val: u64) {
        self.bits = (self.bits & !0b100) | (((val as u8) & 0b01) << 2);
    }
}

/// `struct bio` (`kernel/inc/dev/bio_types.h`). The align(64) is the
/// `completion_t` typedef's `__ALIGNED_CACHELINE` riding in through the
/// `io_completion` member, exactly as bindgen emitted it
/// (`#[repr(C)] #[repr(align(64))]`).
#[repr(C, align(64))]
pub struct Bio {
    pub kobj: Kobject,
    pub list_entry: crate::bindings::list_node_t,
    pub bdev: *mut blkdev_t,
    pub block_shift: crate::bindings::uint16,
    pub vec_length: crate::bindings::int16,
    pub size: crate::bindings::uint16,
    pub done_size: crate::bindings::uint16,
    pub blkno: crate::bindings::uint64,
    pub flags: BioFlagBits,
    pub end_io: Option<unsafe extern "C" fn(bio: *mut Bio)>,
    pub private_data: *mut c_void,
    pub error: c_int,
    pub(crate) _pad0: [u64; 2],
    pub io_completion: crate::bindings::completion_t,
    pub bvecs: crate::bindings::__IncompleteArrayField<BioVec>,
}

// P3-N4 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// bio_vec { bv_page: *mut page_t, len: uint16, offset: uint16 }`,
// `pub struct bio { kobj: kobject, list_entry: list_node_t, bdev: *mut
// blkdev_t, block_shift: uint16, vec_length: int16, size: uint16,
// done_size: uint16, blkno: uint64, __bindgen_anon_1: bio__bindgen_ty_1,
// end_io: Option<unsafe extern "C" fn(*mut bio)>, private_data: *mut
// c_void, error: c_int, __bindgen_padding_0: [u64; 2], io_completion:
// completion_t, bvecs: __IncompleteArrayField<bio_vec> }`,
// `#[repr(align(64))]`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe (rv64gc/lp64d) against
// `kernel/inc/dev/bio_types.h` — see the P3-N4 wave record: bio_vec
// 16/8 offsets 0/8/10, bio 256/64 offsets 0/40/56/64/66/68/70/72/[80]/
// 88/96/104/128/256 (the anonymous bitfield struct occupies [80,88);
// completion_t itself proven 128/64 by the same probe; `bvecs` — the
// C flexible array member — sits at the padded struct end, 256).
const _: () = {
    assert!(core::mem::size_of::<BioVec>() == 16, "bio_vec size");
    assert!(core::mem::align_of::<BioVec>() == 8, "bio_vec alignment");
    assert!(core::mem::offset_of!(BioVec, bv_page) == 0, "bio_vec.bv_page offset");
    assert!(core::mem::offset_of!(BioVec, len) == 8, "bio_vec.len offset");
    assert!(core::mem::offset_of!(BioVec, offset) == 10, "bio_vec.offset offset");
    assert!(core::mem::size_of::<BioFlagBits>() == 8, "bio anon bitfield size");
    assert!(core::mem::align_of::<BioFlagBits>() == 8, "bio anon bitfield alignment");
    assert!(core::mem::size_of::<crate::bindings::completion_t>() == 128, "completion_t size");
    assert!(core::mem::align_of::<crate::bindings::completion_t>() == 64, "completion_t alignment");
    assert!(core::mem::size_of::<Bio>() == 256, "bio size");
    assert!(core::mem::align_of::<Bio>() == 64, "bio alignment");
    assert!(core::mem::offset_of!(Bio, kobj) == 0, "bio.kobj offset");
    assert!(core::mem::offset_of!(Bio, list_entry) == 40, "bio.list_entry offset");
    assert!(core::mem::offset_of!(Bio, bdev) == 56, "bio.bdev offset");
    assert!(core::mem::offset_of!(Bio, block_shift) == 64, "bio.block_shift offset");
    assert!(core::mem::offset_of!(Bio, vec_length) == 66, "bio.vec_length offset");
    assert!(core::mem::offset_of!(Bio, size) == 68, "bio.size offset");
    assert!(core::mem::offset_of!(Bio, done_size) == 70, "bio.done_size offset");
    assert!(core::mem::offset_of!(Bio, blkno) == 72, "bio.blkno offset");
    assert!(core::mem::offset_of!(Bio, flags) == 80, "bio anon bitfield offset");
    assert!(core::mem::offset_of!(Bio, end_io) == 88, "bio.end_io offset");
    assert!(core::mem::offset_of!(Bio, private_data) == 96, "bio.private_data offset");
    assert!(core::mem::offset_of!(Bio, error) == 104, "bio.error offset");
    assert!(core::mem::offset_of!(Bio, io_completion) == 128, "bio.io_completion offset");
    assert!(core::mem::offset_of!(Bio, bvecs) == 256, "bio.bvecs offset");
};

// ---------------------------------------------------------------------------
// KArc<bio> plumbing (Phase 3 P3-9b).
// ---------------------------------------------------------------------------

// `struct bio` embeds a `struct kobject` as its *first* field (see the
// module doc's "already real bindgen types" paragraph), so this is a
// plain reinterpret cast, not `container_of` arithmetic -- identical
// reasoning to `kernel/dev/dev.rs`'s `device_t` impl.
// SAFETY: `bio.kobj` is a stable, non-moving offset-0 field, initialised
// by `kobject_init` in `bio_alloc` below before any pointer to the `bio`
// is ever handed out, and live for as long as any reference to that
// `bio` is held (`bio_release`/`kobject_put` is the only thing that can
// invalidate it, and only once the count reaches zero) -- exactly
// `HasKobject`'s contract.
unsafe impl HasKobject for bio {
    fn kobj_ptr(this: *mut Self) -> *mut Kobject {
        this as *mut Kobject
    }
}

// ---------------------------------------------------------------------------
// kobject release callback.
// ---------------------------------------------------------------------------

/// Mirrors `__bio_relase_kobj_cb()`. See the module doc's "Preserved-as-
/// is" note: the C original's page-cleanup loop was entirely commented
/// out, so this is faithfully just a free.
///
/// FLOOR: address-taken `extern "C"` kobject-release callback (installed
/// as a bare fn-pointer at `Bio::alloc_inner`'s `(*bio_ptr).kobj.ops.
/// release = Some(bio_release_kobj_cb)` below) -- kept a free fn per this
/// wave's floor rule, matching `dev/dev.rs`'s `underlying_kobject_release`.
extern "C" fn bio_release_kobj_cb(obj: *mut kobject) {
    // `kobj` is `struct bio`'s first field (offset 0) -- plain
    // reinterpret cast, not `container_of` arithmetic (see module doc).
    let bio_ptr = obj as *mut bio;
    // SAFETY: `kobject_put` (this callback's only caller) guarantees
    // `obj` was live and its refcount just reached zero, i.e. `bio_ptr`
    // is a valid, uniquely-owned `struct bio` allocation from
    // `kmm_alloc` in `Bio::alloc_inner` below -- freeing it here is
    // exactly the deallocation `Bio::alloc_inner` paired it with.
    unsafe { crate::mm::kalloc::Kmem::kmm_free(bio_ptr as *mut c_void) };
}

// ---------------------------------------------------------------------------
// Public C ABI -- exact symbol/signature parity with `kernel/inc/dev/bio.h`.
//
// KERNEL-OO: the five externally-linked entry points (plus the internal
// `alloc_inner` helper) are now associated functions on [`Bio`] (the
// `bio`-wrapping native type) rather than free `bio_*` functions --
// behavior-preserving relocation only: raw `*mut bio` params kept (`Bio`
// is Freeze and shared cross-hart via the kobject refcount/RCU-adjacent
// device table, so no `&self`/`&Bio` receiver is introduced), bodies
// byte-identical, `unsafe extern "C"` signatures preserved exactly so
// `Bio::method` stays usable wherever a raw fn-pointer of the old
// signature was expected.
// ---------------------------------------------------------------------------

impl Bio {
    /// Allocate a `bio` with `vec_length` (uninitialised) segments. Returns
    /// an `ERR_PTR` on invalid arguments or allocation failure.
    // P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`)
    // now import this via crate-path `use` instead of an `extern`
    // redeclaration -- demoted.
    pub(crate) extern "C" fn alloc(
        bdev: *mut blkdev_t,
        vec_length: i16,
        rw: bool_,
        end_io: Option<unsafe extern "C" fn(bio: *mut bio)>,
        private_data: *mut c_void,
    ) -> *mut bio {
        result_to_errptr(Self::alloc_inner(bdev, vec_length, rw, end_io, private_data))
    }

    fn alloc_inner(
        bdev: *mut blkdev_t,
        vec_length: i16,
        rw: bool_,
        end_io: Option<unsafe extern "C" fn(bio: *mut bio)>,
        private_data: *mut c_void,
    ) -> KResult<*mut bio> {
        if bdev.is_null() || vec_length <= 0 || vec_length > BIO_MAX_VECS {
            return Err(Errno::Inval);
        }
        let bio_size = core::mem::size_of::<bio>() + (vec_length as usize) * core::mem::size_of::<bio_vec>();
        let bio_ptr = unsafe { crate::mm::kalloc::Kmem::kmm_alloc(bio_size) } as *mut bio;
        if bio_ptr.is_null() {
            return Err(Errno::NoMem);
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
            (*bio_ptr).flags.set_rw(rw as u64);
            (*bio_ptr).end_io = end_io;
            (*bio_ptr).private_data = private_data;
            (*bio_ptr).kobj.name = c"bio".as_ptr();
            (*bio_ptr).kobj.ops.release = Some(bio_release_kobj_cb);
            Kobject::kobject_init(&raw mut (*bio_ptr).kobj);
            RawCompletion::init(&raw mut (*bio_ptr).io_completion);
        }
        Ok(bio_ptr)
    }

    /// Install a data segment (`page`/`len`/`offset`) at index `idx` of a
    /// not-yet-submitted `bio`. Returns `0` on success, a negative errno
    /// otherwise.
    // P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`)
    // now import this via crate-path `use` instead of an `extern`
    // redeclaration -- demoted.
    pub(crate) extern "C" fn add_seg(bio_ptr: *mut bio, page: *mut page_t, idx: i16, len: u16, offset: u16) -> c_int {
        if bio_ptr.is_null() || page.is_null() || len == 0 {
            return neg(crate::bindings::EINVAL);
        }
        // SAFETY: `bio_ptr` is non-null, caller-owned (not yet submitted, so
        // exclusively accessed by this thread per the bio lifecycle
        // contract in the module doc); every field read/write below is a
        // plain, in-bounds access on that live allocation once `idx` is
        // validated against `vec_length` just below.
        unsafe {
            if (*bio_ptr).flags.valid() != 0 || (*bio_ptr).flags.done() != 0 {
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
    // `dev/cdev.rs`'s `Cdev::dup`.
    #[allow(dead_code)]
    pub(crate) extern "C" fn dup(bio_ptr: *mut bio) -> c_int {
        if bio_ptr.is_null() {
            return neg(crate::bindings::EINVAL);
        }
        // SAFETY: `bio_ptr` non-null, caller holds a reference (`Bio::dup`'s
        // documented precondition, `kernel/inc/dev/bio.h`).
        unsafe { Kobject::kobject_get(&raw mut (*bio_ptr).kobj) };
        0
    }

    /// Decrement a `bio`'s reference count, freeing it via
    /// [`bio_release_kobj_cb`] once it reaches zero.
    // P3-1D mesh sweep: callers (`bufcache.rs`, `vfs/xv6fs/superblock.rs`)
    // now import this via crate-path `use` instead of an `extern`
    // redeclaration -- demoted.
    pub(crate) extern "C" fn release(bio_ptr: *mut bio) -> c_int {
        if bio_ptr.is_null() {
            return neg(crate::bindings::EINVAL);
        }
        // SAFETY: `bio_ptr` non-null, caller holds a reference being given up.
        unsafe { Kobject::kobject_put(&raw mut (*bio_ptr).kobj) };
        0
    }

    /// Validate a `bio`'s fields against its target block device before
    /// submission. Returns `0` if valid, `-EINVAL` otherwise.
    // P3-1D mesh sweep: callers (`dev/blkdev.rs`, `dev/x1_sdhci.rs`) now
    // import this via crate-path `use` instead of an `extern` redeclaration --
    // demoted.
    pub(crate) extern "C" fn validate(bio_ptr: *mut bio, blkdev: *mut blkdev_t) -> c_int {
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
            if Kobject::kobject_refcount(&raw mut (*bio_ptr).kobj) <= 0 {
                return neg(crate::bindings::EINVAL);
            }
            if (*bio_ptr).error != 0 {
                return neg(crate::bindings::EINVAL);
            }
            if (*bio_ptr).flags.valid() != 0 || (*bio_ptr).flags.done() != 0 {
                return neg(crate::bindings::EINVAL);
            }

            // `vec_length` was bounds-checked (`0 < vec_length <= BIO_MAX_VECS`)
            // above, so `bvecs[0..vec_length)` is the bio's own CPU-side segment
            // metadata (not a DMA-written region -- the device writes the target
            // *pages*, not these descriptors) and safe to walk as a slice.
            let bvecs = core::slice::from_raw_parts((*bio_ptr).bvecs.as_ptr(), (*bio_ptr).vec_length as usize);
            let mut total_size: u32 = 0;
            for bvec in bvecs {
                if bvec.bv_page.is_null() {
                    return neg(crate::bindings::EINVAL);
                }
                if bvec.offset as u32 + bvec.len as u32 > PGSIZE {
                    return neg(crate::bindings::EINVAL);
                }
                total_size += bvec.len as u32;
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
}
