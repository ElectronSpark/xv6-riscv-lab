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
    device_major_t, device_ops_t, device_t, dev_type_e, dev_type_e_DEV_TYPE_BLOCK,
    dev_type_e_DEV_TYPE_CHAR, kobject, rcu_callback_t, rcu_head_t, slab_cache_t,
    spinlock_t, EALREADY, EBUSY, EINVAL, ENODEV, ENOMEM, ENOSPC, ENOTTY, SLAB_FLAG_DEBUG_BITMAP,
    SLAB_FLAG_EMBEDDED,
};
use crate::lock::rcu::KRcuRead;
use crate::sync::KSpinlock;

// ---------------------------------------------------------------------------
// External C symbols. Declared locally rather than via bindgen's function
// allowlist, matching the established per-file convention (irq/trap.rs,
// tty/session.rs, mm/vm.rs, vfs/inode.rs, vfs/devtmpfs/superblock.rs, ...).
// ---------------------------------------------------------------------------

unsafe extern "C" {
    pub safe fn xv6_panic(msg: *const c_char) -> !;

    // mm/slab.rs -- signature matches kernel/lock/rcu.rs's own (already
    // proven, boot-tested) declaration rather than mm/slab.rs's literal
    // Rust definition; both resolve to the same C symbol/ABI (RISC-V lp64d
    // integer args narrower than XLEN are extended by the caller either
    // way), kept consistent with the working precedent instead of
    // "correcting" it in a way nothing has exercised.
    pub safe fn slab_cache_init(
        cache: *mut slab_cache_t,
        name: *const c_char,
        obj_size: u64,
        flags: u32,
    ) -> c_int;
    pub safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    pub safe fn slab_free(obj: *mut c_void);

    // mm/page.rs -- real C signature (kernel/inc/mm/page.h): both args are
    // uint64.
    pub safe fn page_alloc(order: u64, flags: u64) -> *mut c_void;
    pub safe fn page_free(ptr: *mut c_void, order: u64);

    // kernel/kobject.rs. Genuinely unsafe (raw kobject* + caller-upheld
    // refcount invariants), kept as plain (non-`safe`) externs.
    fn kobject_init(obj: *mut kobject);
    fn kobject_try_get(obj: *mut kobject) -> bool;
    fn kobject_put(obj: *mut kobject);

    // kernel/lock/rcu.rs.
    fn call_rcu(head: *mut rcu_head_t, func: rcu_callback_t, data: *mut c_void);

    // printf.rs -- variadic, cannot be marked `safe`.
}
// P3-1C mesh sweep: vfs/devtmpfs/superblock.rs is in scope for this wave;
// signatures are identical (same `mode_t`/`dev_t` types), so these become
// plain crate-path imports instead of `extern "C"` redeclarations. See the
// module doc's "Mandated fix 2" section for the devtmpfs_create_node()
// return-value contract this wave checks.
use crate::vfs::devtmpfs::superblock::{devtmpfs_create_node, devtmpfs_remove_node};

/// Mirrors the C `assert(expr, fmt)` macro (`kernel/inc/printf.h`):
/// panic the kernel if `$cond` is false. Simplified to a fixed message
/// (no `printf`-style formatting), matching the convention already used
/// by `kernel/kobject.rs`'s `kassert!`.
macro_rules! kassert {
    ($cond:expr, $msg:expr) => {
        if !($cond) {
            xv6_panic(concat!($msg, "\0").as_ptr() as *const c_char)
        }
    };
}

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// `ERR_PTR` (`kernel/inc/errno.h`), generic over the pointee type.
/// Reimplemented locally rather than shared, matching the established
/// per-file convention (see `kernel/vfs/inode.rs`'s identical local
/// copy). This file only ever *produces* error-pointers (`device_get`),
/// never consumes one, so `IS_ERR`/`MAX_ERRNO` are not needed here (see
/// `kernel/dev/cdev.rs`/`kernel/dev/blkdev.rs` for the consumer side).
#[inline(always)]
fn err_ptr<T>(errno: c_int) -> *mut T {
    errno as isize as *mut T
}

