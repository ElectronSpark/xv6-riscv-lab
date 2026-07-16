//! SLAB allocator -- Rust port of the original `kernel/mm/slab.c`.
//!
//! All public functions keep the same C ABI / signatures so that the
//! existing kernel callers (declared in `kernel/inc/mm/slab.h`) keep
//! working unchanged. The kernel-side C struct layouts (`slab_cache_t`,
//! `slab_t`, `percpu_slab_cache_t`, `spinlock_t`, `list_node_t`) are
//! mirrored here with `#[repr(C)]`; a set of `_Static_assert`s in
//! `kernel/mm/slab_shims.c` pin those layouts at compile time.
//!
//! Several pieces stay in C (in `slab_shims.c`):
//!   * The global `__all_slab_caches` registry (static initializer).
//!   * `slab_shrink_all` and `slab_dump_all` (variadic printf, registry walk).
//!   * Thin wrappers for the list macros, page-union access and a small
//!     handful of `printf` messages.
//!
//! Everything else -- adaptive slab sizing, three-phase allocation,
//! six-phase free, cross-CPU ownership, optional bitmap tracking, OOM
//! shrink hook -- is implemented here.

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(dead_code)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::sync::KSpinlock;
use core::sync::atomic::{
    AtomicBool, AtomicI32, AtomicI64, AtomicU32, AtomicU64, Ordering,
};

// ===========================================================================
// Constants -- must match the C headers.
// ===========================================================================
const NCPU: usize = 8;
const PAGE_SHIFT: u32 = 12;
const PAGE_SIZE: usize = 1usize << PAGE_SHIFT;

const SLAB_OBJ_MAX_SIZE: usize = PAGE_SIZE;
const SLAB_OBJ_MIN_SHIFT: u32 = 5;
const SLAB_OBJ_MIN_SIZE: usize = 1usize << SLAB_OBJ_MIN_SHIFT;

const SLAB_FLAG_STATIC: u64 = 1;
const SLAB_FLAG_EMBEDDED: u64 = 2;
const SLAB_FLAG_DEBUG_BITMAP: u64 = 4;
const SLAB_FLAG_VALID_MASK: u64 =
    SLAB_FLAG_STATIC | SLAB_FLAG_EMBEDDED | SLAB_FLAG_DEBUG_BITMAP;

const SLAB_STATE_DEQUEUED: u32 = 0;
const SLAB_STATE_FREE: u32 = 1;
const SLAB_STATE_PARTIAL: u32 = 2;
const SLAB_STATE_FULL: u32 = 3;

// Page-union flags (mirror of kernel/inc/mm/page_type.h), needed by the
// page-union access helpers below (`page_attach`/`find_obj_slab`).
const PAGE_FLAG_TYPE_BITS: u64 = 8;
const PAGE_FLAG_TYPE_MASK: u64 = (1u64 << PAGE_FLAG_TYPE_BITS) - 1;
const PAGE_TYPE_SLAB: u64 = 2;
const PAGE_TYPE_TAIL: u64 = 5;

// `sizeof(slab_t)` is needed by `__SLAB_OBJ_OFFSET`. The mirror struct
// below is laid out the same as the C `slab_t`, so we can compute it
// here -- and a static_assert in slab_shims.c keeps both in sync.
const SLAB_T_SIZE: usize = core::mem::size_of::<Slab>();

// ===========================================================================
// Mirror types -- the canonical `Spinlock` / `ListNode` opaque mirrors
// now live in `crate::mm::cffi`; re-export them here so existing
// `crate::mm::slab::{Spinlock,ListNode}` callers compile unchanged.
// ===========================================================================

use crate::mm::cffi::{ListNode, Spinlock};

const SPINLOCK_BYTES: usize = 24;

#[repr(C, align(64))]
pub struct PercpuCache {
    partial_list: ListNode,    // off 0..16
    full_list: ListNode,       // off 16..32
    partial_count: AtomicU32,  // off 32..36
    full_count: AtomicU32,     // off 36..40
    _pad_lock: [u8; 24],       // off 40..64  (align lock to 64)
    lock_bytes: [u8; SPINLOCK_BYTES], // off 64..88
    // align(64) rounds struct size to 128.
}

#[repr(C, align(64))]
pub struct SlabCache {
    name: *const c_char,       // off 0
    flags: u64,                // off 8
    obj_size: usize,           // off 16
    offset: usize,             // off 24
    slab_order: u32,           // off 32
    slab_obj_num: u32,         // off 36
    bitmap_size: u32,          // off 40
    limits: u32,               // off 44
    _pad_pcpu: [u8; 16],       // off 48..64
    percpu_caches: [PercpuCache; NCPU], // off 64..1088
    global_free_list: ListNode,         // off 1088..1104
    _pad_glock: [u8; 48],               // off 1104..1152
    global_free_lock_bytes: [u8; SPINLOCK_BYTES], // off 1152..1176
    global_free_count: AtomicI64,       // off 1176..1184
    slab_total: AtomicI64,              // off 1184..1192
    obj_active: AtomicU64,              // off 1192..1200
    obj_total: AtomicU64,               // off 1200..1208
    cache_list_entry: ListNode,         // off 1208..1224
    // align(64) rounds struct size to 1280.
}

impl PercpuCache {
    /// Pointer to the embedded spinlock. Computed without any raw-pointer
    /// dereference -- safe.
    #[inline]
    fn lock_ptr(&mut self) -> *mut Spinlock {
        core::ptr::addr_of_mut!(self.lock_bytes) as *mut Spinlock
    }
}

impl SlabCache {
    #[inline]
    fn global_lock_ptr(&mut self) -> *mut Spinlock {
        core::ptr::addr_of_mut!(self.global_free_lock_bytes) as *mut Spinlock
    }
}

#[repr(C)]
pub struct Slab {
    list_entry: ListNode,      // off 0
    cache: *mut SlabCache,     // off 16
    page: *mut c_void,         // off 24 -- opaque page_t*
    slab_order: u16,           // off 32
    _pad0: [u8; 6],
    in_use: u64,               // off 40
    next: *mut c_void,         // off 48
    state: u32,                // off 56 (slab_state_t enum)
    _pad1: u32,
    bitmap: *mut u64,          // off 64
    cpu_id: AtomicI32,         // off 72
    _pad2: u32,
}

