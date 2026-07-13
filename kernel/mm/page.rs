//! Buddy page allocator -- Rust port of `kernel/mm/page.c`.
//!
//! Phase 2: the C-ABI surface is unchanged, but the internal algorithm
//! body is plain safe Rust. Every raw pointer crossing the FFI boundary
//! is converted to a `&mut T` reference (via `NonNull::as_mut()`) at the
//! public entry point and never passed back down as a raw pointer.
//!
//! `unsafe` is reduced to:
//!   * The `extern "C"` block declarations.
//!   * The thin wrappers inside `mod ffi`.
//!   * A handful of clearly-scoped pointer-arithmetic helpers
//!     (`page_at`, `lock_pair`, union accessors) where the invariant
//!     being upheld is documented at the use site.
//!
//! Layout-sensitive structs (`page_t`, `buddy_pool_t`, `pcpu_cache_t`,
//! `spinlock_t`, `list_node_t`) are mirrored here with `#[repr(C)]`; a
//! set of `_Static_assert`s in `kernel/mm/page_shims.c` pin the offsets
//! and sizes at C compile time.

#![allow(non_camel_case_types)]
#![allow(non_upper_case_globals)]
#![allow(non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::offset_of;
use core::ptr::{self, NonNull};
use core::sync::atomic::{AtomicI32, AtomicU32, Ordering, fence};

// ===========================================================================
// Constants (must match the C headers)
// ===========================================================================
pub const NCPU: usize = 8;
pub const PAGE_SHIFT: u32 = 12;
pub const PAGE_SIZE: u64 = 1u64 << PAGE_SHIFT;
pub const PAGE_MASK: u64 = PAGE_SIZE - 1;

pub const PAGE_BUDDY_MAX_ORDER: u64 = 10;
pub const PCPU_CACHE_MAX_ORDER: u64 = 8; // SLAB_DEFAULT_ORDER
pub const PCPU_CACHE_SIZE: u32 = 4;
pub const PCPU_HOT_PAGE_CACHE_SIZE: u32 = 64;

pub const BUDDY_STATE_FREE: u32 = 0;
pub const BUDDY_STATE_INTERMEDIATE: u32 = 1;
pub const BUDDY_STATE_CACHED: u32 = 2;

pub const PAGE_FLAG_TYPE_BITS: u64 = 8;
pub const PAGE_FLAG_TYPE_MASK: u64 = (1u64 << PAGE_FLAG_TYPE_BITS) - 1;
pub const PAGE_FLAG_MASK: u64 = !PAGE_FLAG_TYPE_MASK;

pub const PAGE_TYPE_ANON: u64 = 0;
pub const PAGE_TYPE_BUDDY: u64 = 1;
pub const PAGE_TYPE_SLAB: u64 = 2;
pub const PAGE_TYPE_PGTABLE: u64 = 3;
pub const PAGE_TYPE_PCACHE: u64 = 4;
pub const PAGE_TYPE_TAIL: u64 = 5;
pub const PAGE_TYPE_MAX: u64 = 6;

pub const PAGE_FLAG_LOCKED: u64 = 1u64 << 26;
pub const PAGE_FLAG_IO_PROGRESSING: u64 = 1u64 << 28;

pub const SPINLOCK_BYTES: usize = 24;

// ===========================================================================
// Mirror types -- bit-exact copies of the C structs.
// Verified by `_Static_assert`s in page_shims.c.
//
// `Spinlock` and `ListNode` are now defined once in `crate::mm::cffi`; they
// are re-exported here to preserve the `crate::mm::page::{Spinlock,ListNode}`
// paths that other modules import.
// ===========================================================================

pub use crate::mm::cffi::{ListNode, Spinlock};

#[repr(C, align(64))]
pub struct Page {
    pub lock_bytes: [u8; SPINLOCK_BYTES],   // 0..24
    pub physical_address: u64,               // 24..32
    pub flags: u64,                          // 32..40
    pub ref_count: i32,                      // 40..44
    pub _pad0: u32,                          // 44..48
    pub union_bytes: [u8; 24],               // 48..72 (buddy/slab/tail/pcache)
    pub _pad1: [u8; 56],                     // 72..128
}

#[repr(C, align(64))]
pub struct BuddyPool {
    pub lru_head: ListNode,
    pub count: u64,
    pub _pad0: [u8; 40],
    pub lock_bytes: [u8; SPINLOCK_BYTES],
    pub _pad1: [u8; 40],
}

#[repr(C, align(64))]
pub struct PcpuCache {
    pub lru_head: ListNode,
    pub count: AtomicU32,
    pub _pad0: [u8; 44],
    pub lock_bytes: [u8; SPINLOCK_BYTES],
    pub _pad1: [u8; 40],
}

impl ListNode {
    pub const fn new_uninit() -> Self { Self::new() }
}

impl BuddyPool {
    pub const fn new() -> Self {
        BuddyPool {
            lru_head: ListNode::new_uninit(),
            count: 0,
            _pad0: [0; 40],
            lock_bytes: [0; SPINLOCK_BYTES],
            _pad1: [0; 40],
        }
    }
}

impl PcpuCache {
    pub const fn new() -> Self {
        PcpuCache {
            lru_head: ListNode::new_uninit(),
            count: AtomicU32::new(0),
            _pad0: [0; 44],
            lock_bytes: [0; SPINLOCK_BYTES],
            _pad1: [0; 40],
        }
    }
}

// ---------------------------------------------------------------------------
// Layout pins (compile-time). Formerly `_Static_assert`s in the deleted
// `page_shims.c` / `page_shims.rs`; `Page`/`BuddyPool` are hand-rolled
// mirrors of the C `page_t`/`buddy_pool_t`, so these guards are what
// actually catches ABI drift between this file and the kernel headers.
// ---------------------------------------------------------------------------
const _: () = {
    assert!(core::mem::size_of::<Page>() == 128, "Page size != 128");
    assert!(core::mem::align_of::<Page>() == 64, "Page alignment != 64");
    assert!(core::mem::size_of::<BuddyPool>() == 128, "BuddyPool size != 128");
    assert!(core::mem::align_of::<BuddyPool>() == 64, "BuddyPool alignment != 64");
    assert!(
        core::mem::size_of::<crate::bindings::list_node_t>() == 16,
        "list_node_t size != 16"
    );
    assert!(PAGE_BUDDY_MAX_ORDER == 10, "PAGE_BUDDY_MAX_ORDER must be 10");
};

// ---------------------------------------------------------------------------
// Page accessor helpers. The union arms are stored inside `union_bytes`;
// every read/write below is done through safe array indexing +
// `from_ne_bytes` / `to_ne_bytes`, with no raw-pointer dereferences.
// ---------------------------------------------------------------------------
impl Page {
    #[inline]
    pub fn lock_ptr(&mut self) -> *mut Spinlock {
        ptr::addr_of_mut!(self.lock_bytes) as *mut Spinlock
    }

    #[inline] pub fn page_type(&self) -> u64 { self.flags & PAGE_FLAG_TYPE_MASK }
    #[inline] pub fn is_type(&self, t: u64) -> bool { self.page_type() == t }

    // -- buddy union arm --------------------------------------------------
    // Layout within union_bytes:
    //   buddy.lru_entry: ListNode at offset 0..16
    //   buddy.order:     u32      at offset 16..20
    //   buddy.state:     u32      at offset 20..24
    #[inline]
    pub fn buddy_lru_ptr(&mut self) -> *mut ListNode {
        self.union_bytes.as_mut_ptr() as *mut ListNode
    }

    /// Borrow the buddy-LRU node embedded in `self.union_bytes`. Reborrows
    /// from `&mut self`, so this is sound without an `unsafe` block.
    #[inline]
    pub fn buddy_lru(&mut self) -> &mut ListNode {
        // SAFETY: the union_bytes region is `Page`-owned and exclusively
        // borrowed via `&mut self`; reborrowing it as a `ListNode` reference
        // is a same-provenance reborrow.
        unsafe { &mut *(self.union_bytes.as_mut_ptr() as *mut ListNode) }
    }

    #[inline]
    pub fn buddy_order(&self) -> u32 {
        let b: [u8; 4] = self.union_bytes[16..20].try_into().expect("BUG: fixed-width slice cast");
        u32::from_ne_bytes(b)
    }
    #[inline]
    pub fn set_buddy_order(&mut self, v: u32) {
        self.union_bytes[16..20].copy_from_slice(&v.to_ne_bytes());
    }
    #[inline]
    pub fn buddy_state(&self) -> u32 {
        let b: [u8; 4] = self.union_bytes[20..24].try_into().expect("BUG: fixed-width slice cast");
        u32::from_ne_bytes(b)
    }
    #[inline]
    pub fn set_buddy_state(&mut self, v: u32) {
        self.union_bytes[20..24].copy_from_slice(&v.to_ne_bytes());
    }

    // -- slab union arm ---------------------------------------------------
    // slab.slab*  : *mut c_void at offset 0..8
    // slab.order  : u32         at offset 8..12
    #[inline]
    pub fn slab_slab(&self) -> *mut c_void {
        let b: [u8; 8] = self.union_bytes[0..8].try_into().expect("BUG: fixed-width slice cast");
        usize::from_ne_bytes(b) as *mut c_void
    }
    #[inline]
    pub fn set_slab_slab(&mut self, p: *mut c_void) {
        let v = (p as usize).to_ne_bytes();
        self.union_bytes[0..8].copy_from_slice(&v);
    }
    #[inline]
    pub fn slab_order(&self) -> u32 {
        let b: [u8; 4] = self.union_bytes[8..12].try_into().expect("BUG: fixed-width slice cast");
        u32::from_ne_bytes(b)
    }
    #[inline]
    pub fn set_slab_order(&mut self, v: u32) {
        self.union_bytes[8..12].copy_from_slice(&v.to_ne_bytes());
    }

    // -- tail union arm ---------------------------------------------------
    // tail.head_page : *mut Page at offset 0..8
    #[inline]
    pub fn set_tail_head_page(&mut self, h: *mut Page) {
        let v = (h as usize).to_ne_bytes();
        self.union_bytes[0..8].copy_from_slice(&v);
    }

    // -- pcache union arm --------------------------------------------------
    // pcache.pcache*       : *mut c_void at 0..8
    // pcache.pcache_node*  : *mut c_void at 8..16
    #[inline]
    pub fn pcache_node(&self) -> *mut c_void {
        let b: [u8; 8] = self.union_bytes[8..16].try_into().expect("BUG: fixed-width slice cast");
        usize::from_ne_bytes(b) as *mut c_void
    }
    #[inline]
    pub fn set_pcache_node(&mut self, p: *mut c_void) {
        let v = (p as usize).to_ne_bytes();
        self.union_bytes[8..16].copy_from_slice(&v);
    }
}

impl BuddyPool {
    #[inline]
    pub fn lock_ptr(&mut self) -> *mut Spinlock {
        ptr::addr_of_mut!(self.lock_bytes) as *mut Spinlock
    }
}

impl PcpuCache {
    #[inline]
    pub fn lock_ptr(&mut self) -> *mut Spinlock {
        ptr::addr_of_mut!(self.lock_bytes) as *mut Spinlock
    }
}

