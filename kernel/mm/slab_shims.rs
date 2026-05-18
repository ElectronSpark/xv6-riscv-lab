//! Rust port of `kernel/mm/slab_shims.c`.
//!
//! Provides the `xv6_*` C-ABI shim symbols the Rust slab allocator
//! (`slab.rs`) expects via its `extern "C"` block. By exporting them as
//! `#[no_mangle] pub unsafe extern "C" fn`, the staticlib linker resolves
//! the symbols internally; `slab.rs` is unchanged. This file replaces the
//! deleted `kernel/mm/slab_shims.c`.

#![allow(non_snake_case)]
#![allow(non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::{align_of, offset_of, size_of};
use core::sync::atomic::{compiler_fence, AtomicBool, AtomicI64, AtomicI32, Ordering};

use crate::bindings::{
    list_node_t, page_struct, percpu_slab_cache_t, slab_cache_t, slab_t, spinlock_t,
};

// ---------------------------------------------------------------------------
// Constants mirrored from kernel headers.
// ---------------------------------------------------------------------------
const NCPU: usize = 8;
const PAGE_FLAG_TYPE_BITS: u64 = 8;
const PAGE_FLAG_TYPE_MASK: u64 = (1u64 << PAGE_FLAG_TYPE_BITS) - 1;
const PAGE_TYPE_SLAB: u64 = 2;
const PAGE_TYPE_TAIL: u64 = 5;
const PAGE_SIZE: u64 = 4096;

// ---------------------------------------------------------------------------
// Layout pins — these used to be C `_Static_assert`s; replicated as
// compile-time Rust assertions so any ABI drift fails the build.
// ---------------------------------------------------------------------------
const _: () = {
    assert!(size_of::<list_node_t>() == 16, "list_node_t size mismatch");
    assert!(size_of::<spinlock_t>() == 24, "spinlock_t size mismatch");
    // NOTE: in C `spinlock_t` is `align(64)` via a GCC typedef-attribute
    // extension that keeps `sizeof == 24`. Rust cannot express that
    // combination (`#[repr(align(N))]` rounds size up to N), so bindgen
    // emits the struct without alignment. The field-offset asserts below
    // are what actually guarantee ABI parity — bindgen inserts explicit
    // `__bindgen_padding_*` to place each embedded `spinlock_t` at the
    // C-correct 64-aligned offset.
    assert!(
        size_of::<percpu_slab_cache_t>() == 128,
        "percpu_slab_cache_t size mismatch"
    );
    assert!(
        align_of::<percpu_slab_cache_t>() == 64,
        "percpu_slab_cache_t alignment mismatch"
    );
    assert!(size_of::<slab_cache_t>() == 1280, "cache total size");
    assert!(NCPU == 8, "Rust slab mirror assumes NCPU == 8");

    assert!(offset_of!(slab_cache_t, name) == 0, "cache.name");
    assert!(offset_of!(slab_cache_t, flags) == 8, "cache.flags");
    assert!(offset_of!(slab_cache_t, obj_size) == 16, "cache.obj_size");
    assert!(offset_of!(slab_cache_t, offset) == 24, "cache.offset");
    assert!(offset_of!(slab_cache_t, slab_order) == 32, "cache.slab_order");
    assert!(offset_of!(slab_cache_t, slab_obj_num) == 36, "cache.slab_obj_num");
    assert!(offset_of!(slab_cache_t, bitmap_size) == 40, "cache.bitmap_size");
    assert!(offset_of!(slab_cache_t, limits) == 44, "cache.limits");
    assert!(offset_of!(slab_cache_t, percpu_caches) == 64, "cache.percpu_caches");
    assert!(
        offset_of!(slab_cache_t, global_free_list) == 64 + 128 * NCPU,
        "cache.global_free_list"
    );
    assert!(
        offset_of!(slab_cache_t, global_free_lock) == 1152,
        "cache.global_free_lock"
    );
    assert!(
        offset_of!(slab_cache_t, global_free_count) == 1176,
        "cache.global_free_count"
    );
    assert!(offset_of!(slab_cache_t, slab_total) == 1184, "cache.slab_total");
    assert!(offset_of!(slab_cache_t, obj_active) == 1192, "cache.obj_active");
    assert!(offset_of!(slab_cache_t, obj_total) == 1200, "cache.obj_total");
    assert!(
        offset_of!(slab_cache_t, cache_list_entry) == 1208,
        "cache.cache_list_entry"
    );

    assert!(offset_of!(slab_t, list_entry) == 0, "slab.list_entry");
    assert!(offset_of!(slab_t, cache) == 16, "slab.cache");
    assert!(offset_of!(slab_t, page) == 24, "slab.page");
    assert!(offset_of!(slab_t, slab_order) == 32, "slab.slab_order");
    assert!(offset_of!(slab_t, in_use) == 40, "slab.in_use");
    assert!(offset_of!(slab_t, next) == 48, "slab.next");
    assert!(offset_of!(slab_t, state) == 56, "slab.state");
    assert!(offset_of!(slab_t, bitmap) == 64, "slab.bitmap");
    assert!(offset_of!(slab_t, cpu_id) == 72, "slab.cpu_id");
};

