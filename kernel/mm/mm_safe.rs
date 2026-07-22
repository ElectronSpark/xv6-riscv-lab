//! Lifetime-bound, RAII-style safe wrappers around the mm crate's
//! C-ABI page/slab/kmm allocators.
//!
//! The xv6 mm subsystem is fundamentally pointer-based: every allocator
//! (`page_alloc` / `slab_alloc` / `kmm_alloc`) returns a raw pointer and
//! the caller is responsible for an eventual matching `free`. From C
//! that's the only option; from Rust we can do much better.
//!
//! This module introduces lifetime-bound *handles* whose `Drop` impl
//! returns the resource to its owning allocator, and whose lifetime
//! parameter ties the handle to a borrow of that allocator. This
//! mirrors the `FrameTracker` / `MappedPages` pattern used in
//! rCore-tutorial and Theseus OS:
//!
//! * **`BuddyAllocator`** — ZST representing the global page allocator;
//!   the static instance [`BUDDY`] is what callers borrow from.
//! * **`PageHandle<'pool>`** — owned page run with `Drop = page_free`,
//!   parameterised by the lifetime of the borrow of [`BUDDY`] that
//!   produced it.
//! * **`SlabCacheRef<'a>`** — borrowed view of a `SlabCache`; the only
//!   way to obtain a [`SlabBox`].
//! * **`SlabBox<'cache, T>`** — typed slab object whose `Drop` calls
//!   `slab_free` and which cannot outlive its cache.
//!
//! None of these change the existing `extern "C"` surface used by the
//! C kernel — they are additional, Rust-only entry points. The intent
//! is that *new* Rust code in this crate uses the handles, and over
//! time the internal helpers migrate to them.
//!
//! # WP4 status (mm refactor plan)
//!
//! `PageHandle` is wired into `vm.rs`'s file-backed fault path:
//! `vma_fault_file_page` owns the freshly allocated page through every
//! early-return (cache miss / read error), and `vma_validate` re-wraps
//! the pointer `xv6_vm_call_vma_fault` hands back so the race-loser
//! branch's cleanup is a `Drop` instead of a manual `page_free`; the
//! race-winner and the two `vma_fault_file_page` success paths call
//! `into_raw()` where ownership moves to the page table / the caller.
//! `SlabBox` is wired into `pcache.rs`'s `pcache_page_alloc`: the node
//! allocation's error path (page alloc failing) is now a `Drop`, and
//! `into_raw()` fires once the node is linked into the returned page's
//! `pcache.pcache_node` field (a C-visible pointer walked from many
//! other sites).
//!
//! `KBox<T>` was deleted in the WP4 pass: no site in `kernel/mm/*.rs`
//! needed a single-fixed-`T`-value heap box. The one live internal
//! `kmm_alloc` caller outside this file's own FFI declarations
//! (`slab.rs`'s per-slab bitmap buffer) allocates a runtime-sized `[u64]`
//! rather than a `T`, doesn't fit `KBox`'s shape, and already has its
//! own local RAII rollback guards (`PageRollback`/`DescRollback` in
//! `Slab::make`) predating this module. Re-add `KBox` if a real
//! fixed-size `kmm_alloc` site turns up.
//!
//! A handful of `PageHandle`/`SlabBox` accessors (`as_page`,
//! `as_bytes[_mut]`, `byte_len`, `page_count`, `physical_address`,
//! `SlabCacheRef::alloc` for the non-`MaybeUninit` case, ...) still have
//! no caller — kept as prepared API surface for the next site rather
//! than deleted, hence the crate-wide `#![allow(dead_code)]` below
//! staying in place for this file.
//!
//! WP5 deleted this file's `PreemptGuard`, `SpinGuard`, and `PerCpu`
//! types: they were exact-duplicate reimplementations of
//! `crate::machine::PreemptGuard` (used crate-wide) and
//! `crate::sync::KSpinlock`/`KSpinGuard` (the RAII lock type all other
//! `mm` submodules already standardised on in WP1); `PerCpu` only
//! existed to borrow from the now-deleted local `PreemptGuard`. Keeping
//! two live implementations of the same RAII primitive would have been
//! a correctness hazard (e.g. code accidentally taking the "wrong" spin
//! guard type), so they are gone rather than merely marked dead.

