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

/// IRQ descriptor table (RCU-protected register/unregister) and interrupt
/// dispatch -- ZST housing every free fn this file used to export at crate
/// scope (P3-OO mesh sweep, see `git show 91ef670` for the established
/// convention). No receiver: every fn below keeps its exact C-derived
/// parameter list.
pub(crate) struct IrqCore;

impl IrqCore {
    /// `PLIC_IRQ(hw_irq)`.
    #[inline(always)]
    pub const fn plic_irq(hw_irq: c_int) -> c_int {
        hw_irq + PLIC_IRQ_OFFSET
    }
}

/// fn-pointer-callback-slot -> trait-dispatch campaign (precedents:
/// `CdevOps`/`PcacheOps`/`NetdevOps`/`KobjectRelease`/`BioEndIo`).
/// Replaces the old C-shaped `irq_handler_t` (`void (*)(int irq, void
/// *data, device_t *dev)`) fn-pointer typedef. Parameters named after
/// the two call sites below (`do_plic_irq`/`do_irq`): `irq` is the
/// full software IRQ number (already PLIC-offset where applicable),
/// `data` is the registrant-supplied opaque context copied out of
/// `IrqDesc::data` (e.g. a disk index, an `X1EmacSoftc*`, an
/// `&AtomicU64` for the timer tick counter), `dev` is `IrqDesc::dev`
/// cast to `*mut c_void` (a `device_t*`, or null when the registrant
/// didn't supply one).
pub trait IrqHandler: Sync {
    /// # Safety
    /// Must only be invoked from interrupt-dispatch context
    /// (`IrqCore::do_irq`/`do_plic_irq`) with the exact `data`/`dev`
    /// pair this handler's `IrqDesc` was registered with.
    unsafe fn handle(&self, irq: c_int, data: *mut c_void, dev: *mut c_void);
}

/// Mirrors C `struct irq_desc` field-for-field (see module doc for the
/// bindgen-coverage rationale). `handler`/`data`/`dev` are caller-supplied;
/// `irq`/`count` are "ignored when registering" (overwritten by
/// `register_irq_handler`); `rcu_head` backs the deferred-free path in
/// `unregister_irq_handler`.
///
/// `handler` was a `Option<IrqHandlerFn>` (an 8-byte `unsafe extern "C"
/// fn` pointer) before the fn-pointer -> trait-dispatch conversion; it
/// is now a 16-byte `Option<&'static dyn IrqHandler>` fat pointer
/// (`None` is the null-data-pointer niche, same reasoning as
/// `Cdev`'s/`Pcache`'s `Option<&'static dyn _Ops>`). This is a
/// same-hart-visible, RCU-protected-by-the-*pointer* field (the whole
/// `IrqDesc` is reached via a single `AtomicPtr<IrqDesc>` load in
/// `IRQ_DESCS`); the extra 8 bytes only shift `data`/`dev`/`irq`/
/// `count`/`rcu_head` within the block that
/// `register_irq_handler`/`unregister_irq_handler` allocate and free
/// as a whole via the slab cache — no reader ever takes a raw
/// `&IrqDesc` across a wait loop, so this growth does not touch the
/// freeze/noalias hazard class.
#[repr(C)]
pub struct IrqDesc {
    pub handler: Option<&'static dyn IrqHandler>,
    pub data: *mut c_void,
    pub dev: *mut device_t,
    pub irq: c_int,
    pub count: u64,
    pub rcu_head: rcu_head,
}