// ===========================================================================
// Layout pins (compile-time). Formerly `_Static_assert`s in the deleted
// `slab_shims.c` / `slab_shims.rs`; `SlabCache`/`Slab`/`PercpuCache` are
// hand-rolled mirrors of the bindgen-derived C structs, so these guards are
// what actually catches ABI drift between this file and the kernel headers.
// ===========================================================================
const _: () = {
    use crate::bindings::{percpu_slab_cache_t, slab_cache_t, slab_t, spinlock_t};

    assert!(core::mem::size_of::<spinlock_t>() == 24, "spinlock_t size mismatch");
    // NOTE: in C `spinlock_t` is `align(64)` via a GCC typedef-attribute
    // extension that keeps `sizeof == 24`. Rust cannot express that
    // combination (`#[repr(align(N))]` rounds size up to N), so bindgen
    // emits the struct without alignment. The field-offset asserts below
    // are what actually guarantee ABI parity — bindgen inserts explicit
    // `__bindgen_padding_*` to place each embedded `spinlock_t` at the
    // C-correct 64-aligned offset.
    assert!(
        core::mem::size_of::<percpu_slab_cache_t>() == 128,
        "percpu_slab_cache_t size mismatch"
    );
    assert!(
        core::mem::align_of::<percpu_slab_cache_t>() == 64,
        "percpu_slab_cache_t alignment mismatch"
    );
    assert!(core::mem::size_of::<slab_cache_t>() == 1280, "cache total size");
    assert!(NCPU == 8, "Rust slab mirror assumes NCPU == 8");

    // Wave P3-3B: the size/align literals above were previously only
    // pinned indirectly (native mirror asserted against a literal,
    // bindgen type asserted against the same literal, with no direct
    // cross-check tying the two together). Close that gap with direct
    // comparisons.
    assert!(
        core::mem::size_of::<SlabCache>() == core::mem::size_of::<slab_cache_t>(),
        "SlabCache / slab_cache_t size mismatch"
    );
    assert!(
        core::mem::align_of::<SlabCache>() == core::mem::align_of::<slab_cache_t>(),
        "SlabCache / slab_cache_t alignment mismatch"
    );
    assert!(
        core::mem::size_of::<Slab>() == core::mem::size_of::<slab_t>(),
        "Slab / slab_t size mismatch"
    );
    assert!(
        core::mem::align_of::<Slab>() == core::mem::align_of::<slab_t>(),
        "Slab / slab_t alignment mismatch"
    );
    assert!(
        core::mem::size_of::<PercpuCache>() == core::mem::size_of::<percpu_slab_cache_t>(),
        "PercpuCache / percpu_slab_cache_t size mismatch"
    );
    assert!(
        core::mem::align_of::<PercpuCache>() == core::mem::align_of::<percpu_slab_cache_t>(),
        "PercpuCache / percpu_slab_cache_t alignment mismatch"
    );

    assert!(core::mem::offset_of!(slab_cache_t, name) == 0, "cache.name");
    assert!(core::mem::offset_of!(slab_cache_t, flags) == 8, "cache.flags");
    assert!(core::mem::offset_of!(slab_cache_t, obj_size) == 16, "cache.obj_size");
    assert!(core::mem::offset_of!(slab_cache_t, offset) == 24, "cache.offset");
    assert!(core::mem::offset_of!(slab_cache_t, slab_order) == 32, "cache.slab_order");
    assert!(core::mem::offset_of!(slab_cache_t, slab_obj_num) == 36, "cache.slab_obj_num");
    assert!(core::mem::offset_of!(slab_cache_t, bitmap_size) == 40, "cache.bitmap_size");
    assert!(core::mem::offset_of!(slab_cache_t, limits) == 44, "cache.limits");
    assert!(core::mem::offset_of!(slab_cache_t, percpu_caches) == 64, "cache.percpu_caches");
    assert!(
        core::mem::offset_of!(slab_cache_t, global_free_list) == 64 + 128 * NCPU,
        "cache.global_free_list"
    );
    assert!(
        core::mem::offset_of!(slab_cache_t, global_free_lock) == 1152,
        "cache.global_free_lock"
    );
    assert!(
        core::mem::offset_of!(slab_cache_t, global_free_count) == 1176,
        "cache.global_free_count"
    );
    assert!(core::mem::offset_of!(slab_cache_t, slab_total) == 1184, "cache.slab_total");
    assert!(core::mem::offset_of!(slab_cache_t, obj_active) == 1192, "cache.obj_active");
    assert!(core::mem::offset_of!(slab_cache_t, obj_total) == 1200, "cache.obj_total");
    assert!(
        core::mem::offset_of!(slab_cache_t, cache_list_entry) == 1208,
        "cache.cache_list_entry"
    );

    assert!(core::mem::offset_of!(slab_t, list_entry) == 0, "slab.list_entry");
    assert!(core::mem::offset_of!(slab_t, cache) == 16, "slab.cache");
    assert!(core::mem::offset_of!(slab_t, page) == 24, "slab.page");
    assert!(core::mem::offset_of!(slab_t, slab_order) == 32, "slab.slab_order");
    assert!(core::mem::offset_of!(slab_t, in_use) == 40, "slab.in_use");
    assert!(core::mem::offset_of!(slab_t, next) == 48, "slab.next");
    assert!(core::mem::offset_of!(slab_t, state) == 56, "slab.state");
    assert!(core::mem::offset_of!(slab_t, bitmap) == 64, "slab.bitmap");
    assert!(core::mem::offset_of!(slab_t, cpu_id) == 72, "slab.cpu_id");

    // This module's own `Slab` mirror must match the field offsets pinned
    // above (it is already asserted to start with `list_entry` at 0 via the
    // separate `offset_of!(Slab, list_entry) == 0` check further down).
    assert!(core::mem::offset_of!(Slab, cache) == core::mem::offset_of!(slab_t, cache));
    assert!(core::mem::offset_of!(Slab, page) == core::mem::offset_of!(slab_t, page));
};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N6 (mm type family, slab slice).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/mm/
// slab_type.h`'s `struct slab_cache_struct` (`slab_cache_t`), `struct
// slab_struct` (`slab_t`) and the anonymous-struct typedef
// `percpu_slab_cache_t` now: `build.rs` blocklists the bindgen-generated
// forms and injects `pub use crate::mm::slab::... as ...;` facade
// re-exports for both the struct tag and `_t` typedef names. Field
// names/types reproduce bindgen's exactly (`_pad0`/`_pad1` reproduce
// bindgen's `__bindgen_padding_0/1` verbatim — the C
// `__ALIGNED_CACHELINE` rides the `spinlock_t` typedef, the native
// `RawSpinlock` is align 8 in Rust, so the explicit pads carry the
// 64-byte lock placements, exactly as bindgen emitted them); the C
// `_Atomic` qualifiers on the counter fields degrade to plain integers
// in bindgen's output and are reproduced as such (this module's private
// `SlabCache`/`Slab`/`PercpuCache` mirrors above remain the
// atomic-access views, cast per pointer as before — byte-identical
// layout, asserted below). All three derived Copy/Clone in the
// pre-nativization bindgen output; the natives do too.
//
// Layout evidence (P3-N6): temporary in-tree `offset_of!` gate on the
// live bindgen forms (kernel/mm/p3n6_gate.rs, removed with this wave)
// + cross-compiler `_Static_assert` probe (toolchain gcc, rv64gc/
// lp64d — scratchpad p3n6_static_assert_probe.c). gcc's C-header
// layout and bindgen's runtime layout AGREE on every size/align/offset
// of this family (no pipe-style divergence, P3-N5 precedent checked).
// ---------------------------------------------------------------------------

/// `percpu_slab_cache_t` (`kernel/inc/mm/slab_type.h`) — one CPU's
/// partial/full slab lists. `_pad0` reproduces bindgen's
/// `__bindgen_padding_0: [u64; 3]` (places `lock` at its C-correct
/// 64-aligned offset).
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct PercpuSlabCache {
    pub partial_list: crate::bindings::list_node_t,
    pub full_list: crate::bindings::list_node_t,
    pub partial_count: crate::bindings::uint32,
    pub full_count: crate::bindings::uint32,
    pub(crate) _pad0: [u64; 3],
    pub lock: crate::bindings::spinlock_t,
}

/// `struct slab_cache_struct` / `slab_cache_t` (`kernel/inc/mm/
/// slab_type.h`) — one slab cache. `_pad0`/`_pad1` reproduce bindgen's
/// `__bindgen_padding_0/1` (place `percpu_caches`/`global_free_lock`
/// at their C-correct 64-aligned offsets). `percpu_caches` is `[_; 8]`
/// exactly as bindgen emitted for `NCPU == 8` (asserted above).
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct SlabCacheStruct {
    pub name: *const c_char,
    pub flags: crate::bindings::uint64,
    pub obj_size: usize,
    pub offset: usize,
    pub slab_order: crate::bindings::uint32,
    pub slab_obj_num: crate::bindings::uint32,
    pub bitmap_size: crate::bindings::uint32,
    pub limits: crate::bindings::uint32,
    pub(crate) _pad0: [u64; 2],
    pub percpu_caches: [PercpuSlabCache; 8],
    pub global_free_list: crate::bindings::list_node_t,
    pub(crate) _pad1: [u64; 6],
    pub global_free_lock: crate::bindings::spinlock_t,
    pub global_free_count: crate::bindings::int64,
    pub slab_total: crate::bindings::int64,
    pub obj_active: crate::bindings::uint64,
    pub obj_total: crate::bindings::uint64,
    pub cache_list_entry: crate::bindings::list_node_t,
}

/// `struct slab_struct` / `slab_t` (`kernel/inc/mm/slab_type.h`) — one
/// slab descriptor. `state` keeps bindgen's `slab_state_t` c_uint alias;
/// `page` keeps the `*mut page_t` facade path (page family nativized in
/// this same wave).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct SlabStruct {
    pub list_entry: crate::bindings::list_node_t,
    pub cache: *mut SlabCacheStruct,
    pub page: *mut crate::bindings::page_t,
    pub slab_order: crate::bindings::uint16,
    pub in_use: crate::bindings::uint64,
    pub next: *mut c_void,
    pub state: crate::bindings::slab_state_t,
    pub bitmap: *mut crate::bindings::uint64,
    pub cpu_id: c_int,
}

// P3-N6 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (verified in-tree by the temporary
// `offset_of!` gate) and independently confirmed by the cross-compiler
// `_Static_assert` probe (toolchain gcc agrees on every value).
const _: () = {
    assert!(core::mem::size_of::<PercpuSlabCache>() == 128, "percpu_slab_cache_t size");
    assert!(core::mem::align_of::<PercpuSlabCache>() == 64, "percpu_slab_cache_t align");
    assert!(core::mem::offset_of!(PercpuSlabCache, partial_list) == 0, "percpu.partial_list");
    assert!(core::mem::offset_of!(PercpuSlabCache, full_list) == 16, "percpu.full_list");
    assert!(core::mem::offset_of!(PercpuSlabCache, partial_count) == 32, "percpu.partial_count");
    assert!(core::mem::offset_of!(PercpuSlabCache, full_count) == 36, "percpu.full_count");
    assert!(core::mem::offset_of!(PercpuSlabCache, lock) == 64, "percpu.lock");

    assert!(core::mem::size_of::<SlabCacheStruct>() == 1280, "slab_cache_t size");
    assert!(core::mem::align_of::<SlabCacheStruct>() == 64, "slab_cache_t align");
    assert!(core::mem::offset_of!(SlabCacheStruct, name) == 0, "cache.name");
    assert!(core::mem::offset_of!(SlabCacheStruct, flags) == 8, "cache.flags");
    assert!(core::mem::offset_of!(SlabCacheStruct, obj_size) == 16, "cache.obj_size");
    assert!(core::mem::offset_of!(SlabCacheStruct, offset) == 24, "cache.offset");
    assert!(core::mem::offset_of!(SlabCacheStruct, slab_order) == 32, "cache.slab_order");
    assert!(core::mem::offset_of!(SlabCacheStruct, slab_obj_num) == 36, "cache.slab_obj_num");
    assert!(core::mem::offset_of!(SlabCacheStruct, bitmap_size) == 40, "cache.bitmap_size");
    assert!(core::mem::offset_of!(SlabCacheStruct, limits) == 44, "cache.limits");
    assert!(core::mem::offset_of!(SlabCacheStruct, percpu_caches) == 64, "cache.percpu_caches");
    assert!(core::mem::offset_of!(SlabCacheStruct, global_free_list) == 1088, "cache.global_free_list");
    assert!(core::mem::offset_of!(SlabCacheStruct, global_free_lock) == 1152, "cache.global_free_lock");
    assert!(core::mem::offset_of!(SlabCacheStruct, global_free_count) == 1176, "cache.global_free_count");
    assert!(core::mem::offset_of!(SlabCacheStruct, slab_total) == 1184, "cache.slab_total");
    assert!(core::mem::offset_of!(SlabCacheStruct, obj_active) == 1192, "cache.obj_active");
    assert!(core::mem::offset_of!(SlabCacheStruct, obj_total) == 1200, "cache.obj_total");
    assert!(core::mem::offset_of!(SlabCacheStruct, cache_list_entry) == 1208, "cache.cache_list_entry");

    assert!(core::mem::size_of::<SlabStruct>() == 80, "slab_t size");
    assert!(core::mem::align_of::<SlabStruct>() == 8, "slab_t align");
    assert!(core::mem::offset_of!(SlabStruct, list_entry) == 0, "slab.list_entry");
    assert!(core::mem::offset_of!(SlabStruct, cache) == 16, "slab.cache");
    assert!(core::mem::offset_of!(SlabStruct, page) == 24, "slab.page");
    assert!(core::mem::offset_of!(SlabStruct, slab_order) == 32, "slab.slab_order");
    assert!(core::mem::offset_of!(SlabStruct, in_use) == 40, "slab.in_use");
    assert!(core::mem::offset_of!(SlabStruct, next) == 48, "slab.next");
    assert!(core::mem::offset_of!(SlabStruct, state) == 56, "slab.state");
    assert!(core::mem::offset_of!(SlabStruct, bitmap) == 64, "slab.bitmap");
    assert!(core::mem::offset_of!(SlabStruct, cpu_id) == 72, "slab.cpu_id");
};

