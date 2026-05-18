//! Lifetime-bound, RAII-style safe wrappers around the mm crate's
//! C-ABI allocators.
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
//! * **`PreemptGuard`** — RAII for `push_off` / `pop_off`. `!Send` so
//!   it cannot escape the originating CPU.
//! * **`SpinGuard<'l>`** — RAII spinlock guard borrowing `&'l Spinlock`.
//! * **`PageLockGuard<'p>`** — borrows `&'p mut Page`, holds the
//!   page-private spinlock for the duration of the borrow.
//! * **`BuddyAllocator`** — ZST representing the global page allocator;
//!   the static instance [`BUDDY`] is what callers borrow from.
//! * **`PageHandle<'pool>`** — owned page run with `Drop = page_free`,
//!   parameterised by the lifetime of the borrow of [`BUDDY`] that
//!   produced it.
//! * **`SlabCacheRef<'a>`** — borrowed view of a `SlabCache`; the only
//!   way to obtain a [`SlabBox`].
//! * **`SlabBox<'cache, T>`** — typed slab object whose `Drop` calls
//!   `slab_free` and which cannot outlive its cache.
//! * **`KBox<T>`** — typed wrapper over `kmm_alloc`/`kmm_free` (no
//!   cache lifetime since kmm is process-global).
//!
//! None of these change the existing `extern "C"` surface used by the
//! C kernel — they are additional, Rust-only entry points. The intent
//! is that *new* Rust code in this crate uses the handles, and over
//! time the internal helpers migrate to them.

#![allow(dead_code)]

use core::ffi::{c_int, c_void};
use core::marker::PhantomData;
use core::mem::{ManuallyDrop, MaybeUninit};
use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;

use crate::mm::cffi::Spinlock;
use crate::mm::page::{Page, PAGE_BUDDY_MAX_ORDER, PAGE_SIZE};
use crate::mm::slab::SlabCache;

// ---------------------------------------------------------------------------
// FFI surface
//
// Shared kernel primitives (`xv6_push_off`, `spin_lock`, `kmm_alloc`,
// …) come from the crate-wide `crate::mm::cffi` mirror, so this module no
// longer redeclares them. The page / slab cross-Rust helpers
// (`__page_alloc`, `slab_alloc`) operate on `Page` / `SlabCache` types
// owned by their respective modules and stay declared locally to avoid
// a circular dependency on those types from `cffi`.
// ---------------------------------------------------------------------------
mod ffi {
    use super::*;
    pub use crate::mm::cffi::raw::{
        xv6_push_off, xv6_pop_off, xv6_cpuid,
        spin_lock, spin_unlock,
        kmm_alloc, kmm_free,
    };
    unsafe extern "C" {
        pub safe fn __page_alloc(order: u64, flags: u64) -> *mut Page;
        pub safe fn __page_free(page: *mut Page, order: u64);
        pub safe fn __page_to_pa(page: *mut Page) -> u64;

        pub safe fn slab_alloc(cache: *mut SlabCache) -> *mut c_void;
        pub safe fn slab_free(obj: *mut c_void);
    }
}

// ===========================================================================
// PreemptGuard — RAII push_off/pop_off
// ===========================================================================

/// Disables preemption (`push_off`) on construction; re-enables it
/// (`pop_off`) on drop. `!Send` because the push/pop counter lives in
/// the originating CPU's per-cpu area — moving the guard to another
/// hart would underflow that CPU's counter and overflow the other's.
///
/// Use this when you need to safely touch per-CPU state. The lifetime
/// of any per-CPU borrow can be tied to `&PreemptGuard` so the borrow
/// checker prevents you from holding the borrow past the guard's drop.
///
/// ```ignore
/// let g = PreemptGuard::new();
/// let cache = pcpu_cache(&g, order);  // borrow tied to `g`
/// // ... use cache ...
/// drop(g); // preemption re-enabled here
/// ```
#[must_use = "PreemptGuard is dropped immediately if unused, defeating its purpose"]
pub struct PreemptGuard {
    // Raw pointer makes the type automatically `!Send` and `!Sync`.
    _not_send: PhantomData<*const ()>,
}

