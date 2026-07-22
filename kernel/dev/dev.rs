//! Device table core -- Rust port of `kernel/dev/dev.c` (Phase 2 Wave 21;
//! see `docs/rustify/phase2_plan.md`).
//!
//! # Data structures
//!
//! `DEV_TABLE` is a fixed `[major]` array of `AtomicPtr<device_major_t>`
//! (the Rust analogue of the C `static device_major_t
//! *__dev_table[MAX_MAJOR_DEVICES]`). Each live `device_major_t` owns a
//! page-allocated `minors` array (`*mut *mut device_t`, `MAX_MINOR_DEVICES`
//! slots) -- both the major-table slots and the minor-array slots are
//! RCU-protected pointers: readers use `rcu_dereference` (mapped to an
//! `Acquire` load, matching this crate's established `__ATOMIC_CONSUME`
//! house convention, see `kernel/vfs/fdtable.rs`), writers serialize with
//! `dev_tab_lock()` (a [`crate::sync::KSpinlock`]) and publish with
//! `rcu_assign_pointer` (a `Release` store). `device_major_t` itself is
//! reclaimed via `call_rcu` once its last minor is unregistered, so a
//! concurrent reader that already dereferenced it can finish safely.
//!
//! `device_t` embeds a `struct kobject` as its *first* field, so casting
//! between `*mut kobject` and `*mut device_t` at the kobject release
//! callback is a plain offset-0 reinterpretation (mirrors the C
//! `container_of(obj, device_t, kobj)`, which is exactly that on this
//! layout) -- no arithmetic helper needed, same reasoning as
//! `kernel/kobject.rs`'s own `kobj_list_node`.
//!
//! # Mandated fix 1 -- `/dev/tty` minor-0 bug
//!
//! `device_register()` treats `dev->minor == 0` as "auto-assign the
//! lowest free minor" (`dev_slot_get`'s scan loop). The C original found
//! the assigned minor (`ret_minor`) but never wrote it back into
//! `dev->minor` before calling `devtmpfs_create_node()`, which reads
//! `dev->minor` directly to build the `dev_t` (`mkdev(dev->major,
//! dev->minor)`) recorded in the devtmpfs node. For `tty_dev` (registered
//! with `minor = 0`, see `kernel/tty/tty_dev.rs`'s `TTY_DEV_MINOR`), this
//! meant the devtmpfs node for `/dev/tty` was created encoding minor 0 --
//! but `device_get()` unconditionally rejects `minor <= 0` (0 is reserved
//! as the "auto-assign" sentinel, never a real device identity), so an
//! `open("/dev/tty")` would resolve the node, then fail with `-EINVAL`
//! trying to look the device back up.
//!
//! Fixed here by writing the real assigned minor back into `dev->minor`
//! (see `device_register`'s `// FIX 1` comment) before it is used for
//! `mkdev()` and before the device is published to readers. This also
//! fixes `devtmpfs_populate_devices()`'s retroactive-registration path
//! (`kernel/vfs/devtmpfs/superblock.rs`'s `__devtmpfs_register_one_device`),
//! which reads `(*dev).minor` directly from the struct field -- so the
//! field write is required, not just a local-variable fix to the
//! immediate `mkdev()` call. `device_get()`'s `minor <= 0` rejection is
//! *not* changed: it was already the correct invariant (0 is the
//! sentinel, real minors start at 1, see `dev_slot_get`'s scan starting
//! at `i = 1`) -- the bug was purely that `dev->minor` never reflected
//! the real assigned value anywhere a caller could observe it.
//!
//! # Mandated fix 2 -- silent devtmpfs failure
//!
//! `device_register()` called `devtmpfs_create_node()` and discarded its
//! return value entirely. Fixed by checking it and `printf`-ing a warning
//! on failure (see `device_register`'s `// FIX 2` comment) rather than
//! silently losing the diagnostic.
//!
//! **Contract choice**: log, don't propagate as an error return. Every
//! caller of `device_register()` today (`cdev_register`/`blkdev_register`
//! in this same wave, `dev_test.c`'s stress tests, and every real driver
//! registering a cdev/blkdev) treats a non-zero return as "the device
//! itself failed to register" -- several (`kernel/tty/tty_dev.rs`,
//! `kernel/console.rs`, `kernel/tty/ptmx.rs`) `assert`/panic on a non-zero
//! return. The device-table registration (the primary contract) can
//! succeed independently of the optional devtmpfs integration: the device
//! is fully usable via `device_get(major, minor)` even if its `/dev` node
//! never materializes, and a later `devtmpfs_populate_devices()` call
//! (see `kernel/vfs/devtmpfs/superblock.rs`) retries node creation for
//! exactly this class of device. Turning a devtmpfs hiccup into a hard
//! `device_register()` failure would conflate two independent outcomes
//! and newly crash every one of those `assert`-on-failure callers for a
//! problem that isn't theirs. A `printf` warning makes the failure
//! visible (the acceptance criterion) without changing that established
//! contract. Note this only catches the registry-buffering half of
//! `devtmpfs_create_node()`'s own contract (`kernel/vfs/devtmpfs/
//! superblock.rs`'s module doc, "C-ABI surface kept" section): its
//! *live-mount* mknod path (`__devtmpfs_mknod_relative`, taken when
//! devtmpfs is already mounted at registration time) still swallows its
//! own failures into an unconditional `0` -- that return-value shape is
//! `devtmpfs_create_node()`'s own contract, out of this wave's touch
//! scope (`kernel/vfs/devtmpfs/superblock.rs`, Wave 20, already landed);
//! see the Wave-20 pts/N quirk note this wave's report re-checks.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicPtr, Ordering};