// ---------------------------------------------------------------------------
// Externs into the C kernel.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    use core::ffi::{c_char, c_int};
    unsafe extern "C" {
        pub safe fn spin_lock(lk: *mut spinlock_t);
        pub safe fn spin_unlock(lk: *mut spinlock_t);
        pub safe fn spin_init(lk: *mut spinlock_t, name: *const c_char);
        pub safe fn page_lock_acquire(page: *mut page_struct);
        pub safe fn page_lock_release(page: *mut page_struct);
        pub safe fn slab_cache_shrink(cache: *mut slab_cache_t, n: c_int) -> c_int;
        pub safe fn printf(fmt: *const c_char, ...) -> c_int;
        pub safe fn __pa_to_page(pa: u64) -> *mut page_struct;
        // Kernel panic helpers (kernel/printf.c).
        pub safe fn __panic_start();
        pub safe fn __panic_end() -> !;
    }
}
use ffi::*;

// ---------------------------------------------------------------------------
// xv6_cpuid / xv6_push_off / xv6_pop_off — replicate the C macros.
// These mirror the implementations in vm.rs but are needed independently
// here so the staticlib resolves the `extern "C"` declarations in slab.rs
// and page.rs without depending on vm.rs internals.
// ---------------------------------------------------------------------------
const SSTATUS_SIE: u64 = 1 << 1;

