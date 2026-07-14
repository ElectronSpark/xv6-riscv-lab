//! Early Allocator — Rust port of `kernel/mm/early_allocator.c`.
//!
//! Buddy-style bump allocator used during boot, before the main page
//! allocator is initialized. Single-threaded (boot CPU only), so no locking.
//!
//! Allocation strategy:
//!  * Objects of size ≤ 64 KiB (after rounding to a power of two) come from
//!    per-order free lists; user alignment is ignored, chunks are
//!    self-aligned.
//!  * Larger objects are served by advancing the `current` pointer and
//!    respect the user-supplied alignment.
//!  * Alignment gaps are broken into properly aligned power-of-two chunks
//!    and pushed onto the free lists. Fragments smaller than 32 bytes are
//!    discarded.
//!
//! All algorithmic logic lives in safe Rust. The only `unsafe` is the
//! FFI boundary (`mod ffi`), the storage-init for the singleton, the
//! chunk-header writes, and the thin `#[no_mangle] extern "C"` wrappers.

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::ptr;

// ---------------------------------------------------------------------------
// Constants — must mirror early_allocator.c exactly.
// ---------------------------------------------------------------------------
const EARLYALLOC_CHUNK_MAGIC:    u64   = 0xEAACCCCCEAACCCCC;
const EARLYALLOC_SMALLEST_ORDER: u32   = 5;   // 32 bytes
const EARLYALLOC_SMALLEST_CHUNK: u64   = 1u64 << EARLYALLOC_SMALLEST_ORDER;
const EARLYALLOC_LARGEST_ORDER:  u32   = 16;  // 64 KiB
const EARLYALLOC_LARGEST_CHUNK:  u64   = 1u64 << EARLYALLOC_LARGEST_ORDER;
const EARLYALLOC_ORDERS:         usize =
    (EARLYALLOC_LARGEST_ORDER - EARLYALLOC_SMALLEST_ORDER + 1) as usize; // 12

// ---------------------------------------------------------------------------
// Mirror types — layout must match C exactly.
// `ListNode` is the canonical opaque mirror from `crate::mm::cffi`.
// ---------------------------------------------------------------------------
use crate::mm::cffi::ListNode;

#[repr(C)]
struct Chunk {
    magic: u64,
    size: usize,
    list_entry: ListNode,
}

const LIST_ENTRY_OFFSET: usize = 16;

// ---------------------------------------------------------------------------
// FFI boundary.
//
// Shared kernel primitives come from `crate::mm::cffi` (declared as
// `unsafe extern "C" { pub safe fn ... }`). The local `ffi` module is
// kept as a thin re-export so existing `ffi::xv6_list_*` call sites
// compile unchanged.
// ---------------------------------------------------------------------------
mod ffi {
    pub(crate) use crate::mm::cffi::raw::{
        xv6_list_init, xv6_list_is_empty,
        xv6_list_push_front, xv6_list_pop_front,
    };
}

#[inline(never)]
fn panic_early(msg: &[u8]) -> ! {
    debug_assert!(msg.last() == Some(&0));
    crate::mm::cffi::panic_bytes(msg)
}

// ---------------------------------------------------------------------------
// `ChunkPtr` — typed handle to an early-allocator chunk header.
// ---------------------------------------------------------------------------
#[derive(Copy, Clone)]
struct ChunkPtr(*mut Chunk);

impl ChunkPtr {
    fn addr(self) -> u64 { self.0 as u64 }

    /// Initialise a fresh header at `addr` of given `size`.
    fn make(addr: u64, size: u64) -> Self {
        let p = addr as *mut Chunk;
        // SAFETY: caller has reserved a `size`-byte region at `addr` that is
        // not aliased; we exclusively write its header here.
        unsafe {
            (*p).magic = EARLYALLOC_CHUNK_MAGIC;
            (*p).size  = size as usize;
            ffi::xv6_list_init(&mut (*p).list_entry);
        }
        Self(p)
    }