use crate::bindings::{
    device_major_t, device_t, dev_type_e, dev_type_e_DEV_TYPE_BLOCK,
    dev_type_e_DEV_TYPE_CHAR, kobject, slab_cache_t,
    spinlock_t, EALREADY, EBUSY, EINVAL, ENODEV, ENOMEM, ENOSPC, ENOTTY, SLAB_FLAG_DEBUG_BITMAP,
    SLAB_FLAG_EMBEDDED,
};
use crate::kobject::{HasKobject, KArc, Kobject};
use crate::lock::rcu::KRcuRead;
use crate::sync::KSpinlock;

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N4 (device core family).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/dev/
// dev_types.h`'s `struct device_major`/`struct device_ops`/`struct
// device_instance` now: `build.rs` blocklists the bindgen-generated
// forms and injects `pub use crate::dev::dev::... as ...;` facade
// re-exports (struct name + `_t` typedef alias each), so every
// `crate::bindings::{device_major(_t),device_instance,device_t}` path
// across the crate resolves here. P3-10c finished the P3-10 ops-table
// job: `DeviceOps` is a Rust trait (see its doc below), not a
// fn-pointer struct. The still-bindgen `rcu_head_t` is
// embedded *by value* (`DeviceMajor::rcu_head`) — the sanctioned
// mixed-tier pattern (P3-N2/N3). `dev_type_e` stays the bindgen
// constified-enum `c_uint` alias.
//
// Copy fidelity: `device_major`/`device_ops` derived Copy/Clone in the
// pre-nativization bindgen output; `device_instance` derived NEITHER
// (bindgen's derive analysis, kobject-embedder pattern — same as the
// still-bindgen `vfs_fs_type`), so `DeviceInstance` deliberately has no
// derives and `build.rs`'s callbacks answer Copy=No for it.
// ---------------------------------------------------------------------------

/// `struct device_major` (`kernel/inc/dev/dev_types.h`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct DeviceMajor {
    pub num_minors: c_int,
    pub minors: *mut *mut DeviceInstance,
    pub rcu_head: crate::bindings::rcu_head_t,
}

/// The `device_t`-level operations vtable — wave P3-10c's replacement
/// for the C-style `struct device_ops` fn-pointer table (ops-table
/// redesign, following the P3-10a `FileOps` pilot pattern).
/// Implementors are zero-sized unit structs with a `static` instance:
/// the cdev/blkdev forwarders (`CDEV_DEVICE_OPS` in
/// `kernel/dev/cdev.rs`, `BLKDEV_DEVICE_OPS` in
/// `kernel/dev/blkdev.rs`, installed by `cdev_register`/
/// `blkdev_register` — the old fixed "underlying ops" vtables) and the
/// console's post-registration ioctl override (`CONSOLE_DEV_OPS` in
/// `kernel/console.rs`).
///
/// Slot-nullability mapping:
///
/// * `open`/`release` — required methods: the old `dev_opts_validate`
///   rejected registration unless both were `Some`. The console's
///   post-registration override historically bypassed that validation
///   with `None` slots — its implementor now spells out the exact
///   `None`-slot dispatch outcomes instead (see `ConsoleDevOps`):
///   `open` has *no dispatch site anywhere in the tree* (a `None` slot
///   was never callable), and a `None` `release` made
///   [`DevTable::call_release`]'s kassert panic.
/// * `ioctl` — default `None` ("op not provided"): [`DeviceInstance::ioctl`] keeps
///   its exact `neg(ENOTTY)` fallback for it (the old `None`-slot
///   behavior; blkdevs, whose old table left `.ioctl` `None`, simply
///   don't override the default).
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait DeviceOps: Sync {
    /// Device open hook.
    ///
    /// NOTE (dead-slot truth, preserved from the C): no code path in
    /// the tree dispatches device-level `open` today — `DeviceInstance::get`
    /// only takes a reference. The method is kept because the
    /// registrant-facing contract (`cdev`/`blkdev` open forwarding)
    /// remains the documented shape of the device table.
    ///
    /// # Safety
    /// `dev` must be the live, registered `device_t` this instance was
    /// installed on.
    unsafe fn open(&self, dev: *mut DeviceInstance) -> KResult<()>;

    /// Final-teardown hook, invoked (result discarded, matching the C)
    /// by the kobject release callback once the refcount reaches zero.
    ///
    /// # Safety
    /// Same contract as [`DeviceOps::open`]; the caller holds the final
    /// reference.
    unsafe fn release(&self, dev: *mut DeviceInstance) -> KResult<()>;

    /// Driver ioctl: `Some(result)` (the raw `c_int`, which may itself
    /// be a negative errno, passed through exactly as the old
    /// fn-pointer result was) or `None` to let [`DeviceInstance::ioctl`] fall back
    /// to `ENOTTY`.
    ///
    /// # Safety
    /// `dev` as in [`DeviceOps::open`]; `arg`'s validity is
    /// `cmd`-specific.
    unsafe fn ioctl(&self, _dev: *mut DeviceInstance, _cmd: u64, _arg: *mut c_void)
        -> Option<c_int> {
        None
    }
}

/// `struct device_instance` (typedef `device_t`) — as of wave P3-10c
/// this layout is NATIVE-OWNED (post-P3-6 there is no bindgen, no
/// wrapper.h, and no C consumer; `kernel/inc/dev/dev_types.h` no longer
/// constrains it). The former embedded `device_ops` fn-pointer table is
/// now a real Rust trait object, `Option<&'static dyn DeviceOps>` (a
/// 16-byte fat pointer; `None` = the zeroed/unregistered state — valid
/// via the null-data-pointer niche, same reasoning as P3-10b's zeroed
/// root dummy).
#[repr(C)]
pub struct DeviceInstance {
    pub kobj: Kobject,
    pub major: c_int,
    pub minor: c_int,
    pub type_: dev_type_e,
    pub unregistering: c_int,
    pub ops: Option<&'static dyn DeviceOps>,
    pub devname: *const c_char,
    pub devmode: crate::bindings::mode_t,
}