// ===========================================================================
// FFI surface (declared in slab_shims.c, defs.h, mm/page.h).
//
// All raw extern declarations are wrapped in `unsafe extern "C" { pub safe }`
// (Rust 1.82+), which marks each declared symbol as callable from safe code.
// The thin adapters below convert between C-style scalars/pointers and
// idiomatic Rust types (e.g. `*mut T -> Option<&mut T>`, `c_int -> bool`).
// They no longer need `{ ... }` blocks since the underlying
// declarations are now `safe`.
// ===========================================================================
mod ffi {
    use super::*;

    // Shared kernel primitives (spinlocks, per-cpu, intrusive list,
    // kmm, memset, panic) are declared once in `crate::mm::cffi`; bring
    // them in via re-export so the existing `ffi::spin_lock(...)`,
    // `ffi::xv6_list_first(...)`, etc. call sites compile unchanged.
    pub(crate) use crate::mm::cffi::raw::{
        spin_init, spin_lock, spin_unlock,
        xv6_cpuid, xv6_push_off, xv6_pop_off,
        kmm_alloc, kmm_free,
        xv6_list_init, xv6_list_is_empty, xv6_list_is_detached,
        xv6_list_detach, xv6_list_push_front, xv6_list_push_back,
        xv6_list_pop_front, xv6_list_first, xv6_list_last,
        __panic_start, __panic_end,
    };

pub(crate) use crate::mm::page::{__pa_to_page, __page_alloc, __page_free, __page_to_pa};

    // Slab descriptor / cache descriptor allocators (kalloc.rs). Its real
    // signatures are the generic-allocator `*mut c_void`; this module's
    // extern declaration always typed them with the local `Slab`/
    // `SlabCache` views instead (same "locally convenient pointer type"
    // idiom as `cffi::raw`'s spinlock wrappers) — these thin casts
    // preserve that view for the adapter functions just below.
    #[inline]
    pub fn slab_t_desc_alloc() -> *mut Slab {
        // SAFETY: `crate::mm::kalloc::slab_t_desc_alloc` hands back
        // freshly-allocated, correctly-sized/aligned storage for one
        // `Slab` (it is the dedicated backing allocator for this exact
        // type); reinterpreting the returned `*mut c_void` as `*mut Slab`
        // is the intended use.
        unsafe { crate::mm::kalloc::slab_t_desc_alloc() as *mut Slab }
    }
    #[inline]
    pub fn slab_t_desc_free(s: *mut Slab) {
        // SAFETY: `s` must originate from `slab_t_desc_alloc` above,
        // matching `crate::mm::kalloc::slab_t_desc_free`'s contract.
        unsafe { crate::mm::kalloc::slab_t_desc_free(s as *mut c_void) };
    }
    #[inline]
    pub fn slab_cache_t_alloc() -> *mut SlabCache {
        // SAFETY: see `slab_t_desc_alloc` above; dedicated backing
        // allocator for `SlabCache`.
        unsafe { crate::mm::kalloc::slab_cache_t_alloc() as *mut SlabCache }
    }
    #[inline]
    pub fn slab_cache_t_free(c: *mut SlabCache) {
        // SAFETY: `c` must originate from `slab_cache_t_alloc` above.
        unsafe { crate::mm::kalloc::slab_cache_t_free(c as *mut c_void) };
    }

    // --- Type-adapting wrappers (safe Rust, no unsafe blocks unless noted). ---
    #[inline] pub fn cpuid() -> c_int { xv6_cpuid() }
    #[inline] pub fn push_off() { xv6_push_off() }
    #[inline] pub fn pop_off() { xv6_pop_off() }

    #[inline] pub fn slab_desc_alloc() -> *mut Slab { slab_t_desc_alloc() }
    #[inline] pub fn slab_desc_free(s: *mut Slab) { slab_t_desc_free(s) }
    #[inline] pub fn cache_desc_alloc() -> *mut SlabCache { slab_cache_t_alloc() }
    #[inline] pub fn cache_desc_free(c: *mut SlabCache) { slab_cache_t_free(c) }

    // `__page_alloc`/`__page_free`/`__page_to_pa` are real bindgen-free
    // C-ABI exports of `crate::mm::page` typed `*mut Page` (that module's
    // own real page-descriptor struct); this file's original extern
    // declaration typed them `*mut c_void` (its own opaque view, same
    // idiom as the `Slab`/`SlabCache` wrappers above). Cast, don't `use`.
    #[inline]
    pub fn page_alloc(order: u64, flags: u64) -> *mut c_void {
        // SAFETY: pure pointer-value reinterpretation of the allocator's
        // own return value; ownership/lifetime contract is unchanged.
        unsafe { __page_alloc(order, flags) as *mut c_void }
    }
    #[inline]
    pub fn page_free(page: *mut c_void, order: u64) {
        // SAFETY: `page` must be a value previously returned by
        // `page_alloc` above (same contract `__page_free` always had).
        unsafe { __page_free(page as *mut crate::mm::page::Page, order) };
    }
    #[inline]
    pub fn page_to_pa(page: *mut c_void) -> u64 {
        // SAFETY: see `page_free` above.
        unsafe { __page_to_pa(page as *mut crate::mm::page::Page) }
    }

    // --- Slab-cache registry, page-union access and diagnostics --
    // previously round-tripped through the deleted `slab_shims.rs`; this
    // is the only module that ever called them, so they are implemented
    // directly below instead of behind a C-ABI hop. See the free-standing
    // functions after this `mod ffi` block. ------------------------------
    #[inline] pub fn register_cache(cache: &mut SlabCache) { super::register_cache(cache) }
    #[inline] pub fn unregister_cache(cache: &mut SlabCache) { super::unregister_cache(cache) }
    #[inline] pub fn page_attach(head: *mut c_void, slab: &mut Slab, order: u32) {
        super::page_attach(head, slab, order)
    }
    #[inline] pub fn page_set_order(head: *mut c_void, order: u32) {
        super::page_set_order(head, order)
    }
    /// Returns the slab descriptor associated with `ptr`, or `None`.
    #[inline] pub fn find_obj_slab(ptr: *mut c_void) -> Option<&'static mut Slab> {
        super::find_obj_slab(ptr)
    }

    #[inline] pub fn list_init(entry: &mut ListNode) { xv6_list_init(entry) }
    #[inline] pub fn list_is_empty(head: &ListNode) -> bool {
        xv6_list_is_empty(head) != 0
    }
    #[inline] pub fn list_detach(entry: &mut ListNode) { xv6_list_detach(entry) }
    #[inline] pub fn list_push_front(head: &mut ListNode, entry: &mut ListNode) {
        xv6_list_push_front(head, entry)
    }
    #[inline] pub fn list_push_back(head: &mut ListNode, entry: &mut ListNode) {
        xv6_list_push_back(head, entry)
    }
    /// Pops a node from the head of `head`. Returns `None` if empty.
    /// The returned reference's lifetime is fictitious (`'static`) -- the
    /// node is detached and owned by the caller until reattached.
    #[inline] pub fn list_pop_front(head: &mut ListNode) -> Option<&'static mut ListNode> {
        // SAFETY: the node, if non-null, is detached and uniquely owned.
        unsafe { xv6_list_pop_front(head).as_mut() }
    }
    #[inline] pub fn list_first(head: &ListNode) -> Option<&'static mut ListNode> {
        // SAFETY: as for `list_pop_front`.
        unsafe { xv6_list_first(head).as_mut() }
    }

    #[inline] pub fn log_free_null(fn_name: *const c_char) { super::log_free_null(fn_name) }
    #[inline] pub fn log_no_slab(fn_name: *const c_char, obj: *mut c_void) {
        super::log_no_slab(fn_name, obj)
    }
    #[inline] pub fn log_unattached(slab: &mut Slab, obj: *mut c_void) {
        super::log_unattached(slab, obj)
    }
    #[inline] pub fn log_from_free(
        obj: *mut c_void,
        slab: &mut Slab,
        cache: &mut SlabCache,
        cpu_id: c_int,
    ) {
        super::log_from_free(obj, slab, cache, cpu_id)
    }
    #[inline] pub fn panic(msg: &'static [u8]) -> ! { super::slab_panic(msg) }
    #[inline] pub fn shrink_all() { super::slab_shrink_all() }
}

