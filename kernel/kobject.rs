//! Kernel object reference counting — Rust port of `kernel/kobject.c`.
//!
//! `struct kobject` / `struct kobject_ops` are hand-written native
//! `#[repr(C)]` types below (nativized in Wave P3-N3): `build.rs`
//! blocklists the bindgen-generated forms and injects
//! `pub use crate::kobject::Kobject as kobject;` (etc.) raw-line
//! re-exports, so every remaining bindgen struct that embeds a kobject
//! by value (`device.kobj`, `bio.kobj`, `inode.kobj`) and every
//! `crate::bindings::kobject` path across the crate resolves here.
//! The global registry list/lock/count are private
//! Rust statics, initialised at runtime by `kobject_global_init`, exactly
//! mirroring the C `LIST_ENTRY_INITIALIZED`/`SPINLOCK_INITIALIZED`
//! compile-time initializers (which cannot be replicated as a Rust
//! `const` because the list head is self-referential — same pattern as
//! `kernel/mm/pcache.rs`'s `PCACHE_GLOBAL_LIST`/`PCACHE_GLOBAL_SPINLOCK`).
//!
//! `list_entry_init`/`list_node_push_front`/`list_node_detach` are
//! `static inline` in `kernel/inc/list.h` (no external linkage), so they
//! are reimplemented natively here on the raw `list_node_t` pointers.
//! `kobject::list_entry` is the first field of `struct kobject`, so the
//! `list_node_t` <-> `kobject` pointer casts below are offset-0
//! reinterpretations, not `container_of` arithmetic.
//!
//! Locking uses [`crate::sync::KSpinlock`] (RAII guard) instead of
//! hand-paired `spin_lock`/`spin_unlock` calls.

#![allow(non_camel_case_types)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_void};
use core::mem::MaybeUninit;
use core::ops::Deref;
use core::ptr;
use core::ptr::NonNull;
use core::sync::atomic::{AtomicI32, AtomicI64, Ordering};

use crate::bindings::{list_node_t, spinlock_t};
use crate::list::ListNode;
use crate::sync::KSpinlock;

// ---------------------------------------------------------------------------
// Native layout — Wave P3-N3.
//
// `Kobject`/`KobjectOps` ARE the kernel-wide Rust definitions of
// `kernel/inc/kobject.h`'s `struct kobject`/`struct kobject_ops` now
// (blocklist+redirect technique per P3-N1/P3-N2, see `build.rs`).
// `list_entry` is the crate's canonical native `crate::list::ListNode`
// (== `list_node_t` via the P3-N1 redirect); `release` reproduces
// bindgen's `Option<unsafe extern "C" fn(*mut kobject)>` lowering of the
// C function pointer exactly (`*mut Kobject` == `*mut kobject` via this
// wave's redirect).
// ---------------------------------------------------------------------------
#[repr(C)]
#[derive(Copy, Clone)]
pub struct KobjectOps {
    pub release: Option<unsafe extern "C" fn(obj: *mut Kobject)>,
}

#[repr(C)]
#[derive(Copy, Clone)]
pub struct Kobject {
    pub list_entry: ListNode,
    pub refcount: core::ffi::c_int,
    pub name: *const c_char,
    pub ops: KobjectOps,
}