// Offset of buddy.lru_entry within page_t. Compile-time constant; used
// to translate between `&mut Page` and `&mut ListNode` linked-list links.
const PAGE_BUDDY_LRU_OFFSET: usize = offset_of!(Page, union_bytes);

#[inline]
#[allow(dead_code)]
fn page_to_lru(p: &mut Page) -> *mut ListNode {
    (p as *mut Page as usize + PAGE_BUDDY_LRU_OFFSET) as *mut ListNode
}

#[inline]
fn lru_node_to_page(n: *mut ListNode) -> Option<&'static mut Page> {
    if n.is_null() { return None; }
    let p = (n as usize - PAGE_BUDDY_LRU_OFFSET) as *mut Page;
    // SAFETY: the LRU links always point inside Page::union_bytes, so
    // subtracting the offset yields a valid `&mut Page` borrowed from the
    // global `__pages` array (lifetime is fictitious `'static`).
    unsafe { Some(&mut *p) }
}

// ===========================================================================
// FFI surface.
//
// All raw extern declarations are wrapped in `unsafe extern "C" { pub safe }`
// (Rust 1.82+), which marks each declared symbol as callable from safe code.
// The thin adapter wrappers below convert between C-style scalars/pointers
// and idiomatic Rust types (e.g. `c_int -> bool`, `&ListNode -> *const`).
// They no longer need `{ ... }` blocks since the underlying
// declarations are now `safe`.
// ===========================================================================
mod ffi {
    use super::*;

    // Shared kernel primitives (spinlock, per-cpu, list, kmm, memset,
    // panic) are declared once in `crate::mm::cffi`; re-export them here so
    // the existing `ffi::spin_lock(...)` call sites compile unchanged.
    pub use crate::mm::cffi::raw::{
        spin_init, spin_lock, spin_unlock, spin_holding,
        xv6_cpuid, xv6_push_off, xv6_pop_off,
        xv6_list_init, xv6_list_is_empty, xv6_list_is_detached,
        xv6_list_detach, xv6_list_push_front, xv6_list_pop_front,
        memset, __panic_start, __panic_end,
    };

    unsafe extern "C" {
        // Early allocator
        pub safe fn early_alloc_align(size: usize, align: usize) -> *mut c_void;
        pub safe fn early_alloc_end_ptr() -> *mut c_void;

        // Slab interaction (for PCACHE refcount-zero teardown)
        pub safe fn slab_free(obj: *mut c_void);

        // Runtime physical-memory globals (provided by linker / startup)
        pub safe static __physical_memory_start: u64;
        pub safe static __physical_memory_end: u64;
        pub safe static __physical_total_pages: u64;

        // argint() -- syscall argument fetch (kernel/inc/defs.h), needed by
        // `sys_memstat` below.
        pub safe fn argint(n: c_int, ip: *mut c_int);
    }

    // --- Type-adapting wrappers (safe Rust, no unsafe blocks). -----------
    // Renamed to avoid shadowing the extern; `spin_holding_b` returns bool.
    #[inline] pub fn spin_holding_b(l: *mut Spinlock) -> bool { spin_holding(l) != 0 }
    #[inline] pub fn cpuid() -> c_int { xv6_cpuid() }
    #[inline] pub fn push_off() { xv6_push_off() }
    #[inline] pub fn pop_off() { xv6_pop_off() }

    #[inline] pub fn list_init(e: &mut ListNode) { xv6_list_init(e) }
    #[inline] pub fn list_is_empty(h: &ListNode) -> bool { xv6_list_is_empty(h) != 0 }
    #[inline] pub fn list_is_detached(e: *const ListNode) -> bool { xv6_list_is_detached(e) != 0 }
    #[inline] pub fn list_detach(e: *mut ListNode) { xv6_list_detach(e) }
    #[inline] pub fn list_push_front(h: &mut ListNode, e: *mut ListNode) { xv6_list_push_front(h, e) }
    #[inline] pub fn list_pop_front(h: &mut ListNode) -> *mut ListNode { xv6_list_pop_front(h) }

    #[inline] pub fn kernbase() -> u64 { __physical_memory_start }
    #[inline] pub fn phystop() -> u64 { __physical_memory_end }
    #[inline] pub fn totalpages() -> u64 { __physical_total_pages }

    // --- Platform info -- reads the bindgen `platform` global directly.
    // Previously ten `xv6_platform_*` round-trips through the deleted
    // page_shims.rs; page.rs is the only caller, so there is no reason to
    // cross the C-ABI boundary for a plain field read. ---------------------
    #[inline]
    pub fn has_ramdisk() -> bool {
        // SAFETY: `platform` is populated once by `fdt_init()` before any mm
        // init code runs; every reader here executes afterward on the
        // single boot CPU, so this plain field read is race-free.
        unsafe { crate::bindings::platform.has_ramdisk != 0 }
    }
    #[inline] pub fn ramdisk_base() -> u64 { unsafe { crate::bindings::platform.ramdisk_base } }
    #[inline] pub fn ramdisk_size() -> u64 { unsafe { crate::bindings::platform.ramdisk_size } }
    #[inline] pub fn reserved_count() -> i32 { unsafe { crate::bindings::platform.reserved_count } }
    #[inline]
    pub fn reserved_base(i: i32) -> u64 {
        // SAFETY: bounds-checked against `platform.reserved_count` before
        // indexing into the `platform.reserved` array.
        unsafe {
            if i < 0 || i >= crate::bindings::platform.reserved_count { return 0; }
            (*crate::bindings::platform.reserved.offset(i as isize)).base
        }
    }
    #[inline]
    pub fn reserved_size(i: i32) -> u64 {
        // SAFETY: see `reserved_base`.
        unsafe {
            if i < 0 || i >= crate::bindings::platform.reserved_count { return 0; }
            (*crate::bindings::platform.reserved.offset(i as isize)).size
        }
    }

    // --- Panic / diagnostic printf -- previously the `xv6_buddy_panic` and
    // ten `xv6_buddy_log_*` round-trips through the deleted page_shims.rs;
    // now direct `printf` calls guarded by a single-purpose format string
    // each. `printf` is variadic so it cannot be declared `safe`. ---------
    #[inline]
    pub fn panic(msg: &'static [u8]) -> ! {
        __panic_start();
        // SAFETY: `msg` is always a NUL-terminated byte-string literal
        // supplied by call sites in this module.
        unsafe { super::printf(b"%s\n\0".as_ptr() as *const c_char, msg.as_ptr()); }
        __panic_end()
    }

    #[inline] pub fn log_init_range(s: u64, e: u64, f: u64) {
        static FMT: &[u8] = b"init pages from 0x%lx to 0x%lx with flags 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, s, e, f); }
    }
    #[inline] pub fn log_init_invalid_range(s: u64, e: u64) {
        static FMT: &[u8] = b"invalid range, pa_start: 0x%lx, pa_end: 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, s, e); }
    }
    #[inline] pub fn log_init_invalid_base(s: u64, e: u64) {
        static FMT: &[u8] = b"invalid range base, pa_start: 0x%lx, pa_end: 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, s, e); }
    }
    #[inline] pub fn log_init_invalid_flags(f: u64) {
        static FMT: &[u8] = b"invalid flags: 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, f); }
    }
    #[inline] pub fn log_init_get_page(pa: u64) {
        static FMT: &[u8] = b"failed to get page for physical address 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, pa); }
    }
    #[inline] pub fn log_reserved_out_of_range(s: u64, e: u64) {
        static FMT: &[u8] = b"reserved mem out of range: 0x%lx to 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, s, e); }
    }
    #[inline] pub fn log_reserving(s: u64, e: u64) {
        static FMT: &[u8] = b"reserving pages from 0x%lx to 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, s, e); }
    }
    #[inline] pub fn log_init_summary(p: *mut c_void, sz: usize) {
        static FMT: &[u8] = b"page_buddy_init(): page array at 0x%lx, size 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, p as u64, sz as u64); }
    }
    #[inline] pub fn log_init_range_phys(s: u64, e: u64) {
        static FMT: &[u8] = b"__managed_start: 0x%lx, __managed_end: 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, s, e); }
    }
    #[inline] pub fn log_buddy_range(s: u64, e: u64) {
        static FMT: &[u8] = b"buddy init range: 0x%lx - 0x%lx\n\0";
        unsafe { super::printf(FMT.as_ptr() as *const c_char, s, e); }
    }
    #[inline] pub fn print_stat(detailed: c_int) { super::print_buddy_system_stat(detailed) }
}

// `printf` is shared by every diagnostic-printing helper in this module
// (the `ffi::log_*`/`ffi::panic` wrappers above, plus the statistics
// printer and integrity checker below). Declared once here instead of
// once per call site.
unsafe extern "C" {
    fn printf(fmt: *const c_char, ...) -> c_int;
}

// ===========================================================================
// Global state (kept with C-compatible names so the existing C shims and
// any leftover references still link).
// ===========================================================================
#[no_mangle]
pub static mut __buddy_pools: [BuddyPool; (PAGE_BUDDY_MAX_ORDER + 1) as usize] =
    [const { BuddyPool::new() }; (PAGE_BUDDY_MAX_ORDER + 1) as usize];

#[no_mangle]
pub static mut __pcpu_caches: [[PcpuCache; (PCPU_CACHE_MAX_ORDER + 1) as usize]; NCPU] =
    [const {
        [const { PcpuCache::new() }; (PCPU_CACHE_MAX_ORDER + 1) as usize]
    }; NCPU];

#[no_mangle]
pub static mut __pages: *mut Page = ptr::null_mut();
#[no_mangle]
pub static mut __managed_start: u64 = 0;
#[no_mangle]
pub static mut __managed_end: u64 = 0;

static BUDDY_POOL_NAMES: [&[u8]; 11] = [
    b"buddy_pool_0\0",  b"buddy_pool_1\0",  b"buddy_pool_2\0",  b"buddy_pool_3\0",
    b"buddy_pool_4\0",  b"buddy_pool_5\0",  b"buddy_pool_6\0",  b"buddy_pool_7\0",
    b"buddy_pool_8\0",  b"buddy_pool_9\0",  b"buddy_pool_10\0",
];
static PAGE_LOCK_NAME: &[u8] = b"page_t\0";
static PCPU_CACHE_NAME: &[u8] = b"pcpu_cache\0";

// ===========================================================================
// Global-state accessors. These are the only readers of the `static mut`
// globals; the rest of the algorithm body deals exclusively with `&mut`
// references handed back by these helpers.
// ===========================================================================

#[inline]
fn pages_base() -> *mut Page {
    // SAFETY: addr-of read of a `static mut` (no `&`/`&mut` aliasing of the
    // global itself); the pointer it holds is set once during init and
    // immutable thereafter.
    unsafe { *(&raw const __pages) }
}

/// Borrow the buddy pool for `order`. Returns `&mut` because each pool
/// has its own spinlock; callers are responsible for locking before
/// mutating list state.
#[inline]
fn buddy_pool(order: u64) -> &'static mut BuddyPool {
    debug_assert!(order <= PAGE_BUDDY_MAX_ORDER);
    // SAFETY: __buddy_pools is initialized once at boot and lives for the
    // entire kernel lifetime; per-element synchronization is the caller's
    // job (via the embedded spinlock). We go through a raw pointer to avoid
    // creating a `&mut` to the static itself.
    unsafe { &mut (*(&raw mut __buddy_pools))[order as usize] }
}

