//! IRQ descriptor table (RCU-protected register/unregister) and interrupt
//! dispatch.
//!
//! Rust port of `kernel/irq/irq.c`. Called from `kernel/irq/trap.c`
//! (`do_irq`, unported -- Wave 6) and from every still-C or Rust driver
//! that owns an interrupt line (`e1000.c`, `virtio_disk.c`,
//! `timer/timer.c`, `timer/goldfish_rtc.c`, `ipi/ipi.c`, `dev/x1_emac.c`,
//! `console.rs`).
//!
//! `struct irq_desc` (`kernel/inc/trap.h`) is hand-declared below rather
//! than added to `wrapper.h`: `trap.h`'s `__scause_to_str` static-inline
//! body would trip bindgen the same way `rwlock.h` does (see
//! `console.rs`'s doc comment on its own, now-superseded, local
//! declaration) -- but every *field* type (`rcu_head`, `device_t`) is a
//! real bindgen type pulled in transitively by other allowlisted headers,
//! so this is a byte-for-byte layout-compatible mirror, not an opaque
//! stand-in.

use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicPtr, AtomicU64, Ordering};

use crate::bindings::{device_t, rcu_head, slab_cache_t, trapframe};

// ===========================================================================
// IRQ-number space (`kernel/inc/trap.h`). Small compile-time ABI constants
// duplicated locally, same convention as `console.rs`'s (now superseded)
// `PLIC_IRQ_OFFSET` / `start.rs`'s `NCPU`/`KERNEL_STACK_SIZE`.
// ===========================================================================
pub const CLINT_IRQ_CNT: c_int = 1024;
pub const PLIC_IRQ_OFFSET: c_int = CLINT_IRQ_CNT;
pub const PLIC_IRQ_CNT: c_int = 1024;
pub const IRQCNT: usize = (PLIC_IRQ_OFFSET + PLIC_IRQ_CNT) as usize;
const RISCV_S_EXTERNAL_INTERRUPT: c_int = 9;

/// `PLIC_IRQ(hw_irq)`.
#[inline(always)]
pub const fn plic_irq(hw_irq: c_int) -> c_int {
    hw_irq + PLIC_IRQ_OFFSET
}

/// C `irq_handler_t` is `void (*)(int irq, void *data, device_t *dev)`; the
/// third parameter is typed `*mut c_void` here rather than `*mut device_t`
/// so this matches the one live Rust handler today,
/// `uart::uartintr(c_int, *mut c_void, *mut c_void)` (Wave 4, out of this
/// wave's touch scope), without forcing a signature change there. Pointer
/// representation is identical regardless of pointee type, so this is
/// ABI-neutral; `do_irq`/`do_plic_irq` cast `desc.dev` to `*mut c_void` at
/// the two call sites below.
pub type IrqHandlerFn = unsafe extern "C" fn(c_int, *mut c_void, *mut c_void);

/// Mirrors C `struct irq_desc` field-for-field (see module doc for the
/// bindgen-coverage rationale). `handler`/`data`/`dev` are caller-supplied;
/// `irq`/`count` are "ignored when registering" (overwritten by
/// `register_irq_handler`); `rcu_head` backs the deferred-free path in
/// `unregister_irq_handler`.
#[repr(C)]
pub struct IrqDesc {
    pub handler: Option<IrqHandlerFn>,
    pub data: *mut c_void,
    pub dev: *mut device_t,
    pub irq: c_int,
    pub count: u64,
    pub rcu_head: rcu_head,
}

// ===========================================================================
// Externs.
// ===========================================================================
// P3-1B mesh sweep: all three are same-crate `pub(crate)` items as of this
// wave (only caller anywhere in the tree is this file), referenced via
// their crate path instead of `extern "C"` redeclarations.
use crate::irq::plic::{plic_claim, plic_complete, plic_enable_irq};

unsafe extern "C" {
    pub safe fn rcu_read_lock();
    pub safe fn rcu_read_unlock();
    pub safe fn call_rcu(head: *mut rcu_head, func: crate::bindings::rcu_callback_t, data: *mut c_void);

    pub safe fn slab_cache_init(cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64) -> c_int;
    pub safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    pub safe fn slab_free(obj: *mut c_void);

    pub safe fn __panic_start();
    pub safe fn __panic_end() -> !;
    // printf is variadic, so it cannot be declared `safe`.
    pub fn printf(fmt: *const c_char, ...) -> c_int;
}

#[inline]
fn neg(errno: u32) -> c_int {
    -(errno as c_int)
}