// `printf` is shared by every diagnostic-printing helper in this module
// (declared once here instead of once per call site). Variadic, so it
// cannot be declared `safe`.

// ===========================================================================
// Panic / diagnostic printf -- previously the `xv6_slab_panic` and four
// `xv6_slab_log_*` round-trips through the deleted `slab_shims.rs`; this
// module is their only caller, so they are now direct `printf` calls.
// ===========================================================================
fn slab_panic(msg: &'static [u8]) -> ! {
    ffi::__panic_start();
    // `msg` is always a NUL-terminated byte-string literal supplied by call
    // sites in this module; render it via the `Cs` adapter as a runtime C
    // string (its content is not known at this call site).
    crate::kprintln!("{}", crate::printf::Cs(msg.as_ptr() as *const c_char));
    ffi::__panic_end()
}

fn log_free_null(fn_name: *const c_char) {
    crate::kprintln!("{}: obj is NULL", crate::printf::Cs(fn_name));
}
fn log_no_slab(fn_name: *const c_char, obj: *mut c_void) {
    crate::kprintln!(
        "{}: slab is NULL for obj={}",
        crate::printf::Cs(fn_name),
        crate::printf::Ptr(obj as u64),
    );
}
fn log_unattached(slab: &mut Slab, obj: *mut c_void) {
    let cpu_id = slab.cpu_id.load(Ordering::Acquire);
    crate::kprintln!(
        "slab_free: ERROR - slab={} not attached to cache, obj={}",
        crate::printf::Ptr(slab as *mut Slab as u64),
        crate::printf::Ptr(obj as u64),
    );
    crate::kprintln!(
        "  slab->page={}, slab->in_use={}, slab->state={}, slab->cpu_id={}",
        crate::printf::Ptr(slab.page as u64),
        slab.in_use as i64,
        slab.state as c_int,
        cpu_id,
    );
}
fn log_from_free(obj: *mut c_void, slab: &mut Slab, cache: &mut SlabCache, cpu_id: c_int) {
    let gfc = cache.global_free_count.load(Ordering::Acquire);
    crate::kprintln!("slab_free: ERROR - object from free slab");
    crate::kprintln!(
        "  obj={}, slab={}, cache='{}'",
        crate::printf::Ptr(obj as u64),
        crate::printf::Ptr(slab as *mut Slab as u64),
        crate::printf::Cs(cache.name),
    );
    crate::kprintln!(
        "  slab->in_use={}, slab->state={}, slab->cpu_id={}",
        slab.in_use as i64,
        slab.state as c_int,
        cpu_id,
    );
    crate::kprintln!("  cache->global_free_count={}", gfc);
}

// ===========================================================================
// Page-union access for slab pages -- previously `xv6_slab_page_attach` /
// `xv6_slab_page_set_order` / `xv6_slab_find_obj_slab` in the deleted
// `slab_shims.rs`. These still operate on the kernel-wide `page_struct`
// (the native `page::PageStruct` facade since P3-N6, rather than the
// private `page::Page` mirror) because that is the type the union layout
// (`type_data.{slab,tail}`) is expressed in; `page.rs` and `slab.rs`
// otherwise stay decoupled from each other's internal representations.
// ===========================================================================
use crate::bindings::page_struct;

#[inline]
fn page_flags_raw(page: *mut page_struct) -> u64 {
    // SAFETY: `page` is a valid page-array entry (guaranteed by every
    // caller: freshly allocated or resolved via `__pa_to_page`).
    unsafe { (*page).flags }
}
#[inline]
fn page_is_type(page: *mut page_struct, ty: u64) -> bool {
    (page_flags_raw(page) & PAGE_FLAG_TYPE_MASK) == (ty & PAGE_FLAG_TYPE_MASK)
}

/// Stamp `slab`/`order` onto the head page of a freshly allocated slab, and
/// mark every tail page as `PAGE_TYPE_TAIL` pointing back at the head.
fn page_attach(head: *mut c_void, slab: &mut Slab, order: u32) {
    let head = head as *mut page_struct;
    // SAFETY: `head` is the first of `1 << order` freshly allocated,
    // exclusively-owned pages (from `page_alloc`); every page in the group
    // is valid to write through this whole function.
    unsafe {
        let slab_var = &raw mut (*head).type_data.slab;
        (*slab_var).slab = slab as *mut Slab as *mut crate::bindings::slab_t;
        (*slab_var).order = order;
        let page_count = 1u64 << order;
        for i in 1..page_count {
            let p = head.add(i as usize);
            (*p).flags = PAGE_TYPE_TAIL;
            let tail_var = &raw mut (*p).type_data.tail;
            (*tail_var).head_page = head;
        }
    }
}

fn page_set_order(head: *mut c_void, order: u32) {
    let head = head as *mut page_struct;
    // SAFETY: `head` is a live PAGE_TYPE_SLAB head page.
    unsafe {
        let slab_var = &raw mut (*head).type_data.slab;
        (*slab_var).order = order;
    }
}

/// Resolve `ptr` (an object inside some slab's data area) back to its
/// owning `Slab` descriptor, or `None` if `ptr` is not slab-backed.
fn find_obj_slab(ptr: *mut c_void) -> Option<&'static mut Slab> {
    if ptr.is_null() {
        return None;
    }
    let page_base = (ptr as u64) & !((PAGE_SIZE as u64) - 1);
    // SAFETY: `page_base` is a page-aligned address; `__pa_to_page` bounds-
    // checks it and returns NULL for anything outside the managed range.
    let page = unsafe { ffi::__pa_to_page(page_base) } as *mut page_struct;
    if page.is_null() {
        return None;
    }
    let header: *mut page_struct = if page_is_type(page, PAGE_TYPE_SLAB) {
        page
    } else if page_is_type(page, PAGE_TYPE_TAIL) {
        // SAFETY: `page` is a valid PAGE_TYPE_TAIL page; `tail.head_page`
        // was set by `page_attach` above and always points at a live
        // PAGE_TYPE_SLAB head page.
        let h = unsafe { (*page).type_data.tail.head_page } as *mut page_struct;
        if h.is_null() || !page_is_type(h, PAGE_TYPE_SLAB) {
            return None;
        }
        h
    } else {
        return None;
    };
    // SAFETY: `header` is a live PAGE_TYPE_SLAB head page.
    let slab_ptr = unsafe { (*header).type_data.slab.slab } as *mut Slab;
    // SAFETY: turning a freshly-resolved raw pointer into a `&mut` is sound
    // because the slab descriptor is uniquely owned by the allocator's
    // free path while this reference is held.
    unsafe { slab_ptr.as_mut() }
}

// ===========================================================================
// Global slab-cache registry (`__all_slab_caches`) -- previously the
// `xv6_slab_register_cache` / `xv6_slab_unregister_cache` / registry
// globals in the deleted `slab_shims.rs`. A single intrusive list threading
// every live `SlabCache` through its `cache_list_entry` field, guarded by
// one global spinlock and lazily initialized on first use (mirrors the C
// original's runtime-init dance -- Rust `static`s cannot run a
// self-referential list initializer at compile time).
// ===========================================================================
// SAFETY: the only `ListNode` stored here is `ALL_SLAB_CACHES` below,
// whose contents are never accessed except while holding
// `ALL_SLAB_CACHES_LOCK` (see every `RegistryLock::lock_ptr()` call
// site) — the spinlock is the actual synchronization, this impl just
// tells the compiler the `UnsafeCell` may cross thread/hart
// boundaries.
struct RegistryList(UnsafeCell<ListNode>);
unsafe impl Sync for RegistryList {}

// SAFETY: the embedded `[u8; SPINLOCK_BYTES]` is the raw C `spinlock_t`
// storage for `ALL_SLAB_CACHES_LOCK`; `spin_lock`/`spin_unlock`
// (reached via `lock_ptr()`) already serialize concurrent access to
// it, matching every other `spinlock_t`-backed `Sync` newtype in this
// crate.
struct RegistryLock(UnsafeCell<[u8; SPINLOCK_BYTES]>);
unsafe impl Sync for RegistryLock {}
impl RegistryLock {
    fn lock_ptr(&self) -> *mut Spinlock { self.0.get() as *mut Spinlock }
}

static ALL_SLAB_CACHES: RegistryList = RegistryList(UnsafeCell::new(ListNode::new()));
static ALL_SLAB_CACHES_LOCK: RegistryLock = RegistryLock(UnsafeCell::new([0u8; SPINLOCK_BYTES]));

static REGISTRY_INIT_STARTED: AtomicBool = AtomicBool::new(false);
static REGISTRY_INIT_DONE: AtomicBool = AtomicBool::new(false);
static REGISTRY_LOCK_NAME: &[u8] = b"all_slab_caches\0";

fn ensure_registry_init() {
    if REGISTRY_INIT_DONE.load(Ordering::Acquire) {
        return;
    }
    if REGISTRY_INIT_STARTED
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
    {
        // SAFETY: single-writer section guarded by the CAS above; every
        // other caller spins on `REGISTRY_INIT_DONE` until this completes.
        // A list head must be self-referential (`next == prev == &head`)
        // to represent "empty", not null -- `ffi::list_init` (not
        // `ListNode::new()`, which leaves both pointers null) establishes
        // that invariant, matching the C `LIST_ENTRY_INITIALIZED` original.
        unsafe { ffi::list_init(&mut *ALL_SLAB_CACHES.0.get()); }
        ffi::spin_init(ALL_SLAB_CACHES_LOCK.lock_ptr(), REGISTRY_LOCK_NAME.as_ptr() as *const c_char);
        REGISTRY_INIT_DONE.store(true, Ordering::Release);
    } else {
        while !REGISTRY_INIT_DONE.load(Ordering::Acquire) {
            core::hint::spin_loop();
        }
    }
}