#[inline]
fn pcpu_cache(cpu: usize, order: u64) -> &'static mut PcpuCache {
    debug_assert!(cpu < NCPU);
    debug_assert!(order <= PCPU_CACHE_MAX_ORDER);
    // SAFETY: see `buddy_pool`.
    unsafe { &mut (*(&raw mut __pcpu_caches))[cpu][order as usize] }
}

/// Borrow the `Page` at index `i` from the global page array.
#[inline]
fn page_at(i: isize) -> &'static mut Page {
    // SAFETY: `__pages` was sized to `__physical_total_pages` at init.
    // Callers (all internal) validate `i` against the valid range.
    unsafe { &mut *pages_base().offset(i) }
}

/// Borrow the page following `p` by `delta` slots in the global array.
///
/// The pages live in a contiguous `[Page]`. Returning two `&mut Page`
/// borrows that may overlap aliasing would be UB, so we only ever call
/// this *after* releasing the first borrow.
#[inline]
fn page_offset_from(p: &Page, delta: usize) -> &'static mut Page {
    let base = pages_base();
    // Translate `&Page` -> index in the global array.
    let idx = ((p as *const Page as usize) - (base as usize))
              / core::mem::size_of::<Page>();
    page_at((idx + delta) as isize)
}

// ===========================================================================
// Pure-data helpers
// ===========================================================================
#[inline] fn page_buddy_bytes(order: u64) -> u64 { 1u64 << (order + PAGE_SHIFT as u64) }
#[inline] fn page_buddy_offset_mask(order: u64) -> u64 { page_buddy_bytes(order) - 1 }
#[inline] fn page_buddy_base_mask(order: u64) -> u64 { !page_buddy_offset_mask(order) }
#[inline] fn buddy_group_addr(physical: u64, order: u32) -> u64 {
    physical & page_buddy_base_mask(order as u64)
}

#[inline]
fn managed_start() -> u64 {
    // SAFETY: addr-of read; value is set once during `page_buddy_init`.
    unsafe { *(&raw const __managed_start) }
}

#[inline]
fn managed_end() -> u64 {
    // SAFETY: addr-of read; value is set once during `page_buddy_init`.
    unsafe { *(&raw const __managed_end) }
}

#[inline]
fn ptr_addr_in_managed(addr: u64) -> bool {
    addr >= ffi::kernbase() && addr < managed_end()
}

#[inline]
fn page_base_validity(physical: u64) -> bool {
    (physical & PAGE_MASK) == 0 && ptr_addr_in_managed(physical)
}

#[inline]
fn page_init_flags_validity(flags: u64) -> bool {
    (flags & !PAGE_FLAG_LOCKED) == 0
}

#[inline]
fn page_flags_validity(flags: u64) -> bool {
    let t = flags & PAGE_FLAG_TYPE_MASK;
    t < PAGE_TYPE_MAX && (flags & PAGE_FLAG_MASK) == 0
}

#[inline]
fn page_type_supports_tail(flags: u64) -> bool {
    let t = flags & PAGE_FLAG_TYPE_MASK;
    t == PAGE_TYPE_BUDDY || t == PAGE_TYPE_SLAB
}

#[inline]
fn pgrounddown(x: u64) -> u64 { x & !(PAGE_SIZE - 1) }
#[inline]
fn pgroundup(x: u64) -> u64 { (x + PAGE_SIZE - 1) & !(PAGE_SIZE - 1) }

// ===========================================================================
// Per-page predicates / utilities operating on safe references.
// Logic lives on `impl Page` so callers read as `page.is_freeable()` etc.
// ===========================================================================

impl Page {
    /// A page is freeable iff it is not LOCKED and its refcount is <= 1.
    #[inline]
    pub fn is_freeable(&self) -> bool {
        (self.flags & PAGE_FLAG_LOCKED) == 0 && self.ref_count <= 1
    }

    pub fn has_valid_tail_structure(&self, order: u64) -> bool {
        match self.page_type() {
            PAGE_TYPE_BUDDY => self.buddy_order() as u64 == order,
            PAGE_TYPE_SLAB  => self.slab_order() as u64 == order,
            _ => false,
        }
    }

    #[inline]
    pub fn is_buddy_group_head(&self) -> bool {
        self.is_type(PAGE_TYPE_BUDDY)
    }

    pub fn is_buddy_of(&self, other: &Page) -> bool {
        if self.physical_address == other.physical_address { return false; }
        if self.buddy_order() != other.buddy_order() { return false; }
        if (self.buddy_order() as u64) >= PAGE_BUDDY_MAX_ORDER { return false; }
        (self.physical_address ^ page_buddy_bytes(self.buddy_order() as u64))
            == other.physical_address
    }
}

// ===========================================================================
// Locking helpers
//
// All concrete `spin_lock` / `spin_unlock` calls go through the safe
// RAII handle `crate::sync::KSpinlock`. These helpers keep their old
// names and signatures so existing call sites (which acquire and
// release a lock in different statements) continue to compile, but
// they no longer touch the raw FFI in their bodies.
// ===========================================================================
use crate::sync::KSpinlock;

fn buddy_pool_lock(order: u64) {
    if order > PAGE_BUDDY_MAX_ORDER {
        ffi::panic(b"__buddy_pool_lock: invalid order\0");
    }
    // Acquire and immediately `mem::forget` the guard: the matching
    // `buddy_pool_unlock` will release. This is the documented bridge
    // between RAII and the legacy lock/unlock-on-separate-lines style
    // that some buddy-allocator loops require.
    core::mem::forget(KSpinlock::from_ptr(buddy_pool(order).lock_ptr()).lock());
}
fn buddy_pool_unlock(order: u64) {
    if order > PAGE_BUDDY_MAX_ORDER {
        ffi::panic(b"__buddy_pool_unlock: invalid order\0");
    }
    ffi::spin_unlock(buddy_pool(order).lock_ptr());
}
fn buddy_pool_lock_range(s: u64, e: u64) {
    if s > e || e > PAGE_BUDDY_MAX_ORDER {
        ffi::panic(b"__buddy_pool_lock_range: invalid order range\0");
    }
    for i in s..=e {
        core::mem::forget(KSpinlock::from_ptr(buddy_pool(i).lock_ptr()).lock());
    }
}
fn buddy_pool_unlock_range(s: u64, e: u64) {
    if s > e || e > PAGE_BUDDY_MAX_ORDER {
        ffi::panic(b"__buddy_pool_unlock_range: invalid order range\0");
    }
    for i in (s..=e).rev() {
        ffi::spin_unlock(buddy_pool(i).lock_ptr());
    }
}

/// Lock two pages in pointer-address order (or just one if equal). The
/// pages are borrowed through `page_at`; we cannot hold both `&mut`
/// borrows simultaneously because they alias the global array, so we
/// obtain raw lock handles up-front and delegate to
/// [`KSpinlock::lock_two`] for the canonical low-→high ordering.
///
/// We `mem::forget` the guard because the matching `unlock_pair`
/// releases at a later statement (and possibly in a different
/// function); the C original has the same lifecycle. RAII single-
/// scope use of `KSpinlock::lock_two` is available directly when the
/// caller's structure allows it.
fn lock_pair(p1: &mut Page, p2: &mut Page) {
    let h1 = KSpinlock::from_ptr(p1.lock_ptr());
    let h2 = KSpinlock::from_ptr(p2.lock_ptr());
    core::mem::forget(KSpinlock::lock_two(h1, h2));
}
fn unlock_pair(p1: &mut Page, p2: &mut Page) {
    let a = p1 as *mut Page;
    let b = p2 as *mut Page;
    if a < b {
        ffi::spin_unlock(p2.lock_ptr());
        ffi::spin_unlock(p1.lock_ptr());
    } else if a > b {
        ffi::spin_unlock(p1.lock_ptr());
        ffi::spin_unlock(p2.lock_ptr());
    } else {
        ffi::spin_unlock(p1.lock_ptr());
    }
}

// ===========================================================================
// Page initialization / state transitions -- all methods on `impl Page`.
// ===========================================================================
impl Page {
    pub fn init(&mut self, physical: u64, ref_count: i32, flags: u64) {
        ffi::memset(self as *mut Page as *mut c_void, 0, core::mem::size_of::<Page>());
        self.physical_address = physical;
        self.flags = flags;
        self.ref_count = ref_count;
        KSpinlock::from_ptr(self.lock_ptr())
            .init(PAGE_LOCK_NAME.as_ptr() as *const c_char);
    }

    /// Reinitialize a page, preserving the spinlock.
    pub fn reinit(&mut self, physical: u64, ref_count: i32, flags: u64) {
        self.physical_address = physical;
        self.flags = flags;
        self.ref_count = ref_count;
        self.set_slab_slab(ptr::null_mut());
        self.set_slab_order(0);
    }

    pub fn as_buddy_tail(&mut self, head: *mut Page) {
        self.flags = PAGE_TYPE_TAIL;
        self.ref_count = 0;
        self.set_tail_head_page(head);
    }

    pub fn as_buddy_header(&mut self, order: u64, state: u32) {
        self.flags = PAGE_TYPE_BUDDY;
        self.ref_count = 0;
        self.set_buddy_order(order as u32);
        self.set_buddy_state(state);
    }

    /// Mark `self` as a buddy group header, then walk every tail page and
    /// flag it as `PAGE_TYPE_TAIL`. Tail pages are reached via separate
    /// `page_at()` borrows because we cannot hold overlapping `&mut Page`s.
    pub fn as_buddy_group(&mut self, order: u64, state: u32) {
        let head_ptr = self as *mut Page;
        self.flags = PAGE_TYPE_BUDDY;
        self.ref_count = 0;
        self.set_buddy_order(order as u32);
        self.set_buddy_state(state);
        ffi::list_init(self.buddy_lru());
        let count = 1u64 << order;
        for i in 1..count {
            let tail = page_offset_from(self, i as usize);
            tail.as_buddy_tail(head_ptr);
        }
    }

    pub fn as_buddy_group_preserve_tails(&mut self, order: u64, state: u32) {
        self.flags = PAGE_TYPE_BUDDY;
        self.ref_count = 0;
        self.set_buddy_order(order as u32);
        self.set_buddy_state(state);
        ffi::list_init(self.buddy_lru());
    }

    pub fn as_buddy_group_init(&mut self, order: u64, state: u32) {
        let base_pa = self.physical_address;
        self.init(base_pa, 0, PAGE_TYPE_BUDDY);
        self.set_buddy_order(order as u32);
        self.set_buddy_state(state);
        ffi::list_init(self.buddy_lru());

        let head_ptr = self as *mut Page;
        let count = 1u64 << order;
        for i in 1..count {
            let tp = page_offset_from(self, i as usize);
            KSpinlock::from_ptr(tp.lock_ptr())
                .init(PAGE_LOCK_NAME.as_ptr() as *const c_char);
            tp.physical_address = base_pa + (i << PAGE_SHIFT);
            tp.as_buddy_tail(head_ptr);
        }
    }
}