// P3-N3 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// kobject { list_entry: list_node_t, refcount: c_int, name: *const
// c_char, ops: kobject_ops }` / `pub struct kobject_ops { release:
// Option<unsafe extern "C" fn(*mut kobject)> }`) and independently
// confirmed by a riscv64-unknown-elf-gcc `_Static_assert` probe
// (rv64gc/lp64d) against `kernel/inc/kobject.h`: kobject 40/8,
// offsets 0/16/24/32; kobject_ops 8/8, release at 0.
const _: () = {
    assert!(core::mem::size_of::<KobjectOps>() == 8, "kobject_ops size");
    assert!(core::mem::align_of::<KobjectOps>() == 8, "kobject_ops alignment");
    assert!(core::mem::offset_of!(KobjectOps, release) == 0, "kobject_ops.release offset");
    assert!(core::mem::size_of::<Kobject>() == 40, "kobject size");
    assert!(core::mem::align_of::<Kobject>() == 8, "kobject alignment");
    assert!(core::mem::offset_of!(Kobject, list_entry) == 0, "kobject.list_entry offset");
    assert!(core::mem::offset_of!(Kobject, refcount) == 16, "kobject.refcount offset");
    assert!(core::mem::offset_of!(Kobject, name) == 24, "kobject.name offset");
    assert!(core::mem::offset_of!(Kobject, ops) == 32, "kobject.ops offset");
};

use crate::proc::proc_shims::xv6_panic;
// `kmm_free` is now `impl Kmem` (kalloc.rs KERNEL-OO wave); called
// fully-qualified at its call site below instead of `use`-imported.

/// Mirrors the C `assert(expr, fmt)` macro (`kernel/inc/printf.h`):
/// panic the kernel if `$cond` is false. Simplified to a fixed message
/// (no `printf`-style formatting) — matches the convention already used
/// by `kpanic!`/`kassert!` in `kernel/proc/thread.rs`.
macro_rules! kassert {
    ($cond:expr, $msg:expr) => {
        if !($cond) {
            xv6_panic(concat!($msg, "\0").as_ptr() as *const c_char)
        }
    };
}

// ---------------------------------------------------------------------------
// Global registry storage.
// ---------------------------------------------------------------------------

// SAFETY: `SyncCell<T>` is used only for this module's two file-scope
// statics below (`KOBJECT_LIST`, `KOBJECT_LOCK`), both of which are
// mutated exclusively while holding `kobject_lock()` (or, for the lock
// storage itself, initialised once by `kobject_global_init` before any
// other `kobject_*` call per that function's documented contract, then
// only ever touched through the C spinlock primitives' own internal
// synchronization). The blanket `impl<T>` is broader than these two
// use sites strictly need, but every current instantiation upholds the
// invariant that concurrent access is externally serialized.
#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn new(v: T) -> Self {
        SyncCell(UnsafeCell::new(v))
    }
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

static KOBJECT_LIST: SyncCell<list_node_t> =
    SyncCell::new(list_node_t { prev: ptr::null_mut(), next: ptr::null_mut() });
// `uninit()` not `zeroed()`: the storage is never read before
// `kobject_global_init` calls `spin_init` (via `kobject_lock().init()`),
// which unconditionally overwrites every field (name/locked/cpu) rather
// than reading-then-modifying — no zeroed bit pattern is ever observed.
static KOBJECT_LOCK: SyncCell<MaybeUninit<spinlock_t>> = SyncCell::new(MaybeUninit::uninit());
static KOBJECT_COUNT: AtomicI64 = AtomicI64::new(0);

impl Kobject {
    #[inline(always)]
    fn kobject_list_head() -> *mut list_node_t {
        KOBJECT_LIST.get()
    }

    #[inline(always)]
    fn kobject_lock() -> KSpinlock {
        // SAFETY: `KOBJECT_LOCK` is initialised once by `kobject_global_init`,
        // called before any other `kobject_*` entry point per the
        // `start_kernel.c` init order (mirrors every other kernel subsystem's
        // `*_global_init` contract).
        KSpinlock::from_bindings(unsafe { (*KOBJECT_LOCK.get()).as_mut_ptr() })
    }

    // -----------------------------------------------------------------------
    // Minimal intrusive-list primitives. `kernel/inc/list.h` defines these as
    // `static inline` (no external linkage), so they cannot be called from
    // Rust; reimplemented here for the two operations this file needs.
    // -----------------------------------------------------------------------