    fn from_list_entry(entry: *mut ListNode) -> Self {
        // SAFETY: `entry` always points to the `list_entry` field of a
        // `Chunk` placed by `make()`.
        Self(unsafe { (entry as *mut u8).sub(LIST_ENTRY_OFFSET) as *mut Chunk })
    }

    fn size(self) -> u64 {
        // SAFETY: `Self::make` initialised `size`; field reads are valid.
        unsafe { (*self.0).size as u64 }
    }

    fn set_size(self, size: u64) {
        unsafe { (*self.0).size = size as usize; }
    }

    fn check_magic(self, msg: &[u8]) {
        // SAFETY: header was initialised by `Self::make` if the invariant holds.
        if unsafe { (*self.0).magic } != EARLYALLOC_CHUNK_MAGIC {
            panic_early(msg);
        }
    }

    fn list_entry_ptr(self) -> *mut ListNode {
        // SAFETY: just deriving a field pointer.
        unsafe { &mut (*self.0).list_entry as *mut ListNode }
    }
}

// ---------------------------------------------------------------------------
// Free-list head — typed handle, methods are safe FFI dispatch.
// ---------------------------------------------------------------------------
#[derive(Copy, Clone)]
struct FreeList(*mut ListNode);

impl FreeList {
    fn init(self) {
        ffi::xv6_list_init(self.0);
    }
    fn is_empty(self) -> bool {
        ffi::xv6_list_is_empty(self.0) != 0
    }
    fn push_front(self, entry: *mut ListNode) {
        ffi::xv6_list_push_front(self.0, entry);
    }
    fn pop_front(self) -> *mut ListNode {
        ffi::xv6_list_pop_front(self.0)
    }
}

// ---------------------------------------------------------------------------
// Allocator state.
// ---------------------------------------------------------------------------
struct EarlyAllocator {
    free_lists: [ListNode; EARLYALLOC_ORDERS],
    current:    u64,
    end:        u64,
}

const fn null_list_node() -> ListNode {
    ListNode::new()
}

#[repr(transparent)]
struct AllocatorCell(UnsafeCell<EarlyAllocator>);
unsafe impl Sync for AllocatorCell {}

static ALLOC: AllocatorCell = AllocatorCell(UnsafeCell::new(EarlyAllocator {
    free_lists: [const { null_list_node() }; EARLYALLOC_ORDERS],
    current:    0,
    end:        0,
}));

/// Get an exclusive `&mut EarlyAllocator`.
///
/// SAFETY: caller asserts no other thread or interrupt is touching the
/// allocator. The early allocator runs only on the boot hart before SMP
/// and is unused thereafter; this invariant holds for every entry point.
unsafe fn allocator() -> &'static mut EarlyAllocator {
    &mut *ALLOC.0.get()
}

impl EarlyAllocator {
    fn free_list(&mut self, idx: usize) -> FreeList {
        FreeList(&mut self.free_lists[idx] as *mut ListNode)
    }

    /// Initialise heads and the bump range `[start_aligned, end)`.
    fn init(&mut self, pa_start: u64, pa_end: u64) {
        if pa_start == 0 || pa_end == 0 || pa_end <= pa_start {
            panic_early(b"early_allocator_init: invalid memory range\0");
        }
        for i in 0..EARLYALLOC_ORDERS {
            self.free_list(i).init();
        }
        let start_aligned = align_up(pa_start, EARLYALLOC_SMALLEST_CHUNK);
        if start_aligned >= pa_end {
            panic_early(
                b"early_allocator_init: invalid memory range after alignment\0",
            );
        }
        self.current = start_aligned;
        self.end     = pa_end;
    }

    fn add_to_freelist(&mut self, chunk: ChunkPtr) {
        let size = chunk.size();
        let order = size_to_order(size);
        let list_idx = order as i32 - EARLYALLOC_SMALLEST_ORDER as i32;

        if chunk.addr() & (size - 1) != 0 {
            panic_early(b"__add_chunk_to_freelist: chunk not aligned to its size\0");
        }
        if list_idx >= 0 && (list_idx as usize) < EARLYALLOC_ORDERS {
            self.free_list(list_idx as usize).push_front(chunk.list_entry_ptr());
        }
    }