fn buddy_pool_init() {
    for i in 0..=(PAGE_BUDDY_MAX_ORDER as usize) {
        let pool = buddy_pool(i as u64);
        pool.count = 0;
        ffi::list_init(&mut pool.lru_head);
        KSpinlock::from_ptr(pool.lock_ptr())
            .init(BUDDY_POOL_NAMES[i].as_ptr() as *const c_char);
    }
    for cpu in 0..NCPU {
        for order in 0..=(PCPU_CACHE_MAX_ORDER as usize) {
            let cache = pcpu_cache(cpu, order as u64);
            ffi::list_init(&mut cache.lru_head);
            cache.count.store(0, Ordering::Release);
            KSpinlock::from_ptr(cache.lock_ptr())
                .init(PCPU_CACHE_NAME.as_ptr() as *const c_char);
        }
    }
}

fn init_range_flags(pa_start: u64, pa_end: u64, flags: u64) -> i32 {
    if pa_start >= pa_end {
        ffi::log_init_invalid_range(pa_start, pa_end);
        return -1;
    }
    if !page_base_validity(pa_start) || !page_base_validity(pa_end - PAGE_SIZE) {
        ffi::log_init_invalid_base(pa_start, pa_end);
        return -1;
    }
    if !page_init_flags_validity(flags) {
        ffi::log_init_invalid_flags(flags);
        return -1;
    }
    ffi::log_init_range(pa_start, pa_end, flags);
    let mut base = pa_start;
    while base < pa_end {
        match pa_to_page(base) {
            Some(p) => p.init(base, 0, flags),
            None => {
                ffi::log_init_get_page(base);
                return -1;
            }
        }
        base += PAGE_SIZE;
    }
    0
}

// ===========================================================================
// Buddy pool list ops -- methods on `impl BuddyPool`.
// ===========================================================================
impl BuddyPool {
    pub fn push_page(&mut self, page: &mut Page) {
        if ffi::list_is_empty(&self.lru_head) {
            if self.count != 0 { ffi::panic(b"__buddy_push_page\0"); }
        } else if self.count == 0 {
            ffi::panic(b"__buddy_push_page\0");
        }
        let entry = page.buddy_lru_ptr();
        ffi::list_push_front(&mut self.lru_head, entry);
        self.count += 1;
        // SeqCst: publishes the plain (non-atomic) list-node/count update
        // to unlocked readers (e.g. `xv6_buddy_pool_count`,
        // `page_buddy_stat`) that peek at pool state without taking the
        // pool spinlock; kept conservative rather than proven-sufficient
        // Release, since those readers use plain loads with no matching
        // Acquire.
        fence(Ordering::SeqCst);
    }

    pub fn pop_page(&mut self) -> Option<&'static mut Page> {
        let node = ffi::list_pop_front(&mut self.lru_head);
        if node.is_null() {
            if self.count > 0 { ffi::panic(b"__buddy_pop_page\0"); }
            return None;
        }
        self.count -= 1;
        // SeqCst: same rationale as `push_page` — publishes the count/list
        // update to unlocked peekers before the popped page is handed
        // back to the caller for reuse.
        fence(Ordering::SeqCst);
        lru_node_to_page(node)
    }

    pub fn detach_page(&mut self, page: &mut Page) {
        if ffi::list_is_empty(&self.lru_head) {
            ffi::panic(b"__buddy_detach_page\0");
        }
        self.count -= 1;
        ffi::list_detach(page.buddy_lru_ptr());
    }
}

// ===========================================================================
// Buddy finding / state management.
// ===========================================================================
fn get_buddy_addr(physical: u64, order: u32) -> u64 {
    let base = buddy_group_addr(physical, order);
    base ^ page_buddy_bytes(order as u64)
}

impl Page {
    /// Lock `self` and its buddy (when valid and free). Returns the buddy
    /// header on success (with both pages locked); returns `None` and
    /// leaves both pages unlocked otherwise.
    pub fn lock_get_buddy(&mut self) -> Option<&'static mut Page> {
        if !self.is_buddy_group_head() { return None; }
        let order = self.buddy_order();
        if (order as u64) >= PAGE_BUDDY_MAX_ORDER { return None; }
        let buddy_base = get_buddy_addr(self.physical_address, order);
        let buddy_head = pa_to_page(buddy_base)?;

        lock_pair(self, buddy_head);
        let still_valid =
            buddy_head.is_buddy_group_head() &&
            buddy_head.buddy_order() == order &&
            !ffi::list_is_detached(buddy_head.buddy_lru_ptr()) &&
            buddy_head.buddy_state() == BUDDY_STATE_FREE;
        if !still_valid {
            unlock_pair(self, buddy_head);
            return None;
        }
        buddy_head.set_buddy_state(BUDDY_STATE_INTERMEDIATE);
        Some(buddy_head)
    }

    pub fn order_change_commit(&mut self) {
        if !self.is_buddy_group_head() {
            ffi::panic(b"__page_order_change_commit\0");
        }
        self.set_buddy_state(BUDDY_STATE_FREE);
        ffi::list_init(self.buddy_lru());
    }

    pub fn split_commit_later_half(&mut self, order: u64) {
        let header_ptr = self as *mut Page;
        self.set_buddy_state(BUDDY_STATE_FREE);
        ffi::list_init(self.buddy_lru());
        let count = 1u64 << order;
        for i in 1..count {
            let tail = page_offset_from(self, i as usize);
            tail.as_buddy_tail(header_ptr);
        }
    }

    pub fn merge_commit_later_half(&mut self, merged_order: u32) {
        let merged_ptr = self as *mut Page;
        let half_count = 1u64 << (merged_order as u64 - 1);
        for i in 0..half_count {
            let tail = page_offset_from(self, (half_count + i) as usize);
            tail.as_buddy_tail(merged_ptr);
        }
    }
}

// ===========================================================================
// Splitting / merging.
// ===========================================================================
impl Page {
    /// Split `self` into two halves; returns a reference to the later half.
    pub fn buddy_split(&mut self) -> Option<&'static mut Page> {
        if !self.is_buddy_group_head() { return None; }
        let order = self.buddy_order();
        if order == 0 { return None; }
        let order_after = order - 1;
        let half = 1usize << order_after as usize;
        self.set_buddy_order(order_after);
        let buddy = page_offset_from(self, half);
        buddy.as_buddy_header(order_after as u64, BUDDY_STATE_INTERMEDIATE);
        // SeqCst: publishes the new buddy header's state/order fields
        // before the caller (typically `buddy_get`, still holding the
        // pool lock) inserts it back into a pool list that unlocked
        // readers may peek at.
        fence(Ordering::SeqCst);
        Some(buddy)
    }

    /// Merge two buddy pages, returning the header (lower physical address).
    pub fn merge_with<'a>(p1: &'a mut Page, p2: &'a mut Page) -> Option<&'a mut Page> {
        if !p1.is_buddy_of(p2) { return None; }
        let order_after = p1.buddy_order() + 1;
        let header = if p1.physical_address < p2.physical_address { p1 } else { p2 };
        header.set_buddy_order(order_after);
        Some(header)
    }
}

// ===========================================================================
// Per-CPU page cache
// ===========================================================================
fn pcpu_cache_get(order: u64, flags: u64) -> Option<&'static mut Page> {
    if order > PCPU_CACHE_MAX_ORDER { return None; }
    let cpu = ffi::cpuid() as usize;
    let cache = pcpu_cache(cpu, order);
    let popped: Option<&mut Page>;

    let pop = |cache: &mut PcpuCache| -> Option<&'static mut Page> {
        if ffi::list_is_empty(&cache.lru_head) { return None; }
        let node = ffi::list_pop_front(&mut cache.lru_head);
        let p = lru_node_to_page(node)?;
        p.set_buddy_state(BUDDY_STATE_INTERMEDIATE);
        let old = cache.count.fetch_sub(1, Ordering::Release);
        if old == 0 { ffi::panic(b"PCPU cache counter underflow\0"); }
        Some(p)
    };

    if order == 0 {
        let _preempt = crate::machine::PreemptGuard::new();
        popped = pop(cache);
    } else {
        let _g = KSpinlock::from_ptr(cache.lock_ptr()).lock();
        popped = pop(cache);
    }

    let page = popped?;
    let page_ptr = page as *mut Page;
    let pa = page.physical_address;
    if page_type_supports_tail(flags) {
        page.reinit(pa, 1, flags);
    } else {
        let count = 1u64 << order;
        page.reinit(pa, 1, flags);
        // SAFETY: page_at() returns a fresh borrow disjoint from `page`.
        // We release `page` (only used for the head reinit above) before
        // touching tails.
        let _ = page_ptr; // silence dead-code in non-debug builds
        for i in 1..count {
            let base = pages_base();
            let head_idx = ((page_ptr as usize) - (base as usize))
                          / core::mem::size_of::<Page>();
            let tp = page_at((head_idx + i as usize) as isize);
            let tp_pa = tp.physical_address;
            tp.reinit(tp_pa, 1, flags);
        }
    }
    // SAFETY: re-borrow the head we just reinitialized; the previous
    // borrow ended at the loop above.
    Some(unsafe { &mut *page_ptr })
}

fn pcpu_cache_put(page: &mut Page, order: u64) -> i32 {
    if order > PCPU_CACHE_MAX_ORDER { return -1; }
    let cpu = ffi::cpuid() as usize;
    let cache = pcpu_cache(cpu, order);
    let cache_limit = if order == 0 { PCPU_HOT_PAGE_CACHE_SIZE } else { PCPU_CACHE_SIZE };
    let preserve = page.has_valid_tail_structure(order);
    let mut ret: i32 = -1;

    if order == 0 {
        let _preempt = crate::machine::PreemptGuard::new();
        let cur = cache.count.load(Ordering::Acquire);
        if cur < cache_limit {
            let _g = KSpinlock::from_ptr(page.lock_ptr()).lock();
            if preserve {
                page.as_buddy_group_preserve_tails(order, BUDDY_STATE_CACHED);
            } else {
                page.as_buddy_group(order, BUDDY_STATE_CACHED);
            }
            ffi::list_push_front(&mut cache.lru_head, page.buddy_lru_ptr());
            let old = cache.count.fetch_add(1, Ordering::Release);
            if old == u32::MAX { ffi::panic(b"PCPU cache counter overflow\0"); }
            ret = 0;
        }
    } else {
        let _gc = KSpinlock::from_ptr(cache.lock_ptr()).lock();
        let cur = cache.count.load(Ordering::Acquire);
        if cur < cache_limit {
            let _gp = KSpinlock::from_ptr(page.lock_ptr()).lock();
            if preserve {
                page.as_buddy_group_preserve_tails(order, BUDDY_STATE_CACHED);
            } else {
                page.as_buddy_group(order, BUDDY_STATE_CACHED);
            }
            ffi::list_push_front(&mut cache.lru_head, page.buddy_lru_ptr());
            let old = cache.count.fetch_add(1, Ordering::Release);
            if old == u32::MAX { ffi::panic(b"PCPU cache counter overflow\0"); }
            ret = 0;
        }
    }
    ret
}