/// Replicates the C `assert(expr, fmt)` macro expansion (no format args --
/// both call sites below pass a literal message). Same pattern as
/// `console.rs`'s `console_assert_plain` / `spinlock.rs`'s panic helper.
fn irq_assert(cond: bool, line: u32, function: &core::ffi::CStr, msg: &core::ffi::CStr) {
    if cond {
        return;
    }
    __panic_start();
    // SAFETY: all arguments are 'static or caller-owned NUL-terminated C
    // strings matching their format specifiers.
    unsafe {
        printf(
            c"ASSERTION_FAILURE %s:%d: In function '%s':\n".as_ptr(),
            c"kernel/irq/irq_core.rs".as_ptr(),
            line as c_int,
            function.as_ptr(),
        );
        printf(msg.as_ptr());
        printf(c"\n".as_ptr());
    }
    __panic_end()
}

// ===========================================================================
// IRQ descriptor table + write lock.
// ===========================================================================

/// RCU-protected descriptor table, indexed by IRQ number. Readers
/// (`do_irq`) load with `Acquire` inside an `rcu_read_lock`/`unlock`
/// section (no lock needed for the load itself); writers
/// (`register_irq_handler`/`unregister_irq_handler`) serialise against
/// each other with `IRQ_WRITE_LOCK` and publish with `Release`. Mirrors
/// the C `rcu_dereference`/`rcu_assign_pointer` macros
/// (`__ATOMIC_CONSUME`/`__ATOMIC_RELEASE` -- Rust has no `Consume`
/// ordering, so `Acquire`, its safe superset, is used for the load).
static IRQ_DESCS: [AtomicPtr<IrqDesc>; IRQCNT] = [const { AtomicPtr::new(ptr::null_mut()) }; IRQCNT];

/// Compile-time-initialized spinlock protecting the read-modify-write
/// sequence in `register_irq_handler`/`unregister_irq_handler`. Mirrors
/// the C `spinlock_t irq_write_lock = SPINLOCK_INITIALIZED("irq_write_lock")`.
static mut IRQ_WRITE_LOCK: crate::bindings::spinlock_t = crate::bindings::spinlock_t {
    locked: 0,
    name: c"irq_write_lock".as_ptr() as *mut c_char,
    cpu: ptr::null_mut(),
};

/// File-scope `irq_desc` slab cache storage (was `static slab_cache_t
/// __irq_desc_slab = {0}` in the C original). Same `UnsafeCell<MaybeUninit<
/// slab_cache_t>>` + `unsafe impl Sync` pattern as `proc/proc_shims.rs`'s
/// `SlabCacheCell` / `proc/workqueue.rs`'s `CacheCell`.
#[repr(transparent)]
struct CacheCell(core::cell::UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: `slab_cache_t`'s own internal locking serialises all field
// mutation (`kernel/inc/mm/slab.h`); sharing the storage between CPUs is
// exactly what the allocator was designed for.
unsafe impl Sync for CacheCell {}

static IRQ_DESC_SLAB: CacheCell = CacheCell(core::cell::UnsafeCell::new(MaybeUninit::zeroed()));

fn irq_desc_slab_ptr() -> *mut slab_cache_t {
    IRQ_DESC_SLAB.0.get() as *mut slab_cache_t
}

#[inline(always)]
fn count_atomic(desc: *mut IrqDesc) -> &'static AtomicU64 {
    // SAFETY: `desc` is a live, slab-allocated `IrqDesc` that is never
    // moved after publication into `IRQ_DESCS`; `count`'s layout matches
    // `AtomicU64` (a plain, non-atomic `u64` field in the C struct,
    // mutated via `__atomic_add_fetch` there -- same reinterpret-cast
    // pattern `lock/spinlock.rs` uses for `spinlock_t::locked`).
    unsafe { &*(core::ptr::addr_of!((*desc).count) as *const AtomicU64) }
}

unsafe extern "C" fn rcu_free_irq_desc(data: *mut c_void) {
    // SAFETY: `data` is the `IrqDesc*` `unregister_irq_handler` published
    // to `call_rcu` below; by the time RCU invokes this callback, the
    // grace period has elapsed and no reader can still hold a reference,
    // so this is the sole (now exclusive) owner returning the allocation.
    unsafe { slab_free(data) };
}

