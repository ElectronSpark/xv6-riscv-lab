//! Kernel small-object allocator — Rust port of `kernel/mm/kalloc.c`.
//!
//! Public C ABI:
//!   * `kinit`                     — bring up the buddy allocator + 8
//!                                   power-of-two slab caches (32B..4096B)
//!                                   plus two self-describing pools for
//!                                   `slab_t` and `slab_cache_t` descriptors.
//!   * `kmm_alloc` / `kmm_free`    — slab-backed allocation (≤ PGSIZE).
//!   * `kmm_shrink_all`            — emergency reclaim of unused slabs.
//!   * `get_total_free_pages`      — sum of buddy free pages weighted by
//!                                   order.
//!   * `kalloc` / `kfree`          — legacy single-page (de)allocators.
//!   * `slab_t_desc_alloc/free`    — descriptor pool for slabs themselves.
//!   * `slab_cache_t_alloc/free`   — descriptor pool for slab caches.
//!
//! All algorithmic logic lives in safe Rust. The only `unsafe` is the
//! FFI boundary (`mod ffi`) and the thin `#[no_mangle] extern "C"` wrappers.

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::{size_of, MaybeUninit};
use core::ptr::NonNull;

use crate::bindings::{slab_cache_t, slab_t};

// ---------------------------------------------------------------------------
// Constants — mirrored from kernel headers.
// ---------------------------------------------------------------------------
const SLAB_CACHE_NUMS:      usize = 8;     // mm/slab.h
const SLAB_OBJ_MIN_SHIFT:   u32   = 5;     // mm/slab.h
const SLAB_OBJ_MIN_SIZE:    usize = 1 << SLAB_OBJ_MIN_SHIFT;
const SLAB_OBJ_MAX_SHIFT:   u32   = 12;    // = PAGE_SHIFT
const SLAB_OBJ_MAX_SIZE:    usize = 1 << SLAB_OBJ_MAX_SHIFT;
const SLAB_FLAG_STATIC:     u64   = 1;
const SLAB_FLAG_EMBEDDED:   u64   = 2;

const PGSIZE:               usize = 4096;
const PAGE_BUDDY_MAX_ORDER: u32   = 10;    // mm/page_type.h
const PAGE_TYPE_ANON:       u64   = 0;

// ---------------------------------------------------------------------------
// FFI boundary.  Every raw C call lives here.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;

    unsafe extern "C" {
        // Page-level allocator.
        pub safe fn page_buddy_init() -> c_int;
        pub safe fn __page_alloc(order: u64, flags: u64) -> *mut c_void;
        pub safe fn __pa_to_page(physical: u64) -> *mut c_void;
        pub safe fn __page_to_pa(page: *mut c_void) -> u64;
        pub safe fn __page_ref_dec(page: *mut c_void) -> c_int;
        pub safe fn page_buddy_stat(ret_arr: *mut u64, empty_arr: *mut bool, size: usize);

        // Slab allocator.
        pub safe fn slab_cache_init(cache: *mut c_void, name: *const c_char,
                               obj_size: usize, flags: u64) -> c_int;
        pub safe fn slab_cache_shrink(cache: *mut c_void, nums: c_int) -> c_int;
        pub safe fn slab_alloc(cache: *mut c_void) -> *mut c_void;
        pub safe fn slab_free(obj: *mut c_void);

        // libc-style helpers from C side.
        pub safe fn memset(dst: *mut c_void, c: c_int, n: usize) -> *mut c_void;
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
    }

    // `printf` is variadic and cannot be declared `safe`; keep the
    // legacy unsafe-callable form for the few callers that need it.
    unsafe extern "C" {
        pub fn printf(fmt: *const c_char, ...) -> c_int;
    }
}

// ---------------------------------------------------------------------------
// Static storage helper: zero-initialised `MaybeUninit<T>` with `Sync`.
// ---------------------------------------------------------------------------
#[repr(transparent)]
struct Storage<T>(UnsafeCell<MaybeUninit<T>>);
unsafe impl<T> Sync for Storage<T> {}
impl<T> Storage<T> {
    const fn zeroed() -> Self {
        Self(UnsafeCell::new(MaybeUninit::zeroed()))
    }
    fn as_ptr(&self) -> *mut T {
        self.0.get() as *mut T
    }
}

// ---------------------------------------------------------------------------
// Safe wrapper around a `*mut slab_cache_t`.
//
// The wrapped storage is protected by an internal C spinlock; from Rust's
// view it's a shared opaque handle (no `&mut` ever taken).
// ---------------------------------------------------------------------------
#[derive(Copy, Clone)]
struct SlabCachePtr(NonNull<slab_cache_t>);

impl SlabCachePtr {
    /// SAFETY: `ptr` must point at live storage suitable for a `slab_cache_t`
    /// and remain valid for `'static`.
    const unsafe fn from_storage(ptr: *mut slab_cache_t) -> Self {
        Self(NonNull::new_unchecked(ptr))
    }