const MAX_MAJOR_DEVICES: usize = 256; // kernel/inc/dev/dev_types.h
const MAX_MINOR_DEVICES: usize = 256; // kernel/inc/dev/dev_types.h
const PAGE_TYPE_ANON: u64 = 0; // mm/page_type.h: PAGE_TYPE_ANON

// ---------------------------------------------------------------------------
// Global registry storage. `SyncCell<T>` is redefined locally per this
// crate's established convention (see kernel/kobject.rs, mm/pcache.rs,
// tty/{tty,ptmx,session}.rs, proc/sched_fifo.rs, proc/sched_idle.rs).
// ---------------------------------------------------------------------------

#[repr(transparent)]
struct SyncCell<T>(UnsafeCell<T>);
// SAFETY: used only for this module's two file-scope statics below
// (`DEV_TAB_LOCK`, `DEV_TYPE_CACHE`), both written exactly once by
// `dev_table_init()` (documented single-hart, single-call contract, see
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

#[inline(always)]
fn dev_tab_lock() -> KSpinlock {
    // SAFETY: `DEV_TAB_LOCK` is initialised once by `dev_table_init()`,
    // called before any other entry point in this file per that
    // function's documented contract (mirrors every other subsystem's
    // `*_init` contract in this crate, e.g. `kernel/kobject.rs`'s
    // `kobject_global_init`).
    KSpinlock::from_bindings(unsafe { (*DEV_TAB_LOCK.get()).as_mut_ptr() })
}

fn dev_tab_assert_held() {
    // SAFETY: `xv6_panic` never returns; `dev_tab_lock().holding()` reads
    // only the lock's own internal state.
    unsafe { kassert!(dev_tab_lock().holding(), "dev_tab_lock not held") };
}

#[inline(always)]
fn dev_type_cache_ptr() -> *mut slab_cache_t {
    // SAFETY: see `DEV_TAB_LOCK`'s identical contract above.
    unsafe { (*DEV_TYPE_CACHE.get()).as_mut_ptr() }
}

// ---------------------------------------------------------------------------
// Slot helpers: reinterpret plain C pointer-array elements as atomics for
// RCU load/store, matching `kernel/kobject.rs`'s `refcount_atomic` cast
// pattern (the C static type is a plain pointer; `rcu_dereference`/
// `rcu_assign_pointer` are just atomic load/store macros applied to it
// regardless).
// ---------------------------------------------------------------------------

#[inline(always)]
fn major_slot(major: c_int) -> &'static AtomicPtr<device_major_t> {
    &DEV_TABLE[major as usize]
}

/// Reinterpret `dmajor->minors[minor]` as an atomic pointer cell.
///
/// # Safety
/// `dmajor` must be a live `device_major_t*` whose `minors` array has at
/// least `MAX_MINOR_DEVICES` slots (guaranteed by `dev_type_alloc`);
/// `minor` must be in `0..MAX_MINOR_DEVICES` (validated by `dev_slot_get`
/// before any call reaches here).
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

// ---------------------------------------------------------------------------
// device_major_t lifecycle (dev_type_alloc / dev_type_free /
// dev_type_rcu_free).
// ---------------------------------------------------------------------------