// P3-10c layout facts — NATIVE-OWNED, no C mirror exists (bindgen,
// wrapper.h, and every C consumer are gone). `DeviceMajor` (untouched
// by the ops conversion) keeps its P3-N4 pins; for `DeviceInstance`
// only the invariants the code actually leans on are kept: `kobj` at
// offset 0 (the kobject-release `container_of` reinterpretation and
// every `KArc`/`HasKobject` cast) and the niche-optimized
// `Option<&'static dyn DeviceOps>` staying a plain fat pointer (16/8,
// all-zero bytes == `None` — the zeroed-storage registration
// precondition). Deleted: the stale P3-N4 C-fidelity size/offset pins
// for the moved fields and the whole `device_ops` table block (the
// table type itself no longer exists).
const _: () = {
    assert!(core::mem::size_of::<crate::bindings::rcu_head_t>() == 40, "rcu_head_t size");
    assert!(core::mem::align_of::<crate::bindings::rcu_head_t>() == 8, "rcu_head_t alignment");
    assert!(core::mem::size_of::<DeviceMajor>() == 56, "device_major size");
    assert!(core::mem::align_of::<DeviceMajor>() == 8, "device_major alignment");
    assert!(core::mem::offset_of!(DeviceMajor, num_minors) == 0, "device_major.num_minors offset");
    assert!(core::mem::offset_of!(DeviceMajor, minors) == 8, "device_major.minors offset");
    assert!(core::mem::offset_of!(DeviceMajor, rcu_head) == 16, "device_major.rcu_head offset");
    assert!(core::mem::align_of::<DeviceInstance>() == 8, "device_instance alignment");
    assert!(core::mem::offset_of!(DeviceInstance, kobj) == 0, "device_instance.kobj offset");
    assert!(
        core::mem::size_of::<Option<&'static dyn DeviceOps>>() == 16,
        "device ops fat pointer size"
    );
    assert!(
        core::mem::align_of::<Option<&'static dyn DeviceOps>>() == 8,
        "device ops fat pointer alignment"
    );
};

// ---------------------------------------------------------------------------
// External C symbols. Declared locally rather than via bindgen's function
// allowlist, matching the established per-file convention (irq/trap.rs,
// tty/session.rs, mm/vm.rs, vfs/inode.rs, vfs/devtmpfs/superblock.rs, ...).
// ---------------------------------------------------------------------------

// P3-D3c: `kobject.rs`'s entry points are genuinely `unsafe fn`s (raw
// kobject* + caller-upheld refcount invariants) now that their
// `#[no_mangle]` exports are gone; the old plain (non-`safe`) externs
// meant every call site already sits in an `unsafe` context -- the plain
// `use` keeps them unchanged. KERNEL-OO: now reached as `Kobject::` assoc
// fns (already in scope via the `HasKobject, KArc, Kobject` import above).

// P3-D3b: lock/rcu.rs's `call_rcu` is a plain `pub(crate) unsafe fn` now
// that its `#[no_mangle]` export is gone; reached by crate path (the call
// site below already sits in an `unsafe` block).
use crate::lock::rcu::Rcu;

// P3-D3a: the mm page/slab entry points are genuinely `unsafe fn` in
// `crate::mm::{page,slab}` now that their `#[no_mangle]` exports are
// gone; this file's original extern declarations asserted `pub safe fn`
// (usual FFI facade) and typed the cache pointer as the bindgen
// `slab_cache_t` (same layout as `crate::mm::slab::SlabCache`), `name`
// as `*const c_char` (callee only reads it), and `obj_size`/`flags` as
// `u64`/`u32` rather than the real `usize`/`u64` (same-register widths
// under the old C ABI). Thin cast + safe-facade wrappers preserve all of
// this file's call-site types.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *const c_char, obj_size: u64, flags: u32,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(
            cache as *mut crate::mm::slab::SlabCache,
            name as *mut c_char,
            obj_size as usize,
            flags as u64,
        )
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}
/// SAFETY: see [`crate::mm::page::page_alloc`]'s contract.
#[inline]
fn page_alloc(order: u64, flags: u64) -> *mut c_void {
    unsafe { crate::mm::page::Page::page_alloc(order, flags) }
}
/// SAFETY: `ptr` must originate from `page_alloc` above.
#[inline]
fn page_free(ptr: *mut c_void, order: u64) {
    unsafe { crate::mm::page::Page::page_free(ptr, order) };
}
// P3-1C mesh sweep: vfs/devtmpfs/superblock.rs is in scope for this wave;
// signatures are identical (same `mode_t`/`dev_t` types), so these become
// plain crate-path imports instead of `extern "C"` redeclarations. See the
// module doc's "Mandated fix 2" section for the devtmpfs_create_node()
// return-value contract this wave checks.
use crate::vfs::devtmpfs::superblock::{devtmpfs_create_node, devtmpfs_remove_node};

// `kassert!`'s canonical homes are `crate::kstd`/crate root
// (P3-CS1 centralization); `kassert!` still panics via `xv6_panic`
// (`kernel/proc/proc_shims.rs`), just through `kstd`'s single extern
// declaration of it instead of this file's own copy. P3-CS14: `device_get`
// now builds a `KResult<*mut device_t>` internally and encodes it to the
// ABI-fixed `ERR_PTR` exactly once at the `extern "C"` boundary via
// `result_to_errptr`; only the error-return encoding changed.
use crate::kassert;
use crate::kstd::{result_to_errptr, Errno, KResult};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

const MAX_MAJOR_DEVICES: usize = 256; // kernel/inc/dev/dev_types.h
const MAX_MINOR_DEVICES: usize = 256; // kernel/inc/dev/dev_types.h
const PAGE_TYPE_ANON: u64 = 0; // mm/page_type.h: PAGE_TYPE_ANON

// P3-9: `KArc<device_t>` -- see `kernel/kobject.rs`'s `HasKobject`/`KArc`
// doc. `device_t.kobj` is the struct's first field (offset 0, see the
// module doc's opening paragraph), so this is a plain reinterpret cast,
// the same reasoning already used throughout this file's other
// `kobject`<->`device_t` casts.
// SAFETY: `device_t.kobj` is a stable, non-moving offset-0 field, live
// (`kobject_init`-ed) for the entire time any `device_t` is reachable
// through the device table or a caller's raw pointer -- exactly
// `HasKobject`'s contract.
unsafe impl HasKobject for device_t {
    fn kobj_ptr(this: *mut Self) -> *mut Kobject {
        this as *mut Kobject
    }
}