#![allow(dead_code)]

use core::ffi::c_void;
use core::marker::PhantomData;
use core::mem::MaybeUninit;
use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;

use crate::mm::page::{Page, PAGE_BUDDY_MAX_ORDER, PAGE_SIZE};
use crate::mm::slab::SlabCache;

// ---------------------------------------------------------------------------
// FFI surface
//
// The page / slab / kmm cross-Rust helpers below operate on `Page` /
// `SlabCache` types owned by their respective modules and stay declared
// locally to avoid a circular dependency on those types from `cffi`.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;

    // `crate::mm::page::{__page_alloc,__page_free,__page_to_pa,__pa_to_page}`
    // and `crate::mm::slab::{slab_alloc,slab_free}` are all genuinely
    // `unsafe fn` (`pub(crate)` since P3-D3a — no `#[no_mangle]` export
    // surface remains anywhere in the mm cluster); this
    // module's original extern declaration asserted `pub safe fn` (usual
    // FFI-facade convention) and, unlike most other consumers in this
    // crate, already used the exact canonical `Page`/`SlabCache` types
    // (this file imports them directly), so only the safety facade needs
    // preserving here, no pointer-type cast.
    /// SAFETY: see [`crate::mm::page::__page_alloc`]'s contract.
    pub fn __page_alloc(order: u64, flags: u64) -> *mut Page {
        unsafe { crate::mm::page::Page::__page_alloc(order, flags) }
    }
    /// SAFETY: `page` must originate from `__page_alloc` above.
    pub fn __page_free(page: *mut Page, order: u64) {
        unsafe { crate::mm::page::Page::__page_free(page, order) };
    }
    /// SAFETY: `page` must be a live `Page`.
    pub fn __page_to_pa(page: *mut Page) -> u64 {
        unsafe { crate::mm::page::Page::__page_to_pa(page) }
    }
    /// SAFETY: see [`crate::mm::page::__pa_to_page`]'s contract.
    pub fn __pa_to_page(physical: u64) -> *mut Page {
        unsafe { crate::mm::page::Page::__pa_to_page(physical) }
    }
    /// SAFETY: `cache` must be a live `SlabCache`.
    pub fn slab_alloc(cache: *mut SlabCache) -> *mut c_void {
        unsafe { crate::mm::slab::slab_alloc(cache) }
    }
    /// SAFETY: `obj` must originate from `slab_alloc` above.
    pub fn slab_free(obj: *mut c_void) {
        unsafe { crate::mm::slab::slab_free(obj) };
    }
}

// ===========================================================================
// BuddyAllocator + PageHandle
// ===========================================================================

/// Zero-sized handle to the global buddy page allocator. The lifetime
/// of every [`PageHandle`] is tied to a borrow of this allocator.
///
/// The singleton instance is [`BUDDY`].
pub struct BuddyAllocator {
    // Prevent external construction.
    _private: (),
}

/// The one and only buddy allocator instance.
///
/// Callers obtain page allocations via `BUDDY.alloc(order, flags)`,
/// receiving a [`PageHandle`] whose lifetime is bound to the borrow of
/// `BUDDY`. Because `BUDDY` is `'static`, in practice the bound is
/// `'static` too, but the *type-level* link means an API like
/// `fn alloc_for<'a>(_b: &'a BuddyAllocator) -> PageHandle<'a>`
/// composes correctly when you do want a tighter scope (e.g., a unit
/// test passing a `&BuddyAllocator` through a function).
pub static BUDDY: BuddyAllocator = BuddyAllocator { _private: () };