// ===========================================================================
// Buddy allocation core
// ===========================================================================
fn buddy_get(order: u64, flags: u64) -> Option<&'static mut Page> {
    if !page_flags_validity(flags) { return None; }
    if order > PAGE_BUDDY_MAX_ORDER { return None; }

    if order <= PCPU_CACHE_MAX_ORDER {
        if let Some(p) = pcpu_cache_get(order, flags) {
            return Some(p);
        }
    }

    // Walk the free lists from `order` upward looking for any non-empty pool.
    let mut found: Option<(&mut Page, u64)> = None;
    for tmp in order..=PAGE_BUDDY_MAX_ORDER {
        buddy_pool_lock(tmp);
        let popped = buddy_pool(tmp).pop_page();
        if let Some(p) = popped {
            p.set_buddy_state(BUDDY_STATE_INTERMEDIATE);
            buddy_pool_unlock(tmp);
            found = Some((p, tmp));
            break;
        }
        buddy_pool_unlock(tmp);
    }
    let (page, found_order) = found?;

    // Walk back down: each level keep the lower half, push the upper half
    // onto the corresponding buddy pool.
    let mut tmp_order = found_order;
    while tmp_order > order {
        let upper = page.buddy_split().unwrap_or_else(
            || ffi::panic(b"__buddy_get(): failed splitting buddy pages\0"));
        tmp_order -= 1;
        buddy_pool_lock(tmp_order);
        upper.split_commit_later_half(tmp_order);
        buddy_pool(tmp_order).push_page(upper);
        buddy_pool_unlock(tmp_order);
    }

    let page_ptr = page as *mut Page;
    let pa = page.physical_address;
    if page_type_supports_tail(flags) {
        page.reinit(pa, 1, flags);
    } else {
        let count = 1u64 << order;
        page.reinit(pa, 1, flags);
        for i in 1..count {
            let base = pages_base();
            let idx = ((page_ptr as usize) - (base as usize))
                      / core::mem::size_of::<Page>();
            let tp = page_at((idx + i as usize) as isize);
            let tp_pa = tp.physical_address;
            tp.reinit(tp_pa, 1, flags);
        }
    }
    // SAFETY: re-borrow the head after the tail loop ended the prior borrow.
    Some(unsafe { &mut *page_ptr })
}

// ===========================================================================
// Buddy deallocation
// ===========================================================================
fn buddy_merge_and_insert(start_page: &mut Page, start_order: u64) {
    let mut page: &mut Page = start_page;
    let mut tmp_order = start_order;
    while tmp_order <= PAGE_BUDDY_MAX_ORDER {
        buddy_pool_lock(tmp_order);
        match page.lock_get_buddy() {
            Some(buddy) => {
                buddy_pool(tmp_order).detach_page(buddy);
                buddy_pool_unlock(tmp_order);
                let merged_order = page.buddy_order() + 1;
                let merged = Page::merge_with(page, buddy).unwrap_or_else(
                    || ffi::panic(b"__buddy_merge_and_insert(): failed to merge buddies\0"));
                let merged_ptr = merged as *mut Page;
                merged.merge_commit_later_half(merged_order);
                // Re-fetch both pages to unlock them.
                let base = pages_base();
                let merged_idx = ((merged_ptr as usize) - (base as usize))
                                / core::mem::size_of::<Page>();
                let merged_again = page_at(merged_idx as isize);
                // Other half is `merged_idx ^ half_count`; cheaper to use
                // physical_address xor: buddy's address.
                // The two pages we want to unlock are exactly the originals.
                // We saved the original pointer in `page` (still live as
                // `*mut Page` via raw before re-borrow). Avoid double borrow:
                // unlock by raw lock pointers we previously locked.
                // Easiest: lock_get_buddy_page locked both; unlock_pair needs
                // two &mut. We can synthesize them from raw via page_at:
                let merged_pa = merged_again.physical_address;
                let other_pa = merged_pa ^ page_buddy_bytes(merged_order as u64 - 1);
                let other = pa_to_page(other_pa)
                    .expect("buddy_merge_and_insert: other half missing");
                // `merged_again` and `other` are distinct pages -> two
                // disjoint borrows from the global array; safe.
                unlock_pair(merged_again, other);
                page = page_at(merged_idx as isize);
            }
            None => {
                {
                    let _g = KSpinlock::from_ptr(page.lock_ptr()).lock();
                    page.order_change_commit();
                    buddy_pool(tmp_order).push_page(page);
                }
                buddy_pool_unlock(tmp_order);
                return;
            }
        }
        tmp_order += 1;
    }
}

fn buddy_put(page: &mut Page) -> i32 {
    if !page.is_freeable() { return -1; }
    if pcpu_cache_put(page, 0) == 0 { return 0; }
    {
        let _g = KSpinlock::from_ptr(page.lock_ptr()).lock();
        page.as_buddy_header(0, BUDDY_STATE_INTERMEDIATE);
    }
    buddy_merge_and_insert(page, 0);
    0
}

// ===========================================================================
// Buddy init helpers
// ===========================================================================
fn page_buddy_reserve_range(pa_start: u64, pa_end: u64,
                            region_start: u64, region_end: u64) {
    let r_start = core::cmp::max(pgrounddown(region_start), pa_start);
    let r_end   = core::cmp::min(pgroundup(region_end),     pa_end);
    if r_start >= r_end {
        ffi::log_reserved_out_of_range(region_start, region_end);
        return;
    }
    ffi::log_reserving(r_start, r_end);
    let mut base = r_start;
    while base < r_end {
        let p = pa_to_page(base)
            .unwrap_or_else(|| ffi::panic(b"__page_buddy_reserve_range(): get NULL page\0"));
        p.flags |= PAGE_FLAG_LOCKED;
        base += PAGE_SIZE;
    }
}

fn mark_reserved_page(pa_start: u64, pa_end: u64) {
    if ffi::has_ramdisk() && ffi::ramdisk_base() != 0 {
        let s = ffi::ramdisk_base();
        let e = s + ffi::ramdisk_size();
        page_buddy_reserve_range(pa_start, pa_end, s, e);
    }
    for i in 0..ffi::reserved_count() {
        let s = ffi::reserved_base(i);
        let e = s + ffi::reserved_size(i);
        page_buddy_reserve_range(pa_start, pa_end, s, e);
    }
}

fn find_next_avail(start_pa: u64, end_pa: u64) -> u64 {
    let mut pa = start_pa;
    while pa < end_pa {
        let p = pa_to_page(pa)
            .unwrap_or_else(|| ffi::panic(b"find_next_avail: NULL page\0"));
        if (p.flags & PAGE_FLAG_LOCKED) == 0 { break; }
        pa += PAGE_SIZE;
    }
    pa
}

fn find_current_end(start_pa: u64, end_pa: u64) -> u64 {
    let mut pa = start_pa;
    while pa < end_pa {
        let p = pa_to_page(pa)
            .unwrap_or_else(|| ffi::panic(b"find_current_end: NULL page\0"));
        if (p.flags & PAGE_FLAG_LOCKED) != 0 { break; }
        pa += PAGE_SIZE;
    }
    pa
}

fn page_buddy_init_as_order(pa_start: u64, order: u64) {
    let head = pa_to_page(pa_start)
        .unwrap_or_else(|| ffi::panic(b"page_buddy_init_as_order: NULL page\0"));
    head.as_buddy_group_init(order, BUDDY_STATE_FREE);
    buddy_pool(order).push_page(head);
}

fn page_buddy_init_range(pa_start: u64, pa_end: u64) {
    if pa_start >= pa_end { return; }
    let mut remain = pa_end - pa_start;
    let mut order: i64 = 0;
    let mut block = PAGE_SIZE << order;
    let mut pa = pa_start;

    // Align the start up to higher orders by carving small blocks first.
    while remain >= block && order < PAGE_BUDDY_MAX_ORDER as i64 {
        if (pa & block) != 0 {
            page_buddy_init_as_order(pa, order as u64);
            remain -= block;
            pa += block;
        }
        order += 1;
        block = PAGE_SIZE << order;
    }
    // Bulk middle: carve top-order blocks.
    while remain >= block {
        page_buddy_init_as_order(pa, order as u64);
        remain -= block;
        pa += block;
    }
    // Trail: shrink the order down until the remainder is consumed.
    while remain > 0 && order >= 0 {
        if remain >= block {
            page_buddy_init_as_order(pa, order as u64);
            remain -= block;
            pa += block;
        }
        order -= 1;
        if order < 0 { break; }
        block = PAGE_SIZE << order;
    }
}

// ===========================================================================
// Reference counting (internal, locked-or-atomic paths) -- methods on Page.
// ===========================================================================
impl Page {
    pub fn ref_inc_unlocked_impl(&mut self) -> i32 {
        if !ffi::spin_holding_b(self.lock_ptr()) {
            ffi::panic(b"__page_ref_inc_unlocked: page lock not held\0");
        }
        if self.ref_count == 0 { return -1; }
        self.ref_count += 1;
        self.ref_count
    }

    pub fn ref_dec_unlocked_impl(&mut self) -> i32 {
        if !ffi::spin_holding_b(self.lock_ptr()) {
            ffi::panic(b"__page_ref_dec_unlocked: page lock not held\0");
        }
        if self.ref_count > 0 {
            self.ref_count -= 1;
            return self.ref_count;
        }
        -1
    }
}

// ===========================================================================
// Address translation (internal helper used by init and FFI)
// ===========================================================================
fn pa_to_page(physical: u64) -> Option<&'static mut Page> {
    if !page_base_validity(physical) { return None; }
    let idx = ((physical - ffi::kernbase()) >> PAGE_SHIFT) as isize;
    Some(page_at(idx))
}

// ===========================================================================
// Borrow helpers for raw `*mut Page` arriving via FFI
// ===========================================================================
// `&'static mut Page` (not a free `<'a>`) because every `Page` lives in
// the global page array allocated once by `page_buddy_init` and never
// freed/moved for the rest of the kernel's lifetime, matching
// `pa_to_page`'s return type above. Exclusivity of the resulting
// `&mut` is a caller obligation documented on each public FFI entry
// point that calls this helper (the C side must not hold or create a
// second reference into the same `Page` for the call's duration).
#[inline]
fn page_ref(p: *mut Page) -> Option<&'static mut Page> {
    // SAFETY: `p`, if non-null, is trusted (by every caller's own
    // `# Safety` contract) to be a live pointer into the global `Page`
    // array with no other outstanding reference to the same page.
    NonNull::new(p).map(|mut nn| unsafe { nn.as_mut() })
}

// ===========================================================================
// PUBLIC API (C ABI)
// ===========================================================================