    /// Break `[start, end)` into properly aligned power-of-two chunks.
    fn free_region(&mut self, mut start: u64, end: u64) {
        while start < end {
            let remaining = end - start;
            let mut order = size_to_order(remaining);
            if (1u64 << order) > remaining {
                order = order.saturating_sub(1);
            }

            let mut placed = false;
            while order >= EARLYALLOC_SMALLEST_ORDER {
                let chunk_size = 1u64 << order;
                if start & (chunk_size - 1) == 0 && order <= EARLYALLOC_LARGEST_ORDER {
                    let chunk = ChunkPtr::make(start, chunk_size);
                    self.add_to_freelist(chunk);
                    start += chunk_size;
                    placed = true;
                    break;
                }
                order -= 1;
            }
            if !placed {
                start += 1;
            }
        }
    }

    /// Pop a chunk of `2^target_order` bytes, splitting if needed.
    fn get_from_freelist(&mut self, target_order: u32) -> Option<ChunkPtr> {
        let list_idx = (target_order as i32) - (EARLYALLOC_SMALLEST_ORDER as i32);
        if list_idx < 0 || (list_idx as usize) >= EARLYALLOC_ORDERS {
            return None;
        }

        // Exact fit.
        let exact = self.free_list(list_idx as usize);
        if !exact.is_empty() {
            let chunk = ChunkPtr::from_list_entry(exact.pop_front());
            chunk.check_magic(b"__get_chunk_from_freelist: corrupted chunk\0");
            return Some(chunk);
        }

        // Find a larger chunk to split.
        let mut order = target_order + 1;
        while order <= EARLYALLOC_LARGEST_ORDER {
            let idx = (order - EARLYALLOC_SMALLEST_ORDER) as usize;
            if idx >= EARLYALLOC_ORDERS {
                break;
            }
            let list = self.free_list(idx);
            if !list.is_empty() {
                let chunk = ChunkPtr::from_list_entry(list.pop_front());
                chunk.check_magic(b"__get_chunk_from_freelist: corrupted chunk\0");

                // Split down to target_order.
                let mut cur_order = order;
                while cur_order > target_order {
                    cur_order -= 1;
                    let half_size = 1u64 << cur_order;
                    let buddy = ChunkPtr::make(chunk.addr() + half_size, half_size);
                    self.add_to_freelist(buddy);
                    chunk.set_size(half_size);
                }
                return Some(chunk);
            }
            order += 1;
        }
        None
    }

    /// Bump-allocate `size` bytes at `align`, salvaging any alignment gap.
    fn alloc_advancing(&mut self, size: u64, align: u64) -> u64 {
        let aligned_addr = align_up(self.current, align);
        let end_addr = aligned_addr + size;

        if end_addr > self.end {
            panic_early(b"early_alloc_align: out of memory\0");
        }
        if aligned_addr > self.current {
            let prev = self.current;
            self.free_region(prev, aligned_addr);
        }
        self.current = end_addr;
        aligned_addr
    }

    fn alloc(&mut self, size: u64, align: u64) -> *mut c_void {
        if align == 0 || (align & (align - 1)) != 0 {
            panic_early(b"early_alloc_align: alignment must be a power of 2\0");
        }
        if size == 0 {
            return ptr::null_mut();
        }

        let chunk_size = size.max(EARLYALLOC_SMALLEST_CHUNK);
        let order = size_to_order(chunk_size);
        let actual_size = 1u64 << order;

        // Small object → free-list (or self-aligned bump on miss).
        if actual_size <= EARLYALLOC_LARGEST_CHUNK && order <= EARLYALLOC_LARGEST_ORDER {
            if let Some(chunk) = self.get_from_freelist(order) {
                return chunk.addr() as *mut c_void;
            }
            let addr = self.alloc_advancing(actual_size, actual_size);
            return addr as *mut c_void;
        }

        // Large object → user-aligned bump.
        self.alloc_advancing(size, align) as *mut c_void
    }
}

// ---------------------------------------------------------------------------
// Pure helpers.
// ---------------------------------------------------------------------------