// Layout assert (host-native rustc layout probe + const-eval
// confirmation, precedent: `pcache.rs`/`kobject.rs`/`bio.rs`). `handler`
// widened 8 -> 16 bytes (fn pointer -> fat pointer); every field after
// it shifts +8; the struct itself has no tail padding to absorb the
// growth (unlike `Pcache`'s `align(64)`), so `size_of::<IrqDesc>()`
// grows 80 -> 88. `IRQ_DESCS` stores `AtomicPtr<IrqDesc>` (a plain
// pointer, not an embedded struct), so its array sizing is unaffected.
const _: () = {
    assert!(core::mem::size_of::<Option<&'static dyn IrqHandler>>() == 16, "irq handler fat pointer size");
    assert!(core::mem::align_of::<Option<&'static dyn IrqHandler>>() == 8, "irq handler fat pointer alignment");
    assert!(core::mem::size_of::<IrqDesc>() == 88, "irq_desc size");
    assert!(core::mem::align_of::<IrqDesc>() == 8, "irq_desc alignment");
    assert!(core::mem::offset_of!(IrqDesc, handler) == 0, "irq_desc.handler offset");
    assert!(core::mem::offset_of!(IrqDesc, data) == 16, "irq_desc.data offset");
    assert!(core::mem::offset_of!(IrqDesc, dev) == 24, "irq_desc.dev offset");
    assert!(core::mem::offset_of!(IrqDesc, irq) == 32, "irq_desc.irq offset");
    assert!(core::mem::offset_of!(IrqDesc, count) == 40, "irq_desc.count offset");
    assert!(core::mem::offset_of!(IrqDesc, rcu_head) == 48, "irq_desc.rcu_head offset");
};

// ===========================================================================
// Externs.
// ===========================================================================
// P3-1B mesh sweep: all three are same-crate `pub(crate)` items as of this
// wave (only caller anywhere in the tree is this file), referenced via
// their crate path instead of `extern "C"` redeclarations.
use crate::irq::plic::Plic;

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::Printf;

unsafe extern "C" {
    // printf is variadic, so it cannot be declared `safe`.
}

// P3-D3b: lock/rcu.rs's entry points are plain crate-path items now that
// their `#[no_mangle]` exports are gone. `rcu_read_lock`/`_unlock` are
// safe fns (as the old `safe fn` redeclarations asserted); `call_rcu` is
// genuinely `unsafe fn` and its single call site below already sits in an
// `unsafe` block.
use crate::lock::rcu::Rcu;

// P3-D3a: the slab entry points are genuinely `unsafe fn` in
// `crate::mm::slab` now that their `#[no_mangle]` exports are gone; this
// file's original extern declarations asserted `safe fn` (usual FFI
// facade) and typed the cache pointer as the bindgen `slab_cache_t`
// rather than `crate::mm::slab::SlabCache` (same layout — see
// `cffi::raw`'s identical note). Thin cast + safe-facade wrappers
// preserve both.
impl IrqCore {
    /// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
    #[inline]
    fn slab_cache_init(
        cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
    ) -> c_int {
        unsafe {
            crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
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
        Printf::__panic_start();
        crate::kprintln!(
            "ASSERTION_FAILURE {}:{}: In function '{}':",
            "kernel/irq/irq_core.rs",
            line as c_int,
            crate::printf::Cs(function.as_ptr()),
        );
        crate::kprint!("{}", crate::printf::Cs(msg.as_ptr()));
        crate::kprintln!();
        Printf::__panic_end()
    }
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

impl IrqCore {
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
}

unsafe extern "C" fn rcu_free_irq_desc(data: *mut c_void) {
    // SAFETY: `data` is the `IrqDesc*` `unregister_irq_handler` published
    // to `call_rcu` below; by the time RCU invokes this callback, the
    // grace period has elapsed and no reader can still hold a reference,
    // so this is the sole (now exclusive) owner returning the allocation.
    unsafe { IrqCore::slab_free(data) };
}

impl IrqCore {
    /// Rust port of `irq_desc_init()`, called once from `start_kernel.c`.
    // P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
    // `extern` redeclaration) -- demoted.
    pub(crate) extern "C" fn irq_desc_init() {
        const NAME: &[u8] = b"irq_desc\0";
        // SAFETY: `NAME` is a 'static NUL-terminated string; `irq_desc_slab_ptr()`
        // is a unique static not yet touched by anything else at this boot
        // stage (single-threaded boot-hart init, before `register_irq_handler`
        // can be called).
        let ret = Self::slab_cache_init(
            Self::irq_desc_slab_ptr(),
            NAME.as_ptr() as *mut c_char,
            core::mem::size_of::<IrqDesc>(),
            crate::bindings::SLAB_FLAG_EMBEDDED as u64,
        );
        Self::irq_assert(
            ret == 0,
            line!(),
            c"irq_desc_init",
            c"irq_desc_init: Failed to initialize irq_desc slab cache",
        );
    }
}

/// Rust port of `register_irq_handler()`.
///
/// # Safety
/// `desc`, if non-null, must point at a live, fully-initialized `IrqDesc`
/// for the duration of this call (its `handler`/`data`/`dev` fields are
/// copied out; `irq`/`count`/`rcu_head` are overwritten, matching the C
/// original's documented "ignored when registering" contract).
// P3-D3c: `#[no_mangle] extern "C"` dropped -- every caller imports it by
// crate path.
impl IrqCore {
    pub unsafe fn register_irq_handler(irq_num: c_int, desc: *mut IrqDesc) -> c_int {
        if !(0..IRQCNT as c_int).contains(&irq_num) {
            return Self::neg(crate::bindings::EINVAL);
        }
        // SAFETY: function-level `# Safety` contract: `desc`, if non-null,
        // points at a live `IrqDesc` for the duration of this call.
        let Some(desc) = (unsafe { desc.as_ref() }) else {
            return Self::neg(crate::bindings::EINVAL);
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
        let slab_ptr = Self::slab_alloc(Self::irq_desc_slab_ptr()) as *mut IrqDesc;
        let Some(new_desc_ptr) = ptr::NonNull::new(slab_ptr) else {
            return Self::neg(crate::bindings::ENOMEM);
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
            unsafe { Self::slab_free(new_desc_ptr.as_ptr() as *mut c_void) };
            return Self::neg(crate::bindings::EEXIST); // Handler already registered.
        }

        // Enable the PLIC interrupt after the handler is registered.
        if irq_num >= PLIC_IRQ_OFFSET && irq_num < PLIC_IRQ_OFFSET + PLIC_IRQ_CNT {
            Plic::plic_enable_irq(irq_num - PLIC_IRQ_OFFSET);
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
            return Self::neg(crate::bindings::EINVAL);
        }

        let old_desc = {
            let _g = crate::sync::KSpinlock::from_bindings(&raw mut IRQ_WRITE_LOCK).lock();
            let old = IRQ_DESCS[irq_num as usize].load(Ordering::Acquire);
            if old.is_null() {
                return Self::neg(crate::bindings::ENOENT); // No handler registered.
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
            Rcu::call(&raw mut (*old_desc).rcu_head, Some(rcu_free_irq_desc), old_desc as *mut c_void);
        }
        0
    }

    fn do_plic_irq() -> c_int {
        let irq = Plic::plic_claim();
        if irq == 0 {
            return 0; // Assume the hart may receive spurious interrupts.
        }
        if irq >= PLIC_IRQ_CNT {
            crate::kprintln!("do_irq: invalid PLIC irq {}", irq);
            return Self::neg(crate::bindings::ENODEV);
        }
        let irq_full = irq + PLIC_IRQ_OFFSET;

        Rcu::read_lock();
        let desc = IRQ_DESCS[irq_full as usize].load(Ordering::Acquire);
        if desc.is_null() {
            Rcu::read_unlock();
            crate::kprintln!("do_irq: no handler for irq_num {}", irq_full);
            Plic::plic_complete(irq);
            return Self::neg(crate::bindings::ENODEV);
        }

        // An IRQ with a descriptor always bumps its counter, whether or not a
        // handler is installed (matches the C original).
        Self::count_atomic(desc).fetch_add(1, Ordering::SeqCst);
        // SAFETY: `desc` is a live, RCU-protected `IrqDesc`; if `handler` is
        // present it was supplied by the registrant along with `data`/`dev`
        // and is safe to call with that exact triple.
        unsafe {
            if let Some(h) = (*desc).handler {
                h.handle(irq_full, (*desc).data, (*desc).dev as *mut c_void);
            }
        }
        Rcu::read_unlock();
        Plic::plic_complete(irq);
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
        Self::irq_assert(scause >> 63 != 0, line!(), c"do_irq", c"do_irq: not an interrupt");
        let irq_num = (scause & ((1u64 << 63) - 1)) as c_int;
        if irq_num >= CLINT_IRQ_CNT {
            crate::kprintln!("do_irq: invalid irq_num {}", irq_num);
            return Self::neg(crate::bindings::ENODEV);
        }

        if irq_num == RISCV_S_EXTERNAL_INTERRUPT {
            // PLIC IRQ: handled separately.
            return Self::do_plic_irq();
        }

        Rcu::read_lock();
        let desc = IRQ_DESCS[irq_num as usize].load(Ordering::Acquire);
        if desc.is_null() {
            Rcu::read_unlock();
            crate::kprintln!("do_irq: no handler for irq_num {}", irq_num);
            return Self::neg(crate::bindings::ENODEV);
        }

        Self::count_atomic(desc).fetch_add(1, Ordering::SeqCst);
        // SAFETY: see `do_plic_irq`.
        unsafe {
            if let Some(h) = (*desc).handler {
                h.handle(irq_num, (*desc).data, (*desc).dev as *mut c_void);
            }
        }
        Rcu::read_unlock();
        irq_num
    }
}