/// Rust port of `irq_desc_init()`, called once from `start_kernel.c`.
// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn irq_desc_init() {
    const NAME: &[u8] = b"irq_desc\0";
    // SAFETY: `NAME` is a 'static NUL-terminated string; `irq_desc_slab_ptr()`
    // is a unique static not yet touched by anything else at this boot
    // stage (single-threaded boot-hart init, before `register_irq_handler`
    // can be called).
    let ret = slab_cache_init(
        irq_desc_slab_ptr(),
        NAME.as_ptr() as *mut c_char,
        core::mem::size_of::<IrqDesc>(),
        crate::bindings::SLAB_FLAG_EMBEDDED as u64,
    );
    irq_assert(
        ret == 0,
        line!(),
        c"irq_desc_init",
        c"irq_desc_init: Failed to initialize irq_desc slab cache",
    );
}

/// Rust port of `register_irq_handler()`.
///
/// # Safety
/// `desc`, if non-null, must point at a live, fully-initialized `IrqDesc`
/// for the duration of this call (its `handler`/`data`/`dev` fields are
/// copied out; `irq`/`count`/`rcu_head` are overwritten, matching the C
/// original's documented "ignored when registering" contract).
#[no_mangle]
pub unsafe extern "C" fn register_irq_handler(irq_num: c_int, desc: *mut IrqDesc) -> c_int {
    if !(0..IRQCNT as c_int).contains(&irq_num) {
        return neg(crate::bindings::EINVAL);
    }
    // SAFETY: function-level `# Safety` contract: `desc`, if non-null,
    // points at a live `IrqDesc` for the duration of this call.
    let Some(desc) = (unsafe { desc.as_ref() }) else {
        return neg(crate::bindings::EINVAL);
    };

    let new_desc = IrqDesc {
        handler: desc.handler,
        data: desc.data,
        dev: desc.dev,
        irq: irq_num,
        count: 0,
        // SAFETY: `rcu_head` is plain-old-data (pointers/u64/a one-bit
        // bitfield); the all-zero bit pattern is a valid "not yet queued"
        // state, matching the C original's explicit
        // `memset(&desc->rcu_head, 0, sizeof(desc->rcu_head))`.
        rcu_head: unsafe { core::mem::zeroed() },
    };

    // SAFETY: `irq_desc_slab_ptr()` was initialized once by `irq_desc_init`
    // (called before any driver can reach this function) with `IrqDesc`'s
    // exact size.
    let slab_ptr = slab_alloc(irq_desc_slab_ptr()) as *mut IrqDesc;
    let Some(new_desc_ptr) = ptr::NonNull::new(slab_ptr) else {
        return neg(crate::bindings::ENOMEM);
    };
    // SAFETY: freshly slab-allocated, exclusively-owned, correctly sized
    // and aligned memory -- a plain write into uninitialized storage.
    unsafe { new_desc_ptr.as_ptr().write(new_desc) };

    let old_desc = {
        let _g = crate::sync::KSpinlock::from_bindings(&raw mut IRQ_WRITE_LOCK).lock();
        let old = IRQ_DESCS[irq_num as usize].load(Ordering::Acquire);
        if old.is_null() {
            IRQ_DESCS[irq_num as usize].store(new_desc_ptr.as_ptr(), Ordering::Release);
        }
        old
    };
    if !old_desc.is_null() {
        // SAFETY: the slot already held `old_desc`, so `new_desc_ptr` was
        // never published; freeing it back is returning the sole
        // reference to its owning allocator.
        unsafe { slab_free(new_desc_ptr.as_ptr() as *mut c_void) };
        return neg(crate::bindings::EEXIST); // Handler already registered.
    }

    // Enable the PLIC interrupt after the handler is registered.
    if irq_num >= PLIC_IRQ_OFFSET && irq_num < PLIC_IRQ_OFFSET + PLIC_IRQ_CNT {
        plic_enable_irq(irq_num - PLIC_IRQ_OFFSET);
    }

    0
}