    fn as_void(self) -> *mut c_void {
        self.0.as_ptr() as *mut c_void
    }

    /// Initialise the cache for `obj_size`-byte objects.
    fn init(self, name: *const c_char, obj_size: usize, flags: u64) -> c_int {
        ffi::slab_cache_init(self.as_void(), name, obj_size, flags)
    }

    fn alloc(self) -> *mut c_void {
        ffi::slab_alloc(self.as_void())
    }

    fn shrink(self, n: c_int) -> c_int {
        ffi::slab_cache_shrink(self.as_void(), n)
    }
}

// ---------------------------------------------------------------------------
// Backing storage — formerly C arrays in `kalloc_shims.c`.
// ---------------------------------------------------------------------------
static KMM_SLAB_CACHE: [Storage<slab_cache_t>; SLAB_CACHE_NUMS] =
    [const { Storage::zeroed() }; SLAB_CACHE_NUMS];
static SLAB_T_POOL:        Storage<slab_cache_t> = Storage::zeroed();
static SLAB_CACHE_T_POOL:  Storage<slab_cache_t> = Storage::zeroed();
static KMM_SLAB_NAMES: [Storage<[u8; 32]>; SLAB_CACHE_NUMS] =
    [const { Storage::zeroed() }; SLAB_CACHE_NUMS];

// Typed accessors.
fn kmm_cache(i: usize) -> SlabCachePtr {
    unsafe { SlabCachePtr::from_storage(KMM_SLAB_CACHE[i].as_ptr()) }
}
fn slab_t_pool() -> SlabCachePtr {
    unsafe { SlabCachePtr::from_storage(SLAB_T_POOL.as_ptr()) }
}
fn slab_cache_t_pool() -> SlabCachePtr {
    unsafe { SlabCachePtr::from_storage(SLAB_CACHE_T_POOL.as_ptr()) }
}

/// Borrow the name slot for slab `i` mutably during init (single-threaded).
fn kmm_name_slot(i: usize) -> &'static mut [u8; 32] {
    // SAFETY: called only from `kinit`, which runs once at boot on a single
    // hart before any reads. The storage outlives `'static`.
    unsafe { &mut *KMM_SLAB_NAMES[i].as_ptr() }
}
fn kmm_name_ptr(i: usize) -> *const c_char {
    KMM_SLAB_NAMES[i].as_ptr() as *const c_char
}

// ---------------------------------------------------------------------------
// Panic / diagnostic helpers — small safe wrappers around the variadic
// C `printf`/panic primitives.
// ---------------------------------------------------------------------------
#[inline(never)]
fn panic_kalloc(msg: &[u8]) -> ! {
    debug_assert!(msg.last() == Some(&0), "panic msg must be NUL-terminated");
    ffi::__panic_start();
    // SAFETY: variadic printf; format and `msg` are NUL-terminated.
    unsafe {
        ffi::printf(b"PANIC kalloc: %s\n\0".as_ptr() as *const c_char,
                    msg.as_ptr() as *const c_char);
    }
    ffi::__panic_end()
}

fn log_slab_init_failed(name: *const c_char) {
    // SAFETY: variadic printf; format is NUL-terminated; `name` is a
    // NUL-terminated string supplied by the caller.
    unsafe {
        ffi::printf(b"failed to initialize kmm slab: %s\n\0".as_ptr() as *const c_char,
                    name);
    }
}

// ---------------------------------------------------------------------------
// Pure algorithms.
// ---------------------------------------------------------------------------

/// Render `bytes` as decimal ASCII into the 32-byte name slot for slab `idx`.
/// Mirrors `__init_kmm_slab_name()` from the C original.
fn init_kmm_slab_name(idx: usize, mut bytes: usize) {
    let dst = kmm_name_slot(idx);
    dst.fill(0);
    let mut tmp = [0u8; 32];
    let mut i: usize = 29;
    while bytes != 0 {
        tmp[i] = b'0' + (bytes % 10) as u8;
        bytes /= 10;
        i -= 1;
    }
    let start = i + 1;
    let len = 30 - start;
    dst[..len].copy_from_slice(&tmp[start..start + len]);
}

/// Map a requested allocation size to a slab-cache index, or `None` if it
/// exceeds `SLAB_OBJ_MAX_SIZE`.
fn slab_index_for(mut size: usize) -> Option<usize> {
    if size > SLAB_OBJ_MAX_SIZE {
        return None;
    }
    if size < SLAB_OBJ_MIN_SIZE {
        size = SLAB_OBJ_MIN_SIZE;
    }
    // ceil(log2(size)); size is clamped to ≥ 32 so (size-1) > 0.
    let obj_shift: u32 = 64 - ((size - 1) as u64).leading_zeros();
    if obj_shift > SLAB_OBJ_MAX_SHIFT {
        return None;
    }
    let mut idx = 0;
    let mut obj_size = SLAB_OBJ_MIN_SIZE;
    while idx < SLAB_CACHE_NUMS && obj_size < size {
        obj_size <<= 1;
        idx += 1;
    }
    (idx < SLAB_CACHE_NUMS).then_some(idx)
}