/// Initializes the buddy page allocator: allocates the global `Page`
/// array, sets `__pages`/`__managed_start`/`__managed_end`, and builds
/// the free lists over the managed physical range.
///
/// # Safety
///
/// - Must be called exactly once, at boot time, before any other
///   function in this module (all of them read `__pages` /
///   `__managed_start` / `__managed_end`, which are unwritten until
///   this returns).
/// - Must run single-threaded (no other hart may be executing kernel
///   code that touches page-allocator state concurrently) — the
///   internal writes to the allocator's global statics are not
///   synchronized.
#[no_mangle]
pub unsafe extern "C" fn page_buddy_init() -> c_int {
    let total_pages = ffi::totalpages();
    let page_arr_size = core::mem::size_of::<Page>() * total_pages as usize;
    let pages = ffi::early_alloc_align(page_arr_size, PAGE_SIZE as usize) as *mut Page;
    if pages.is_null() {
        ffi::panic(b"page_buddy_init(): failed to allocate page array\0");
    }
    // SAFETY: single-threaded boot-time init; addr-of writes avoid `&mut` to statics.
    {
        (&raw mut __pages).write(pages);
        (&raw mut __managed_start).write(pgroundup(ffi::early_alloc_end_ptr() as u64));
        (&raw mut __managed_end).write(ffi::phystop());
    }
    let mstart = managed_start();
    let mend = managed_end();
    ffi::log_init_summary(pages as *mut c_void, page_arr_size);
    ffi::log_init_range_phys(mstart, mend);

    let kernbase = ffi::kernbase();
    if kernbase >= mstart {
        ffi::panic(b"page_buddy_init(): KERNBASE not less than managed_start\0");
    }
    if mend > ffi::phystop() {
        ffi::panic(b"page_buddy_init(): managed_end higher than PHYSTOP\0");
    }
    if mstart >= mend {
        ffi::panic(b"page_buddy_init(): managed_start not less than managed_end\0");
    }
    if init_range_flags(kernbase, mstart, PAGE_FLAG_LOCKED) != 0 {
        ffi::panic(b"page_buddy_init(): lower locked range failed\0");
    }
    if mend < ffi::phystop()
        && init_range_flags(mend, ffi::phystop(), PAGE_FLAG_LOCKED) != 0 {
        ffi::panic(b"page_buddy_init(): higher locked range failed\0");
    }
    if init_range_flags(mstart, mend, 0) != 0 {
        ffi::panic(b"page_buddy_init(): free range failed\0");
    }

    buddy_pool_init();
    mark_reserved_page(mstart, mend);

    let mut base = find_next_avail(mstart, mend);
    let mut curr_end = find_current_end(base, mend);
    while base < mend {
        page_buddy_init_range(base, curr_end);
        ffi::log_buddy_range(base, curr_end);
        base = find_next_avail(curr_end, mend);
        curr_end = find_current_end(base, mend);
    }

    ffi::print_stat(1);
    0
}

/// Allocates a `2^order`-page block from the buddy allocator. Returns
/// null on failure or if `order` exceeds `PAGE_BUDDY_MAX_ORDER`.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called.
#[no_mangle]
pub unsafe extern "C" fn __page_alloc(order: u64, flags: u64) -> *mut Page {
    if order > PAGE_BUDDY_MAX_ORDER { return ptr::null_mut(); }
    match buddy_get(order, flags) {
        Some(p) => p as *mut Page,
        None => ptr::null_mut(),
    }
}

/// Frees a `2^order`-page block previously returned by
/// [`__page_alloc`]/[`page_alloc`] with the same `order`. No-op if
/// `page` is null.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array previously returned by [`__page_alloc`]/[`page_alloc`]
///   at the given `order`, not already freed (no double free), and the
///   caller must hold no other reference into the `count`-page group
///   starting at `page` for the duration of this call.
/// - `page_buddy_init` must have completed before this is called.
#[no_mangle]
pub unsafe extern "C" fn __page_free(page: *mut Page, order: u64) {
    let Some(p) = page_ref(page) else { return };
    if order > PAGE_BUDDY_MAX_ORDER {
        ffi::panic(b"__page_free(): order too large\0");
    }
    let count = 1u64 << order;

    if (p.physical_address & page_buddy_offset_mask(order)) != 0 {
        ffi::panic(b"free pages not aligned to order\0");
    }
    if p.is_type(PAGE_TYPE_SLAB) && p.slab_order() as u64 != order {
        ffi::panic(b"__page_free(): SLAB page freed with wrong order\0");
    }
    if p.is_type(PAGE_TYPE_TAIL) {
        ffi::panic(b"__page_free(): trying to free TAIL page directly\0");
    }

    // Validate every page in the group is freeable. Borrow each tail
    // separately to avoid overlapping `&mut`s.
    for i in 0..count {
        let tail = if i == 0 { &*p } else { &*page_offset_from(p, i as usize) };
        if !tail.is_freeable() {
            ffi::panic(b"__page_free(): trying to free non-freeable page\0");
        }
    }

    if order <= PCPU_CACHE_MAX_ORDER && pcpu_cache_put(p, order) == 0 {
        return;
    }

    let has_valid_tails = p.has_valid_tail_structure(order);
    {
        let _g = KSpinlock::from_ptr(p.lock_ptr()).lock();
        if has_valid_tails {
            p.as_buddy_header(order, BUDDY_STATE_INTERMEDIATE);
        } else {
            p.as_buddy_group(order, BUDDY_STATE_INTERMEDIATE);
        }
    }

    buddy_merge_and_insert(p, order);
}

/// Allocates a `2^order`-page block and returns its physical address
/// as `*mut c_void`, zero-filled (byte pattern `5`, matching the C
/// implementation's debug-fill convention). Panics if the allocation
/// succeeds but its physical address cannot be computed.
///
/// # Safety
///
/// - Same preconditions as [`__page_alloc`]: `page_buddy_init` must
///   have completed before this is called.
#[no_mangle]
pub unsafe extern "C" fn page_alloc(order: u64, flags: u64) -> *mut c_void {
    let page = __page_alloc(order, flags);
    if page.is_null() { return ptr::null_mut(); }
    let pa = __page_to_pa(page) as *mut c_void;
    if pa.is_null() { ffi::panic(b"page_alloc\0"); }
    ffi::memset(pa, 5, (PAGE_SIZE << order) as usize);
    pa
}

/// Frees the `2^order`-page block whose physical base address is
/// `ptr`. No-op if `ptr` does not translate to a page inside the
/// managed range.
///
/// # Safety
///
/// - `ptr`, if it does translate, must satisfy the same preconditions
///   as [`__page_free`]: a live, previously-allocated block at this
///   `order`, not already freed, with no other outstanding reference
///   into the group.
#[no_mangle]
pub unsafe extern "C" fn page_free(ptr: *mut c_void, order: u64) {
    let page = __pa_to_page(ptr as u64);
    __page_free(page, order);
}

// ---------------------------------------------------------------------------
// Page locking
// ---------------------------------------------------------------------------
/// Acquires `page`'s embedded spinlock. No-op if `page` is null.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array.
/// - The lock is released by a *separate* call to
///   [`page_lock_release`]; caller must not deadlock by re-acquiring
///   without an intervening release, and must call `page_lock_release`
///   exactly once for each successful acquire.
#[no_mangle]
pub unsafe extern "C" fn page_lock_acquire(page: *mut Page) {
    // Split-statement lock/unlock pair across C ABI boundary: matching
    // `page_lock_release` provides the release. The `KSpinlock` handle
    // is constructed, used for `lock()`, and its guard `mem::forget`'d
    // because the release happens in a separate ABI call. Going through
    // `KSpinlock::lock` keeps the call site uniform with the rest of mm.
    if let Some(p) = page_ref(page) {
        core::mem::forget(KSpinlock::from_ptr(p.lock_ptr()).lock());
    }
}

/// Releases `page`'s embedded spinlock previously acquired via
/// [`page_lock_acquire`]. No-op if `page` is null.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array whose lock is currently held by the calling hart.
#[no_mangle]
pub unsafe extern "C" fn page_lock_release(page: *mut Page) {
    if let Some(p) = page_ref(page) { ffi::spin_unlock(p.lock_ptr()); }
}

/// Panics if `page`'s embedded spinlock is not held by the calling
/// hart. No-op if `page` is null.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array.
#[no_mangle]
pub unsafe extern "C" fn page_lock_assert_holding(page: *mut Page) {
    if let Some(p) = page_ref(page) {
        if !ffi::spin_holding_b(p.lock_ptr()) {
            ffi::panic(b"page_lock_assert_holding failed\0");
        }
    }
}

/// Panics if `page`'s embedded spinlock *is* held by the calling
/// hart. No-op if `page` is null.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array.
#[no_mangle]
pub unsafe extern "C" fn page_lock_assert_unholding(page: *mut Page) {
    if let Some(p) = page_ref(page) {
        if ffi::spin_holding_b(p.lock_ptr()) {
            ffi::panic(b"page_lock_assert_unholding failed\0");
        }
    }
}

// ---------------------------------------------------------------------------
// Reference counting
// ---------------------------------------------------------------------------
/// Locks `page` and increments its reference count. Returns -1 if
/// `page` is null, otherwise the pre-increment behavior of
/// `ref_inc_unlocked_impl` (new count).
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array whose lock is *not* already held by the calling
///   hart (self-deadlock otherwise).
#[no_mangle]
pub unsafe extern "C" fn __page_ref_inc(page: *mut Page) -> c_int {
    let Some(p) = page_ref(page) else { return -1 };
    let _g = KSpinlock::from_ptr(p.lock_ptr()).lock();
    p.ref_inc_unlocked_impl()
}

/// Increments `page`'s reference count. Caller must already hold
/// `page`'s lock (checked by `ref_inc_unlocked_impl` via
/// `spin_holding_b`, which panics if not held).
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array whose lock is held by the calling hart.
#[no_mangle]
pub unsafe extern "C" fn page_ref_inc_unlocked(page: *mut Page) -> c_int {
    match page_ref(page) {
        Some(p) => p.ref_inc_unlocked_impl(),
        None => -1,
    }
}

/// Lockless (atomic) decrement of `page`'s reference count on the
/// fast path; returns -1 without decrementing if the count would drop
/// below 1.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array.
/// - No code path may treat `ref_count` as non-atomic while lockless
///   decrements are in flight — this function reinterprets the field
///   as `AtomicI32` for the duration of the RMW; callers that hold
///   `page`'s spinlock and touch `ref_count` directly must ensure
///   they are not doing so concurrently with a lockless decrement on
///   another hart.
#[no_mangle]
pub unsafe extern "C" fn page_ref_dec_unlocked(page: *mut Page) -> c_int {
    let Some(p) = page_ref(page) else { return -1 };
    // Lockless decrement: treat ref_count as atomic on the fast path.
    // SAFETY: `&mut Page` proves the storage is valid and exclusively
    // accessed for the duration of this borrow; we re-interpret the
    // i32 field as AtomicI32 to perform a sequentially-consistent
    // compare-decrement.
    let ac = {
        &*(ptr::addr_of!(p.ref_count) as *const AtomicI32)
    };
    let old = ac.fetch_sub(1, Ordering::SeqCst);
    if old < 2 {
        ac.fetch_add(1, Ordering::SeqCst);
        return -1;
    }
    old - 1
}

/// Locks `page`, decrements its reference count, and frees the page
/// (returning its slab/pcache node and the block itself to the buddy
/// allocator) if the count reaches zero. Returns 0 if `page` is null
/// or the count was already zero.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array whose lock is *not* already held by the calling
///   hart.
#[no_mangle]
pub unsafe extern "C" fn __page_ref_dec(page: *mut Page) -> c_int {
    let Some(p) = page_ref(page) else { return -1 };
    let ret = {
        let _g = KSpinlock::from_ptr(p.lock_ptr()).lock();
        let original = p.ref_count;
        if original < 1 {
            return 0;
        }
        let ret = p.ref_dec_unlocked_impl();
        if original - ret != 1 {
            ffi::panic(b"__page_ref_dec: ref_count should be decreased by 1\0");
        }
        ret
    };
    if ret == 0 {
        if p.is_type(PAGE_TYPE_PCACHE) {
            let node = p.pcache_node();
            if !node.is_null() {
                ffi::slab_free(node);
                p.set_pcache_node(ptr::null_mut());
            }
        }
        if buddy_put(p) != 0 {
            ffi::panic(b"page_ref_dec\0");
        }
    }
    ret
}