impl BuddyAllocator {
    /// Allocate `2^order` contiguous pages with the given page flags.
    /// Returns `None` if the allocator is out of memory or the order
    /// exceeds [`PAGE_BUDDY_MAX_ORDER`].
    #[inline]
    pub fn alloc(&self, order: u64, flags: u64) -> Option<PageHandle<'_>> {
        if order > PAGE_BUDDY_MAX_ORDER {
            return None;
        }
        let raw = ffi::__page_alloc(order, flags);
        NonNull::new(raw).map(|page| PageHandle {
            page,
            order,
            _pool: PhantomData,
        })
    }
}

/// Owned page run produced by [`BuddyAllocator::alloc`]. Dropping the
/// handle returns the pages via `__page_free`.
///
/// The `'pool` lifetime ties the handle to the borrow of the
/// allocator. Once `into_raw` is called the page becomes the caller's
/// responsibility (e.g., when installed into a page table that will
/// later free it).
#[must_use = "page allocations leak unless freed via the handle's Drop or into_raw"]
pub struct PageHandle<'pool> {
    page: NonNull<Page>,
    order: u64,
    _pool: PhantomData<&'pool BuddyAllocator>,
}

// SAFETY: pages live in a kernel-wide pool, not hart-local storage;
// the handle owns exclusive access to its page run (enforced by
// `NonNull` + no `Clone`/`Copy`), so moving it to another hart for
// freeing is sound — matches what `__page_free` accepts (any hart may
// call it for any live page).
unsafe impl<'pool> Send for PageHandle<'pool> {}

impl<'pool> PageHandle<'pool> {
    /// Borrow the head [`Page`] descriptor.
    #[inline]
    pub fn as_page(&self) -> &Page {
        // SAFETY: We own the allocation for the duration of `&self`.
        unsafe { self.page.as_ref() }
    }

    /// Mutably borrow the head [`Page`] descriptor.
    #[inline]
    pub fn as_page_mut(&mut self) -> &mut Page {
        // SAFETY: We own the allocation for the duration of `&mut self`.
        unsafe { self.page.as_mut() }
    }

    /// Raw `*mut Page` pointer (head). Does **not** transfer ownership.
    #[inline]
    pub fn as_ptr(&self) -> *mut Page {
        self.page.as_ptr()
    }

    /// Order: this handle owns `2^order` pages.
    #[inline]
    pub fn order(&self) -> u64 {
        self.order
    }

    /// Number of pages owned by this handle.
    #[inline]
    pub fn page_count(&self) -> u64 {
        1u64 << self.order
    }

    /// Total byte length of the page run.
    #[inline]
    pub fn byte_len(&self) -> usize {
        ((PAGE_SIZE as usize) << self.order) as usize
    }

    /// Physical address of the first byte.
    #[inline]
    pub fn physical_address(&self) -> u64 {
        ffi::__page_to_pa(self.page.as_ptr())
    }

    /// Direct-mapped kernel virtual pointer to the data region.
    #[inline]
    pub fn data_ptr(&self) -> *mut u8 {
        self.physical_address() as *mut u8
    }

    /// Borrow the page data as a `[u8]` slice.
    ///
    /// # Safety
    /// The caller must ensure no other code is concurrently mutating
    /// the page (e.g., DMA in flight or another CPU holding a mapping).
    #[inline]
    pub unsafe fn as_bytes(&self) -> &[u8] {
        core::slice::from_raw_parts(self.data_ptr(), self.byte_len())
    }

    /// Mutable byte view. Same safety contract as [`as_bytes`].
    #[inline]
    pub unsafe fn as_bytes_mut(&mut self) -> &mut [u8] {
        core::slice::from_raw_parts_mut(self.data_ptr(), self.byte_len())
    }