// ---------------------------------------------------------------------------
// Global registry storage. `SyncCell<T>` is redefined locally per this
// crate's established convention (see kernel/kobject.rs, mm/pcache.rs,
// tty/{tty,ptmx,session}.rs, proc/sched_fifo.rs, proc/sched_idle.rs).
// ---------------------------------------------------------------------------

#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
// SAFETY: used only for this module's two file-scope statics below
// (`DEV_TAB_LOCK`, `DEV_TYPE_CACHE`), both written exactly once by
// `DevTable::init()` (documented single-hart, single-call contract, see
// `start_kernel.c`'s init order) before any other entry point in this
// file can run, and thereafter mutated only through their own internal
// synchronization (`spin_lock`/`spin_unlock`, the slab allocator's own
// locking).
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

static DEV_TAB_LOCK: SyncCell<MaybeUninit<spinlock_t>> = SyncCell::new(MaybeUninit::uninit());
static DEV_TYPE_CACHE: SyncCell<MaybeUninit<slab_cache_t>> = SyncCell::new(MaybeUninit::uninit());

/// The major device table (`static device_major_t *__dev_table[...]` in
/// the C original). `[const { ... }; N]` array-repeat of a non-`Copy`
/// atomic, same idiom as `kernel/irq/irq_core.rs`'s `IRQ_DESCS`.
static DEV_TABLE: [AtomicPtr<device_major_t>; MAX_MAJOR_DEVICES] =
    [const { AtomicPtr::new(ptr::null_mut()) }; MAX_MAJOR_DEVICES];

// ---------------------------------------------------------------------------
// `DevTable` -- zero-sized marker hosting every table/major-level (global
// registry) operation: the device-table lock/slab-cache accessors, the
// major/minor slot reinterpretation helpers, the `device_major_t`
// lifecycle (alloc/free/validate), `slot_get`'s combined
// lookup-or-allocate, table init, the kobject-release ->
// unregister-from-table path, and the whole-table iterator. Plays the
// same namespacing role `Rcu` plays for `kernel/lock/rcu.rs`'s
// singleton-global operations and `SessionTable` plays for
// `kernel/tty/session.rs`. KERNEL-OO relocation only -- raw pointer
// params kept throughout (the device table is Freeze and read lock-free
// cross-hart via RCU, so no `&self`/`&T` receiver is introduced anywhere
// in this impl), bodies byte-identical to the old free fns, `unsafe
// extern "C"` signatures preserved exactly.
// ---------------------------------------------------------------------------

pub(crate) struct DevTable;

impl DevTable {
    #[inline(always)]
    fn tab_lock() -> KSpinlock {
        // SAFETY: `DEV_TAB_LOCK` is initialised once by `DevTable::init()`,
        // called before any other entry point in this file per that
        // function's documented contract (mirrors every other subsystem's
        // `*_init` contract in this crate, e.g. `kernel/kobject.rs`'s
        // `kobject_global_init`).
        KSpinlock::from_bindings(unsafe { (*DEV_TAB_LOCK.get()).as_mut_ptr() })
    }

    fn tab_assert_held() {
        // SAFETY: `xv6_panic` never returns; `DevTable::tab_lock().holding()`
        // reads only the lock's own internal state.
        unsafe { kassert!(Self::tab_lock().holding(), "dev_tab_lock not held") };
    }

    #[inline(always)]
    fn type_cache_ptr() -> *mut slab_cache_t {
        // SAFETY: see `DEV_TAB_LOCK`'s identical contract above.
        unsafe { (*DEV_TYPE_CACHE.get()).as_mut_ptr() }
    }

    // -----------------------------------------------------------------
    // Slot helpers: reinterpret plain C pointer-array elements as atomics
    // for RCU load/store, matching `kernel/kobject.rs`'s
    // `refcount_atomic` cast pattern (the C static type is a plain
    // pointer; `rcu_dereference`/`rcu_assign_pointer` are just atomic
    // load/store macros applied to it regardless).
    // -----------------------------------------------------------------