fn dev_type_alloc() -> *mut device_major_t {
    let dev_type = slab_alloc(dev_type_cache_ptr()) as *mut device_major_t;
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

fn dev_type_free(dev_type: *mut device_major_t) {
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

/// RCU callback (`call_rcu`'s `rcu_callback_t`) to free a `device_major_t`
/// after the grace period.
extern "C" fn dev_type_rcu_free(data: *mut c_void) {
    dev_type_free(data as *mut device_major_t);
}

// ---------------------------------------------------------------------------
// dev_slot_get (writer + dead reader path, see the module doc).
// ---------------------------------------------------------------------------

/// Mirrors `__dev_slot_get()`.
///
/// For the writer path (`alloc = true`) the caller must already hold
/// `dev_tab_lock()`. Allocates a fresh `device_major_t` if the major slot
/// is empty, and if `minor == 0` scans for the first free minor (the
/// auto-assignment sentinel) instead of using a literal minor.
///
/// The reader path (`alloc = false`) is preserved for fidelity with the C
/// original but is presently unreachable: `device_register()` and
/// `dev_unregister_from_table()`, the only two call sites (both in the C
/// original and in this port), always pass `alloc = true`.
///
/// Returns `Ok((dmajor, minor))` on success, `Err(-errno)` on failure.
fn dev_slot_get(major: c_int, minor: c_int, alloc: bool) -> Result<(*mut device_major_t, c_int), c_int> {
    if alloc {
        dev_tab_assert_held();
    }
    if major <= 0 || major as usize >= MAX_MAJOR_DEVICES || minor < 0 || minor as usize >= MAX_MINOR_DEVICES {
        return Err(neg(EINVAL));
    }

    let slot = major_slot(major);
    let mut dmajor = if alloc {
        // Writer path: serialized by `dev_tab_lock()` (asserted above); a
        // plain relaxed load is sufficient here (matches the C's direct,
        // non-atomic `dmajor = __dev_table[major];` read) -- publication
        // to concurrent RCU readers is the `Release` store below/at
        // `device_register`, not this read.
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
        dmajor = dev_type_alloc();
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
        assigned_minor = 0;
        for i in 1..MAX_MINOR_DEVICES {
            // SAFETY: `dmajor` is live: either just-allocated above
            // (exclusively owned until published a few lines up) or an
            // existing entry, only ever freed after an RCU grace period
            // once its last minor is unregistered -- both safe to scan
            // while holding `dev_tab_lock()` (asserted at function entry).
            let slot = unsafe { minor_slot_atomic(dmajor, i as c_int) }.load(Ordering::Relaxed);
            if slot.is_null() {
                assigned_minor = i as c_int;
                break;
            }
        }
        if assigned_minor == 0 {
            return Err(neg(ENOSPC)); // no available minor slots
        }
    }

    Ok((dmajor, assigned_minor))
}

// ---------------------------------------------------------------------------
// Public C ABI.
// ---------------------------------------------------------------------------

// P3-1D mesh sweep: callers (`start_kernel.rs`, `vfs/devtmpfs/superblock.rs`)
// now import this via crate-path `use` instead of an `extern`
// redeclaration -- demoted.
pub(crate) extern "C" fn dev_table_init() {
    dev_tab_lock().init(c"dev_tab_lock".as_ptr());
    let ret = slab_cache_init(
        dev_type_cache_ptr(),
        c"dev_type_cache".as_ptr(),
        core::mem::size_of::<device_major_t>() as u64,
        SLAB_FLAG_EMBEDDED | SLAB_FLAG_DEBUG_BITMAP,
    );
    // SAFETY: `xv6_panic` never returns.
    unsafe { kassert!(ret == 0, "Failed to initialize device type slab cache") };
}

/// Get a device by its major and minor numbers, incrementing its
/// reference count. Returns the device on success, or `ERR_PTR` on error.
// P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn device_get(major: c_int, minor: c_int) -> *mut device_t {
    let _rcu = KRcuRead::new();

    if major <= 0 || major as usize >= MAX_MAJOR_DEVICES || minor <= 0 || minor as usize >= MAX_MINOR_DEVICES {
        return err_ptr(neg(EINVAL));
    }

    // rcu_dereference (CONSUME -> Acquire, house mapping).
    let dmajor = major_slot(major).load(Ordering::Acquire);
    if dmajor.is_null() {
        return err_ptr(neg(ENODEV));
    }

    // SAFETY: `dmajor`/`minor` satisfy `minor_slot_atomic`'s contract
    // (range-checked above).
    let device = unsafe { minor_slot_atomic(dmajor, minor) }.load(Ordering::Acquire);
    if device.is_null() {
        return err_ptr(neg(ENODEV));
    }

    // smp_load_acquire(&device->unregistering).
    if unregistering_atomic(device).load(Ordering::Acquire) != 0 {
        return err_ptr(neg(ENODEV));
    }

    // Use kobject_try_get to avoid racing with a concurrent final put;
    // must succeed before we exit the RCU read-side critical section.
    // SAFETY: `device` is live for the duration of this RCU read-side
    // section (`_rcu` still held); `.kobj` is device_t's first field.
    if !unsafe { kobject_try_get(&raw mut (*device).kobj) } {
        return err_ptr(neg(ENODEV));
    }

    device
}

/// Increment the reference count of a device. Returns `-ENODEV` if the
/// device is being unregistered or its refcount already reached 0.
// P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn device_dup(dev: *mut device_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    if unregistering_atomic(dev).load(Ordering::Acquire) != 0 {
        return neg(ENODEV);
    }
    // SAFETY: caller-provided live `device_t*` (matches `device_get`'s
    // callers -- `device_dup`'s documented contract).
    if !unsafe { kobject_try_get(&raw mut (*dev).kobj) } {
        return neg(ENODEV);
    }
    0
}