#[inline(always)]
fn read_tp() -> u64 {
    let tp: u64;
    unsafe {
        core::arch::asm!("mv {0}, tp", out(reg) tp,
            options(nomem, nostack, preserves_flags));
    }
    tp
}
#[inline(always)]
fn read_sstatus() -> u64 {
    let v: u64;
    unsafe {
        core::arch::asm!("csrr {0}, sstatus", out(reg) v,
            options(nomem, nostack, preserves_flags));
    }
    v
}
#[inline(always)]
fn intr_off_raw() {
    unsafe {
        core::arch::asm!("csrc sstatus, {0}", in(reg) SSTATUS_SIE,
            options(nomem, nostack, preserves_flags));
    }
}
#[inline(always)]
fn intr_on_raw() {
    unsafe {
        core::arch::asm!("csrs sstatus, {0}", in(reg) SSTATUS_SIE,
            options(nomem, nostack, preserves_flags));
    }
}
#[inline(always)]
fn intr_get_raw() -> bool {
    (read_sstatus() & SSTATUS_SIE) != 0
}
#[inline(always)]
unsafe fn mycpu_local() -> *mut crate::bindings::cpu_local {
    read_tp() as *mut crate::bindings::cpu_local
}
#[no_mangle]
pub unsafe extern "C" fn xv6_cpuid() -> c_int {
    let page_mask: u64 = PAGE_SIZE - 1;
    let off = read_tp() & page_mask;
    (off / core::mem::size_of::<crate::bindings::cpu_local>() as u64) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn xv6_push_off() {
    let old = intr_get_raw();
    if old {
        intr_off_raw();
    }
    let c = mycpu_local();
    let noff = (*c).noff;
    if noff == 0 {
        (*c).intena = if old { 1 } else { 0 };
    }
    (*c).noff = noff + 1;
}

#[no_mangle]
pub unsafe extern "C" fn xv6_pop_off() {
    let c = mycpu_local();
    let n = (*c).noff - 1;
    (*c).noff = n;
    if n == 0 && (*c).intena != 0 {
        intr_on_raw();
    }
}

// ---------------------------------------------------------------------------
// Inline list helpers (replicate kernel/inc/list.h static-inlines).
// ---------------------------------------------------------------------------
#[inline]
unsafe fn list_init(e: *mut list_node_t) {
    (*e).next = e;
    (*e).prev = e;
}
#[inline]
unsafe fn list_is_empty(h: *const list_node_t) -> bool {
    (*h).next == h as *mut list_node_t
}
#[inline]
unsafe fn list_is_detached(e: *const list_node_t) -> bool {
    let p = (*e).prev;
    let n = (*e).next;
    (p == e as *mut list_node_t) && (n == e as *mut list_node_t)
}
#[inline]
unsafe fn list_detach(e: *mut list_node_t) {
    let p = (*e).prev;
    let n = (*e).next;
    (*p).next = n;
    (*n).prev = p;
    list_init(e);
}
#[inline]
unsafe fn list_insert_after(prev: *mut list_node_t, new: *mut list_node_t) {
    let nxt = (*prev).next;
    (*new).next = nxt;
    (*new).prev = prev;
    (*nxt).prev = new;
    (*prev).next = new;
}
#[inline]
unsafe fn list_push_front(h: *mut list_node_t, e: *mut list_node_t) {
    list_insert_after(h, e);
}
#[inline]
unsafe fn list_pop_front(h: *mut list_node_t) -> *mut list_node_t {
    if list_is_empty(h) {
        return core::ptr::null_mut();
    }
    let first = (*h).next;
    list_detach(first);
    first
}
#[inline]
unsafe fn list_first(h: *const list_node_t) -> *mut list_node_t {
    if list_is_empty(h) {
        core::ptr::null_mut()
    } else {
        (*h).next
    }
}
#[inline]
unsafe fn list_last(h: *const list_node_t) -> *mut list_node_t {
    if list_is_empty(h) {
        core::ptr::null_mut()
    } else {
        (*h).prev
    }
}

// ---------------------------------------------------------------------------
// Static globals for the slab-cache registry. Initialised lazily on first
// `xv6_slab_register_cache` call. The C version used compile-time initializers
// (`LIST_ENTRY_INITIALIZED`, `SPINLOCK_INITIALIZED`); Rust does the same at
// runtime under an `AtomicBool` once-flag.
// ---------------------------------------------------------------------------
#[repr(C)]
struct GlobalListCell(UnsafeCell<list_node_t>);
unsafe impl Sync for GlobalListCell {}

#[repr(C)]
struct GlobalLockCell(UnsafeCell<spinlock_t>);
unsafe impl Sync for GlobalLockCell {}

static __ALL_SLAB_CACHES: GlobalListCell = GlobalListCell(UnsafeCell::new(list_node_t {
    prev: core::ptr::null_mut(),
    next: core::ptr::null_mut(),
}));

static __ALL_SLAB_CACHES_LOCK: GlobalLockCell = GlobalLockCell(UnsafeCell::new(spinlock_t {
    locked: 0,
    name: core::ptr::null_mut(),
    cpu: core::ptr::null_mut(),
}));

static ALL_CACHES_INIT: AtomicBool = AtomicBool::new(false);
static ALL_CACHES_INIT_DONE: AtomicBool = AtomicBool::new(false);

static ALL_CACHES_NAME: &[u8] = b"all_slab_caches\0";

unsafe fn ensure_global_init() {
    if ALL_CACHES_INIT_DONE.load(Ordering::Acquire) {
        return;
    }
    if ALL_CACHES_INIT
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_ok()
    {
        let head = __ALL_SLAB_CACHES.0.get();
        list_init(head);
        spin_init(
            __ALL_SLAB_CACHES_LOCK.0.get(),
            ALL_CACHES_NAME.as_ptr() as *const c_char,
        );
        compiler_fence(Ordering::Release);
        ALL_CACHES_INIT_DONE.store(true, Ordering::Release);
    } else {
        while !ALL_CACHES_INIT_DONE.load(Ordering::Acquire) {
            core::hint::spin_loop();
        }
    }
}
// ---------------------------------------------------------------------------
// Cache registry (replaces C versions in slab_shims.c).
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn xv6_slab_register_cache(cache: *mut slab_cache_t) {
    ensure_global_init();
    let entry = &raw mut (*cache).cache_list_entry;
    list_init(entry);
    spin_lock(__ALL_SLAB_CACHES_LOCK.0.get());
    // Insert at tail: insert_after(LIST_LAST_ENTRY(head))
    let head = __ALL_SLAB_CACHES.0.get();
    let tail = (*head).prev;
    list_insert_after(tail, entry);
    spin_unlock(__ALL_SLAB_CACHES_LOCK.0.get());
}

#[no_mangle]
pub unsafe extern "C" fn xv6_slab_unregister_cache(cache: *mut slab_cache_t) {
    ensure_global_init();
    spin_lock(__ALL_SLAB_CACHES_LOCK.0.get());
    let entry = &raw mut (*cache).cache_list_entry;
    if !list_is_detached(entry) {
        list_detach(entry);
    }
    spin_unlock(__ALL_SLAB_CACHES_LOCK.0.get());
}

// ---------------------------------------------------------------------------
// slab_shrink_all / slab_dump_all — iterate global registry.
// ---------------------------------------------------------------------------
#[inline]
unsafe fn cache_from_entry(entry: *mut list_node_t) -> *mut slab_cache_t {
    (entry as *mut u8).wrapping_sub(offset_of!(slab_cache_t, cache_list_entry)) as *mut slab_cache_t
}
#[no_mangle]
pub unsafe extern "C" fn slab_shrink_all() {
    ensure_global_init();
    let head = __ALL_SLAB_CACHES.0.get();
    spin_lock(__ALL_SLAB_CACHES_LOCK.0.get());
    let mut cur = (*head).next;
    while cur != head {
        let cache = cache_from_entry(cur);
        let free_count = AtomicI64::from_ptr(&raw mut (*cache).global_free_count)
            .load(Ordering::Acquire);
        let nxt = (*cur).next;
        if free_count > 0 {
            let to_shrink = ((free_count + 1) / 2) as c_int;
            if to_shrink > 0 {
                spin_unlock(__ALL_SLAB_CACHES_LOCK.0.get());
                slab_cache_shrink(cache, to_shrink);
                spin_lock(__ALL_SLAB_CACHES_LOCK.0.get());
                // Restart iteration; nxt may be stale.
                cur = (*head).next;
                continue;
            }
        }
        cur = nxt;
    }
    spin_unlock(__ALL_SLAB_CACHES_LOCK.0.get());
}

// printf format strings for slab_dump_all
static DUMP_HDR: &[u8] = b"\n=== SLAB CACHE STATISTICS ===\n\0";
static DUMP_COL: &[u8] =
    b"NAME             OBJSZ    TOTAL   ACTIVE     FREE    PAGES\n\0";
static DUMP_ROW: &[u8] =
    b"%s: objsz=%ld total=%ld active=%lu free=%ld pages=%lu\n\0";
static DUMP_SEP: &[u8] = b"-----------------------------\n\0";
static DUMP_END: &[u8] = b"=============================\n\0";
static DUMP_SLAB_PG: &[u8] = b"Slab:  %lu pages (\0";
static DUMP_MB: &[u8] = b"%lu.%luMB\0";
static DUMP_KB: &[u8] = b"%luKB\0";
static DUMP_TAIL: &[u8] = b")\n\0";

#[no_mangle]
pub unsafe extern "C" fn slab_dump_all(detailed: c_int) -> u64 {
    ensure_global_init();
    let head = __ALL_SLAB_CACHES.0.get();
    let mut total_pages: u64 = 0;

    if detailed >= 2 {
        printf(DUMP_HDR.as_ptr() as *const c_char);
        printf(DUMP_COL.as_ptr() as *const c_char);
    }

    spin_lock(__ALL_SLAB_CACHES_LOCK.0.get());
    let mut cur = (*head).next;
    while cur != head {
        let cache = cache_from_entry(cur);
        let nxt = (*cur).next;
        let slab_total = AtomicI64::from_ptr(&raw mut (*cache).slab_total)
            .load(Ordering::Acquire);
        let obj_active = AtomicI64::from_ptr(&raw mut (*cache).obj_active as *mut i64)
            .load(Ordering::Acquire) as u64;
        let global_free = AtomicI64::from_ptr(&raw mut (*cache).global_free_count)
            .load(Ordering::Acquire);
        let pages = (slab_total as u64) * (1u64 << (*cache).slab_order);
        total_pages += pages;
        if detailed >= 2 {
            printf(
                DUMP_ROW.as_ptr() as *const c_char,
                (*cache).name,
                (*cache).obj_size as i64,
                slab_total,
                obj_active,
                global_free,
                pages,
            );
        }
        cur = nxt;
    }
    spin_unlock(__ALL_SLAB_CACHES_LOCK.0.get());

    if detailed >= 2 {
        printf(DUMP_SEP.as_ptr() as *const c_char);
    }
    let total_bytes = total_pages * PAGE_SIZE;
    if detailed >= 1 {
        printf(DUMP_SLAB_PG.as_ptr() as *const c_char, total_pages);
        if total_bytes >= 1024 * 1024 {
            let mb = total_bytes / (1024 * 1024);
            let kb_remainder = (total_bytes % (1024 * 1024)) / 1024;
            printf(DUMP_MB.as_ptr() as *const c_char, mb, kb_remainder * 10 / 1024);
        } else {
            printf(DUMP_KB.as_ptr() as *const c_char, total_bytes / 1024);
        }
        printf(DUMP_TAIL.as_ptr() as *const c_char);
        if detailed >= 2 {
            printf(DUMP_END.as_ptr() as *const c_char);
        }
    }
    total_bytes
}

// ---------------------------------------------------------------------------
// List wrappers (operate on raw list_node_t pointers; the slab uses these
// for its per-cpu / per-slab free lists which have list_entry at offset 0).
// ---------------------------------------------------------------------------
#[no_mangle]
pub unsafe extern "C" fn xv6_list_init(entry: *mut list_node_t) {
    list_init(entry)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_is_empty(head: *const list_node_t) -> c_int {
    if list_is_empty(head) { 1 } else { 0 }
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_is_detached(entry: *const list_node_t) -> c_int {
    if list_is_detached(entry) { 1 } else { 0 }
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_detach(entry: *mut list_node_t) {
    list_detach(entry)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_insert_after(prev: *mut list_node_t, entry: *mut list_node_t) {
    list_insert_after(prev, entry)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_push_front(head: *mut list_node_t, entry: *mut list_node_t) {
    list_push_front(head, entry)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_pop_front(head: *mut list_node_t) -> *mut list_node_t {
    list_pop_front(head)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_first(head: *const list_node_t) -> *mut list_node_t {
    list_first(head)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_list_last(head: *const list_node_t) -> *mut list_node_t {
    list_last(head)
}

// ---------------------------------------------------------------------------
// Page-union access for slab pages.
// ---------------------------------------------------------------------------
#[inline]
unsafe fn page_flags_raw(page: *mut page_struct) -> u64 {
    (*page).__bindgen_anon_1.flags
}
#[inline]
unsafe fn page_is_type(page: *mut page_struct, ty: u64) -> bool {
    (page_flags_raw(page) & PAGE_FLAG_TYPE_MASK) == (ty & PAGE_FLAG_TYPE_MASK)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_slab_page_attach(
    head: *mut page_struct,
    slab: *mut slab_t,
    order: u32,
) {
    // head[0].slab.slab = slab; head[0].slab.order = order;
    let slab_var = &raw mut (*head).__bindgen_anon_2.slab;
    (*slab_var).slab = slab;
    (*slab_var).order = order;
    let page_count = 1u64 << order;
    for i in 1..page_count {
        let p = head.add(i as usize);
        // p->flags = PAGE_TYPE_TAIL  (preserve only the type bits; rest were 0)
        (*p).__bindgen_anon_1.flags = PAGE_TYPE_TAIL;
        let tail_var = &raw mut (*p).__bindgen_anon_2.tail;
        (*tail_var).head_page = head;
    }
}

#[no_mangle]
pub unsafe extern "C" fn xv6_slab_page_set_order(head: *mut page_struct, order: u32) {
    let slab_var = &raw mut (*head).__bindgen_anon_2.slab;
    (*slab_var).order = order;
}

#[no_mangle]
pub unsafe extern "C" fn xv6_slab_find_obj_slab(ptr: *mut c_void) -> *mut slab_t {
    if ptr.is_null() {
        return core::ptr::null_mut();
    }
    let page_base = (ptr as u64) & !(PAGE_SIZE - 1);
    let page = __pa_to_page(page_base);
    if page.is_null() {
        return core::ptr::null_mut();
    }
    let header: *mut page_struct = if page_is_type(page, PAGE_TYPE_SLAB) {
        page
    } else if page_is_type(page, PAGE_TYPE_TAIL) {
        let tail_var = &raw mut (*page).__bindgen_anon_2.tail;
        let h = (*tail_var).head_page;
        if h.is_null() || !page_is_type(h, PAGE_TYPE_SLAB) {
            return core::ptr::null_mut();
        }
        h
    } else {
        return core::ptr::null_mut();
    };
    let slab_var = &raw mut (*header).__bindgen_anon_2.slab;
    if (*slab_var).slab.is_null() {
        return core::ptr::null_mut();
    }
    (*slab_var).slab
}

#[no_mangle]
pub unsafe extern "C" fn xv6_page_type_slab_value() -> u64 {
    PAGE_TYPE_SLAB
}

// ---------------------------------------------------------------------------
// Diagnostic printf wrappers.
// ---------------------------------------------------------------------------
static LOG_FREE_NULL: &[u8] = b"%s: obj is NULL\n\0";
static LOG_NO_SLAB: &[u8] = b"%s: slab is NULL for obj=%p\n\0";
static LOG_UNATTACHED_1: &[u8] =
    b"slab_free: ERROR - slab=%p not attached to cache, obj=%p\n\0";
static LOG_UNATTACHED_2: &[u8] =
    b"  slab->page=%p, slab->in_use=%ld, slab->state=%d, slab->cpu_id=%d\n\0";
static LOG_FROM_FREE_1: &[u8] = b"slab_free: ERROR - object from free slab\n\0";
static LOG_FROM_FREE_2: &[u8] = b"  obj=%p, slab=%p, cache='%s'\n\0";
static LOG_FROM_FREE_3: &[u8] =
    b"  slab->in_use=%ld, slab->state=%d, slab->cpu_id=%d\n\0";
static LOG_FROM_FREE_4: &[u8] = b"  cache->global_free_count=%ld\n\0";

#[no_mangle]
pub unsafe extern "C" fn xv6_slab_log_free_null(fname: *const c_char) {
    printf(LOG_FREE_NULL.as_ptr() as *const c_char, fname);
}
#[no_mangle]
pub unsafe extern "C" fn xv6_slab_log_no_slab(fname: *const c_char, obj: *mut c_void) {
    printf(LOG_NO_SLAB.as_ptr() as *const c_char, fname, obj);
}
#[no_mangle]
pub unsafe extern "C" fn xv6_slab_log_unattached(slab: *mut slab_t, obj: *mut c_void) {
    printf(LOG_UNATTACHED_1.as_ptr() as *const c_char, slab, obj);
    let cpu_id = AtomicI32::from_ptr(&raw mut (*slab).cpu_id).load(Ordering::Acquire);
    printf(
        LOG_UNATTACHED_2.as_ptr() as *const c_char,
        (*slab).page,
        (*slab).in_use as i64,
        (*slab).state as c_int,
        cpu_id,
    );
}
#[no_mangle]
pub unsafe extern "C" fn xv6_slab_log_from_free(
    obj: *mut c_void,
    slab: *mut slab_t,
    cache: *mut slab_cache_t,
    cpu_id: c_int,
) {
    printf(LOG_FROM_FREE_1.as_ptr() as *const c_char);
    printf(LOG_FROM_FREE_2.as_ptr() as *const c_char, obj, slab, (*cache).name);
    printf(
        LOG_FROM_FREE_3.as_ptr() as *const c_char,
        (*slab).in_use as i64,
        (*slab).state as c_int,
        cpu_id,
    );
    let gfc = AtomicI64::from_ptr(&raw mut (*cache).global_free_count).load(Ordering::Acquire);
    printf(LOG_FROM_FREE_4.as_ptr() as *const c_char, gfc);
}

#[no_mangle]
pub unsafe extern "C" fn xv6_slab_panic(msg: *const c_char) -> ! {
    static PANIC_FMT: &[u8] = b"%s\n\0";
    __panic_start();
    printf(PANIC_FMT.as_ptr() as *const c_char, msg);
    __panic_end()
}

// Page lock pass-throughs (some Rust modules declare these as extern).
#[no_mangle]
pub unsafe extern "C" fn xv6_slab_page_lock_acquire(p: *mut page_struct) {
    page_lock_acquire(p)
}
#[no_mangle]
pub unsafe extern "C" fn xv6_slab_page_lock_release(p: *mut page_struct) {
    page_lock_release(p)
}
