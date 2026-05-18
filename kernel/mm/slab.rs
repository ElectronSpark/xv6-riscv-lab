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

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::sync::KSpinlock;
use core::sync::atomic::{
    AtomicI32, AtomicI64, AtomicU32, AtomicU64, Ordering,
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

// `sizeof(slab_t)` is needed by `__SLAB_OBJ_OFFSET`. The mirror struct
// below is laid out the same as the C `slab_t`, so we can compute it
// here -- and a static_assert in slab_shims.c keeps both in sync.
const SLAB_T_SIZE: usize = core::mem::size_of::<Slab>();

// ===========================================================================
// Mirror types -- the canonical `Spinlock` / `ListNode` opaque mirrors
// now live in `crate::mm::cffi`; re-export them here so existing
// `crate::mm::slab::{Spinlock,ListNode}` callers compile unchanged.
// ===========================================================================

pub use crate::mm::cffi::{ListNode, Spinlock};

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
    pub use crate::mm::cffi::raw::{
        spin_init, spin_lock, spin_unlock,
        xv6_cpuid, xv6_push_off, xv6_pop_off,
        kmm_alloc, kmm_free,
        xv6_list_init, xv6_list_is_empty, xv6_list_is_detached,
        xv6_list_detach, xv6_list_push_front, xv6_list_pop_front,
        xv6_list_first, xv6_list_last,
    };

    unsafe extern "C" {
        // Slab descriptor / cache descriptor allocators (defined in kalloc.c)
        pub safe fn slab_t_desc_alloc() -> *mut Slab;
        pub safe fn slab_t_desc_free(s: *mut Slab);
        pub safe fn slab_cache_t_alloc() -> *mut SlabCache;
        pub safe fn slab_cache_t_free(c: *mut SlabCache);

        // Buddy page allocator
        pub safe fn __page_alloc(order: u64, flags: u64) -> *mut c_void;
        pub safe fn __page_free(page: *mut c_void, order: u64);
        pub safe fn __page_to_pa(page: *mut c_void) -> u64;

        // Shims from slab_shims.c
        pub safe fn xv6_slab_register_cache(cache: *mut SlabCache);
        pub safe fn xv6_slab_unregister_cache(cache: *mut SlabCache);
        pub safe fn xv6_slab_page_attach(head: *mut c_void, slab: *mut Slab, order: u32);
        pub safe fn xv6_slab_page_set_order(head: *mut c_void, order: u32);
        pub safe fn xv6_slab_find_obj_slab(ptr: *mut c_void) -> *mut Slab;
        pub safe fn xv6_page_type_slab_value() -> u64;

        pub safe fn xv6_slab_log_free_null(fn_name: *const c_char);
        pub safe fn xv6_slab_log_no_slab(fn_name: *const c_char, obj: *mut c_void);
        pub safe fn xv6_slab_log_unattached(slab: *mut Slab, obj: *mut c_void);
        pub safe fn xv6_slab_log_from_free(
            obj: *mut c_void,
            slab: *mut Slab,
            cache: *mut SlabCache,
            cpu_id: c_int,
        );
        pub safe fn xv6_slab_panic(msg: *const c_char) -> !;

        // `slab_shrink_all` lives in slab_shims.c (variadic printf, registry walk).
        pub safe fn slab_shrink_all();
    }

    // --- Type-adapting wrappers (safe Rust, no unsafe blocks). -----------
    #[inline] pub fn cpuid() -> c_int { xv6_cpuid() }
    #[inline] pub fn push_off() { xv6_push_off() }
    #[inline] pub fn pop_off() { xv6_pop_off() }

    #[inline] pub fn slab_desc_alloc() -> *mut Slab { slab_t_desc_alloc() }
    #[inline] pub fn slab_desc_free(s: *mut Slab) { slab_t_desc_free(s) }
    #[inline] pub fn cache_desc_alloc() -> *mut SlabCache { slab_cache_t_alloc() }
    #[inline] pub fn cache_desc_free(c: *mut SlabCache) { slab_cache_t_free(c) }

    #[inline] pub fn page_alloc(order: u64, flags: u64) -> *mut c_void {
        __page_alloc(order, flags)
    }
    #[inline] pub fn page_free(page: *mut c_void, order: u64) {
        __page_free(page, order)
    }
    #[inline] pub fn page_to_pa(page: *mut c_void) -> u64 { __page_to_pa(page) }

    #[inline] pub fn register_cache(cache: &mut SlabCache) {
        xv6_slab_register_cache(cache)
    }
    #[inline] pub fn unregister_cache(cache: &mut SlabCache) {
        xv6_slab_unregister_cache(cache)
    }
    #[inline] pub fn page_attach(head: *mut c_void, slab: &mut Slab, order: u32) {
        xv6_slab_page_attach(head, slab, order)
    }
    #[inline] pub fn page_set_order(head: *mut c_void, order: u32) {
        xv6_slab_page_set_order(head, order)
    }
    /// Returns the slab descriptor associated with `ptr`, or `None`.
    #[inline] pub fn find_obj_slab(ptr: *mut c_void) -> Option<&'static mut Slab> {
        // SAFETY: turning a freshly-returned raw pointer into a `&mut` is
        // sound because the slab descriptor is uniquely owned by the
        // allocator's free path while held.
        unsafe { xv6_slab_find_obj_slab(ptr).as_mut() }
    }
    #[inline] pub fn page_type_slab() -> u64 { xv6_page_type_slab_value() }

    #[inline] pub fn list_init(entry: &mut ListNode) { xv6_list_init(entry) }
    #[inline] pub fn list_is_empty(head: &ListNode) -> bool {
        xv6_list_is_empty(head) != 0
    }
    #[inline] pub fn list_detach(entry: &mut ListNode) { xv6_list_detach(entry) }
    #[inline] pub fn list_push_front(head: &mut ListNode, entry: &mut ListNode) {
        xv6_list_push_front(head, entry)
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

    #[inline] pub fn log_free_null(fn_name: *const c_char) {
        xv6_slab_log_free_null(fn_name)
    }
    #[inline] pub fn log_no_slab(fn_name: *const c_char, obj: *mut c_void) {
        xv6_slab_log_no_slab(fn_name, obj)
    }
    #[inline] pub fn log_unattached(slab: &mut Slab, obj: *mut c_void) {
        xv6_slab_log_unattached(slab, obj)
    }
    #[inline] pub fn log_from_free(
        obj: *mut c_void,
        slab: &mut Slab,
        cache: &mut SlabCache,
        cpu_id: c_int,
    ) {
        xv6_slab_log_from_free(obj, slab, cache, cpu_id)
    }
    #[inline] pub fn panic(msg: &'static [u8]) -> ! {
        xv6_slab_panic(msg.as_ptr() as *const c_char)
    }
    #[inline] pub fn shrink_all() { slab_shrink_all() }
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

        let page_type_slab = ffi::page_type_slab();
        let mut raw_page = ffi::page_alloc(order as u64, page_type_slab);
        if raw_page.is_null() {
            // Emergency reclaim -- shrink everything and retry once.
            ffi::shrink_all();
            raw_page = ffi::page_alloc(order as u64, page_type_slab);
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

#[no_mangle]
pub unsafe extern "C" fn slab_cache_init(
    cache: *mut SlabCache,
    name: *mut c_char,
    obj_size: usize,
    flags: u64,
) -> c_int {
    let Some(cache) = cache.as_mut() else { return -1; };
    cache.init_impl(name, obj_size, flags)
}

#[no_mangle]
pub unsafe extern "C" fn slab_cache_create(
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

#[no_mangle]
pub unsafe extern "C" fn slab_cache_destroy(cache: *mut SlabCache) -> c_int {
    let Some(cache) = cache.as_mut() else { return -1; };
    cache.destroy_impl()
}

#[no_mangle]
pub unsafe extern "C" fn slab_cache_shrink(
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
#[no_mangle]
pub unsafe extern "C" fn slab_alloc(cache: *mut SlabCache) -> *mut c_void {
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

/// `obj` is a raw pointer crossing the C/Rust boundary -- the unsafe contract
/// belongs to the caller (must point to a live object owned by a slab).
#[no_mangle]
pub unsafe extern "C" fn slab_free(obj: *mut c_void) {
    if let Some(cache) =
        slab_free_core(obj, b"slab_free\0".as_ptr() as *const c_char)
    {
        cache.try_shrink_global();
    }
}

#[no_mangle]
pub unsafe extern "C" fn slab_free_noshrink(obj: *mut c_void) {
    // Phases 1-5 identical to slab_free; PHASE 6 deliberately skipped.
    let _ = slab_free_core(
        obj,
        b"slab_free_noshrink\0".as_ptr() as *const c_char,
    );
}
