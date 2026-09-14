//! Owned page runs and typed slab allocations at the raw allocator boundary.
//!
//! `PageHandle` owns a page run until it is transferred into a page table or
//! returned to the buddy allocator. `SlabCacheRef<T>` validates the immutable
//! cache geometry before allocating `MaybeUninit<T>` slots; `SlabBox` runs an
//! initialized value's destructor before returning its storage to the cache.
//!
//! Adopting raw pages or cache pointers remains unsafe: the caller establishes
//! exclusive allocation ownership or pins the cache for the chosen lifetime.
//! No shared reference to the concurrently mutable cache is constructed here.
//! These handles do not repair the allocator's separate internal aliasing and
//! synchronization contracts.

#![deny(unsafe_op_in_unsafe_fn)]

use core::alloc::Layout;
use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::marker::PhantomData;
use core::mem::MaybeUninit;
use core::ops::{Deref, DerefMut};
use core::ptr::NonNull;

#[cfg(not(test))]
use crate::mm::page::{Page, PAGE_BUDDY_MAX_ORDER, PAGE_SIZE};
#[cfg(not(test))]
use crate::mm::slab::{slab_alloc, slab_free, SlabCache};
#[cfg(test)]
use test_backend::{slab_alloc, slab_free, Page, SlabCache, PAGE_BUDDY_MAX_ORDER, PAGE_SIZE};

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
        // SAFETY: allocation begins after the global buddy allocator is initialized.
        let raw = unsafe { Page::__page_alloc(order, flags) };
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
        // SAFETY: the handle owns a live page allocation.
        unsafe { Page::__page_to_pa(self.page.as_ptr()) }
    }

    /// Direct-mapped kernel virtual pointer to the data region.
    #[inline]
    pub fn data_ptr(&self) -> *mut u8 {
        self.physical_address() as *mut u8
    }

    /// Borrow the page data as a `[u8]` slice.
    ///
    /// # Safety
    /// All bytes must be initialized, and no other accessor (including DMA
    /// and mapped users) may mutate them for the returned borrow's lifetime.
    #[inline]
    pub unsafe fn as_bytes(&self) -> &[u8] {
        // SAFETY: the caller supplies initialization and shared-access guarantees.
        unsafe { core::slice::from_raw_parts(self.data_ptr(), self.byte_len()) }
    }

    /// Mutable byte view.
    ///
    /// # Safety
    /// All bytes must be initialized, and the caller must exclude every other
    /// accessor, including DMA and mapped users, for the returned borrow.
    #[inline]
    pub unsafe fn as_bytes_mut(&mut self) -> &mut [u8] {
        // SAFETY: the caller supplies initialization and exclusive-access guarantees.
        unsafe { core::slice::from_raw_parts_mut(self.data_ptr(), self.byte_len()) }
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
    /// `__page_alloc(order, _)`), with the exact allocation order. Ownership
    /// must be transferred exclusively: no second owner, active mapping, DMA,
    /// or borrowed reference may remain when this handle frees the run.
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
    /// handler that itself allocated the page that way). It must be the head
    /// address, with the exact allocation order. Ownership is transferred
    /// exclusively; outstanding mappings, DMA, and references must end before
    /// the handle can free the run.
    #[inline]
    pub unsafe fn from_pa(pa: *mut c_void, order: u64) -> Option<Self> {
        // SAFETY: the caller transfers the specified live page run.
        let raw = unsafe { Page::__pa_to_page(pa as u64) };
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
        // SAFETY: this handle owns the allocation at its recorded order.
        unsafe { Page::__page_free(self.page.as_ptr(), self.order) };
    }
}

// ===========================================================================
// SlabCacheRef + SlabBox
// ===========================================================================

/// A typed allocation capability for a live, externally pinned slab cache.
///
/// The unsafe constructor establishes the cache lifetime; this type does not
/// borrow the cache's mutable metadata. Size and alignment are checked once,
/// so subsequent allocation can safely return uninitialized `T` storage.
pub struct SlabCacheRef<'cache, T> {
    cache: NonNull<SlabCache>,
    _lifetime: PhantomData<&'cache UnsafeCell<SlabCache>>,
    _value: PhantomData<fn() -> T>,
}