    /// Mirrors `list_entry_init()`.
    ///
    /// # Safety
    /// `e` must point to a live `list_node_t`.
    #[inline(always)]
    unsafe fn ln_init(e: *mut list_node_t) {
        unsafe {
            (*e).next = e;
            (*e).prev = e;
        }
    }

    /// Mirrors `list_entry_detach()`.
    ///
    /// # Safety
    /// `e` must point to a live, linked `list_node_t`.
    #[inline(always)]
    unsafe fn ln_detach(e: *mut list_node_t) {
        unsafe {
            let prev = (*e).prev;
            let next = (*e).next;
            (*prev).next = next;
            (*next).prev = prev;
            Kobject::ln_init(e);
        }
    }

    /// Mirrors `list_entry_push_front()` / `list_node_push_front(head, obj,
    /// list_entry)`.
    ///
    /// # Safety
    /// `head` and `e` must point to live `list_node_t`s.
    #[inline(always)]
    unsafe fn ln_push_front(head: *mut list_node_t, e: *mut list_node_t) {
        unsafe {
            let next = (*head).next;
            (*e).prev = head;
            (*e).next = next;
            (*head).next = e;
            (*next).prev = e;
        }
    }

    /// `kobject::list_entry` is the first field of `struct kobject` (offset
    /// 0), so this is a plain reinterpret cast, not `container_of` arithmetic.
    #[inline(always)]
    fn kobj_list_node(obj: *mut Kobject) -> *mut list_node_t {
        obj as *mut list_node_t
    }

    #[inline(always)]
    fn refcount_atomic<'a>(obj: *mut Kobject) -> &'a AtomicI32 {
        // SAFETY: caller ensures `obj` points to a live, aligned `struct
        // kobject`; `refcount` is a plain C `int` field, same size and
        // alignment as `AtomicI32`.
        unsafe { &*(ptr::addr_of_mut!((*obj).refcount) as *const AtomicI32) }
    }

    /// Mirrors `__kobject_attach()`.
    ///
    /// # Safety
    /// `obj` must point to a live, properly `list_entry`-initialised
    /// `struct kobject`.
    unsafe fn attach(obj: *mut Kobject) {
        let _guard = Kobject::kobject_lock().lock();
        unsafe { Kobject::ln_push_front(Kobject::kobject_list_head(), Kobject::kobj_list_node(obj)) };
        // Relaxed: `KOBJECT_COUNT` is a pure diagnostic counter (see
        // `kobject_count()`), and every mutation here already happens
        // under `kobject_lock()`, which provides the real ordering for
        // the registry list this counter tracks. No other memory access
        // is synchronized *through* this counter.
        let count = KOBJECT_COUNT.fetch_add(1, Ordering::Relaxed) + 1;
        unsafe { kassert!(count > 0, "kobject count underflow") };
    }

    /// Mirrors `__kobject_detach()`.
    ///
    /// # Safety
    /// `obj` must point to a live `struct kobject` currently attached to the
    /// global registry.
    unsafe fn detach(obj: *mut Kobject) {
        let _guard = Kobject::kobject_lock().lock();
        unsafe { Kobject::ln_detach(Kobject::kobj_list_node(obj)) };
        // Relaxed: same rationale as `attach` — diagnostic counter, real
        // ordering comes from `kobject_lock()`.
        let count = KOBJECT_COUNT.fetch_sub(1, Ordering::Relaxed) - 1;
        unsafe { kassert!(count >= 0, "kobject count underflow") };
    }

    // -----------------------------------------------------------------------
    // Public C ABI — exact symbol/signature parity with `kernel/inc/kobject.h`.
    // -----------------------------------------------------------------------

    /// # Safety
    /// Must be called exactly once, before any other `kobject_*` entry point
    /// (no concurrent access to the registry statics yet).
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now.
    pub(crate) fn kobject_global_init() {
        unsafe { Kobject::ln_init(Kobject::kobject_list_head()) };
        Kobject::kobject_lock().init(c"kobject_lock".as_ptr());
    }