/// Smallest `order` such that `2^order >= size`.
fn size_to_order(size: u64) -> u32 {
    let mut order: u32 = 0;
    while (1u64 << order) < size {
        order += 1;
    }
    order
}

fn align_up(x: u64, align: u64) -> u64 {
    (x + (align - 1)) & !(align - 1)
}

// ---------------------------------------------------------------------------
// Public C ABI — thin wrappers.
// ---------------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn early_allocator_init(
    pa_start: *mut c_void,
    pa_end: *mut c_void,
) {
    allocator().init(pa_start as u64, pa_end as u64);
}

#[no_mangle]
pub unsafe extern "C" fn early_alloc_align(size: usize, align: usize) -> *mut c_void {
    allocator().alloc(size as u64, align as u64)
}

// Demoted from `#[no_mangle]` (P3-1A mesh sweep): neither has any caller
// outside this crate, so the C-ABI symbol was dropped. `#[no_mangle]`
// previously suppressed `dead_code` for both regardless of real usage;
// `early_alloc_end_ptr` gains a genuine in-crate caller (`mm/page.rs`,
// full build only) but the `cfg(test)` host-test seam compiles this file
// standalone (see the crate doc's "Host-test seam" section: only
// `mm::bits`/`mm::early_allocator` are built for `cargo test`, not
// `mm::page`), so both look unused from that reduced tree's perspective.
#[cfg_attr(test, allow(dead_code))]
pub(crate) unsafe fn early_alloc(size: usize) -> *mut c_void {
    early_alloc_align(size, EARLYALLOC_SMALLEST_CHUNK as usize)
}

#[cfg_attr(test, allow(dead_code))]
pub(crate) unsafe fn early_alloc_end_ptr() -> *mut c_void {
    allocator().current as *mut c_void
}

// ---------------------------------------------------------------------------
// Host-test suite (`cargo test --target x86_64-unknown-linux-gnu`; see
// `kernel/lib.rs`'s "Host-test seam" doc for the module-tree/mock design
// this depends on -- `crate::mm::cffi` here resolves to the small
// hand-written host mock declared in `lib.rs`, not the real `mm/cffi.rs`).
//
// Every test below constructs its own `EarlyAllocator` directly (bypassing
// the `ALLOC` singleton / `#[no_mangle]` C-ABI wrappers above entirely) over
// a private, heap-boxed arena, instead of sharing the one process-wide
// static the C reference suite uses with a setup/teardown pair per test
// (`setup_allocator`/`teardown_allocator` in `test/src/ut_early_allocator_main.c`).
// This is a deliberate improvement, not just a style choice: Rust's `cargo
// test` runs tests concurrently by default, and every test in this module
// mutates allocator state, so sharing one global would be a data race
// between test threads; giving each test its own allocator and arena gets
// the C suite's per-test isolation *and* safe parallelism.
//
// The `EarlyAllocator` struct is self-referential once initialized (each
// free-list head's `ListNode` self-loops to its own address via
// `xv6_list_init`) -- exactly like the real kernel's `static ALLOC`, it must
// never move after `init()` runs. `Box::new` gives it a stable heap address
// up front, and `new_test_allocator` never moves the struct out of the box
// afterward (only the `Box` handle itself moves, which is just a pointer).
//
// Reference-coverage note: all 13 cases in
// `test/src/ut_early_allocator_main.c` are ported below (same scenario,
// same assertions in spirit); nothing from that suite is skipped. Three
// additional tests (marked below) exercise `#[should_panic]` guard-rail
// paths (invalid init range, non-power-of-two alignment) that the C suite
// cannot observe at all (its mock `panic()` calls cmocka's `fail_msg`,
// which *fails* the test rather than letting it assert the panic was the
// expected outcome) -- a strict improvement in coverage over the C
// reference, called out explicitly rather than silently claimed as
// "ported".
#[cfg(test)]
mod tests {
    use super::*;

    const ARENA_SIZE: usize = 2 * 1024 * 1024; // 2 MiB; C reference used 1 MiB.