/// Sum `(1<<order) * free_pages[order]` across the buddy allocator.
fn total_free_pages() -> u64 {
    const N: usize = (PAGE_BUDDY_MAX_ORDER as usize) + 1;
    let mut ret_arr: [u64; N] = [0; N];
    let mut empty_arr: [bool; N] = [false; N];
    ffi::page_buddy_stat(ret_arr.as_mut_ptr(), empty_arr.as_mut_ptr(), N);
    let mut total: u64 = 0;
    for i in 0..N {
        total = total.wrapping_add((1u64 << i).wrapping_mul(ret_arr[i]));
    }
    total
}

/// Initialise the slab caches.  Panics if any cache fails to init.
fn init_kmm() {
    ffi::page_buddy_init();

    let flags = SLAB_FLAG_STATIC | SLAB_FLAG_EMBEDDED;
    let slab_t_size = size_of::<slab_t>();
    let slab_cache_t_size = size_of::<slab_cache_t>();

    if slab_t_pool().init(b"slab_t_pool\0".as_ptr() as *const c_char,
                          slab_t_size, flags) != 0 {
        panic_kalloc(b"kinit: failed to initialize slab_t pool\0");
    }
    if slab_cache_t_pool().init(b"slab_cache_t_pool\0".as_ptr() as *const c_char,
                                slab_cache_t_size, flags) != 0 {
        panic_kalloc(b"kinit: failed to initialize slab_cache_t pool\0");
    }

    let mut obj_size = SLAB_OBJ_MIN_SIZE;
    for i in 0..SLAB_CACHE_NUMS {
        init_kmm_slab_name(i, obj_size);
        let name = kmm_name_ptr(i);
        if kmm_cache(i).init(name, obj_size, flags) != 0 {
            log_slab_init_failed(name);
            panic_kalloc(b"kinit\0");
        }
        obj_size <<= 1;
    }
}

/// Free a single page-sized allocation (legacy `kalloc`/`kfree` path).
fn page_free(pa: *mut c_void) {
    let page = ffi::__pa_to_page(pa as u64);
    if ffi::__page_ref_dec(page) == -1 {
        panic_kalloc(b"kfree\0");
    }
}

/// Allocate a single page; fill with junk byte `5` to catch use-of-uninit.
fn page_alloc_legacy() -> *mut c_void {
    let page = ffi::__page_alloc(0, PAGE_TYPE_ANON);
    if page.is_null() {
        return core::ptr::null_mut();
    }
    let pa = ffi::__page_to_pa(page);
    if pa == 0 {
        panic_kalloc(b"kalloc\0");
    }
    ffi::memset(pa as *mut c_void, 5, PGSIZE);
    pa as *mut c_void
}

// ---------------------------------------------------------------------------
// Public C ABI — thin wrappers.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn kinit() {
    init_kmm()
}

#[no_mangle]
pub unsafe extern "C" fn slab_t_desc_alloc() -> *mut c_void {
    slab_t_pool().alloc()
}

#[no_mangle]
pub unsafe extern "C" fn slab_t_desc_free(slab_desc: *mut c_void) {
    if !slab_desc.is_null() {
        ffi::slab_free(slab_desc);
    }
}

#[no_mangle]
pub unsafe extern "C" fn slab_cache_t_alloc() -> *mut c_void {
    slab_cache_t_pool().alloc()
}

#[no_mangle]
pub unsafe extern "C" fn slab_cache_t_free(cache_desc: *mut c_void) {
    if !cache_desc.is_null() {
        ffi::slab_free(cache_desc);
    }
}

#[no_mangle]
pub unsafe extern "C" fn kmm_alloc(size: usize) -> *mut c_void {
    match slab_index_for(size) {
        Some(idx) => kmm_cache(idx).alloc(),
        None      => core::ptr::null_mut(),
    }
}

#[no_mangle]
pub unsafe extern "C" fn kmm_free(ptr: *mut c_void) {
    ffi::slab_free(ptr)
}

#[no_mangle]
pub unsafe extern "C" fn kmm_shrink_all() {
    for i in 0..SLAB_CACHE_NUMS {
        kmm_cache(i).shrink(c_int::MAX);
    }
}

#[no_mangle]
pub unsafe extern "C" fn get_total_free_pages() -> u64 {
    total_free_pages()
}

#[no_mangle]
pub unsafe extern "C" fn kfree(pa: *mut c_void) {
    page_free(pa)
}

#[no_mangle]
pub unsafe extern "C" fn kalloc() -> *mut c_void {
    page_alloc_legacy()
}