    /// # Safety
    /// `obj` must be null or point to a live `struct kobject` whose
    /// `refcount` is currently zero (fresh/never-initialised, or fully
    /// dropped and being reused).
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now.
    pub(crate) unsafe fn kobject_init(obj: *mut Kobject) {
        unsafe {
            kassert!(!obj.is_null(), "kobject_init: obj is NULL");
            Kobject::ln_init(Kobject::kobj_list_node(obj));
            // Relaxed: caller's contract guarantees `obj` is either fresh
            // or fully dropped (refcount == 0, no other holder), so there
            // is no concurrent writer to synchronize with here — mirrors
            // `Arc`'s use of `Relaxed` for reference-count increments.
            let expected = Kobject::refcount_atomic(obj).fetch_add(1, Ordering::Relaxed);
            kassert!(expected == 0, "kobject_init: obj->refcount is not zero");
            Kobject::attach(obj);
        }
    }

    /// # Safety
    /// `obj` must be null or point to a live `struct kobject` with a
    /// currently-nonzero `refcount` (i.e. the caller already holds a
    /// reference).
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now.
    pub(crate) unsafe fn kobject_get(obj: *mut Kobject) {
        unsafe {
            kassert!(!obj.is_null(), "kobject_get: obj is NULL");
            // Relaxed: incrementing from an already-held reference needs no
            // ordering — the caller's existing reference is itself proof
            // the object (and anything it publishes) is already visible on
            // this hart. Standard `Arc::clone` pattern.
            let count = Kobject::refcount_atomic(obj).fetch_add(1, Ordering::Relaxed) + 1;
            kassert!(count > 0, "kobject_get: refcount underflow");
        }
    }

    /// # Safety
    /// `obj` must be null or point to a live `struct kobject`.
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now.
    pub(crate) unsafe fn kobject_try_get(obj: *mut Kobject) -> bool {
        unsafe { kassert!(!obj.is_null(), "kobject_try_get: obj is NULL") };
        let rc = Kobject::refcount_atomic(obj);
        let mut old = rc.load(Ordering::Relaxed);
        loop {
            if old == 0 {
                return false;
            }
            // Acquire on success (mirrors `std::sync::Arc::upgrade`): this
            // is a weak-style "upgrade" from a bare pointer whose liveness
            // is guaranteed by the caller's RCU read-side section, not by
            // an existing strong reference (contrast with `kobject_get`,
            // which may use `Relaxed` because it always starts from an
            // already-held reference). Acquire prevents any subsequent
            // read of `*obj`'s data from being reordered before this CAS
            // observes the object as still live, and synchronizes with the
            // `Release` in `kobject_put`'s decrement. Failure ordering is
            // `Relaxed`: a failed CAS only feeds back into the retry loop.
            match rc.compare_exchange(old, old + 1, Ordering::Acquire, Ordering::Relaxed) {
                Ok(_) => return true,
                Err(cur) => old = cur,
            }
        }
    }

    /// # Safety
    /// `obj` must be null or point to a live `struct kobject` with a
    /// currently-nonzero `refcount`. When the refcount reaches zero, `obj` is
    /// detached from the global registry and either freed via `kmm_free` (if
    /// `ops.release` is unset) or handed to `ops.release`, which must be safe
    /// to call with `obj` as its sole argument.
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now.
    pub(crate) unsafe fn kobject_put(obj: *mut Kobject) {
        unsafe {
            kassert!(!obj.is_null(), "kobject_put: obj is NULL");
            // Release: must publish every write this hart made to `*obj`
            // (and anything reachable through it) before a concurrent hart
            // can observe the count reaching zero and proceed to free it.
            // Paired with the `Acquire` fence below on the branch that
            // actually reaches zero — standard `Arc`-style refcount-drop
            // pattern.
            let count = Kobject::refcount_atomic(obj).fetch_sub(1, Ordering::Release) - 1;
            kassert!(count >= 0, "kobject_put: refcount underflow");
            if count == 0 {
                // Acquire: synchronizes with every `Release` decrement
                // (including this one and any from other harts that raced
                // to decrement first) before we detach and free `obj` —
                // without this, writes from a hart that decremented
                // concurrently (but not to zero) could still be in flight
                // when we free the object.
                core::sync::atomic::fence(Ordering::Acquire);
                Kobject::detach(obj);
                match (*obj).ops.release {
                    None => crate::mm::kalloc::Kmem::kmm_free(obj as *mut c_void),
                    Some(release) => release(obj),
                }
            }
        }
    }