    #[inline(always)]
    fn major_slot(major: c_int) -> &'static AtomicPtr<device_major_t> {
        &DEV_TABLE[major as usize]
    }

    /// Reinterpret `dmajor->minors[minor]` as an atomic pointer cell.
    ///
    /// # Safety
    /// `dmajor` must be a live `device_major_t*` whose `minors` array has at
    /// least `MAX_MINOR_DEVICES` slots (guaranteed by `DevTable::type_alloc`);
    /// `minor` must be in `0..MAX_MINOR_DEVICES` (validated by
    /// `DevTable::slot_get` before any call reaches here).
    unsafe fn minor_slot_atomic<'a>(dmajor: *mut device_major_t, minor: c_int) -> &'a AtomicPtr<device_t> {
        // SAFETY: forwarded from the caller's contract above.
        unsafe {
            let minors = (*dmajor).minors;
            &*(minors.add(minor as usize) as *const AtomicPtr<device_t>)
        }
    }

    #[inline(always)]
    fn unregistering_atomic<'a>(dev: *mut device_t) -> &'a AtomicI32 {
        // SAFETY: caller ensures `dev` is a live `device_t*`; `unregistering`
        // is a plain C `int` field, same size/alignment as `AtomicI32` (same
        // pattern as `kobject.rs`'s `refcount_atomic`).
        unsafe { &*(ptr::addr_of_mut!((*dev).unregistering) as *const AtomicI32) }
    }

    // -----------------------------------------------------------------
    // device_major_t lifecycle (type_alloc / type_free / [`dev_type_rcu_free`]).
    // -----------------------------------------------------------------

    fn type_alloc() -> *mut device_major_t {
        let dev_type = slab_alloc(Self::type_cache_ptr()) as *mut device_major_t;
        if dev_type.is_null() {
            return ptr::null_mut();
        }
        let minors = page_alloc(0, PAGE_TYPE_ANON);
        if minors.is_null() {
            slab_free(dev_type as *mut c_void);
            return ptr::null_mut();
        }
        // SAFETY: `minors` is a freshly allocated, exclusively-owned page (at
        // least `MAX_MINOR_DEVICES * size_of::<*mut device_t>()` bytes, i.e.
        // 2048 of 4096 available); `dev_type` is likewise freshly allocated
        // and exclusively owned until returned to the caller.
        unsafe {
            ptr::write_bytes(
                minors as *mut u8,
                0,
                MAX_MINOR_DEVICES * core::mem::size_of::<*mut device_t>(),
            );
            ptr::write_bytes(dev_type as *mut u8, 0, core::mem::size_of::<device_major_t>());
            (*dev_type).minors = minors as *mut *mut device_t;
        }
        dev_type
    }

    fn type_free(dev_type: *mut device_major_t) {
        if dev_type.is_null() {
            return;
        }
        // SAFETY: `dev_type` is exclusively owned at every call site (either
        // unwound synchronously during allocation failure, or delivered by
        // the RCU callback after the grace period, when no reader can still
        // observe it).
        unsafe {
            let minors = (*dev_type).minors;
            if !minors.is_null() {
                page_free(minors as *mut c_void, 0);
                (*dev_type).minors = ptr::null_mut();
            }
        }
        slab_free(dev_type as *mut c_void);
    }

    fn type_validate(t: dev_type_e) -> bool {
        t == dev_type_e_DEV_TYPE_BLOCK || t == dev_type_e_DEV_TYPE_CHAR
    }

    // -----------------------------------------------------------------
    // slot_get (writer + dead reader path, see the module doc).
    // -----------------------------------------------------------------

    /// Mirrors `__dev_slot_get()`.
    ///
    /// For the writer path (`alloc = true`) the caller must already hold
    /// `DevTable::tab_lock()`. Allocates a fresh `device_major_t` if the major
    /// slot is empty, and if `minor == 0` scans for the first free minor
    /// (the auto-assignment sentinel) instead of using a literal minor.
    ///
    /// The reader path (`alloc = false`) is preserved for fidelity with the C
    /// original but is presently unreachable: `DeviceInstance::register()`
    /// and `DevTable::unregister_from_table()`, the only two call sites
    /// (both in the C original and in this port), always pass `alloc = true`.
    ///
    /// Returns `Ok((dmajor, minor))` on success, `Err(-errno)` on failure.
    fn slot_get(major: c_int, minor: c_int, alloc: bool) -> Result<(*mut device_major_t, c_int), c_int> {
        if alloc {
            Self::tab_assert_held();
        }
        if major <= 0 || major as usize >= MAX_MAJOR_DEVICES || minor < 0 || minor as usize >= MAX_MINOR_DEVICES {
            return Err(neg(EINVAL));
        }

        let slot = Self::major_slot(major);
        let mut dmajor = if alloc {
            // Writer path: serialized by `DevTable::tab_lock()` (asserted
            // above); a plain relaxed load is sufficient here (matches the
            // C's direct, non-atomic `dmajor = __dev_table[major];` read) --
            // publication to concurrent RCU readers is the `Release` store
            // below/at `DeviceInstance::register`, not this read.
            slot.load(Ordering::Relaxed)
        } else {
            // Reader path (dead today, see doc above): `rcu_dereference`
            // (`__ATOMIC_CONSUME` -> `Acquire`, house mapping).
            slot.load(Ordering::Acquire)
        };

        if dmajor.is_null() {
            if !alloc {
                return Err(neg(ENODEV)); // device type not found
            }
            dmajor = Self::type_alloc();
            if dmajor.is_null() {
                return Err(neg(ENOMEM));
            }
            // rcu_assign_pointer: publish with Release.
            slot.store(dmajor, Ordering::Release);
        }

        let mut assigned_minor = minor;
        if minor == 0 {
            if !alloc {
                return Err(neg(EINVAL)); // minor number 0 is invalid for lookup
            }
            // Scan for the first free minor slot (0 stays reserved as the
            // auto-assign sentinel). The slots are plain RCU-protected CPU
            // pointer-array cells, so this is a clean collection walk.
            assigned_minor = (1..MAX_MINOR_DEVICES)
                .find(|&i| {
                    // SAFETY: `dmajor` is live: either just-allocated above
                    // (exclusively owned until published a few lines up) or an
                    // existing entry, only ever freed after an RCU grace period
                    // once its last minor is unregistered -- both safe to scan
                    // while holding `DevTable::tab_lock()` (asserted at
                    // function entry).
                    unsafe { Self::minor_slot_atomic(dmajor, i as c_int) }.load(Ordering::Relaxed).is_null()
                })
                .map_or(0, |i| i as c_int);
            if assigned_minor == 0 {
                return Err(neg(ENOSPC)); // no available minor slots
            }
        }

        Ok((dmajor, assigned_minor))
    }

    // -----------------------------------------------------------------
    // Public C ABI.
    // -----------------------------------------------------------------

    // P3-1D mesh sweep: callers (`start_kernel.rs`, `vfs/devtmpfs/superblock.rs`)
    // now import this via crate-path `use` instead of an `extern`
    // redeclaration -- demoted.
    pub(crate) extern "C" fn init() {
        Self::tab_lock().init(c"dev_tab_lock".as_ptr());
        let ret = slab_cache_init(
            Self::type_cache_ptr(),
            c"dev_type_cache".as_ptr(),
            core::mem::size_of::<device_major_t>() as u64,
            SLAB_FLAG_EMBEDDED | SLAB_FLAG_DEBUG_BITMAP,
        );
        // SAFETY: `xv6_panic` never returns.
        unsafe { kassert!(ret == 0, "Failed to initialize device type slab cache") };
    }

    /// Because only a validated device's ops are ever installed here (see
    /// `DeviceInstance::register`'s ops check), the table is asserted rather
    /// than branched on, matching the C's own comment ("their validity is
    /// asserted to be true"). P3-10c: `release` itself is a required trait
    /// method, so the old per-slot `release.is_some()` half of the assert
    /// is unrepresentable; the driver's `KResult` is discarded exactly as
    /// the old `c_int` return was (its one caller,
    /// `underlying_kobject_release`, always dropped it).
    fn call_release(dev: *mut device_t) {
        // SAFETY: `dev` is a live device_t (kobject release contract);
        // `.ops` is a plain field read.
        let ops = unsafe { (*dev).ops };
        // SAFETY: `xv6_panic` never returns.
        unsafe { kassert!(!dev.is_null() && ops.is_some(), "Invalid device or release operation") };
        // SAFETY: `dev` is the caller-verified live pointer this callback
        // owns, holding the final reference -- exactly
        // `DeviceOps::release`'s contract; `ops` is `Some` per the assert.
        let _ = unsafe { ops.unwrap().release(dev) };
    }

    /// Unregister a device from the device table. Called either by
    /// `DeviceInstance::unregister()` or when the kobject refcount reaches
    /// 0. Uses RCU for safe removal -- the old `device_major_t`, if now
    /// empty, is freed after the grace period.
    fn unregister_from_table(dev: *mut device_t) {
        let mut to_free: *mut device_major_t = ptr::null_mut();
        {
            let _guard = Self::tab_lock().lock();
            // SAFETY: `dev` is caller-provided and live for this call.
            let (major, minor) = unsafe { ((*dev).major, (*dev).minor) };
            let dmajor = match Self::slot_get(major, minor, true) {
                Ok((dmajor, _)) => dmajor,
                Err(_) => return, // device already removed from table
            };
            // SAFETY: `dmajor`/`minor` satisfy `minor_slot_atomic`'s contract
            // (validated by `slot_get` above).
            let dev_slot = unsafe { Self::minor_slot_atomic(dmajor, minor) };
            if dev_slot.load(Ordering::Relaxed) != dev {
                return; // device already removed or replaced
            }
            // rcu_assign_pointer(*dev_slot, NULL).
            dev_slot.store(ptr::null_mut(), Ordering::Release);

            // SAFETY: `dmajor` is live and only ever mutated under
            // `DevTable::tab_lock()` (this guard).
            unsafe { (*dmajor).num_minors -= 1 };
            if unsafe { (*dmajor).num_minors } == 0 {
                // No more minors: schedule the device type for RCU-deferred
                // free.
                to_free = dmajor;
                Self::major_slot(major).store(ptr::null_mut(), Ordering::Release);
            }
        } // `_guard` drops here, unlocking `dev_tab_lock` -- matches the C's
          // explicit `__dev_tab_unlock()` before `call_rcu()`.

        if !to_free.is_null() {
            // SAFETY: `to_free` was just unpublished from `DEV_TABLE` above
            // while still holding the lock, so no *new* reader can observe
            // it; `call_rcu` defers the actual free until any reader that
            // dereferenced it earlier has finished.
            unsafe {
                Rcu::call(
                    &raw mut (*to_free).rcu_head,
                    Some(dev_type_rcu_free),
                    to_free as *mut c_void,
                );
            }
        }
    }

    /// Iterate all registered devices. Calls `cb` for every live
    /// (non-unregistering) device with a held kobject reference. The callback
    /// may inspect the device but must NOT call `DeviceInstance::unregister`
    /// (that would deadlock on the device-table lock). Returning non-zero
    /// from `cb` stops the iteration early.
    ///
    /// Uses RCU for safe traversal of the sparse device table without holding
    /// the writer spinlock; the RCU read-side section is dropped and
    /// reacquired around each callback invocation so `cb` may sleep, matching
    /// the C original.
    // P3-1D mesh sweep: caller (`vfs/devtmpfs/superblock.rs`) now imports this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn for_each_device(
        cb: extern "C" fn(*mut device_t, *mut c_void) -> c_int,
        ctx: *mut c_void,
    ) -> c_int {
        let mut ret: c_int = 0;
        let mut guard = Some(KRcuRead::new());

        'outer: for maj in 1..MAX_MAJOR_DEVICES {
            let dmajor = Self::major_slot(maj as c_int).load(Ordering::Acquire);
            if dmajor.is_null() {
                continue;
            }
            for min in 1..MAX_MINOR_DEVICES {
                // SAFETY: `dmajor`/`min` satisfy `minor_slot_atomic`'s
                // contract (`min` is in `1..MAX_MINOR_DEVICES`).
                let dev = unsafe { Self::minor_slot_atomic(dmajor, min as c_int) }.load(Ordering::Acquire);
                if dev.is_null() {
                    continue;
                }
                if Self::unregistering_atomic(dev).load(Ordering::Acquire) != 0 {
                    continue;
                }
                // SAFETY: `dev` is live for the duration of the current RCU
                // read-side section (`guard` still held at this point) --
                // exactly `KArc::try_from_raw`'s precondition.
                let dev_arc = match unsafe { KArc::<device_t>::try_from_raw(dev) } {
                    Some(a) => a,
                    None => continue,
                };
                // Release RCU so the callback can sleep if needed.
                drop(guard.take());
                ret = cb(KArc::as_ptr(&dev_arc), ctx);
                // `dev_arc` owns the reference `try_from_raw` acquired above;
                // dropping it here runs the equivalent of the old manual
                // `kobject_put` automatically -- no separate call needed (and
                // no way to forget it on a future early-`continue`/`break`
                // added to this loop body).
                drop(dev_arc);
                guard = Some(KRcuRead::new());
                if ret != 0 {
                    break 'outer;
                }
            }
        }

        drop(guard);
        ret
    }
}