    /// Relinquish ownership *without* freeing. The returned pointer
    /// becomes the caller's responsibility to eventually pass to
    /// `__page_free` (or install in a page table that will).
    #[inline]
    pub fn into_raw(self) -> *mut Page {
        let p = self.page.as_ptr();
        core::mem::forget(self);
        p
    }

    /// Re-wrap a previously-`into_raw`'d pointer.
    ///
    /// # Safety
    /// `ptr` must be the result of a prior `into_raw` (or equivalent
    /// `__page_alloc(order, _)`), and must not yet have been freed.
    #[inline]
    pub unsafe fn from_raw(ptr: *mut Page, order: u64) -> Option<Self> {
        NonNull::new(ptr).map(|page| Self {
            page,
            order,
            _pool: PhantomData,
        })
    }

    /// Re-wrap a page identified by its *data pointer* / physical
    /// address (the `page_alloc`/`page_free(void*, order)` C-ABI, used
    /// e.g. by `xv6_vm_call_vma_fault` and its callees), rather than by
    /// the `Page` descriptor pointer `from_raw` expects.
    ///
    /// Returns `None` if `pa` does not map to a tracked page (e.g. a
    /// bogus pointer from a misbehaving `.fault` callback) — nothing to
    /// take ownership of in that case.
    ///
    /// # Safety
    /// `pa` must be a live, order-`order` page owned by the buddy
    /// allocator that has not yet been freed (i.e. the result of a
    /// prior `page_alloc(order, _)`, or a value handed back by a fault
    /// handler that itself allocated the page that way).
    #[inline]
    pub unsafe fn from_pa(pa: *mut c_void, order: u64) -> Option<Self> {
        let raw = ffi::__pa_to_page(pa as u64);
        NonNull::new(raw).map(|page| Self {
            page,
            order,
            _pool: PhantomData,
        })
    }
}

impl<'pool> Drop for PageHandle<'pool> {
    #[inline]
    fn drop(&mut self) {
        ffi::__page_free(self.page.as_ptr(), self.order);
    }
}

// ===========================================================================
// SlabCacheRef + SlabBox
// ===========================================================================

/// Borrowed handle to a `SlabCache`. The only way to obtain a
/// [`SlabBox`] is through this type, so a slab object cannot outlive
/// the `&'a SlabCache` it came from.
#[derive(Copy, Clone)]
pub struct SlabCacheRef<'a> {
    cache: NonNull<SlabCache>,
    _borrow: PhantomData<&'a SlabCache>,
}

impl<'a> SlabCacheRef<'a> {
    /// Wrap a raw cache pointer.
    ///
    /// # Safety
    /// The cache must be initialised, live for at least `'a`, and not
    /// be concurrently destroyed.
    #[inline]
    pub unsafe fn from_raw(cache: &'a SlabCache) -> Self {
        Self {
            cache: NonNull::from(cache),
            _borrow: PhantomData,
        }
    }

    /// Allocate one zero-initialised object of type `T` from the cache.
    ///
    /// Returns `None` if the cache cannot satisfy the request. The
    /// returned [`SlabBox`] cannot outlive `'a`.
    ///
    /// # Safety
    /// The caller must ensure `size_of::<T>() <= cache.obj_size` and
    /// `align_of::<T>() <= cache.alignment`. There is no run-time check
    /// (the cache stores its object size opaquely from Rust's view).
    /// Additionally, `T` must be valid when every byte is zero (this
    /// function hands back all-zero-bits memory without running any
    /// `T`-specific initializer): sound for plain-old-data structs and
    /// integer/array types, unsound for `bool`, `char`, `NonZero*`,
    /// references, or enums whose discriminant `0` is not a valid
    /// variant. Prefer [`alloc_uninit`](Self::alloc_uninit) followed by
    /// explicit field initialization for any `T` that is not
    /// known-zeroable.
    pub unsafe fn alloc<T>(self) -> Option<SlabBox<'a, T>> {
        let raw = ffi::slab_alloc(self.cache.as_ptr());
        let ptr = NonNull::new(raw)?.cast::<T>();
        Some(SlabBox {
            ptr,
            _cache: PhantomData,
        })
    }

    /// Allocate an uninitialised `MaybeUninit<T>` slot.
    ///
    /// # Safety
    /// Same size/alignment contract as [`alloc`](Self::alloc).
    pub unsafe fn alloc_uninit<T>(self) -> Option<SlabBox<'a, MaybeUninit<T>>> {
        let raw = ffi::slab_alloc(self.cache.as_ptr());
        let ptr = NonNull::new(raw)?.cast::<MaybeUninit<T>>();
        Some(SlabBox {
            ptr,
            _cache: PhantomData,
        })
    }
}