/// Every slot starts at page_base + offset + index * stride. The allocator
/// guarantees page alignment, so all three terms must preserve T's alignment.
fn layout_fits_slot(layout: Layout, stride: usize, offset: usize) -> bool {
    stride != 0
        && layout.size() <= stride
        && layout.align() <= PAGE_SIZE as usize
        && offset % layout.align() == 0
        && stride % layout.align() == 0
}

impl<'cache, T> SlabCacheRef<'cache, T> {
    /// Validate a raw cache's geometry for `T`; reject null or incompatible
    /// cache pointers without allocating an object.
    ///
    /// # Safety
    /// A nonnull pointer must name an initialized cache. Its geometry must
    /// remain immutable and its allocation must remain live throughout
    /// `'cache`, including every allocation returned through this capability.
    /// The caller supplies that lifetime; this function does not create a
    /// cache owner or synchronize destruction. The global allocators must
    /// already be initialized.
    pub unsafe fn from_raw(cache: *mut SlabCache) -> Option<Self> {
        let cache = NonNull::new(cache)?;
        // SAFETY: the caller pins the cache and its immutable geometry.
        let (stride, offset) = unsafe { SlabCache::object_geometry(cache.as_ptr()) };
        layout_fits_slot(Layout::new::<T>(), stride, offset).then_some(Self {
            cache,
            _lifetime: PhantomData,
            _value: PhantomData,
        })
    }

    /// Allocate one uninitialized slot. Dropping it before initialization
    /// releases the storage without running a `T` destructor.
    pub fn alloc_uninit(&self) -> Option<SlabBox<'cache, MaybeUninit<T>>> {
        // SAFETY: construction validated geometry and established cache life.
        let ptr = NonNull::new(unsafe { slab_alloc(self.cache.as_ptr()) })?.cast();
        Some(SlabBox {
            ptr,
            _cache: PhantomData,
        })
    }
}

/// Owned object allocated from a slab cache. `Drop` returns the object
/// via `slab_free`. The `'cache` lifetime retains the unsafe constructor's
/// requirement that the backing cache remain live, without borrowing its
/// concurrently mutable metadata.
#[must_use = "slab allocations leak unless freed via the handle's Drop"]
pub struct SlabBox<'cache, T: ?Sized> {
    ptr: NonNull<T>,
    _cache: PhantomData<&'cache UnsafeCell<SlabCache>>,
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
            // SAFETY: the original owning pointer was nonnull.
            ptr: unsafe { NonNull::new_unchecked(raw) },
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
            slab_free(self.ptr.as_ptr() as *mut c_void);
        }
    }
}

// Only allocation is substituted in host tests. Geometry checks, initialization,
// ownership transfers, dereferencing, and destruction use the production code.
#[cfg(test)]
mod test_backend {
    use super::*;
    use std::alloc::{alloc, dealloc};
    use std::boxed::Box;
    use std::cell::{Cell, RefCell};
    use std::collections::HashMap;

    pub const PAGE_SIZE: u64 = 4096;
    pub const PAGE_BUDDY_MAX_ORDER: u64 = 10;

    std::thread_local! {
        static OBJECTS: RefCell<HashMap<usize, Layout>> = RefCell::new(HashMap::new());
        static PAGES: RefCell<HashMap<usize, *mut Page>> = RefCell::new(HashMap::new());
    }

    pub struct SlabCache {
        pub stride: usize,
        pub offset: usize,
        pub fail: Cell<bool>,
    }

    impl SlabCache {
        pub fn new(stride: usize, offset: usize) -> Self {
            Self {
                stride,
                offset,
                fail: Cell::new(false),
            }
        }

        pub unsafe fn object_geometry(cache: *const Self) -> (usize, usize) {
            // SAFETY: the test's stack-owned cache outlives the capability.
            unsafe { ((*cache).stride, (*cache).offset) }
        }
    }

    pub unsafe fn slab_alloc(cache: *mut SlabCache) -> *mut c_void {
        // SAFETY: the tested capability carries the cache lifetime contract.
        let cache = unsafe { &*cache };
        if cache.fail.get() {
            return core::ptr::null_mut();
        }
        let layout = Layout::from_size_align(cache.stride, PAGE_SIZE as usize).unwrap();
        // SAFETY: the layout is nonzero and every allocation is tracked below.
        let raw = unsafe { alloc(layout) };
        if !raw.is_null() {
            // Deliberately dirty storage: typed allocation promises no zeroing.
            unsafe { raw.write_bytes(0xa5, cache.stride) };
            OBJECTS.with(|objects| {
                assert!(objects.borrow_mut().insert(raw as usize, layout).is_none())
            });
        }
        raw.cast()
    }