    /// # Safety
    /// `obj` must be null or point to a live `struct kobject`.
    // P3-D3c: `#[no_mangle] extern "C"` dropped -- every consumer reaches it
    // by crate path now.
    pub(crate) unsafe fn kobject_refcount(obj: *mut Kobject) -> i64 {
        unsafe {
            kassert!(!obj.is_null(), "kobject_refcount: obj is NULL");
            // Relaxed: diagnostic snapshot read only (mirrors
            // `Arc::strong_count`'s use of `Relaxed`); callers needing a
            // linearizable value must serialize externally.
            Kobject::refcount_atomic(obj).load(Ordering::Relaxed) as i64
        }
    }

    pub(crate) fn kobject_count() -> i64 {
        // Relaxed: diagnostic registry-size snapshot; real synchronization
        // for the registry itself is `kobject_lock()` (see `attach`/`detach`).
        KOBJECT_COUNT.load(Ordering::Relaxed)
    }
}

// ---------------------------------------------------------------------------
// KArc<T> -- an Arc-analog over the kobject refcount (Phase 3 P3-9).
//
// Every kobject consumer to date (`dev/dev.rs`'s `device_t`, `dev/bio.rs`'s
// `bio`, `vfs/fs.rs`'s `fs_type_t`) hand-pairs `kobject_get`/`kobject_put`
// (or the `_try_get` RCU-upgrade variant) around each use, which is exactly
// the bug class `Drop` exists to make unrepresentable: an added early
// `return` after the `get` and before the matching `put` silently leaks a
// reference (see e.g. the pre-KArc `dev/cdev.rs::cdev_get` -- one `get`, one
// conditional `put`, easy to forget on a third branch). `KArc<T>` collapses
// that pattern into ordinary clone/scope-exit.
// ---------------------------------------------------------------------------

/// Types whose reference count is a `struct kobject` embedded somewhere
/// within them. [`KArc<T>`] uses this to locate the refcount to
/// increment/decrement without needing a bespoke `container_of` helper at
/// every call site.
///
/// # Safety
/// `kobj_ptr(this)` must return a pointer to a live, `kobject_init`-ed
/// `Kobject` reachable from `*this` for the entire lifetime of `*this` --
/// i.e. a stable, non-moving field of `T`. Every current implementor
/// embeds it at offset 0 (a plain reinterpret cast, matching the
/// `container_of(obj, T, kobj)` idiom this crate already relies on in
/// `kernel/dev/dev.rs`/`kernel/dev/bio.rs`/`kernel/vfs/fs.rs`), but that
/// is a per-implementor choice, not part of this trait's contract.
pub unsafe trait HasKobject {
    fn kobj_ptr(this: *mut Self) -> *mut Kobject;
}