/// Rust port of `unregister_irq_handler()`.
// P3-1B: zero callers anywhere in the tree (grep-verified -- matches the
// C original, which also never called it). Demoted from `#[no_mangle]`;
// `#[allow(dead_code)]` documents the gap rather than silently deleting
// still-plausible public API (matches this wave's `goldfish_rtc_init`/
// `sched_timer_add` precedent).
#[allow(dead_code)]
pub(crate) extern "C" fn unregister_irq_handler(irq_num: c_int) -> c_int {
    if !(0..IRQCNT as c_int).contains(&irq_num) {
        return neg(crate::bindings::EINVAL);
    }

    let old_desc = {
        let _g = crate::sync::KSpinlock::from_bindings(&raw mut IRQ_WRITE_LOCK).lock();
        let old = IRQ_DESCS[irq_num as usize].load(Ordering::Acquire);
        if old.is_null() {
            return neg(crate::bindings::ENOENT); // No handler registered.
        }
        IRQ_DESCS[irq_num as usize].store(ptr::null_mut(), Ordering::Release);
        old
    };

    // SAFETY: `old_desc` is non-null (checked above) and was allocated via
    // `slab_alloc(irq_desc_slab_ptr())`; `call_rcu` only touches the
    // `rcu_head` field until the grace period elapses, then hands the
    // whole pointer to `rcu_free_irq_desc`, which returns it to the same
    // allocator it came from -- non-blocking deferred free, matching the
    // C original.
    unsafe {
        call_rcu(&raw mut (*old_desc).rcu_head, Some(rcu_free_irq_desc), old_desc as *mut c_void);
    }
    0
}

fn do_plic_irq() -> c_int {
    let irq = plic_claim();
    if irq == 0 {
        return 0; // Assume the hart may receive spurious interrupts.
    }
    if irq >= PLIC_IRQ_CNT {
        // SAFETY: format string matches its argument.
        unsafe { printf(c"do_irq: invalid PLIC irq %d\n".as_ptr(), irq) };
        return neg(crate::bindings::ENODEV);
    }
    let irq_full = irq + PLIC_IRQ_OFFSET;

    rcu_read_lock();
    let desc = IRQ_DESCS[irq_full as usize].load(Ordering::Acquire);
    if desc.is_null() {
        rcu_read_unlock();
        // SAFETY: format string matches its argument.
        unsafe { printf(c"do_irq: no handler for irq_num %d\n".as_ptr(), irq_full) };
        plic_complete(irq);
        return neg(crate::bindings::ENODEV);
    }

    // An IRQ with a descriptor always bumps its counter, whether or not a
    // handler is installed (matches the C original).
    count_atomic(desc).fetch_add(1, Ordering::SeqCst);
    // SAFETY: `desc` is a live, RCU-protected `IrqDesc`; if `handler` is
    // present it was supplied by the registrant along with `data`/`dev`
    // and is safe to call with that exact triple.
    unsafe {
        if let Some(h) = (*desc).handler {
            h(irq_full, (*desc).data, (*desc).dev as *mut c_void);
        }
    }
    rcu_read_unlock();
    plic_complete(irq);
    irq_full
}

/// Rust port of `do_irq()`, called from `kernel/irq/trap.c` (unported --
/// Wave 6) on every trap whose `scause` top bit is set.
///
/// # Safety
/// `tf` must point at a live `struct trapframe` for a trap that is
/// actually an interrupt (`scause`'s top bit set).
// P3-1B: only caller is `irq/trap.rs` (crate-path `use`, not an `extern`
// redeclaration) -- demoted.
pub(crate) unsafe extern "C" fn do_irq(tf: *mut trapframe) -> c_int {
    // SAFETY: `tf` is supplied by the trap-entry asm contract
    // (`kernel/irq/trap.c`); this is the sole dereference in this
    // function, everything else operates on the resulting `scause` value.
    let scause = unsafe { (*tf).scause };
    irq_assert(scause >> 63 != 0, line!(), c"do_irq", c"do_irq: not an interrupt");
    let irq_num = (scause & ((1u64 << 63) - 1)) as c_int;
    if irq_num >= CLINT_IRQ_CNT {
        // SAFETY: format string matches its argument.
        unsafe { printf(c"do_irq: invalid irq_num %d\n".as_ptr(), irq_num) };
        return neg(crate::bindings::ENODEV);
    }

    if irq_num == RISCV_S_EXTERNAL_INTERRUPT {
        // PLIC IRQ: handled separately.
        return do_plic_irq();
    }

    rcu_read_lock();
    let desc = IRQ_DESCS[irq_num as usize].load(Ordering::Acquire);
    if desc.is_null() {
        rcu_read_unlock();
        // SAFETY: format string matches its argument.
        unsafe { printf(c"do_irq: no handler for irq_num %d\n".as_ptr(), irq_num) };
        return neg(crate::bindings::ENODEV);
    }

    count_atomic(desc).fetch_add(1, Ordering::SeqCst);
    // SAFETY: see `do_plic_irq`.
    unsafe {
        if let Some(h) = (*desc).handler {
            h(irq_num, (*desc).data, (*desc).dev as *mut c_void);
        }
    }
    rcu_read_unlock();
    irq_num
}