fn registry_head() -> *mut ListNode {
    // SAFETY: initialized exactly once by `ensure_registry_init` before any
    // caller reaches here; the registry lock (held by every caller) then
    // serializes all further access to the list contents (the head address
    // itself is immutable for the process lifetime).
    ALL_SLAB_CACHES.0.get()
}

/// `container_of(entry, SlabCache, cache_list_entry)`, in terms of the one
/// canonical `crate::mm::cffi::container_of` helper.
fn cache_from_entry(entry: *mut ListNode) -> *mut SlabCache {
    crate::mm::cffi::container_of(entry, core::mem::offset_of!(SlabCache, cache_list_entry))
}

fn register_cache(cache: &mut SlabCache) {
    ensure_registry_init();
    ffi::list_init(&mut cache.cache_list_entry);
    let _g = KSpinlock::from_ptr(ALL_SLAB_CACHES_LOCK.lock_ptr()).lock();
    // SAFETY: `registry_head()` is a valid, initialized list head.
    unsafe { ffi::list_push_back(&mut *registry_head(), &mut cache.cache_list_entry); }
}

fn unregister_cache(cache: &mut SlabCache) {
    ensure_registry_init();
    let _g = KSpinlock::from_ptr(ALL_SLAB_CACHES_LOCK.lock_ptr()).lock();
    if ffi::xv6_list_is_detached(&cache.cache_list_entry) == 0 {
        ffi::list_detach(&mut cache.cache_list_entry);
    }
}

/// Drop up to half the free slabs of every registered cache under memory
/// pressure (the OOM-shrink hook `Slab::make` falls back to on allocation
/// failure). C-called via `kernel/inc/mm/slab.h`.
pub(crate) fn slab_shrink_all() {
    ensure_registry_init();
    let head = registry_head();
    let mut guard = KSpinlock::from_ptr(ALL_SLAB_CACHES_LOCK.lock_ptr()).lock();
    // SAFETY: `head` is a valid, initialized list head; walking it under
    // the registry lock is race-free.
    let mut cur = unsafe { (*head).next };
    while cur != head {
        let cache = unsafe { &mut *cache_from_entry(cur) };
        let free_count = cache.global_free_count.load(Ordering::Acquire);
        let next = unsafe { (*cur).next };
        if free_count > 0 {
            let to_shrink = ((free_count + 1) / 2) as c_int;
            if to_shrink > 0 {
                drop(guard);
                // SAFETY: `cache` is a live, registered cache; shrinking it
                // takes only its own locks, not the registry lock.
                unsafe { slab_cache_shrink(cache as *mut SlabCache, to_shrink); }
                guard = KSpinlock::from_ptr(ALL_SLAB_CACHES_LOCK.lock_ptr()).lock();
                // Restart iteration: `next` may now be stale.
                cur = unsafe { (*head).next };
                continue;
            }
        }
        cur = next;
    }
}

/// Dump per-cache statistics: `detailed >= 2` prints a full table,
/// `detailed >= 1` a one-line total, `0` prints nothing. Always returns the
/// total byte footprint across every registered cache. C-called via
/// `kernel/inc/mm/slab.h` (also from `page::sys_memstat`).
pub(crate) fn slab_dump_all(detailed: c_int) -> u64 {
    ensure_registry_init();
    let mut total_pages: u64 = 0;

    // SAFETY: every pointer dereference in this function operates on the
    // slab-cache registry list, valid for the duration of the registry lock
    // held below.
    unsafe {
        if detailed >= 2 {
            crate::kprintln!("\n=== SLAB CACHE STATISTICS ===");
            crate::kprintln!("NAME             OBJSZ    TOTAL   ACTIVE     FREE    PAGES");
        }

        let head = registry_head();
        let _g = KSpinlock::from_ptr(ALL_SLAB_CACHES_LOCK.lock_ptr()).lock();
        let mut cur = (*head).next;
        while cur != head {
            let cache = &mut *cache_from_entry(cur);
            let next = (*cur).next;
            let slab_total = cache.slab_total.load(Ordering::Acquire);
            let obj_active = cache.obj_active.load(Ordering::Acquire);
            let global_free = cache.global_free_count.load(Ordering::Acquire);
            let pages = (slab_total as u64) * (1u64 << cache.slab_order);
            total_pages += pages;
            if detailed >= 2 {
                crate::kprintln!(
                    "{}: objsz={} total={} active={} free={} pages={}",
                    crate::printf::Cs(cache.name),
                    cache.obj_size as i64,
                    slab_total,
                    obj_active,
                    global_free,
                    pages,
                );
            }
            cur = next;
        }
        drop(_g);

        if detailed >= 2 {
            crate::kprintln!("-----------------------------");
        }
        let total_bytes = total_pages * (PAGE_SIZE as u64);
        if detailed >= 1 {
            crate::kprint!("Slab:  {} pages (", total_pages);
            if total_bytes >= 1024 * 1024 {
                let mb = total_bytes / (1024 * 1024);
                let kb_remainder = (total_bytes % (1024 * 1024)) / 1024;
                crate::kprint!("{}.{}MB", mb, kb_remainder * 10 / 1024);
            } else {
                crate::kprint!("{}KB", total_bytes / 1024);
            }
            crate::kprintln!(")");
            if detailed >= 2 {
                crate::kprintln!("=============================");
            }
        }
        total_bytes
    }
}

// ===========================================================================
// Tiny helpers. The per-type logic now lives in `impl Slab` /
// `impl SlabCache` blocks; the only standalone helpers remaining are the
// ones that don't naturally bind to a single receiver type.
// ===========================================================================

// Pin the layout invariant `slab_from_list_entry` relies on at compile time.
const _: () = assert!(core::mem::offset_of!(Slab, list_entry) == 0);

/// Read the `*mut c_void` link stored in the first 8 bytes of a free
/// object. Wrapping this in a named helper lets every algorithmic caller
/// stay safe.
#[inline(always)]
fn read_free_link(obj: *mut c_void) -> *mut c_void {
    // SAFETY: by invariant every free object carries a valid link there.
    unsafe { core::ptr::read(obj as *const *mut c_void) }
}

#[inline(always)]
fn write_free_link(obj: *mut c_void, next: *mut c_void) {
    // SAFETY: see `read_free_link`; the object slot is exclusively owned.
    unsafe { core::ptr::write(obj as *mut *mut c_void, next) }
}

/// Cast a `list_entry` back to its containing `Slab`. The C layout asserts
/// `offsetof(slab_t, list_entry) == 0`, so this is a pure pointer cast.
#[inline(always)]
fn slab_from_list_entry<'a>(node: &'a mut ListNode) -> &'a mut Slab {
    // SAFETY: `list_entry` is the first field of `Slab` (C `_Static_assert`).
    unsafe { &mut *(node as *mut ListNode as *mut Slab) }
}

#[inline(always)]
fn obj_offset(obj_size: usize) -> usize {
    ((SLAB_T_SIZE + obj_size - 1) / obj_size) * obj_size
}

#[inline(always)]
fn order_objs(order: u32, offs: usize, obj_size: usize) -> u32 {
    let total = PAGE_SIZE << order;
    ((total - offs) / obj_size) as u32
}

// ===========================================================================
// `impl Slab` -- per-slab algorithm methods.
// ===========================================================================
impl Slab {
    /// Borrow the bitmap as an ordinary `&mut [u64]`. Returns `None` when
    /// the slab has no bitmap. All bitmap arithmetic afterwards is plain
    /// safe Rust indexing.
    #[inline]
    fn bitmap_slice(&mut self) -> Option<&mut [u64]> {
        let len = self.cache_ref()?.bitmap_size as usize;
        if self.bitmap.is_null() || len == 0 { return None; }
        // SAFETY: `bitmap` was `kmm_alloc`'d for at least `len * 8` bytes
        // and is exclusively owned by this slab (we hold `&mut self`).
        Some(unsafe { core::slice::from_raw_parts_mut(self.bitmap, len) })
    }

    #[inline(always)]
    fn is_attached(&self) -> bool { !self.cache.is_null() }