    /// Heap-boxed backing memory for one test's allocator. Boxed so the
    /// byte range has a stable address for the test's lifetime regardless
    /// of where the `Arena` value itself lives.
    struct Arena {
        buf: std::boxed::Box<[u8]>,
    }

    impl Arena {
        fn new() -> Self {
            Self { buf: std::vec![0u8; ARENA_SIZE].into_boxed_slice() }
        }
        fn start(&self) -> u64 {
            self.buf.as_ptr() as u64
        }
        fn end(&self) -> u64 {
            self.start() + self.buf.len() as u64
        }
    }

    /// Construct a fresh, heap-pinned `EarlyAllocator` initialized over
    /// `arena`. See this module's doc above for why `Box` is load-bearing
    /// here (self-referential free-list heads).
    fn new_test_allocator(arena: &Arena) -> std::boxed::Box<EarlyAllocator> {
        let mut a = std::boxed::Box::new(EarlyAllocator {
            free_lists: [const { null_list_node() }; EARLYALLOC_ORDERS],
            current: 0,
            end: 0,
        });
        a.init(arena.start(), arena.end());
        a
    }

    /// Mirrors the C reference's `early_alloc(size)` wrapper: same default
    /// alignment (`EARLYALLOC_SMALLEST_CHUNK`) as the real
    /// `#[no_mangle] early_alloc` C-ABI function above.
    fn early_alloc(a: &mut EarlyAllocator, size: u64) -> *mut c_void {
        a.alloc(size, EARLYALLOC_SMALLEST_CHUNK)
    }

    /// Counts every chunk currently sitting in any free-list, across all
    /// orders. Read-only traversal of the real `ListNode` chain (mirrors
    /// the C reference's `list_foreach_entry` counting loops).
    fn count_free_chunks(a: &EarlyAllocator) -> usize {
        let mut total = 0usize;
        for i in 0..EARLYALLOC_ORDERS {
            let head = &a.free_lists[i] as *const ListNode as *mut ListNode;
            // SAFETY: `head` is a valid, initialized (self-looped-or-linked)
            // `ListNode` belonging to `a`, which outlives this read-only walk.
            let mut cur = unsafe { (*head).next };
            while cur != head {
                total += 1;
                cur = unsafe { (*cur).next };
            }
        }
        total
    }

    // --- test_init -----------------------------------------------------
    #[test]
    fn init_aligns_current_and_records_the_full_range_with_empty_freelists() {
        let arena = Arena::new();
        let a = new_test_allocator(&arena);

        assert!(a.current >= arena.start());
        assert!(a.current < arena.end());
        assert_eq!(a.current & (EARLYALLOC_SMALLEST_CHUNK - 1), 0);
        assert_eq!(a.end, arena.end());
        for i in 0..EARLYALLOC_ORDERS {
            let head = &a.free_lists[i] as *const ListNode as *mut ListNode;
            assert!(FreeList(head).is_empty(), "free list {i} should start empty");
        }
    }

    // --- test_small_alloc_basic -----------------------------------------
    #[test]
    fn small_allocations_are_self_aligned_and_do_not_overlap() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let p1 = early_alloc(&mut a, 64) as u64;
        let p2 = early_alloc(&mut a, 128) as u64;
        let p3 = early_alloc(&mut a, 32) as u64;

        assert_ne!(p1, 0);
        assert_ne!(p2, 0);
        assert_ne!(p3, 0);
        assert_eq!(p1 & 63, 0);
        assert_eq!(p2 & 127, 0);
        assert_eq!(p3 & 31, 0);

