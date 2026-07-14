//! Kernel object reference counting — Rust port of `kernel/kobject.c`.
//!
//! `struct kobject` is the real bindgen-generated `crate::bindings::kobject`
//! type (added to `wrapper.h` for this port, see `kernel/inc/kobject.h`)
//! — no opaque stand-in. The global registry list/lock/count are private
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
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicI64, Ordering};

use crate::bindings::{kobject, list_node_t, spinlock_t};
use crate::sync::KSpinlock;

pub type Kobject = kobject;

unsafe extern "C" {
    pub safe fn xv6_panic(msg: *const c_char) -> !;
}
pub(crate) use crate::kmm_free;

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

// ---------------------------------------------------------------------------
// Minimal intrusive-list primitives. `kernel/inc/list.h` defines these as
// `static inline` (no external linkage), so they cannot be called from
// Rust; reimplemented here for the two operations this file needs.
// ---------------------------------------------------------------------------

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
        ln_init(e);
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
    let _guard = kobject_lock().lock();
    unsafe { ln_push_front(kobject_list_head(), kobj_list_node(obj)) };
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
    let _guard = kobject_lock().lock();
    unsafe { ln_detach(kobj_list_node(obj)) };
    // Relaxed: same rationale as `attach` — diagnostic counter, real
    // ordering comes from `kobject_lock()`.
    let count = KOBJECT_COUNT.fetch_sub(1, Ordering::Relaxed) - 1;
    unsafe { kassert!(count >= 0, "kobject count underflow") };
}

// ---------------------------------------------------------------------------
// Public C ABI — exact symbol/signature parity with `kernel/inc/kobject.h`.
// ---------------------------------------------------------------------------

/// # Safety
/// Must be called exactly once, before any other `kobject_*` entry point
/// (no concurrent access to the registry statics yet).
#[no_mangle]
pub extern "C" fn kobject_global_init() {
    unsafe { ln_init(kobject_list_head()) };
    kobject_lock().init(c"kobject_lock".as_ptr());
}

/// # Safety
/// `obj` must be null or point to a live `struct kobject` whose
/// `refcount` is currently zero (fresh/never-initialised, or fully
/// dropped and being reused).
#[no_mangle]
pub unsafe extern "C" fn kobject_init(obj: *mut Kobject) {
    unsafe {
        kassert!(!obj.is_null(), "kobject_init: obj is NULL");
        ln_init(kobj_list_node(obj));
        // Relaxed: caller's contract guarantees `obj` is either fresh
        // or fully dropped (refcount == 0, no other holder), so there
        // is no concurrent writer to synchronize with here — mirrors
        // `Arc`'s use of `Relaxed` for reference-count increments.
        let expected = refcount_atomic(obj).fetch_add(1, Ordering::Relaxed);
        kassert!(expected == 0, "kobject_init: obj->refcount is not zero");
        attach(obj);
    }
}

/// # Safety
/// `obj` must be null or point to a live `struct kobject` with a
/// currently-nonzero `refcount` (i.e. the caller already holds a
/// reference).
#[no_mangle]
pub unsafe extern "C" fn kobject_get(obj: *mut Kobject) {
    unsafe {
        kassert!(!obj.is_null(), "kobject_get: obj is NULL");
        // Relaxed: incrementing from an already-held reference needs no
        // ordering — the caller's existing reference is itself proof
        // the object (and anything it publishes) is already visible on
        // this hart. Standard `Arc::clone` pattern.
        let count = refcount_atomic(obj).fetch_add(1, Ordering::Relaxed) + 1;
        kassert!(count > 0, "kobject_get: refcount underflow");
    }
}

/// # Safety
/// `obj` must be null or point to a live `struct kobject`.
#[no_mangle]
pub unsafe extern "C" fn kobject_try_get(obj: *mut Kobject) -> bool {
    unsafe { kassert!(!obj.is_null(), "kobject_try_get: obj is NULL") };
    let rc = refcount_atomic(obj);
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
#[no_mangle]
pub unsafe extern "C" fn kobject_put(obj: *mut Kobject) {
    unsafe {
        kassert!(!obj.is_null(), "kobject_put: obj is NULL");
        // Release: must publish every write this hart made to `*obj`
        // (and anything reachable through it) before a concurrent hart
        // can observe the count reaching zero and proceed to free it.
        // Paired with the `Acquire` fence below on the branch that
        // actually reaches zero — standard `Arc`-style refcount-drop
        // pattern.
        let count = refcount_atomic(obj).fetch_sub(1, Ordering::Release) - 1;
        kassert!(count >= 0, "kobject_put: refcount underflow");
        if count == 0 {
            // Acquire: synchronizes with every `Release` decrement
            // (including this one and any from other harts that raced
            // to decrement first) before we detach and free `obj` —
            // without this, writes from a hart that decremented
            // concurrently (but not to zero) could still be in flight
            // when we free the object.
            core::sync::atomic::fence(Ordering::Acquire);
            detach(obj);
            match (*obj).ops.release {
                None => kmm_free(obj as *mut c_void),
                Some(release) => release(obj),
            }
        }
    }
}

/// # Safety
/// `obj` must be null or point to a live `struct kobject`.
#[no_mangle]
pub unsafe extern "C" fn kobject_refcount(obj: *mut Kobject) -> i64 {
    unsafe {
        kassert!(!obj.is_null(), "kobject_refcount: obj is NULL");
        // Relaxed: diagnostic snapshot read only (mirrors
        // `Arc::strong_count`'s use of `Relaxed`); callers needing a
        // linearizable value must serialize externally.
        refcount_atomic(obj).load(Ordering::Relaxed) as i64
    }
}

pub(crate) fn kobject_count() -> i64 {
    // Relaxed: diagnostic registry-size snapshot; real synchronization
    // for the registry itself is `kobject_lock()` (see `attach`/`detach`).
    KOBJECT_COUNT.load(Ordering::Relaxed)
}