/// Per-type "run when the refcount reaches zero" behavior -- the Rust-side
/// analog of the raw C `kobject_ops.release` function pointer.
///
/// This is *not* wired into every `KArc` automatically: `kobject_put`
/// (which `KArc`'s `Drop` calls) always dispatches through whatever is
/// installed in `obj->ops.release` at `kobject_init` time -- several
/// existing consumers (`dev/dev.rs`'s `underlying_kobject_release`) do
/// real work there beyond a plain free (table unregistration) and must
/// keep doing so. `KobjectRelease` plus [`karc_release_trampoline`] is
/// offered so a *new*, pure-Rust `KArc`-only type can hand a monomorphic
/// trampoline to `ops.release` instead of hand-writing an `extern "C" fn`
/// shim (see `dev/bio.rs`'s `bio_release_kobj_cb` for the shape being
/// replaced) -- adoption is opt-in per type, not required by `KArc`
/// itself.
pub trait KobjectRelease: HasKobject {
    fn release(self_ptr: *mut Self);
}

/// `kobject_ops.release`-shaped trampoline for [`KobjectRelease`]: install
/// this (`Some(karc_release_trampoline::<T>)`) as `kobj.ops.release`
/// instead of hand-writing a per-type `extern "C" fn(*mut Kobject)` shim.
/// Only sound to install for a `T` whose `HasKobject::kobj_ptr` impl
/// returns the exact pointer `kobject_put` will pass back in here.
pub extern "C" fn karc_release_trampoline<T: KobjectRelease>(obj: *mut Kobject) {
    // SAFETY: `kobject_put` (this callback's only caller) only invokes
    // `ops.release` once the refcount it manages has reached zero, and
    // `T::kobj_ptr`'s contract (see `HasKobject`) is that `obj` is exactly
    // the pointer that function returns for the live `T` this kobject is
    // embedded in -- so reinterpreting it back to `*mut T` here recovers
    // that same live object, now uniquely owned (no other reference
    // exists once the count hit zero).
    T::release(obj as *mut T);
}

/// An `Arc`-analog over the kernel's kobject refcount. `Clone` bumps the
/// count, `Drop` decrements it and runs `T`'s release path (whatever is
/// installed in `ops.release`) once it reaches zero, `Deref` gives shared
/// access to `T`.
///
/// Built directly on [`kobject_get`]/[`kobject_put`]/[`kobject_try_get`]
/// rather than re-deriving their atomics, so it inherits their exact,
/// already-proven ordering verbatim (Relaxed increment, Release decrement
/// + Acquire fence on the zero branch, Acquire CAS for the RCU `try_get`
/// upgrade case -- see those functions' own doc comments) instead of a
/// second, independently-reasoned-about copy.
///
/// # Invariant
/// A live `KArc<T>` owns exactly one held reference on `T`'s kobject.
/// `Clone`/`Drop` are the only ways that invariant is created/destroyed
/// internally; [`KArc::from_raw`]/[`KArc::try_from_raw`] accept one from
/// the caller and [`KArc::into_raw`] hands one back out -- the same
/// contract as `Box`/`Arc`'s `from_raw`/`into_raw` pair, for the C-ABI
/// boundary where a refcounted object is passed to/from code still using
/// raw `kobject_get`/`kobject_put`/pointer calls.
pub struct KArc<T: HasKobject> {
    ptr: NonNull<T>,
}

// SAFETY: a `KArc<T>` only ever grants `&T` access (via `Deref`) to
// whichever hart holds it, and the refcount underneath is atomic --
// identical reasoning to `std::sync::Arc<T>`'s `Send`/`Sync` bounds. `T:
// Sync` is required because a clone handed to another hart lets that hart
// read `*ptr` concurrently with this one (`&T` access, gated by `Sync`);
// `T: Send` is required because the *last* `KArc<T>` to drop may run on a
// different hart than the one that created `T`, transferring ownership of
// `T` across harts (gated by `Send`) before `release` consumes it.
unsafe impl<T: HasKobject + Send + Sync> Send for KArc<T> {}
unsafe impl<T: HasKobject + Send + Sync> Sync for KArc<T> {}