    /// Borrow the cache this slab is attached to, if any. The returned
    /// reference's `'static` lifetime reflects the long-lived nature of
    /// cache descriptors; the slab and cache are distinct allocations
    /// so this does not alias the slab borrow.
    #[inline(always)]
    fn cache_ref(&self) -> Option<&'static SlabCache> {
        // SAFETY: `self.cache`, if non-null, points to a cache descriptor
        // that outlives every operation we perform here.
        unsafe { self.cache.as_ref() }
    }

    #[inline(always)]
    fn cache_mut(&self) -> Option<&'static mut SlabCache> {
        // SAFETY: see `cache_ref`.
        unsafe { self.cache.as_mut() }
    }

    #[inline(always)]
    fn obj_free_count(&self) -> u64 {
        let Some(cache) = self.cache_ref() else { return 0 };
        let cap = cache.slab_obj_num as u64;
        if self.in_use < cap { cap - self.in_use } else { 0 }
    }

    #[inline(always)]
    fn is_full(&self) -> bool { self.obj_free_count() == 0 }

    #[inline(always)]
    fn is_empty(&self) -> bool { self.in_use == 0 }

    #[inline(always)]
    fn page_base(&self) -> *mut u8 {
        if self.page.is_null() { return ptr::null_mut(); }
        ffi::page_to_pa(self.page) as *mut u8
    }

    // ---- Bitmap helpers (safe after `bitmap_slice` borrow) ----

    fn bitmap_test_and_set(&mut self, idx: c_int) -> c_int {
        let n = self.cache_ref()
                    .map(|c| c.slab_obj_num as c_int)
                    .unwrap_or(c_int::MAX);
        if idx < 0 || idx >= n { return -1; }
        let Some(bm) = self.bitmap_slice() else { return -1; };
        let word = (idx / 64) as usize;
        let mask = 1u64 << ((idx % 64) as u32);
        let old = bm[word];
        bm[word] = old | mask;
        if (old & mask) != 0 { 1 } else { 0 }
    }

    fn bitmap_test_and_clear(&mut self, idx: c_int) -> c_int {
        let n = self.cache_ref()
                    .map(|c| c.slab_obj_num as c_int)
                    .unwrap_or(c_int::MAX);
        if idx < 0 || idx >= n { return -1; }
        let Some(bm) = self.bitmap_slice() else { return -1; };
        let word = (idx / 64) as usize;
        let mask = 1u64 << ((idx % 64) as u32);
        let old = bm[word];
        bm[word] = old & !mask;
        if (old & mask) != 0 { 1 } else { 0 }
    }

    /// Map an object pointer back to its slot index inside this slab.
    fn obj_to_idx(&self, ptr: *mut c_void) -> c_int {
        if ptr.is_null() || ((ptr as usize) & 7) != 0 { return -1; }
        let Some(cache) = self.cache_ref() else { return -1; };
        let base = self.page_base();
        if base.is_null() { return -1; }
        // Pointer arithmetic done via `usize` -- no raw deref, no `unsafe`.
        let data_base = (base as usize).wrapping_add(cache.offset);
        if (ptr as usize) < data_base { return -1; }
        let off = (ptr as usize) - data_base;
        let idx = (off / cache.obj_size) as c_int;
        if idx >= cache.slab_obj_num as c_int { return -1; }
        idx
    }

    // ---- Free-list pop / push (threaded through the objects themselves) ----

    fn obj_get(&mut self) -> *mut c_void {
        let ret = self.next;
        if ret.is_null() { return ret; }
        self.next = read_free_link(ret);
        self.in_use += 1;
        if !self.bitmap.is_null() {
            let idx = self.obj_to_idx(ret);
            if idx >= 0 {
                let old = self.bitmap_test_and_set(idx);
                if old != 0 {
                    ffi::panic(b"slab_obj_get: double allocation detected\0");
                }
            }
        }
        ret
    }

    fn obj_put(&mut self, ptr: *mut c_void) {
        if !self.bitmap.is_null() {
            let idx = self.obj_to_idx(ptr);
            if idx >= 0 {
                let old = self.bitmap_test_and_clear(idx);
                if old != 1 {
                    ffi::panic(b"slab_obj_put: double free detected\0");
                }
            }
        }
        write_free_link(ptr, self.next);
        self.next = ptr;
        self.in_use -= 1;
    }

    // ---- Slab lifecycle ----

    /// Allocate a fresh slab backed by `1 << order` pages, thread its
    /// internal free list, and return a `'static` borrow of the
    /// descriptor. Returns `None` if the page allocator is exhausted
    /// (even after a shrink attempt) or descriptor allocation fails.
    fn make(
        flags: u64,
        order: u32,
        offs: usize,
        obj_size: usize,
        obj_num: u32,
        bitmap_size: u32,
    ) -> Option<&'static mut Slab> {
        // RAII rollback wrappers: each owns a resource and frees it on
        // Drop. Success paths call `.commit()` which converts the guard
        // into the raw pointer and forgets the guard, so no free fires.
        struct PageRollback { page: *mut c_void, order: u32 }
        impl PageRollback {
            fn commit(self) -> *mut c_void {
                let p = self.page;
                core::mem::forget(self);
                p
            }
        }
        impl Drop for PageRollback {
            fn drop(&mut self) {
                ffi::page_free(self.page, self.order as u64);
            }
        }

        struct DescRollback { desc: *mut Slab }
        impl DescRollback {
            fn commit(self) -> *mut Slab {
                let d = self.desc;
                core::mem::forget(self);
                d
            }
        }
        impl Drop for DescRollback {
            fn drop(&mut self) { ffi::slab_desc_free(self.desc); }
        }

        let mut raw_page = ffi::page_alloc(order as u64, PAGE_TYPE_SLAB);
        if raw_page.is_null() {
            // Emergency reclaim -- shrink everything and retry once.
            ffi::shrink_all();
            raw_page = ffi::page_alloc(order as u64, PAGE_TYPE_SLAB);
        }
        if raw_page.is_null() { return None; }
        let page_guard = PageRollback { page: raw_page, order };

        let page_base_pa = ffi::page_to_pa(raw_page);
        if page_base_pa == 0 {
            ffi::panic(b"slab_make: null page base\0");
        }
        let page_base = page_base_pa as *mut u8;

        // Stamp the order on the head page now so a later page_free
        // (on the failure path below) sees the right order.
        ffi::page_set_order(raw_page, order);

        // Acquire the slab descriptor (embedded or separately allocated).
        // For the embedded case there's no second allocation, so no second
        // rollback guard is needed.
        let mut desc_guard: Option<DescRollback> = None;
        let slab_ptr: *mut Slab = if (flags & SLAB_FLAG_EMBEDDED) != 0 {
            page_base as *mut Slab
        } else {
            let s = ffi::slab_desc_alloc();
            if s.is_null() {
                // page_guard drop frees the page.
                return None;
            }
            desc_guard = Some(DescRollback { desc: s });
            s
        };
        // SAFETY: `slab_ptr` is either a freshly allocated descriptor or the
        // head of a fresh page; both are valid, exclusively owned writable
        // memory of size `sizeof(Slab)`.
        let slab: &'static mut Slab =
            core::ptr::NonNull::new(slab_ptr).map(|mut nn| unsafe { nn.as_mut() })
                .expect("slab_make: NULL slab descriptor");

        // Mark the head page and every tail page.
        ffi::page_attach(raw_page, slab, order);

        slab.cache = ptr::null_mut();
        slab.slab_order = order as u16;
        slab.in_use = 0;
        slab.page = raw_page;
        slab.state = SLAB_STATE_DEQUEUED;
        slab.bitmap = ptr::null_mut();
        slab.cpu_id.store(-1, Ordering::Release);
        ffi::list_init(&mut slab.list_entry);

        if bitmap_size > 0 {
            let bm = ffi::kmm_alloc(bitmap_size as usize * 8) as *mut u64;
            if bm.is_null() {
                // Drop releases desc_guard (if any) then page_guard.
                return None;
            }
            // SAFETY: `kmm_alloc` returned at least `bitmap_size * 8` writable
            // bytes, exclusively owned here. The slice has no aliases.
            let bitmap = unsafe {
                core::slice::from_raw_parts_mut(bm, bitmap_size as usize)
            };
            bitmap.fill(0);
            slab.bitmap = bm;
        }

        // Thread the free list through the objects themselves. Pointer math
        // is done via `usize`; the only raw write is `write_free_link`.
        let data_start = (page_base as usize).wrapping_add(offs);
        let mut prev: *mut c_void = ptr::null_mut();
        for i in 0..(obj_num as usize) {
            let tmp = (data_start + i * obj_size) as *mut c_void;
            write_free_link(tmp, prev);
            prev = tmp;
        }
        slab.next = prev;

        // Success — disarm the rollback guards.
        if let Some(g) = desc_guard { let _ = g.commit(); }
        let _ = page_guard.commit();
        Some(slab)
    }

    /// Tear down a free, unattached slab and return its pages to the
    /// buddy allocator.
    fn destroy(&mut self) {
        if self.is_attached() {
            ffi::panic(b"slab_destroy: destroy an attached SLAB\0");
        }
        if !self.is_empty() {
            ffi::panic(b"slab_destroy: destroy a non-empty SLAB\0");
        }
        let page = self.page;
        let order = self.slab_order;
        let page_base = ffi::page_to_pa(page);
        if page_base == 0 {
            ffi::panic(b"slab_destroy: null page base\0");
        }
        if !self.bitmap.is_null() {
            ffi::kmm_free(self.bitmap as *mut c_void);
            self.bitmap = ptr::null_mut();
        }
        let s_addr = self as *mut Slab as usize;
        if s_addr != (page_base as usize) {
            ffi::slab_desc_free(self);
        }
        ffi::page_free(page, order as u64);
    }
}

// ===========================================================================
// `impl SlabCache` -- cache lifecycle and shrink logic.
// ===========================================================================
impl SlabCache {
    /// Detach a slab from this cache and update the counters atomically.
    /// The slab must be empty and currently owned by `self`.
    fn detach_counters(&mut self, slab: &mut Slab) {
        if slab.cache != (self as *mut SlabCache) {
            ffi::panic(b"slab_detach: wrong SLAB cache\0");
        }
        if !slab.is_empty() {
            ffi::panic(b"slab_detach: detach non-empty SLAB\0");
        }
        let total = self.slab_total.load(Ordering::Acquire);
        let obj_total = self.obj_total.load(Ordering::Acquire);
        if total == 0 || obj_total < self.slab_obj_num as u64 {
            ffi::panic(b"slab_detach: counter error\0");
        }
        self.obj_total.fetch_sub(self.slab_obj_num as u64, Ordering::Release);
        self.slab_total.fetch_sub(1, Ordering::Release);
        slab.cache = ptr::null_mut();
    }