/// Owned object allocated from a slab cache. `Drop` returns the object
/// via `slab_free`. The `'cache` lifetime prevents the box from
/// outliving the cache borrow that produced it.
#[must_use = "slab allocations leak unless freed via the handle's Drop"]
pub struct SlabBox<'cache, T: ?Sized> {
    ptr: NonNull<T>,
    _cache: PhantomData<&'cache SlabCache>,
}

impl<'cache, T> SlabBox<'cache, MaybeUninit<T>> {
    /// Promote `SlabBox<MaybeUninit<T>>` to `SlabBox<T>` after init.
    ///
    /// # Safety
    /// The `T` value must have been fully initialised.
    #[inline]
    pub unsafe fn assume_init(self) -> SlabBox<'cache, T> {
        let raw = self.ptr.as_ptr() as *mut T;
        core::mem::forget(self);
        SlabBox {
            ptr: NonNull::new_unchecked(raw),
            _cache: PhantomData,
        }
    }
}

impl<'cache, T: ?Sized> SlabBox<'cache, T> {
    #[inline]
    pub fn as_ptr(&self) -> *mut T {
        self.ptr.as_ptr()
    }

    /// Release without freeing. Caller assumes ownership of the raw
    /// pointer and must eventually pass it to `slab_free`.
    #[inline]
    pub fn into_raw(self) -> *mut T {
        let p = self.ptr.as_ptr();
        core::mem::forget(self);
        p
    }
}

impl<'cache, T: ?Sized> Deref for SlabBox<'cache, T> {
    type Target = T;
    #[inline]
    fn deref(&self) -> &T {
        // SAFETY: we own the allocation while `self` lives.
        unsafe { self.ptr.as_ref() }
    }
}

impl<'cache, T: ?Sized> DerefMut for SlabBox<'cache, T> {
    #[inline]
    fn deref_mut(&mut self) -> &mut T {
        // SAFETY: we own the allocation while `self` lives.
        unsafe { self.ptr.as_mut() }
    }
}

impl<'cache, T: ?Sized> Drop for SlabBox<'cache, T> {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: we own the allocation; T's destructor runs first.
        unsafe {
            core::ptr::drop_in_place(self.ptr.as_ptr());
            ffi::slab_free(self.ptr.as_ptr() as *mut c_void);
        }
    }
}

// ===========================================================================
// Compile-time documentation of the lifetime guarantees
// ===========================================================================
//
// The patterns below intentionally fail to compile and are commented
// out; they document what the lifetime parameters actually prevent.
//
//     // Cannot leak a PageHandle past the BuddyAllocator borrow:
//     fn bad<'a>() -> PageHandle<'a> {
//         let b = BuddyAllocator { _private: () };
//         b.alloc(0, 0).unwrap()  // ERROR: `b` does not live long enough
//     }
//
//     // Cannot retain a SlabBox after dropping its cache borrow:
//     fn outlive_cache(c: &SlabCache) -> SlabBox<'static, u64> {
//         let r = { SlabCacheRef::from_raw(c) };
//         { r.alloc::<u64>() }.unwrap()  // ERROR: lifetime mismatch
//     }