    pub unsafe fn slab_free(raw: *mut c_void) {
        let layout = OBJECTS
            .with(|objects| objects.borrow_mut().remove(&(raw as usize)))
            .expect("slab pointer must have one live owner");
        // SAFETY: remove above enforces one matching free for the allocation.
        unsafe { dealloc(raw.cast(), layout) };
    }

    pub fn live_objects() -> usize {
        OBJECTS.with(|objects| objects.borrow().len())
    }

    pub struct Page {
        data: *mut u8,
        order: u64,
    }

    impl Page {
        pub unsafe fn __page_alloc(order: u64, _flags: u64) -> *mut Self {
            let layout =
                Layout::from_size_align((PAGE_SIZE << order) as usize, PAGE_SIZE as usize).unwrap();
            // SAFETY: the nonzero layout is retained by the Page's order.
            let data = unsafe { alloc(layout) };
            if data.is_null() {
                return core::ptr::null_mut();
            }
            let page = Box::into_raw(Box::new(Self { data, order }));
            PAGES.with(|pages| assert!(pages.borrow_mut().insert(data as usize, page).is_none()));
            page
        }

        pub unsafe fn __page_free(page: *mut Self, order: u64) {
            // SAFETY: each test transfers a uniquely owned page into its handle.
            let page = unsafe { Box::from_raw(page) };
            assert_eq!(page.order, order);
            assert!(PAGES
                .with(|pages| pages.borrow_mut().remove(&(page.data as usize)))
                .is_some());
            let layout =
                Layout::from_size_align((PAGE_SIZE << order) as usize, PAGE_SIZE as usize).unwrap();
            // SAFETY: order and allocation ownership were verified above.
            unsafe { dealloc(page.data, layout) };
        }

        pub unsafe fn __page_to_pa(page: *mut Self) -> u64 {
            // SAFETY: the PageHandle owns this live descriptor.
            unsafe { (*page).data as u64 }
        }

        pub unsafe fn __pa_to_page(address: u64) -> *mut Self {
            PAGES
                .with(|pages| pages.borrow().get(&(address as usize)).copied())
                .unwrap_or(core::ptr::null_mut())
        }
    }