/// Decrement the reference count of a device.
// P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn device_put(device: *mut device_t) -> c_int {
    if device.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: caller-provided live `device_t*` with a currently-held
    // reference (device_put's documented contract).
    unsafe { kobject_put(&raw mut (*device).kobj) };
    0
}

fn dev_opts_validate(ops: *const device_ops_t) -> bool {
    if ops.is_null() {
        return false;
    }
    // SAFETY: caller (device_register) guarantees `ops` points at the
    // live, embedded `dev->ops` field of a caller-owned `device_t`.
    let ops = unsafe { &*ops };
    // Both open and release operations must be defined.
    ops.open.is_some() && ops.release.is_some()
}

fn dev_type_validate(t: dev_type_e) -> bool {
    t == dev_type_e_DEV_TYPE_BLOCK || t == dev_type_e_DEV_TYPE_CHAR
}

/// kobject release callback (`kobject_ops.release`): unregister the
/// device from the table, then invoke its own release operation.
extern "C" fn underlying_kobject_release(obj: *mut kobject) {
    // SAFETY: `kobject_put` only invokes this once refcount reaches zero;
    // `kobj` is `device_t`'s first field (offset 0), so this cast is
    // exact -- matches the C `container_of(obj, device_t, kobj)`, which
    // reduces to the same reinterpretation on this layout.
    let dev = obj as *mut device_t;
    dev_unregister_from_table(dev);
    dev_call_release(dev);
}

/// Because only a validated device's ops are ever installed here (see
/// `__dev_opts_validate`/`__cdev_opts_validate`/`__blkdev_ops_validate`),
/// `.release` is asserted rather than branched on, matching the C's own
/// comment ("their validity is asserted to be true").
fn dev_call_release(dev: *mut device_t) -> c_int {
    // SAFETY: `dev` is a live device_t (kobject release contract);
    // `.ops.release` is a plain field read.
    let release = unsafe { (*dev).ops.release };
    // SAFETY: `xv6_panic` never returns.
    unsafe { kassert!(!dev.is_null() && release.is_some(), "Invalid device or release operation") };
    // SAFETY: `release` is `Some` per the assertion above; `dev` is the
    // caller-verified live pointer this callback owns.
    unsafe { release.unwrap()(dev) }
}