impl PreemptGuard {
    #[inline]
    pub fn new() -> Self {
        ffi::xv6_push_off();
        Self { _not_send: PhantomData }
    }

    /// CPU id, valid only while this guard is alive (preemption off).
    #[inline]
    pub fn cpuid(&self) -> u32 {
        ffi::xv6_cpuid() as u32
    }
}

impl Drop for PreemptGuard {
    #[inline]
    fn drop(&mut self) {
        ffi::xv6_pop_off();
    }
}

// ===========================================================================
// SpinGuard — RAII spinlock guard
// ===========================================================================

/// RAII guard for a kernel `spinlock_t`. The guard borrows the lock
/// (`&'l Spinlock`) so the borrow checker prevents the lock storage
/// from being moved / freed while held.
///
/// Note: `spin_lock` already calls `push_off` internally on xv6, so a
/// separate `PreemptGuard` is not required while a `SpinGuard` is
/// alive.
#[must_use = "SpinGuard releases the lock when dropped"]
pub struct SpinGuard<'l> {
    lock: *mut Spinlock,
    _borrow: PhantomData<&'l Spinlock>,
    _not_send: PhantomData<*const ()>,
}

impl<'l> SpinGuard<'l> {
    /// # Safety
    /// The lock storage must outlive `'l` and must be properly
    /// initialised via `spin_init`.
    #[inline]
    pub unsafe fn new(lock: &'l Spinlock) -> Self {
        let p = lock as *const Spinlock as *mut Spinlock;
        ffi::spin_lock(p);
        Self {
            lock: p,
            _borrow: PhantomData,
            _not_send: PhantomData,
        }
    }
}