/// RCU callback (`call_rcu`'s `rcu_callback_t`) to free a `device_major_t`
/// after the grace period.
///
/// FLOOR: address-taken `extern "C"` RCU callback (installed as a bare
/// fn-pointer at `DevTable::unregister_from_table`'s `Rcu::call(...,
/// Some(dev_type_rcu_free), ...)` above) -- kept a free fn per this wave's
/// floor rule.
extern "C" fn dev_type_rcu_free(data: *mut c_void) {
    DevTable::type_free(data as *mut device_major_t);
}

/// kobject release callback (`kobject_ops.release`): unregister the
/// device from the table, then invoke its own release operation.
///
/// FLOOR: address-taken `extern "C"` kobject-release callback (installed
/// as a bare fn-pointer at `DeviceInstance::register`'s `(*dev).kobj.ops.
/// release = Some(underlying_kobject_release)` below) -- kept a free fn
/// per this wave's floor rule.
extern "C" fn underlying_kobject_release(obj: *mut kobject) {
    // SAFETY: `kobject_put` only invokes this once refcount reaches zero;
    // `kobj` is `device_t`'s first field (offset 0), so this cast is
    // exact -- matches the C `container_of(obj, device_t, kobj)`, which
    // reduces to the same reinterpretation on this layout.
    let dev = obj as *mut device_t;
    DevTable::unregister_from_table(dev);
    DevTable::call_release(dev);
}

