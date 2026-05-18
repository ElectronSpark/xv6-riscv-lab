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
    pub use crate::mm::cffi::raw::{
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

#[no_mangle]
pub unsafe extern "C" fn early_alloc(size: usize) -> *mut c_void {
    early_alloc_align(size, EARLYALLOC_SMALLEST_CHUNK as usize)
}

#[no_mangle]
pub unsafe extern "C" fn early_alloc_end_ptr() -> *mut c_void {
    allocator().current as *mut c_void
}