    /// Internal cache initialization -- mirrors `__slab_cache_init`.
    fn init_internal(&mut self, name: *mut c_char, mut obj_size: usize, flags: u64) {
        // Round object size up to 8.
        obj_size = (obj_size + 7) & !7usize;
        let mut offset = 0usize;
        if (flags & SLAB_FLAG_EMBEDDED) != 0 {
            offset = obj_offset(obj_size);
        }

        // Adaptive slab order.
        let mut slab_order: u32 = if obj_size <= 128 {
            0
        } else if obj_size <= 512 {
            1
        } else if obj_size <= 1024 {
            2
        } else if obj_size <= 2048 {
            3
        } else {
            4
        };

        let mut slab_obj_num = order_objs(slab_order, offset, obj_size);
        while slab_obj_num < 8 && slab_order < 5 {
            slab_order += 1;
            slab_obj_num = order_objs(slab_order, offset, obj_size);
        }
        let limits = slab_obj_num * 4;

        let bitmap_size = if (flags & SLAB_FLAG_DEBUG_BITMAP) != 0 {
            (slab_obj_num + 63) / 64
        } else {
            0
        };

        self.name = name as *const c_char;
        self.flags = flags;
        self.obj_size = obj_size;
        self.offset = offset;
        self.slab_order = slab_order;
        self.slab_obj_num = slab_obj_num;
        self.bitmap_size = bitmap_size;
        self.limits = limits;

        for pc in self.percpu_caches.iter_mut() {
            ffi::list_init(&mut pc.partial_list);
            ffi::list_init(&mut pc.full_list);
            pc.partial_count.store(0, Ordering::Release);
            pc.full_count.store(0, Ordering::Release);
            ffi::spin_init(pc.lock_ptr(), name);
        }

        ffi::list_init(&mut self.global_free_list);
        ffi::spin_init(
            self.global_lock_ptr(),
            b"global_free\0".as_ptr() as *mut c_char,
        );
        self.global_free_count.store(0, Ordering::Release);
        self.slab_total.store(0, Ordering::Release);
        self.obj_active.store(0, Ordering::Release);
        self.obj_total.store(0, Ordering::Release);

        ffi::register_cache(self);
    }

    /// Drop up to `nums` slabs from the global free list, parking the
    /// detached slabs in `tmp_list` for the caller to actually free
    /// outside the cache lock. Returns the number actually moved.
    fn shrink_unlocked(&mut self, nums: c_int, tmp_list: &mut ListNode) -> c_int {
        let _g = KSpinlock::from_ptr(self.global_lock_ptr()).lock();
        let free_count = self.global_free_count.load(Ordering::Acquire);
        let target_after: i64 = if nums == 0 || (nums as i64) > free_count {
            0
        } else {
            free_count - nums as i64
        };

        let mut counter: c_int = 0;
        loop {
            let now = self.global_free_count.load(Ordering::Acquire);
            if now <= target_after { break; }
            if ffi::list_is_empty(&self.global_free_list) {
                ffi::panic(b"slab_shrink: list empty but count > 0\0");
            }
            let Some(node) = ffi::list_pop_front(&mut self.global_free_list) else {
                ffi::panic(b"slab_shrink: pop NULL\0");
            };
            let slab = slab_from_list_entry(node);
            self.global_free_count.fetch_sub(1, Ordering::Release);
            let total_before = self.slab_total.load(Ordering::Acquire);
            self.detach_counters(slab);
            if self.slab_total.load(Ordering::Acquire) >= total_before {
                ffi::panic(b"slab_shrink: slab_total did not decrease\0");
            }
            ffi::list_push_front(tmp_list, &mut slab.list_entry);
            counter += 1;
        }
        drop(_g);
        counter
    }
}

fn free_tmp_list(tmp_list: &mut ListNode, expected: c_int) {
    if expected <= 0 {
        if !ffi::list_is_empty(tmp_list) {
            ffi::panic(b"free_tmp_list: list not empty\0");
        }
        return;
    }
    let mut counter: c_int = 0;
    while let Some(node) = ffi::list_pop_front(tmp_list) {
        counter += 1;
        slab_from_list_entry(node).destroy();
    }
    if counter != expected {
        ffi::panic(b"free_tmp_list: counter mismatch\0");
    }
}

// ===========================================================================
// Public API -- exact C signatures from kernel/inc/mm/slab.h
//
// Each entrypoint is `unsafe extern "C"` because every caller hands in raw
// pointers from C whose validity Rust cannot statically check. The body of
// each entrypoint immediately converts those pointers into safe `&mut`
// references and delegates to a private safe implementation function.
// ===========================================================================

// `SlabCache` public-API impls. The thin `unsafe extern "C"` wrappers below
// dereference the raw pointer once and dispatch into safe methods.
impl SlabCache {
    fn init_impl(&mut self, name: *mut c_char, mut obj_size: usize, flags: u64) -> c_int {
        if (flags & !SLAB_FLAG_VALID_MASK) != 0 { return -1; }
        if obj_size > SLAB_OBJ_MAX_SIZE { return -1; }
        if obj_size < SLAB_OBJ_MIN_SIZE { obj_size = SLAB_OBJ_MIN_SIZE; }
        self.init_internal(name, obj_size, flags);
        0
    }

    fn destroy_impl(&mut self) -> c_int {
        if (self.flags & SLAB_FLAG_STATIC) != 0 { return -1; }
        for pc in self.percpu_caches.iter() {
            if pc.partial_count.load(Ordering::Acquire) != 0
                || pc.full_count.load(Ordering::Acquire) != 0
            {
                return -1;
            }
        }
        let free_count = self.global_free_count.load(Ordering::Acquire);
        let mut tmp = ListNode { prev: ptr::null_mut(), next: ptr::null_mut() };
        ffi::list_init(&mut tmp);
        let shrink_ret = self.shrink_unlocked(free_count as c_int, &mut tmp);
        if shrink_ret as i64 != free_count {
            free_tmp_list(&mut tmp, shrink_ret);
            return -1;
        }
        free_tmp_list(&mut tmp, shrink_ret);
        ffi::unregister_cache(self);
        ffi::cache_desc_free(self);
        0
    }
}

/// Initializes an already-allocated `SlabCache` in place (used for
/// statically-embedded caches; contrast with [`slab_cache_create`],
/// which also allocates the descriptor).
///
/// # Safety
///
/// - `cache`, if non-null, must be a valid, writable `*mut SlabCache`
///   (e.g. a `static mut` or embedding-struct field) that no other
///   code is concurrently accessing.
/// - `name`, if used by `init_impl`, must be a valid NUL-terminated
///   C string for at least as long as the cache retains it.
pub(crate) unsafe fn slab_cache_init(
    cache: *mut SlabCache,
    name: *mut c_char,
    obj_size: usize,
    flags: u64,
) -> c_int {
    let Some(cache) = cache.as_mut() else { return -1; };
    cache.init_impl(name, obj_size, flags)
}

/// Allocates and initializes a new `SlabCache` descriptor. Returns
/// null on allocation or initialization failure.
///
/// # Safety
///
/// - `name` must be a valid NUL-terminated C string for at least as
///   long as the cache retains it.
pub(crate) unsafe fn slab_cache_create(
    name: *mut c_char,
    obj_size: usize,
    flags: u64,
) -> *mut SlabCache {
    let cache_ptr = ffi::cache_desc_alloc();
    let Some(cache) = cache_ptr.as_mut() else { return ptr::null_mut(); };
    if cache.init_impl(name, obj_size, flags) != 0 {
        ffi::cache_desc_free(cache);
        return ptr::null_mut();
    }
    cache_ptr
}

/// Destroys `cache`: frees all its slabs and unregisters it. Fails
/// (returns nonzero, leaves the cache intact) if any objects are
/// still allocated out of it.
///
/// # Safety
///
/// - `cache`, if non-null, must be a live, exclusively-owned
///   `*mut SlabCache` previously returned by [`slab_cache_create`] (or
///   initialized via [`slab_cache_init`]) — not concurrently accessed
///   by another hart, and not used again after this call succeeds.
pub(crate) unsafe fn slab_cache_destroy(cache: *mut SlabCache) -> c_int {
    let Some(cache) = cache.as_mut() else { return -1; };
    cache.destroy_impl()
}

/// Frees up to `nums` fully-empty slabs back to the page allocator.
/// Returns the number actually freed, or -1 if `cache` is null.
///
/// # Safety
///
/// - `cache`, if non-null, must be a live `*mut SlabCache` previously
///   returned by [`slab_cache_create`] or initialized via
///   [`slab_cache_init`].
pub(crate) unsafe fn slab_cache_shrink(
    cache: *mut SlabCache,
    nums: c_int,
) -> c_int {
    let Some(cache) = cache.as_mut() else { return -1; };
    let mut tmp = ListNode { prev: ptr::null_mut(), next: ptr::null_mut() };
    ffi::list_init(&mut tmp);
    let ret = cache.shrink_unlocked(nums, &mut tmp);
    free_tmp_list(&mut tmp, ret);
    ret
}

// `slab_shrink_all` is now declared in the `mod ffi` block above.

// ===========================================================================
// slab_alloc -- three-phase allocation.
// ===========================================================================
/// Allocates one object from `cache`. Returns null if `cache` is null
/// or the allocation fails.
///
/// # Safety
///
/// - `cache`, if non-null, must be a live `*mut SlabCache` previously
///   returned by [`slab_cache_create`] or initialized via
///   [`slab_cache_init`].
pub(crate) unsafe fn slab_alloc(cache: *mut SlabCache) -> *mut c_void {
    let Some(cache) = cache.as_mut() else { return ptr::null_mut(); };
    cache.alloc_impl()
}