impl<'l> Drop for SpinGuard<'l> {
    #[inline]
    fn drop(&mut self) {
        ffi::spin_unlock(self.lock);
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

// Pages live in a kernel-wide pool; the handle can be sent to another
// hart for freeing (matches what `__page_free` accepts).
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
// KBox<T> — typed RAII over kmm_alloc / kmm_free
// ===========================================================================

/// Owned object allocated from the generic kernel heap (`kmm_alloc`).
/// Unlike [`SlabBox`] there is no per-cache lifetime — `kmm` is a
/// process-global allocator — but the wrapper still provides typed
/// access and a `Drop` that calls `kmm_free`.
#[must_use = "kmm allocations leak unless freed via the handle's Drop"]
pub struct KBox<T: ?Sized> {
    ptr: NonNull<T>,
}

unsafe impl<T: ?Sized + Send> Send for KBox<T> {}
unsafe impl<T: ?Sized + Sync> Sync for KBox<T> {}

impl<T> KBox<T> {
    /// Allocate and initialise `value` on the kernel heap.
    pub fn try_new(value: T) -> Option<Self> {
        let raw = ffi::kmm_alloc(core::mem::size_of::<T>());
        let ptr = NonNull::new(raw)?.cast::<T>();
        // SAFETY: ptr points to writable memory of size_of::<T>() bytes.
        unsafe { core::ptr::write(ptr.as_ptr(), value) };
        Some(Self { ptr })
    }

    /// Allocate an uninitialised slot.
    pub fn try_new_uninit() -> Option<KBox<MaybeUninit<T>>> {
        let raw = ffi::kmm_alloc(core::mem::size_of::<MaybeUninit<T>>());
        let ptr = NonNull::new(raw)?.cast::<MaybeUninit<T>>();
        Some(KBox { ptr })
    }
}

impl<T> KBox<MaybeUninit<T>> {
    /// # Safety
    /// The `T` value must have been fully initialised.
    #[inline]
    pub unsafe fn assume_init(self) -> KBox<T> {
        let raw = self.ptr.as_ptr() as *mut T;
        core::mem::forget(self);
        KBox {
            ptr: NonNull::new_unchecked(raw),
        }
    }
}

impl<T: ?Sized> KBox<T> {
    #[inline]
    pub fn as_ptr(&self) -> *mut T {
        self.ptr.as_ptr()
    }

    #[inline]
    pub fn into_raw(self) -> *mut T {
        let p = self.ptr.as_ptr();
        core::mem::forget(self);
        p
    }

    /// # Safety
    /// `ptr` must come from a prior `KBox::into_raw` or a compatible
    /// `kmm_alloc` of at least `size_of_val(ptr)` bytes.
    #[inline]
    pub unsafe fn from_raw(ptr: *mut T) -> Option<Self> {
        NonNull::new(ptr).map(|ptr| Self { ptr })
    }
}

impl<T: ?Sized> Deref for KBox<T> {
    type Target = T;
    #[inline]
    fn deref(&self) -> &T {
        unsafe { self.ptr.as_ref() }
    }
}

impl<T: ?Sized> DerefMut for KBox<T> {
    #[inline]
    fn deref_mut(&mut self) -> &mut T {
        unsafe { self.ptr.as_mut() }
    }
}

impl<T: ?Sized> Drop for KBox<T> {
    #[inline]
    fn drop(&mut self) {
        unsafe {
            core::ptr::drop_in_place(self.ptr.as_ptr());
            ffi::kmm_free(self.ptr.as_ptr() as *mut c_void);
        }
    }
}

// ===========================================================================
// Per-CPU borrow tied to PreemptGuard
// ===========================================================================

/// View of per-CPU state whose lifetime is bound by a [`PreemptGuard`].
///
/// Because the guard is `!Send`, and `PerCpu<'_, T>` borrows from it,
/// the borrow checker prevents the per-cpu reference from leaking past
/// `pop_off()` *or* being smuggled to a different hart.
pub struct PerCpu<'g, T: ?Sized> {
    inner: NonNull<T>,
    _guard: PhantomData<&'g PreemptGuard>,
    _not_send: PhantomData<*const ()>,
}

impl<'g, T: ?Sized> PerCpu<'g, T> {
    /// Wrap a raw per-cpu pointer obtained while `guard` is held.
    ///
    /// # Safety
    /// `ptr` must point to per-CPU data for the CPU that owns `guard`,
    /// and must remain valid as long as preemption stays disabled.
    #[inline]
    pub unsafe fn new(_guard: &'g PreemptGuard, ptr: *mut T) -> Option<Self> {
        NonNull::new(ptr).map(|inner| Self {
            inner,
            _guard: PhantomData,
            _not_send: PhantomData,
        })
    }
}

impl<'g, T: ?Sized> Deref for PerCpu<'g, T> {
    type Target = T;
    #[inline]
    fn deref(&self) -> &T {
        unsafe { self.inner.as_ref() }
    }
}

// Note: no `DerefMut` — per-CPU access patterns generally want
// interior-mutability (atomics, locks) rather than `&mut`. If a
// caller really wants `&mut`, they can build a thin wrapper.

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
//     // Cannot move a PreemptGuard between harts (it's !Send):
//     fn move_guard() {
//         let g = PreemptGuard::new();
//         std::thread::spawn(move || drop(g));  // ERROR: !Send
//     }
//
//     // Cannot retain a SlabBox after dropping its cache borrow:
//     fn outlive_cache(c: &SlabCache) -> SlabBox<'static, u64> {
//         let r = { SlabCacheRef::from_raw(c) };
//         { r.alloc::<u64>() }.unwrap()  // ERROR: lifetime mismatch
//     }

// Suppress unused-import warnings if no caller in this crate uses
// `ManuallyDrop` directly yet.
#[allow(dead_code)]
const _: fn() = || {
    let _ = core::mem::size_of::<ManuallyDrop<PageHandle<'static>>>();
};