/// Returns the reference count of the page containing physical
/// address `physical`, or -1 if it does not translate to a managed
/// page.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called.
///   `physical` itself is validated internally (`pa_to_page` range
///   checks), so no additional pointer-validity precondition applies.
#[no_mangle]
pub unsafe extern "C" fn page_refcnt(physical: *mut c_void) -> c_int {
    let page = __pa_to_page(physical as u64);
    page_ref_count(page)
}

/// Locks and increments the reference count of the page containing
/// physical address `ptr`. See [`__page_ref_inc`] for the locking
/// precondition once translated.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called.
/// - If `ptr` translates to a managed page, that page's lock must not
///   already be held by the calling hart.
#[no_mangle]
pub unsafe extern "C" fn page_ref_inc(ptr: *mut c_void) -> c_int {
    let page = __pa_to_page(ptr as u64);
    __page_ref_inc(page)
}

/// Locks and decrements the reference count of the page containing
/// physical address `ptr`, freeing it if the count reaches zero. See
/// [`__page_ref_dec`] for the locking precondition once translated.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called.
/// - If `ptr` translates to a managed page, that page's lock must not
///   already be held by the calling hart.
#[no_mangle]
pub unsafe extern "C" fn page_ref_dec(ptr: *mut c_void) -> c_int {
    let page = __pa_to_page(ptr as u64);
    __page_ref_dec(page)
}

// ---------------------------------------------------------------------------
// Address translation
// ---------------------------------------------------------------------------
/// Translates a physical address to its owning `Page*`, or null if
/// `physical` is outside the managed range.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called
///   (`__pages`/`__managed_start`/`__managed_end` must be set).
#[no_mangle]
pub unsafe extern "C" fn __pa_to_page(physical: u64) -> *mut Page {
    match pa_to_page(physical) {
        Some(p) => p as *mut Page,
        None => ptr::null_mut(),
    }
}

/// Returns `page`'s physical address, or 0 if `page` is null.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array.
#[no_mangle]
pub unsafe extern "C" fn __page_to_pa(page: *mut Page) -> u64 {
    match page_ref(page) {
        Some(p) => p.physical_address,
        None => 0,
    }
}

/// Returns `page`'s current reference count, or -1 if `page` is null.
///
/// # Safety
///
/// - `page`, if non-null, must be a live pointer into the global
///   `Page` array. The read is not synchronized with concurrent
///   lockless decrements ([`page_ref_dec_unlocked`]); callers wanting
///   a linearizable snapshot must hold `page`'s lock.
#[no_mangle]
pub unsafe extern "C" fn page_ref_count(page: *mut Page) -> c_int {
    match page_ref(page) {
        Some(p) => p.ref_count,
        None => -1,
    }
}

/// Returns the physical base address of the managed page range.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called (the
///   static is otherwise a leftover zero/uninitialized value).
#[no_mangle]
pub unsafe extern "C" fn managed_page_base() -> u64 {
    __managed_start
}

// ---------------------------------------------------------------------------
// Statistics / sanity helpers
// ---------------------------------------------------------------------------
/// Fills `ret_arr[0..n]` with each buddy order's free-block count and,
/// if `empty_arr` is non-null, `empty_arr[0..n]` with a 0/1 empty
/// flag, where `n = min(size, PAGE_BUDDY_MAX_ORDER + 1)`. No-op if
/// `ret_arr` is null or `size` is too small.
///
/// # Safety
///
/// - `ret_arr` must be valid for `size` writes of `u64`.
/// - `empty_arr`, if non-null, must be valid for `size` writes of
///   `u8`.
/// - `page_buddy_init` must have completed before this is called.
#[no_mangle]
pub unsafe extern "C" fn page_buddy_stat(ret_arr: *mut u64,
                                         empty_arr: *mut u8,
                                         size: usize) {
    if ret_arr.is_null() || size < (PAGE_BUDDY_MAX_ORDER + 1) as usize { return; }
    buddy_pool_lock_range(0, PAGE_BUDDY_MAX_ORDER);
    let n = core::cmp::min(size, (PAGE_BUDDY_MAX_ORDER + 1) as usize);
    let ret = core::slice::from_raw_parts_mut(ret_arr, n);
    let mut empty: Option<&mut [u8]> = if empty_arr.is_null() {
        None
    } else {
        Some(core::slice::from_raw_parts_mut(empty_arr, n))
    };
    for i in 0..n {
        let pool = buddy_pool(i as u64);
        ret[i] = pool.count;
        if let Some(em) = empty.as_deref_mut() {
            em[i] = if ffi::list_is_empty(&pool.lru_head) { 1 } else { 0 };
        }
    }
    buddy_pool_unlock_range(0, PAGE_BUDDY_MAX_ORDER);
}

/// Panics if `ptr` is null or does not fall strictly inside the
/// global `Page` array's address range.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called
///   (`pages_base()`/`totalpages()` must be valid).
/// - `ptr` need not itself be dereferenced by this function — only
///   its numeric value is compared — so no pointer-validity
///   precondition applies to `ptr` beyond being non-null.
#[no_mangle]
pub unsafe extern "C" fn __check_page_pointer_in_range(ptr: *mut c_void) {
    if ptr.is_null() {
        ffi::panic(b"__check_page_pointer_in_range: NULL pointer\0");
    }
    let p_start = pages_base() as usize;
    let p_end = p_start + (ffi::totalpages() as usize) * core::mem::size_of::<Page>();
    let p = ptr as usize;
    if !(p > p_start && p < p_end) {
        ffi::panic(b"__check_page_pointer_in_range: page pointer out of range\0");
    }
}

// ---------------------------------------------------------------------------
// Helpers exposed to the C shim file (`page_shims.c`)
// ---------------------------------------------------------------------------
/// Returns the free-block count of buddy pool `order`, or 0 if
/// `order` exceeds `PAGE_BUDDY_MAX_ORDER`.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called
///   (the pool array is otherwise uninitialized).
/// - The read is unsynchronized (no pool lock taken); caller must
///   tolerate a torn/stale value or hold the range lock itself (see
///   [`xv6_buddy_pool_lock_range_all`]).
#[no_mangle]
pub unsafe extern "C" fn xv6_buddy_pool_count(order: u64) -> u64 {
    if order > PAGE_BUDDY_MAX_ORDER { return 0; }
    buddy_pool(order).count
}

/// Returns the free-block count of `cpu`'s per-CPU cache at `order`,
/// or 0 if `cpu`/`order` are out of range.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called.
/// - `cpu` must be a valid hart index; the per-CPU cache array must
///   have at least `NCPU` entries (invariant of the allocator's
///   static layout, not caller-controlled).
#[no_mangle]
pub unsafe extern "C" fn xv6_pcpu_cache_count(cpu: u32, order: u64) -> u32 {
    if (cpu as usize) >= NCPU || order > PCPU_CACHE_MAX_ORDER { return 0; }
    pcpu_cache(cpu as usize, order).count.load(Ordering::Acquire)
}

/// Locks every buddy pool's spinlock, orders 0..=`PAGE_BUDDY_MAX_ORDER`.
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called.
/// - Must be paired with exactly one later call to
///   [`xv6_buddy_pool_unlock_range_all`] on the same hart before any
///   other buddy-pool-locking call is made (the locks are held across
///   the ABI boundary, mirroring [`page_lock_acquire`]).
#[no_mangle]
pub unsafe extern "C" fn xv6_buddy_pool_lock_range_all() {
    buddy_pool_lock_range(0, PAGE_BUDDY_MAX_ORDER);
}

/// Releases the locks taken by [`xv6_buddy_pool_lock_range_all`].
///
/// # Safety
///
/// - Caller must currently hold every buddy pool lock, orders
///   0..=`PAGE_BUDDY_MAX_ORDER`, acquired via a prior matching call to
///   [`xv6_buddy_pool_lock_range_all`].
#[no_mangle]
pub unsafe extern "C" fn xv6_buddy_pool_unlock_range_all() {
    buddy_pool_unlock_range(0, PAGE_BUDDY_MAX_ORDER);
}

/// Returns the total number of managed pages
/// (`(managed_end - managed_start) >> PAGE_SHIFT`).
///
/// # Safety
///
/// - `page_buddy_init` must have completed before this is called.
#[no_mangle]
pub unsafe extern "C" fn xv6_total_managed_pages() -> u64 {
    let s = __managed_start;
    let e = __managed_end;
    (e - s) >> PAGE_SHIFT
}

// ===========================================================================
// Statistics reporting (`print_buddy_system_stat`) and the `memstat`
// syscall body. Formerly `page_shims.rs`; moved here because it is real
// logic (formatting + syscall dispatch), not a pure accessor round-trip.
// ===========================================================================
const ORDER_COUNT: usize = (PAGE_BUDDY_MAX_ORDER + 1) as usize;

/// Print a byte count in the largest whole unit (G/M/K/B) that fits.
fn print_size(bytes: u64) {
    static F_G: &[u8] = b"%ld.%ldG\0";
    static F_M: &[u8] = b"%ld.%ldM\0";
    static F_K: &[u8] = b"%ldK\0";
    static F_B: &[u8] = b"%ldB\0";
    // SAFETY: every format string above matches its argument list exactly.
    unsafe {
        if bytes >= (1u64 << 30) {
            let gb = bytes >> 30;
            let mb = (bytes & ((1u64 << 30) - 1)) >> 20;
            printf(F_G.as_ptr() as *const c_char, gb, (mb * 10) / 1024);
        } else if bytes >= (1u64 << 20) {
            let mb = bytes >> 20;
            let kb = (bytes & ((1u64 << 20) - 1)) >> 10;
            printf(F_M.as_ptr() as *const c_char, mb, (kb * 10) / 1024);
        } else if bytes >= (1u64 << 10) {
            let kb = bytes >> 10;
            printf(F_K.as_ptr() as *const c_char, kb);
        } else {
            printf(F_B.as_ptr() as *const c_char, bytes);
        }
    }
}