impl<T: HasKobject> KArc<T> {
    /// Take ownership of an existing, already-held kobject reference.
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a live `T` whose embedded
    /// kobject currently carries a reference the caller is transferring
    /// to the returned `KArc` -- the caller must not also call
    /// `kobject_put` (or otherwise drop another `KArc`) on that same
    /// reference. This is exactly the success postcondition of
    /// `device_get()`/`bio_alloc()`/`kobject_init()`-style C-ABI
    /// constructors.
    pub unsafe fn from_raw(ptr: *mut T) -> Self {
        debug_assert!(!ptr.is_null(), "KArc::from_raw: ptr is NULL");
        // SAFETY: caller guarantees `ptr` is non-null (also debug-checked
        // above).
        KArc { ptr: unsafe { NonNull::new_unchecked(ptr) } }
    }

    /// `kobject_try_get` analog: attempt to upgrade a bare, liveness-
    /// unproven `T*` (e.g. one just read out of an RCU-protected table,
    /// with no existing reference of its own) into an owned `KArc`.
    /// Returns `None` if the refcount had already reached zero (a
    /// concurrent final `Drop`/`kobject_put` raced ahead of this call).
    ///
    /// # Safety
    /// `ptr` must be non-null and point to a `T` that is still valid to
    /// dereference for the duration of this call (e.g. still inside the
    /// RCU read-side critical section it was loaded under) -- the same
    /// precondition a raw call to `kobject_try_get` has.
    pub unsafe fn try_from_raw(ptr: *mut T) -> Option<Self> {
        debug_assert!(!ptr.is_null(), "KArc::try_from_raw: ptr is NULL");
        // SAFETY: caller guarantees `ptr` is live for the duration of
        // this call; `T::kobj_ptr`'s contract (`HasKobject`) guarantees
        // the returned pointer is a live `Kobject` for as long as `*ptr`
        // is.
        if unsafe { Kobject::kobject_try_get(T::kobj_ptr(ptr)) } {
            // SAFETY: `ptr` was just proven non-null-derived and live by
            // the successful `try_get` above.
            Some(KArc { ptr: unsafe { NonNull::new_unchecked(ptr) } })
        } else {
            None
        }
    }

    /// Give up the owned reference without decrementing the refcount,
    /// returning the raw pointer -- the flip side of [`KArc::from_raw`],
    /// for a C-ABI boundary that expects to receive one held reference as
    /// a bare pointer.
    pub fn into_raw(this: Self) -> *mut T {
        let ptr = this.ptr.as_ptr();
        core::mem::forget(this);
        ptr
    }

    /// The raw pointer this `KArc` owns a reference on, without giving up
    /// ownership (contrast [`KArc::into_raw`]).
    pub fn as_ptr(this: &Self) -> *mut T {
        this.ptr.as_ptr()
    }
}

impl<T: HasKobject> Clone for KArc<T> {
    fn clone(&self) -> Self {
        // SAFETY: struct invariant -- `self` owns a currently-held
        // reference, so `kobject_get`'s precondition (an already-held
        // reference to increment from) holds.
        unsafe { Kobject::kobject_get(T::kobj_ptr(self.ptr.as_ptr())) };
        KArc { ptr: self.ptr }
    }
}

impl<T: HasKobject> Drop for KArc<T> {
    fn drop(&mut self) {
        // SAFETY: struct invariant -- `self` owns exactly one held
        // reference, which this decrement gives up; `kobject_put` runs
        // the release path itself if this was the last one.
        unsafe { Kobject::kobject_put(T::kobj_ptr(self.ptr.as_ptr())) };
    }
}

impl<T: HasKobject> Deref for KArc<T> {
    type Target = T;
    fn deref(&self) -> &T {
        // SAFETY: struct invariant -- a live `KArc` holds a reference,
        // which (by every implementor's `kobject_put`/release contract)
        // keeps `*ptr` allocated and not exclusively mutated elsewhere
        // for as long as any reference is held -- the same sharing
        // discipline `Arc<T>`'s `Deref` relies on.
        unsafe { self.ptr.as_ref() }
    }
}