impl SlabCache {
    fn alloc_impl(&mut self) -> *mut c_void {
        let _preempt = crate::machine::PreemptGuard::new();
        let cpu_id = ffi::cpuid();
        let cpu_idx = cpu_id as usize;

        // PHASE 1: local CPU partial list (fast path).
        {
            let _g = KSpinlock::from_ptr(
                self.percpu_caches[cpu_idx].lock_ptr()).lock();
            if !ffi::list_is_empty(&self.percpu_caches[cpu_idx].partial_list) {
                let node = ffi::list_first(&self.percpu_caches[cpu_idx].partial_list)
                    .expect("partial_list non-empty but list_first returned NULL");
                let slab = slab_from_list_entry(node);
                let obj = slab.obj_get();
                if slab.is_full() {
                    let pc = &mut self.percpu_caches[cpu_idx];
                    ffi::list_detach(&mut slab.list_entry);
                    pc.partial_count.fetch_sub(1, Ordering::Release);
                    ffi::list_push_front(&mut pc.full_list, &mut slab.list_entry);
                    pc.full_count.fetch_add(1, Ordering::Release);
                    slab.state = SLAB_STATE_FULL;
                }
                self.obj_active.fetch_add(1, Ordering::Release);
                drop(_g);
                return obj;
            }
        }

        // PHASE 2: global free list.
        let popped_node = {
            let _g = KSpinlock::from_ptr(self.global_lock_ptr()).lock();
            if ffi::list_is_empty(&self.global_free_list) {
                None
            } else {
                let node = ffi::list_pop_front(&mut self.global_free_list)
                    .expect("global_free_list non-empty but pop returned NULL");
                self.global_free_count.fetch_sub(1, Ordering::Release);
                Some(node)
            }
        };
        if let Some(node) = popped_node {
            let slab = slab_from_list_entry(node);
            slab.cpu_id.store(cpu_id, Ordering::Release);
            slab.state = SLAB_STATE_DEQUEUED;
            let obj = slab.obj_get();

            let _g = KSpinlock::from_ptr(
                self.percpu_caches[cpu_idx].lock_ptr()).lock();
            {
                let pc = &mut self.percpu_caches[cpu_idx];
                if slab.is_full() {
                    ffi::list_push_front(&mut pc.full_list, &mut slab.list_entry);
                    pc.full_count.fetch_add(1, Ordering::Release);
                    slab.state = SLAB_STATE_FULL;
                } else {
                    ffi::list_push_front(&mut pc.partial_list, &mut slab.list_entry);
                    pc.partial_count.fetch_add(1, Ordering::Release);
                    slab.state = SLAB_STATE_PARTIAL;
                }
            }
            self.obj_active.fetch_add(1, Ordering::Release);
            drop(_g);
            return obj;
        }

        // PHASE 3: make a new slab (no locks held).
        let flags = self.flags;
        let order = self.slab_order;
        let offset = self.offset;
        let obj_size = self.obj_size;
        let obj_num = self.slab_obj_num;
        let bitmap_size = self.bitmap_size;
        let cache_ptr: *mut SlabCache = self;
        let Some(slab) = Slab::make(flags, order, offset, obj_size, obj_num, bitmap_size) else {
            return ptr::null_mut();
        };
        slab.cache = cache_ptr;
        slab.cpu_id.store(cpu_id, Ordering::Release);
        self.slab_total.fetch_add(1, Ordering::Release);
        self.obj_total.fetch_add(self.slab_obj_num as u64, Ordering::Release);

        let obj = slab.obj_get();
        let _g = KSpinlock::from_ptr(
            self.percpu_caches[cpu_idx].lock_ptr()).lock();
        {
            let pc = &mut self.percpu_caches[cpu_idx];
            if slab.is_full() {
                ffi::list_push_front(&mut pc.full_list, &mut slab.list_entry);
                pc.full_count.fetch_add(1, Ordering::Release);
                slab.state = SLAB_STATE_FULL;
            } else {
                ffi::list_push_front(&mut pc.partial_list, &mut slab.list_entry);
                pc.partial_count.fetch_add(1, Ordering::Release);
                slab.state = SLAB_STATE_PARTIAL;
            }
        }
        self.obj_active.fetch_add(1, Ordering::Release);
        drop(_g);
        obj
    }
}

// ===========================================================================
// slab_free / slab_free_noshrink
// ===========================================================================

/// Shared body for slab_free / slab_free_noshrink (phases 1-5).
/// Returns the cache the object belonged to (for an optional PHASE 6
/// shrink by the caller), or `None` if the obj was invalid.
fn slab_free_core(
    obj: *mut c_void,
    fn_name: *const c_char,
) -> Option<&'static mut SlabCache> {
    if obj.is_null() {
        ffi::log_free_null(fn_name);
        return None;
    }

    // PHASE 1: find slab (no lock needed; page descriptor is immutable).
    let Some(slab) = ffi::find_obj_slab(obj) else {
        ffi::log_no_slab(fn_name, obj);
        return None;
    };
    let Some(cache) = slab.cache_mut() else {
        ffi::log_unattached(slab, obj);
        ffi::panic(b"slab_free: slab not attached to cache\0");
    };

    // PHASE 2: determine ownership.
    let slab_cpu_id = slab.cpu_id.load(Ordering::Acquire);
    if slab_cpu_id < 0 {
        ffi::log_from_free(obj, slab, cache, slab_cpu_id);
        ffi::panic(b"slab_free: object from free slab\0");
    }
    let cpu_idx = slab_cpu_id as usize;

    // PHASE 3: lock the owner CPU's per-CPU cache.
    let pc_guard = KSpinlock::from_ptr(
        cache.percpu_caches[cpu_idx].lock_ptr()).lock();

    // Double-check ownership under the lock.
    if slab.cpu_id.load(Ordering::Acquire) != slab_cpu_id {
        drop(pc_guard);
        ffi::panic(b"slab_free: slab cpu_id changed during free\0");
    }

    // PHASE 4: return the object.
    let old_state = slab.state;
    let was_full = slab.is_full();
    slab.obj_put(obj);
    cache.obj_active.fetch_sub(1, Ordering::Release);

    // PHASE 5: move slab between lists if state changed.
    if slab.is_empty() {
        {
            let pc = &mut cache.percpu_caches[cpu_idx];
            if old_state == SLAB_STATE_PARTIAL {
                ffi::list_detach(&mut slab.list_entry);
                pc.partial_count.fetch_sub(1, Ordering::Release);
            } else if old_state == SLAB_STATE_FULL {
                ffi::list_detach(&mut slab.list_entry);
                pc.full_count.fetch_sub(1, Ordering::Release);
            }
        }
        slab.cpu_id.store(-1, Ordering::Release);
        slab.state = SLAB_STATE_FREE;
        drop(pc_guard);

        let _g = KSpinlock::from_ptr(cache.global_lock_ptr()).lock();
        ffi::list_push_front(&mut cache.global_free_list, &mut slab.list_entry);
        cache.global_free_count.fetch_add(1, Ordering::Release);
    } else if was_full && !slab.is_full() {
        {
            let pc = &mut cache.percpu_caches[cpu_idx];
            ffi::list_detach(&mut slab.list_entry);
            pc.full_count.fetch_sub(1, Ordering::Release);
            ffi::list_push_front(&mut pc.partial_list, &mut slab.list_entry);
            pc.partial_count.fetch_add(1, Ordering::Release);
        }
        slab.state = SLAB_STATE_PARTIAL;
        drop(pc_guard);
    } else {
        drop(pc_guard);
    }

    Some(cache)
}

impl SlabCache {
    /// PHASE 6 (optional): if the global free list grew past `limits`, drop half.
    fn try_shrink_global(&mut self) {
        let free_count = self.global_free_count.load(Ordering::Acquire);
        if (free_count * self.slab_obj_num as i64) < self.limits as i64 {
            return;
        }
        let mut tmp = ListNode {
            prev: ptr::null_mut(),
            next: ptr::null_mut(),
        };
        ffi::list_init(&mut tmp);

        {
            let _g = KSpinlock::from_ptr(self.global_lock_ptr()).lock();
            let target_shrink = (free_count / 2) as c_int;

            let mut i: c_int = 0;
            while i < target_shrink && !ffi::list_is_empty(&self.global_free_list) {
                let Some(node) = ffi::list_pop_front(&mut self.global_free_list) else { break };
                let free_slab = slab_from_list_entry(node);
                self.global_free_count.fetch_sub(1, Ordering::Release);
                self.detach_counters(free_slab);
                ffi::list_push_front(&mut tmp, &mut free_slab.list_entry);
                i += 1;
            }
        }

        // Free outside the lock.
        while let Some(node) = ffi::list_pop_front(&mut tmp) {
            slab_from_list_entry(node).destroy();
        }
    }
}

/// Frees `obj` back to its owning slab, then opportunistically shrinks
/// the cache's global free list. No-op if `obj` is null.
///
/// # Safety
///
/// - `obj`, if non-null, must be a pointer previously returned by
///   [`slab_alloc`] on some live cache, not already freed (no double
///   free), and not concurrently accessed by another hart.
pub(crate) unsafe fn slab_free(obj: *mut c_void) {
    if let Some(cache) =
        slab_free_core(obj, b"slab_free\0".as_ptr() as *const c_char)
    {
        cache.try_shrink_global();
    }
}

/// Frees `obj` back to its owning slab, identical to [`slab_free`] but
/// skipping the opportunistic global-shrink phase (used by call sites
/// that free in a loop and shrink once at the end).
///
/// # Safety
///
/// - Same as [`slab_free`]: `obj`, if non-null, must be a pointer
///   previously returned by [`slab_alloc`] on some live cache, not
///   already freed, and not concurrently accessed by another hart.
pub(crate) unsafe fn slab_free_noshrink(obj: *mut c_void) {
    // Phases 1-5 identical to slab_free; PHASE 6 deliberately skipped.
    let _ = slab_free_core(
        obj,
        b"slab_free_noshrink\0".as_ptr() as *const c_char,
    );
}