// ---------------------------------------------------------------------------
// `impl DeviceInstance` -- `device_t`-level public C ABI. KERNEL-OO
// relocation only: raw `*mut device_t` params kept (`DeviceInstance` is
// Freeze and shared cross-hart via the kobject refcount/device-table
// RCU, so no `&self` receiver), bodies byte-identical, `unsafe extern
// "C"` signatures preserved exactly.
// ---------------------------------------------------------------------------

impl DeviceInstance {
    /// Get a device by its major and minor numbers, incrementing its
    /// reference count. Returns the device on success, or `ERR_PTR` on error.
    // P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`) now import this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn get(major: c_int, minor: c_int) -> *mut device_t {
        result_to_errptr(Self::get_inner(major, minor))
    }

    fn get_inner(major: c_int, minor: c_int) -> KResult<*mut device_t> {
        let _rcu = KRcuRead::new();

        if major <= 0 || major as usize >= MAX_MAJOR_DEVICES || minor <= 0 || minor as usize >= MAX_MINOR_DEVICES {
            return Err(Errno::Inval);
        }

        // rcu_dereference (CONSUME -> Acquire, house mapping).
        let dmajor = DevTable::major_slot(major).load(Ordering::Acquire);
        if dmajor.is_null() {
            return Err(Errno::NoDev);
        }

        // SAFETY: `dmajor`/`minor` satisfy `minor_slot_atomic`'s contract
        // (range-checked above).
        let device = unsafe { DevTable::minor_slot_atomic(dmajor, minor) }.load(Ordering::Acquire);
        if device.is_null() {
            return Err(Errno::NoDev);
        }

        // smp_load_acquire(&device->unregistering).
        if DevTable::unregistering_atomic(device).load(Ordering::Acquire) != 0 {
            return Err(Errno::NoDev);
        }

        // Use kobject_try_get to avoid racing with a concurrent final put;
        // must succeed before we exit the RCU read-side critical section.
        // SAFETY: `device` is live for the duration of this RCU read-side
        // section (`_rcu` still held); `.kobj` is device_t's first field.
        if !unsafe { Kobject::kobject_try_get(&raw mut (*device).kobj) } {
            return Err(Errno::NoDev);
        }

        Ok(device)
    }

    /// Increment the reference count of a device. Returns `-ENODEV` if the
    /// device is being unregistered or its refcount already reached 0.
    // P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`) now import this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn dup(dev: *mut device_t) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        if DevTable::unregistering_atomic(dev).load(Ordering::Acquire) != 0 {
            return neg(ENODEV);
        }
        // SAFETY: caller-provided live `device_t*` (matches `DeviceInstance::
        // get`'s callers -- `DeviceInstance::dup`'s documented contract).
        if !unsafe { Kobject::kobject_try_get(&raw mut (*dev).kobj) } {
            return neg(ENODEV);
        }
        0
    }

    /// Decrement the reference count of a device.
    // P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`) now import this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn put(device: *mut device_t) -> c_int {
        if device.is_null() {
            return neg(EINVAL);
        }
        // SAFETY: caller-provided live `device_t*` with a currently-held
        // reference (`DeviceInstance::put`'s documented contract).
        unsafe { Kobject::kobject_put(&raw mut (*device).kobj) };
        0
    }

    // P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`,
    // `tty/ptmx.rs`, `tty/pty.rs`, `vfs/devtmpfs/superblock.rs`) now import this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn register(dev: *mut device_t) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        // SAFETY: `dev` is caller-provided; reading `.type_`/`.ops` is a
        // plain field read of caller-owned data (`DeviceInstance::register`
        // does not yet publish `dev` to any other observer).
        let (dtype, ops) = unsafe { ((*dev).type_, (*dev).ops) };
        if !DevTable::type_validate(dtype) {
            return neg(EINVAL);
        }
        // Ops validation (was `dev_opts_validate`): the old open/release
        // `Some` checks are unrepresentable-by-construction now that both
        // are required trait methods -- only "no table at all" remains
        // rejectable.
        if ops.is_none() {
            return neg(EINVAL);
        }

        let (assigned_minor, devname, devmode, major_num) = {
            let _guard = DevTable::tab_lock().lock();
            // SAFETY: `dev` is caller-owned, not yet published.
            let (major, minor) = unsafe { ((*dev).major, (*dev).minor) };
            let (dm, am) = match DevTable::slot_get(major, minor, true) {
                Ok(v) => v,
                Err(e) => return e,
            };
            // SAFETY: `dm`/`am` satisfy `minor_slot_atomic`'s contract.
            let dev_slot = unsafe { DevTable::minor_slot_atomic(dm, am) };
            if !dev_slot.load(Ordering::Relaxed).is_null() {
                return neg(EBUSY); // device already registered
            }

            // Initialize kobject before making the device visible to readers.
            // SAFETY: `dev` is still caller-exclusive (not yet published via
            // `dev_slot.store` below).
            unsafe {
                (*dev).kobj.name = c"device".as_ptr();
                (*dev).kobj.refcount = 0;
                (*dev).unregistering = 0;
                // FIX 1: write the auto-assigned minor back into `dev->minor`
                // -- see the module doc's "Mandated fix 1" section. Every
                // reader of `dev->minor` from here on (devtmpfs_create_node
                // below, devtmpfs_populate_devices's retroactive path,
                // `DeviceInstance::unregister`/`DevTable::unregister_from_
                // table`, any future caller) must see the real assigned
                // minor, not the `0` auto-assign sentinel.
                (*dev).minor = am;
                (*dev).kobj.ops.release = Some(underlying_kobject_release);
                Kobject::kobject_init(&raw mut (*dev).kobj);
            }
            // SAFETY: `dm` is live and only ever mutated under
            // `DevTable::tab_lock()` (this guard).
            unsafe { (*dm).num_minors += 1 };

            // rcu_assign_pointer: publish the device.
            dev_slot.store(dev, Ordering::Release);

            // SAFETY: `dev` fields read after publication are still only
            // written by this thread (registration is single-writer per
            // device); this snapshot is for the devtmpfs call below, taken
            // while still holding the lock for simplicity.
            let (devname, devmode, major_num) = unsafe { ((*dev).devname, (*dev).devmode, (*dev).major) };
            (am, devname, devmode, major_num)
        }; // `_guard` drops here, unlocking `dev_tab_lock`.

        // Create the devtmpfs node if the device carries a name.
        // devtmpfs_create_node() is safe to call before devtmpfs is mounted
        // -- it buffers the entry and materialises it on first mount.
        if !devname.is_null() {
            let ret = devtmpfs_create_node(devname, devmode, mkdev(major_num, assigned_minor));
            if ret != 0 {
                // FIX 2: log the failure instead of discarding it silently
                // -- see the module doc's "Mandated fix 2" section for why
                // this is a warning, not a `DeviceInstance::register()`
                // failure. `devname` is a live NUL-terminated C string (the
                // device's own, still valid) rendered via the `Cs`
                // runtime-C-string adapter (kernel/printf.rs).
                crate::kprintln!(
                    "device_register: devtmpfs_create_node('{}') failed: {}",
                    crate::printf::Cs(devname),
                    ret
                );
            }
        }

        0
    }

    /// Mark a device as unregistering. After this call, `DeviceInstance::
    /// get()` and `DeviceInstance::dup()` will fail for this device. The
    /// device is also removed from the lookup table immediately. The actual
    /// release callback runs once the refcount reaches 0.
    // P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`,
    // `tty/ptmx.rs`, `tty/pty.rs`, `vfs/devtmpfs/superblock.rs`) now import this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn unregister(dev: *mut device_t) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        // atomic_cas(&dev->unregistering, 0, 1): SeqCst/SeqCst CAS, matching
        // `kernel/inc/smp/atomic.h`'s `atomic_cas` macro.
        let cur = DevTable::unregistering_atomic(dev)
            .compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst);
        if cur.is_err() {
            return neg(EALREADY); // already unregistering
        }

        // Remove the devtmpfs node before the device becomes unreachable.
        // devtmpfs_remove_node() is safe even if devtmpfs is not mounted.
        // SAFETY: `dev` is caller-provided and live for this call.
        let devname = unsafe { (*dev).devname };
        if !devname.is_null() {
            devtmpfs_remove_node(devname);
        }

        // Remove from the device table immediately so no new lookups find it.
        DevTable::unregister_from_table(dev);
        // Drop the initial reference from registration. When the refcount
        // reaches 0, `underlying_kobject_release` runs.
        // SAFETY: `dev` is live; `.kobj` is device_t's first field.
        unsafe { Kobject::kobject_put(&raw mut (*dev).kobj) };
        0
    }

    // P3-1D mesh sweep: callers (`console.rs`, `vfs/file.rs`) now import this
    // via crate-path `use` instead of an `extern` redeclaration -- demoted.
    pub(crate) extern "C" fn ioctl(dev: *mut device_t, cmd: u64, arg: *mut c_void) -> c_int {
        if dev.is_null() {
            return neg(EINVAL);
        }
        if DevTable::unregistering_atomic(dev).load(Ordering::Acquire) != 0 {
            return neg(ENODEV);
        }
        // SAFETY: `dev` is caller-provided and live for this call (the
        // dispatch target's contract); `arg`'s validity is forwarded from
        // the caller. `DeviceOps::ioctl`'s `None` return means "op not
        // provided" -- the default, mirroring the old `None` table slot --
        // and keeps the exact `ENOTTY` fallback (a `None` table, impossible
        // on a registered device, takes the same fallback).
        match unsafe { (*dev).ops.and_then(|o| o.ioctl(dev, cmd, arg)) } {
            Some(ret) => ret,
            None => neg(ENOTTY),
        }
    }
}

/// `mkdev(major, minor)` (`kernel/inc/defs.h`: `((uint)((m) << 20 |
/// (n)))`). Reimplemented locally, matching
/// `kernel/vfs/devtmpfs/superblock.rs`'s identical local copy (`defs.h`
/// is a `static inline`-free macro but not itself part of the bindgen
/// surface).
///
/// FLOOR: a plain scalar formula with no single `device_t`/`device_major_t`
/// subject -- left as a free fn, matching every other `mkdev` copy in the
/// tree (`vfs/devtmpfs/superblock.rs`, `vfs/xv6fs/*.rs`, `vfs/vfs_syscall.rs`).
#[inline(always)]
fn mkdev(major: c_int, minor: c_int) -> crate::bindings::dev_t {
    ((major << 20) | minor) as crate::bindings::dev_t
}