    pub fn live_pages() -> usize {
        PAGES.with(|pages| pages.borrow().len())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::Cell;
    use std::panic::{catch_unwind, AssertUnwindSafe};

    std::thread_local! {
        static DROPS: Cell<usize> = const { Cell::new(0) };
    }

    struct DropProbe(u64);
    impl Drop for DropProbe {
        fn drop(&mut self) {
            assert_eq!(
                test_backend::live_objects(),
                1,
                "storage must remain live during T::drop"
            );
            DROPS.set(DROPS.get() + 1);
        }
    }

    #[test]
    fn geometry_checks_size_offset_stride_and_page_alignment() {
        let aligned = Layout::from_size_align(64, 64).unwrap();
        assert!(layout_fits_slot(aligned, 128, 128));
        assert!(!layout_fits_slot(aligned, 32, 128));
        assert!(!layout_fits_slot(aligned, 128, 80));
        assert!(!layout_fits_slot(aligned, 80, 128));
        let over_aligned = Layout::from_size_align(8192, 8192).unwrap();
        assert!(!layout_fits_slot(over_aligned, 8192, 0));
        assert!(!layout_fits_slot(Layout::new::<()>(), 0, 0));
    }

    #[test]
    fn typed_cache_rejects_null_and_incompatible_geometry_before_allocation() {
        // SAFETY: null is an explicitly supported input.
        assert!(unsafe { SlabCacheRef::<u64>::from_raw(core::ptr::null_mut()) }.is_none());
        let mut cache = SlabCache::new(64, 0);
        // SAFETY: the stack cache remains live and unchanged for this check.
        assert!(unsafe { SlabCacheRef::<[u8; 65]>::from_raw(&raw mut cache) }.is_none());
        assert_eq!(test_backend::live_objects(), 0);
    }

    #[test]
    fn uninitialized_slot_drop_frees_storage_without_dropping_t() {
        DROPS.set(0);
        let mut cache = SlabCache::new(64, 0);
        // SAFETY: the cache outlives both the capability and its allocation.
        let typed = unsafe { SlabCacheRef::<DropProbe>::from_raw(&raw mut cache) }.unwrap();
        let slot = typed.alloc_uninit().unwrap();
        assert_eq!(test_backend::live_objects(), 1);
        drop(slot);
        assert_eq!(DROPS.get(), 0);
        assert_eq!(test_backend::live_objects(), 0);
    }

    #[test]
    fn initialized_value_is_dropped_once_before_storage_release() {
        DROPS.set(0);
        let mut cache = SlabCache::new(64, 0);
        // SAFETY: the cache outlives both the capability and its allocation.
        let typed = unsafe { SlabCacheRef::<DropProbe>::from_raw(&raw mut cache) }.unwrap();
        let mut slot = typed.alloc_uninit().unwrap();
        slot.write(DropProbe(73));
        // SAFETY: MaybeUninit::write just initialized the full value.
        let mut value = unsafe { slot.assume_init() };
        assert_eq!(value.0, 73);
        value.0 = 91;
        assert_eq!(value.0, 91);
        drop(value);
        assert_eq!(DROPS.get(), 1);
        assert_eq!(test_backend::live_objects(), 0);
    }

    #[test]
    fn allocation_failure_preserves_cache_and_leaves_no_owned_storage() {
        let mut cache = SlabCache::new(64, 0);
        cache.fail.set(true);
        // SAFETY: the cache lives until the capability and any allocations end.
        let typed = unsafe { SlabCacheRef::<u64>::from_raw(&raw mut cache) }.unwrap();
        assert!(typed.alloc_uninit().is_none());
        assert_eq!(test_backend::live_objects(), 0);
        cache.fail.set(false);
        assert!(typed.alloc_uninit().is_some());
        assert_eq!(test_backend::live_objects(), 0);
    }

    #[test]
    fn raw_transfer_defers_destruction_to_the_new_owner() {
        DROPS.set(0);
        let mut cache = SlabCache::new(64, 0);
        // SAFETY: the cache remains live until manual teardown below.
        let typed = unsafe { SlabCacheRef::<DropProbe>::from_raw(&raw mut cache) }.unwrap();
        let mut slot = typed.alloc_uninit().unwrap();
        slot.write(DropProbe(42));
        // SAFETY: the value was fully initialized above.
        let raw = unsafe { slot.assume_init() }.into_raw();
        assert_eq!(DROPS.get(), 0);
        assert_eq!(test_backend::live_objects(), 1);
        // SAFETY: into_raw transferred exclusive ownership to this test.
        unsafe {
            core::ptr::drop_in_place(raw);
            slab_free(raw.cast());
        }
        assert_eq!(DROPS.get(), 1);
        assert_eq!(test_backend::live_objects(), 0);
    }

    #[test]
    fn early_unwind_drops_initialized_value_and_allocation() {
        DROPS.set(0);
        let mut cache = SlabCache::new(64, 0);
        // SAFETY: the cache outlives the closure and its owned allocation.
        let typed = unsafe { SlabCacheRef::<DropProbe>::from_raw(&raw mut cache) }.unwrap();
        let result = catch_unwind(AssertUnwindSafe(|| {
            let mut slot = typed.alloc_uninit().unwrap();
            slot.write(DropProbe(1));
            // SAFETY: write initialized the value.
            let _value = unsafe { slot.assume_init() };
            panic!("exercise rollback");
        }));
        assert!(result.is_err());
        assert_eq!(DROPS.get(), 1);
        assert_eq!(test_backend::live_objects(), 0);
    }

    #[test]
    fn page_transfer_and_adoption_preserve_order_and_single_ownership() {
        assert!(BUDDY.alloc(PAGE_BUDDY_MAX_ORDER + 1, 0).is_none());
        let page = BUDDY.alloc(2, 0).unwrap();
        assert_eq!(page.page_count(), 4);
        let data = page.data_ptr();
        let raw = page.into_raw();
        assert_eq!(test_backend::live_pages(), 1);
        // SAFETY: the prior owner transferred this exact order-2 allocation.
        let page = unsafe { PageHandle::from_pa(data.cast(), 2) }.unwrap();
        assert_eq!(page.as_ptr(), raw);
        drop(page);
        assert_eq!(test_backend::live_pages(), 0);
    }
}