/// Unregister a device from the device table. Called either by
/// `device_unregister()` or when the kobject refcount reaches 0. Uses RCU
/// for safe removal -- the old `device_major_t`, if now empty, is freed
/// after the grace period.
fn dev_unregister_from_table(dev: *mut device_t) {
    let mut to_free: *mut device_major_t = ptr::null_mut();
    {
        let _guard = dev_tab_lock().lock();
        // SAFETY: `dev` is caller-provided and live for this call.
        let (major, minor) = unsafe { ((*dev).major, (*dev).minor) };
        let dmajor = match dev_slot_get(major, minor, true) {
            Ok((dmajor, _)) => dmajor,
            Err(_) => return, // device already removed from table
        };
        // SAFETY: `dmajor`/`minor` satisfy `minor_slot_atomic`'s contract
        // (validated by `dev_slot_get` above).
        let dev_slot = unsafe { minor_slot_atomic(dmajor, minor) };
        if dev_slot.load(Ordering::Relaxed) != dev {
            return; // device already removed or replaced
        }
        // rcu_assign_pointer(*dev_slot, NULL).
        dev_slot.store(ptr::null_mut(), Ordering::Release);

        // SAFETY: `dmajor` is live and only ever mutated under
        // `dev_tab_lock()` (this guard).
        unsafe { (*dmajor).num_minors -= 1 };
        if unsafe { (*dmajor).num_minors } == 0 {
            // No more minors: schedule the device type for RCU-deferred
            // free.
            to_free = dmajor;
            major_slot(major).store(ptr::null_mut(), Ordering::Release);
        }
    } // `_guard` drops here, unlocking `dev_tab_lock` -- matches the C's
      // explicit `__dev_tab_unlock()` before `call_rcu()`.

    if !to_free.is_null() {
        // SAFETY: `to_free` was just unpublished from `DEV_TABLE` above
        // while still holding the lock, so no *new* reader can observe
        // it; `call_rcu` defers the actual free until any reader that
        // dereferenced it earlier has finished.
        unsafe {
            call_rcu(
                &raw mut (*to_free).rcu_head,
                Some(dev_type_rcu_free),
                to_free as *mut c_void,
            );
        }
    }
}

// P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`,
// `tty/ptmx.rs`, `tty/pty.rs`, `vfs/devtmpfs/superblock.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn device_register(dev: *mut device_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    // SAFETY: `dev` is caller-provided; reading `.type_`/`.ops` is a
    // plain field read of caller-owned data (device_register does not
    // yet publish `dev` to any other observer).
    let (dtype, ops_ptr) = unsafe { ((*dev).type_, &raw const (*dev).ops) };
    if !dev_type_validate(dtype) {
        return neg(EINVAL);
    }
    if !dev_opts_validate(ops_ptr) {
        return neg(EINVAL);
    }

    let (assigned_minor, devname, devmode, major_num) = {
        let _guard = dev_tab_lock().lock();
        // SAFETY: `dev` is caller-owned, not yet published.
        let (major, minor) = unsafe { ((*dev).major, (*dev).minor) };
        let (dm, am) = match dev_slot_get(major, minor, true) {
            Ok(v) => v,
            Err(e) => return e,
        };
        // SAFETY: `dm`/`am` satisfy `minor_slot_atomic`'s contract.
        let dev_slot = unsafe { minor_slot_atomic(dm, am) };
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
            // device_unregister/dev_unregister_from_table, any future
            // caller) must see the real assigned minor, not the `0`
            // auto-assign sentinel.
            (*dev).minor = am;
            (*dev).kobj.ops.release = Some(underlying_kobject_release);
            kobject_init(&raw mut (*dev).kobj);
        }
        // SAFETY: `dm` is live and only ever mutated under
        // `dev_tab_lock()` (this guard).
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
            // this is a warning, not a `device_register()` failure.
            // `devname` is a live NUL-terminated C string (the device's
            // own, still valid) rendered via the `Cs` runtime-C-string
            // adapter (kernel/printf.rs).
            crate::kprintln!(
                "device_register: devtmpfs_create_node('{}') failed: {}",
                crate::printf::Cs(devname),
                ret
            );
        }
    }

    0
}

/// Mark a device as unregistering. After this call, `device_get()` and
/// `device_dup()` will fail for this device. The device is also removed
/// from the lookup table immediately. The actual release callback runs
/// once the refcount reaches 0.
// P3-1D mesh sweep: callers (`dev/cdev.rs`, `dev/blkdev.rs`,
// `tty/ptmx.rs`, `tty/pty.rs`, `vfs/devtmpfs/superblock.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn device_unregister(dev: *mut device_t) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    // atomic_cas(&dev->unregistering, 0, 1): SeqCst/SeqCst CAS, matching
    // `kernel/inc/smp/atomic.h`'s `atomic_cas` macro.
    let cur = unregistering_atomic(dev)
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
    dev_unregister_from_table(dev);
    // Drop the initial reference from registration. When the refcount
    // reaches 0, `underlying_kobject_release` runs.
    // SAFETY: `dev` is live; `.kobj` is device_t's first field.
    unsafe { kobject_put(&raw mut (*dev).kobj) };
    0
}