        assert!(p1 + 64 <= p2 || p2 + 128 <= p1);
        assert!(p1 + 64 <= p3 || p3 + 32 <= p1);
        assert!(p2 + 128 <= p3 || p3 + 32 <= p2);
    }

    // --- test_chunk_splitting --------------------------------------------
    #[test]
    fn allocations_after_freeing_a_gap_are_served_from_the_freelist_and_do_not_overlap() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let initial_chunks = count_free_chunks(&a);

        let p1 = early_alloc(&mut a, 256);
        assert_ne!(p1 as u64, 0);

        let gap_start = align_up(a.current, 64);
        let gap_end = gap_start + 2048;
        a.free_region(gap_start, gap_end);

        let after_free_chunks = count_free_chunks(&a);
        assert!(after_free_chunks > initial_chunks);

        let p2 = early_alloc(&mut a, 64) as u64;
        let p3 = early_alloc(&mut a, 64) as u64;
        assert_ne!(p2, 0);
        assert_ne!(p3, 0);
        assert!(p2 + 64 <= p3 || p3 + 64 <= p2);
    }

    // --- test_large_alloc_alignment ---------------------------------------
    #[test]
    fn large_allocations_respect_the_caller_supplied_alignment() {
        const PAGE_SIZE: u64 = 4096;
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let p1 = a.alloc(128 * 1024, PAGE_SIZE) as u64;
        assert_ne!(p1, 0);
        assert_eq!(p1 & (PAGE_SIZE - 1), 0);

        let p2 = a.alloc(256 * 1024, 8192) as u64;
        assert_ne!(p2, 0);
        assert_eq!(p2 & (8192 - 1), 0);

        assert!(p1 + 128 * 1024 <= p2);
    }

    // --- test_small_alloc_ignores_user_alignment --------------------------
    #[test]
    fn small_allocations_ignore_the_caller_supplied_alignment_and_use_chunk_size_instead() {
        const PAGE_SIZE: u64 = 4096;
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        // 128 bytes is well within `EARLYALLOC_LARGEST_CHUNK`, so this must
        // come from the self-aligned freelist/bump path, not a page-aligned
        // one -- even though `PAGE_SIZE` alignment was requested.
        let p = a.alloc(128, PAGE_SIZE) as u64;
        assert_ne!(p, 0);
        assert_eq!(p & 127, 0);
    }

    // --- test_alignment_gap_recycling --------------------------------------
    #[test]
    fn a_large_aligned_allocation_recycles_its_leading_gap_into_the_freelists() {
        const PAGE_SIZE: u64 = 4096;
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let p1 = early_alloc(&mut a, 100) as u64;
        assert_ne!(p1, 0);

        let p2 = a.alloc(128 * 1024, PAGE_SIZE) as u64;
        assert_ne!(p2, 0);
        assert_eq!(p2 & (PAGE_SIZE - 1), 0);

        assert!(count_free_chunks(&a) > 0);
    }

    // --- test_end_ptr_tracking ----------------------------------------------
    #[test]
    fn the_current_pointer_advances_past_every_allocation_made_so_far() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let start = a.current;
        let alloc1 = early_alloc(&mut a, 1024) as u64;
        let alloc2 = early_alloc(&mut a, 2048) as u64;
        let end = a.current;

        assert!(end > start);
        assert!(end >= alloc1 + 1024);
        assert!(end >= alloc2 + 2048);
    }

    // --- test_multiple_small_from_freelist -----------------------------------
    #[test]
    fn many_small_allocations_from_a_freed_region_are_all_unique_and_aligned() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let old_current = a.current;
        a.current = old_current + 4096;
        a.free_region(old_current, a.current);

        let mut ptrs = std::vec::Vec::new();
        for _ in 0..10 {
            let p = early_alloc(&mut a, 64) as u64;
            assert_ne!(p, 0);
            assert_eq!(p & 63, 0);
            ptrs.push(p);
        }

        for i in 0..ptrs.len() {
            for j in (i + 1)..ptrs.len() {
                assert_ne!(ptrs[i], ptrs[j]);
            }
        }
    }

    // --- test_size_rounding ---------------------------------------------------
    #[test]
    fn allocation_sizes_round_up_to_the_next_power_of_two_chunk() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let p1 = early_alloc(&mut a, 100) as u64; // -> 128
        assert_ne!(p1, 0);
        assert_eq!(p1 & 127, 0);

        let p2 = early_alloc(&mut a, 200) as u64; // -> 256
        assert_ne!(p2, 0);
        assert_eq!(p2 & 255, 0);

        let p3 = early_alloc(&mut a, 10) as u64; // -> 32 (minimum chunk)
        assert_ne!(p3, 0);
        assert_eq!(p3 & 31, 0);
    }

    // --- test_zero_size -----------------------------------------------------
    #[test]
    fn zero_size_allocation_returns_null_without_touching_the_freelists() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let before = count_free_chunks(&a);
        let p = early_alloc(&mut a, 0);
        assert!(p.is_null());
        assert_eq!(count_free_chunks(&a), before);
    }

    // --- test_chunk_magic -----------------------------------------------------
    #[test]
    fn freed_chunks_carry_the_documented_magic_number() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let old_current = a.current;
        a.current = old_current + 1024;
        a.free_region(old_current, a.current);

        let mut checked_any = false;
        for i in 0..EARLYALLOC_ORDERS {
            let head = &a.free_lists[i] as *const ListNode as *mut ListNode;
            // SAFETY: read-only walk of `a`'s own freelist chain, `a` is
            // alive for the whole test.
            let mut cur = unsafe { (*head).next };
            while cur != head {
                let chunk = (cur as *mut u8).wrapping_sub(LIST_ENTRY_OFFSET) as *mut Chunk;
                assert_eq!(unsafe { (*chunk).magic }, EARLYALLOC_CHUNK_MAGIC);
                checked_any = true;
                cur = unsafe { (*cur).next };
            }
        }
        assert!(checked_any, "the freed region should have produced at least one chunk");
    }

    // --- test_stress_many_allocations --------------------------------------
    #[test]
    fn one_hundred_mixed_size_allocations_all_stay_within_the_arena_and_are_aligned() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);
        let sizes = [32u64, 64, 128, 256, 512, 1024, 2048];

        for i in 0..100 {
            let size = sizes[i % sizes.len()];
            let p = early_alloc(&mut a, size) as u64;
            assert_ne!(p, 0);
            let order = size_to_order(size);
            let actual_size = 1u64 << order;
            assert_eq!(p & (actual_size - 1), 0);
        }
        assert!(a.current <= a.end);
    }

    // --- test_chunk_alignment_verification -----------------------------------
    #[test]
    fn every_free_chunk_is_aligned_to_its_own_power_of_two_size() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);

        let old_current = a.current;
        a.current = old_current + 8192;
        a.free_region(old_current, a.current);

        let mut checked_any = false;
        for i in 0..EARLYALLOC_ORDERS {
            let head = &a.free_lists[i] as *const ListNode as *mut ListNode;
            // SAFETY: read-only walk of `a`'s own freelist chain.
            let mut cur = unsafe { (*head).next };
            while cur != head {
                let chunk = (cur as *mut u8).wrapping_sub(LIST_ENTRY_OFFSET) as *mut Chunk;
                let addr = chunk as u64;
                let size = unsafe { (*chunk).size } as u64;
                assert_eq!(addr & (size - 1), 0, "chunk not aligned to its own size");
                assert_eq!(size & (size - 1), 0, "chunk size is not a power of two");
                checked_any = true;
                cur = unsafe { (*cur).next };
            }
        }
        assert!(checked_any, "the freed region should have produced at least one chunk");
    }

    // --- Additional guard-rail coverage (beyond the C reference; see this
    // module's doc for why the C suite cannot observe these itself) --------

    #[test]
    #[should_panic(expected = "invalid memory range")]
    fn init_with_end_at_or_before_start_panics() {
        let mut a = EarlyAllocator {
            free_lists: [const { null_list_node() }; EARLYALLOC_ORDERS],
            current: 0,
            end: 0,
        };
        a.init(0x2000, 0x1000);
    }

    #[test]
    #[should_panic(expected = "alignment must be a power of 2")]
    fn alloc_with_a_non_power_of_two_alignment_panics() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);
        a.alloc(64, 3);
    }

    #[test]
    #[should_panic(expected = "out of memory")]
    fn allocating_past_the_end_of_the_arena_panics() {
        let arena = Arena::new();
        let mut a = new_test_allocator(&arena);
        let remaining = a.end - a.current;
        // One allocation larger than everything left in the arena.
        a.alloc(remaining + EARLYALLOC_LARGEST_CHUNK, EARLYALLOC_LARGEST_CHUNK);
    }
}