/// Sum free + per-cpu-cached pages across every order. `empty_arr` mirrors
/// `page_buddy_stat`'s raw `u8` output convention (0/1, not `bool`) so the
/// call below needs no type-punning.
fn buddy_stat_totals(
    total_free_pages: &mut u64,
    total_cached_pages: &mut u64,
    ret_arr: &mut [u64; ORDER_COUNT],
    empty_arr: &mut [u8; ORDER_COUNT],
) {
    *total_free_pages = 0;
    *total_cached_pages = 0;

    // SAFETY: `ret_arr`/`empty_arr` are stack arrays of exactly `ORDER_COUNT`
    // elements, matching the `size` argument.
    unsafe {
        page_buddy_stat(ret_arr.as_mut_ptr(), empty_arr.as_mut_ptr(), ORDER_COUNT);
    }

    for i in 0..=(PAGE_BUDDY_MAX_ORDER as usize) {
        let order_pages = (1u64 << i) * ret_arr[i];
        *total_free_pages += order_pages;

        if (i as u64) <= PCPU_CACHE_MAX_ORDER {
            let mut cache_total: u64 = 0;
            for cpu in 0..NCPU {
                // SAFETY: `cpu`/`i` are within `xv6_pcpu_cache_count`'s
                // documented bounds (checked internally; out-of-range
                // returns 0).
                cache_total += unsafe { xv6_pcpu_cache_count(cpu as u32, i as u64) } as u64;
            }
            if cache_total > 0 {
                *total_cached_pages += (1u64 << i) * cache_total;
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn print_buddy_system_stat(detailed: c_int) {
    static F_SUMMARY: &[u8] = b"Buddy: %ld free + %ld cached = %ld pages (\0";
    static F_HDR: &[u8] = b"Buddy System Statistics:\n\0";
    static F_BAR: &[u8] = b"========================\n\0";
    static F_BAR2: &[u8] = b"------------------------\n\0";
    static F_ORDER: &[u8] = b"order(%d): %ld blocks (\0";
    static F_CACHED: &[u8] = b" + %ld cached (\0";
    static F_CLOSE: &[u8] = b")\n\0";
    static F_RCLOSE: &[u8] = b")\0";
    static F_NEWLINE: &[u8] = b"\n\0";

    let mut total_free_pages: u64 = 0;
    let mut total_cached_pages: u64 = 0;
    let mut ret_arr: [u64; ORDER_COUNT] = [0; ORDER_COUNT];
    let mut empty_arr: [u8; ORDER_COUNT] = [0; ORDER_COUNT];

    buddy_stat_totals(
        &mut total_free_pages,
        &mut total_cached_pages,
        &mut ret_arr,
        &mut empty_arr,
    );

    // SAFETY: every `printf` call below matches its own format string.
    unsafe {
        if detailed <= 0 {
            printf(
                F_SUMMARY.as_ptr() as *const c_char,
                total_free_pages,
                total_cached_pages,
                total_free_pages + total_cached_pages,
            );
            print_size((total_free_pages + total_cached_pages) * PAGE_SIZE);
            printf(F_CLOSE.as_ptr() as *const c_char);
            return;
        }

        printf(F_HDR.as_ptr() as *const c_char);
        printf(F_BAR.as_ptr() as *const c_char);

        for i in 0..=(PAGE_BUDDY_MAX_ORDER as usize) {
            let order_pages = (1u64 << i) * ret_arr[i];
            let order_bytes = order_pages * PAGE_SIZE;

            printf(F_ORDER.as_ptr() as *const c_char, i as c_int, ret_arr[i]);
            print_size(order_bytes);
            printf(F_RCLOSE.as_ptr() as *const c_char);

            if (i as u64) <= PCPU_CACHE_MAX_ORDER {
                let mut cache_total: u64 = 0;
                for cpu in 0..NCPU {
                    cache_total += xv6_pcpu_cache_count(cpu as u32, i as u64) as u64;
                }
                if cache_total > 0 {
                    let cached_pages = (1u64 << i) * cache_total;
                    let cached_bytes = cached_pages * PAGE_SIZE;
                    printf(F_CACHED.as_ptr() as *const c_char, cache_total);
                    print_size(cached_bytes);
                    printf(F_RCLOSE.as_ptr() as *const c_char);
                }
            }
            printf(F_NEWLINE.as_ptr() as *const c_char);
        }

        printf(F_BAR2.as_ptr() as *const c_char);
        printf(
            F_SUMMARY.as_ptr() as *const c_char,
            total_free_pages,
            total_cached_pages,
            total_free_pages + total_cached_pages,
        );
        print_size((total_free_pages + total_cached_pages) * PAGE_SIZE);
        printf(F_CLOSE.as_ptr() as *const c_char);
    }
}

// ===========================================================================
// check_buddy_system_integrity — walk every buddy pool and verify counts +
// pointers. Diagnostic only; not on the hot path, and not currently called
// from anywhere (no C caller, no Rust caller) -- kept as a plain fn
// available for ad-hoc debugging rather than exported over the C ABI.
//
// The original `page_shims.rs` version reinterpreted `BuddyPool::lru_head`
// (a `crate::mm::cffi::ListNode`) as `bindings::list_node_t` and computed
// page addresses via a hardcoded `.wrapping_sub(48)`. Neither cast is
// needed here: `lru_head` is already a `ListNode`, and `lru_node_to_page`
// already encodes the head-offset via `offset_of!`, so the walk below
// reuses both directly.
// ===========================================================================
#[allow(dead_code)]
pub fn check_buddy_system_integrity() {
    static FMT_PREVNEXT: &[u8] = b"prev page: %p, next page: %p\n\0";
    static FMT_ENTRY: &[u8] = b"count = %d, buddy page: %p, order: %d, physical: 0x%lx\n\0";

    let mut total_free_pages: u64 = 0;
    buddy_pool_lock_range(0, PAGE_BUDDY_MAX_ORDER);
    for i in 0..=(PAGE_BUDDY_MAX_ORDER as usize) {
        let pool = buddy_pool(i as u64);
        let count_field = pool.count as i64;
        let head: *mut ListNode = &mut pool.lru_head;
        // SAFETY: `head` is the address of a live, initialized `ListNode`
        // embedded in the global `__buddy_pools` array; reading its
        // `prev`/`next` fields through the raw pointer is a plain load.
        let (head_prev, head_next) = unsafe { ((*head).prev, (*head).next) };
        let empty = head_prev == head || head_prev.is_null();

        if count_field < 0 {
            ffi::panic(b"buddy pool count is negative\0");
        }
        if !(empty || count_field > 0) {
            ffi::panic(b"buddy pool is not empty but count is zero\0");
        }
        if !(!empty || count_field == 0) {
            ffi::panic(b"buddy pool is empty but count is not zero\0");
        }
        total_free_pages += (1u64 << i) * count_field as u64;

        if !empty {
            // SAFETY: `__check_page_pointer_in_range` and `printf` are
            // C-ABI functions; the pointers passed are the raw list-node
            // pointers just read above.
            unsafe {
                __check_page_pointer_in_range(head_prev as *mut c_void);
                __check_page_pointer_in_range(head_next as *mut c_void);
                printf(FMT_PREVNEXT.as_ptr() as *const c_char, head_prev, head_next);
            }
        }

        // Walk from head.next until we loop back (list_foreach_node_safe).
        let mut count_check = count_field as i32;
        let mut pos = head_next;
        while !pos.is_null() && pos != head {
            // SAFETY: `pos` is a live list node inside the pool's LRU list.
            let next = unsafe { (*pos).next };
            let Some(page) = lru_node_to_page(pos) else {
                ffi::panic(b"buddy pool LRU node has no owning page\0");
            };
            if !page.is_type(PAGE_TYPE_BUDDY) {
                ffi::panic(b"buddy page is not a group head\0");
            }
            if page.buddy_order() as usize != i {
                ffi::panic(b"buddy page order mismatch\0");
            }
            // SAFETY: `page` is a valid `&mut Page` from `lru_node_to_page`.
            unsafe {
                __check_page_pointer_in_range(page as *mut Page as *mut c_void);
                if __page_to_pa(page as *mut Page) != page.physical_address {
                    ffi::panic(b"buddy page physical address mismatch\0");
                }
                count_check -= 1;
                printf(
                    FMT_ENTRY.as_ptr() as *const c_char,
                    count_check as c_int,
                    page as *mut Page,
                    page.buddy_order() as c_int,
                    page.physical_address,
                );
            }
            pos = next;
        }
        if count_check != 0 {
            ffi::panic(b"buddy pool count mismatch\0");
        }
    }
    buddy_pool_unlock_range(0, PAGE_BUDDY_MAX_ORDER);
    let _ = total_free_pages;
}

// ===========================================================================
// sys_memstat — syscall body. Reads syscall arg 0 (flags), prints requested
// stats, returns combined byte counts depending on flags.
// ===========================================================================
const MEMSTAT_VERBOSE: u32 = 1 << 0;
const MEMSTAT_DETAILED: u32 = 1 << 1;
const MEMSTAT_INCLUDE_SLAB: u32 = 1 << 2;
const MEMSTAT_INCLUDE_BUDDY: u32 = 1 << 3;
const MEMSTAT_ADD_FREE: u32 = 1 << 4;
const MEMSTAT_ADD_USED: u32 = 1 << 5;

#[no_mangle]
pub extern "C" fn sys_memstat() -> u64 {
    static FMT_FREE_LABEL: &[u8] = b"Free: \0";
    static FMT_USED_LABEL: &[u8] = b"Used: \0";
    static FMT_NEWLINE_ONLY: &[u8] = b"\n\0";

    let mut flags_arg: c_int = 0;
    ffi::argint(0, &mut flags_arg);
    let flags = flags_arg as u32;

    let mut total_free_pages: u64 = 0;
    let mut total_cached_pages: u64 = 0;
    let mut ret_arr: [u64; ORDER_COUNT] = [0; ORDER_COUNT];
    let mut empty_arr: [u8; ORDER_COUNT] = [0; ORDER_COUNT];

    if (flags & MEMSTAT_INCLUDE_BUDDY) != 0 {
        if (flags & MEMSTAT_DETAILED) != 0 {
            print_buddy_system_stat(1);
        } else if (flags & MEMSTAT_VERBOSE) != 0 {
            print_buddy_system_stat(0);
        }
    }
    if (flags & MEMSTAT_INCLUDE_SLAB) != 0 {
        if (flags & MEMSTAT_DETAILED) != 0 {
            crate::mm::slab::slab_dump_all(2);
        } else if (flags & MEMSTAT_VERBOSE) != 0 {
            crate::mm::slab::slab_dump_all(1);
        }
    }

    buddy_stat_totals(
        &mut total_free_pages,
        &mut total_cached_pages,
        &mut ret_arr,
        &mut empty_arr,
    );
    let free_bytes = (total_free_pages + total_cached_pages) * PAGE_SIZE;
    // SAFETY: plain unsafe extern "C" fn call, defined earlier in this file.
    let managed_bytes = unsafe { xv6_total_managed_pages() } * PAGE_SIZE;
    let used_bytes = if managed_bytes > free_bytes {
        managed_bytes - free_bytes
    } else {
        0
    };

    // SAFETY: every `printf` call below matches its own format string.
    unsafe {
        if (flags & (MEMSTAT_VERBOSE | MEMSTAT_DETAILED)) != 0 {
            if (flags & MEMSTAT_ADD_FREE) != 0 {
                printf(FMT_FREE_LABEL.as_ptr() as *const c_char);
                print_size(free_bytes);
                printf(FMT_NEWLINE_ONLY.as_ptr() as *const c_char);
            }
            if (flags & MEMSTAT_ADD_USED) != 0 {
                printf(FMT_USED_LABEL.as_ptr() as *const c_char);
                print_size(used_bytes);
                printf(FMT_NEWLINE_ONLY.as_ptr() as *const c_char);
            }
        }
    }

    let mut ret: u64 = 0;
    if (flags & MEMSTAT_ADD_FREE) != 0 {
        ret += free_bytes;
    }
    if (flags & MEMSTAT_ADD_USED) != 0 {
        ret += used_bytes;
    }
    ret
}