// P3-1D mesh sweep: callers (`console.rs`, `vfs/file.rs`) now import this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn dev_ioctl(dev: *mut device_t, cmd: u64, arg: *mut c_void) -> c_int {
    if dev.is_null() {
        return neg(EINVAL);
    }
    if unregistering_atomic(dev).load(Ordering::Acquire) != 0 {
        return neg(ENODEV);
    }
    // SAFETY: `dev` is caller-provided and live for this call.
    let ioctl = unsafe { (*dev).ops.ioctl };
    match ioctl {
        None => neg(ENOTTY),
        // SAFETY: `ioctl` is a valid C-ABI function pointer installed by
        // a validated `device_ops_t` (device_register's contract); `dev`
        // is the same live pointer being dispatched on.
        Some(f) => unsafe { f(dev, cmd, arg) },
    }
}

/// Iterate all registered devices. Calls `cb` for every live
/// (non-unregistering) device with a held kobject reference. The callback
/// may inspect the device but must NOT call `device_unregister` (that
/// would deadlock on the device-table lock). Returning non-zero from `cb`
/// stops the iteration early.
///
/// Uses RCU for safe traversal of the sparse device table without holding
/// the writer spinlock; the RCU read-side section is dropped and
/// reacquired around each callback invocation so `cb` may sleep, matching
/// the C original.
// P3-1D mesh sweep: caller (`vfs/devtmpfs/superblock.rs`) now imports this
// via crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) extern "C" fn dev_for_each_device(
    cb: extern "C" fn(*mut device_t, *mut c_void) -> c_int,
    ctx: *mut c_void,
) -> c_int {
    let mut ret: c_int = 0;
    let mut guard = Some(KRcuRead::new());

    'outer: for maj in 1..MAX_MAJOR_DEVICES {
        let dmajor = major_slot(maj as c_int).load(Ordering::Acquire);
        if dmajor.is_null() {
            continue;
        }
        for min in 1..MAX_MINOR_DEVICES {
            // SAFETY: `dmajor`/`min` satisfy `minor_slot_atomic`'s
            // contract (`min` is in `1..MAX_MINOR_DEVICES`).
            let dev = unsafe { minor_slot_atomic(dmajor, min as c_int) }.load(Ordering::Acquire);
            if dev.is_null() {
                continue;
            }
            if unregistering_atomic(dev).load(Ordering::Acquire) != 0 {
                continue;
            }
            // SAFETY: `dev` is live for the duration of the current RCU
            // read-side section (`guard` still held at this point).
            if !unsafe { kobject_try_get(&raw mut (*dev).kobj) } {
                continue;
            }
            // Release RCU so the callback can sleep if needed.
            drop(guard.take());
            ret = cb(dev, ctx);
            // SAFETY: `dev` is still live -- this call owns the
            // reference `kobject_try_get` just acquired above.
            unsafe { kobject_put(&raw mut (*dev).kobj) };
            guard = Some(KRcuRead::new());
            if ret != 0 {
                break 'outer;
            }
        }
    }

    drop(guard);
    ret
}

/// `mkdev(major, minor)` (`kernel/inc/defs.h`: `((uint)((m) << 20 |
/// (n)))`). Reimplemented locally, matching
/// `kernel/vfs/devtmpfs/superblock.rs`'s identical local copy (`defs.h`
/// is a `static inline`-free macro but not itself part of the bindgen
/// surface).
#[inline(always)]
fn mkdev(major: c_int, minor: c_int) -> crate::bindings::dev_t {
    ((major << 20) | minor) as crate::bindings::dev_t
}
