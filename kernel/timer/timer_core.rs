//! Timer wheel core — Rust port of `kernel/timer/timer.c` (Phase 2 Wave 8,
//! see `docs/rustify/phase2_plan.md`).
//!
//! # Data structure
//!
//! A [`timer_root`] is a red-black tree (`kernel/rbtree.c`/`kernel/bintree.c`
//! — Wave 1) of [`timer_node`]s ordered by `expires`, *plus* a doubly-linked
//! list (`list_entry`) threaded through the same nodes in the same order.
//! The tree gives O(log n) insert/remove; the list gives O(expired count)
//! sequential expiry scanning in [`timer_tick`] without re-walking the tree.
//! `timer_add` keeps both in sync: after linking into the tree it uses
//! `rb_prev_node` on the node's own (freshly linked) rb-node to find its
//! in-order predecessor and splices the list entry right after it (or at
//! the head if there is no predecessor) — an O(1) list insert once the
//! O(log n) tree search has already located the right spot.
//!
//! # Interrupt-context discipline
//!
//! [`clockintr`] is the raw CLINT timer IRQ handler (registered on
//! `CLINT_TIMER_IRQ` = `RISCV_S_TIMER_INTERRUPT`, dispatched from
//! `irq::irq_core::do_irq`'s non-PLIC path). It runs with interrupts
//! disabled, does **no** allocation, **no** sleep, and touches only atomics
//! and CSRs: rearm `stimecmp`, bump the jiffies counter, clear the
//! scheduler-tick dirty flag (`sched_timer::sched_timer_tick`), and set
//! `NEEDS_RESCHED`. The heavier work — walking the timer list and invoking
//! callbacks — happens in [`timer_tick`], which is **not** called from this
//! handler. Its only caller is `sched_timer::__do_timer_tick`, itself called
//! once per `scheduler_yield()` (thread context, proc/sched.rs) — so
//! `timer_tick`'s callback dispatch (and everything reachable from it,
//! including workqueue submission) never runs in true interrupt context,
//! even though the plan's hazard note describes this file as "runs partly
//! in interrupt context": only the rearm/bump/flag-clear sliver in
//! `clockintr` actually does.
//!
//! `timer_add`/`timer_remove`/`get_jiffs` are plain spinlock/atomic-guarded
//! operations callable from any context (interrupt or thread) that does not
//! already hold `timer->lock`.

#![allow(non_camel_case_types)]

use core::ffi::{c_char, c_int, c_void};
use core::mem::offset_of;
use core::ptr;
use core::sync::atomic::{AtomicU64, Ordering};

use crate::bindings::{list_node_t, rb_node, rb_root_opts, timer_node, timer_root};
use crate::irq::irq_core::{register_irq_handler, IrqDesc};
use crate::machine;
use crate::sync::KSpinlock;

// ===========================================================================
// FDT/boot-configured globals, exact C names/types preserved. P3-D3c: the
// whole extern mesh around them is gone -- `start.rs`'s `timerinit()` and
// `dev/fdt.rs`'s `fdt_apply_platform_config` write `__jiff_ticks` by crate
// path, and `machine::tick_ms()`/`tick_s()` (through which everything else,
// e.g. `lock/spinlock.rs`, reads the timebase) import
// `__timebase_frequency` by crate path too. Single boot-time writer,
// read-only thereafter — identical contract to `uart.rs`'s `__uart0_*` /
// `irq/plic.rs`'s `__plic_mmio_base`.
// ===========================================================================

/// `RISCV_S_TIMER_INTERRUPT` (`kernel/inc/trap.h`).
const RISCV_S_TIMER_INTERRUPT: u64 = 5;

// P3-1B: no other file references this symbol (grep-verified across the
// whole tree) -- demoted from `#[no_mangle]`.
pub(crate) static mut __clint_timer_irqno: u64 = RISCV_S_TIMER_INTERRUPT;

// P3-D3c: `#[no_mangle]` dropped -- the readers (`ll/ll.rs`,
// `machine/machine.rs`) reach it by crate path now instead of their own
// `extern` redeclarations.
pub(crate) static mut __timebase_frequency: u64 = 10_000_000;

/// Owned here (was `kernel/timer/timer.c`); written once by `start.rs`'s
/// `timerinit()` and by `dev/fdt.rs`'s `fdt_apply_platform_config`, both
/// during single-hart early boot before any interrupt is enabled (both by
/// crate path since P3-D3c -- no more `#[no_mangle]`/`extern` mesh).
pub(crate) static mut __jiff_ticks: u64 = 0;

const CPU_FLAG_NEEDS_RESCHED: u64 = 1;
const CPU_FLAG_BOOT_HART: u64 = 2;

#[inline(always)]
fn is_boot_hart() -> bool {
    machine::CpuLocal::current().flags() & CPU_FLAG_BOOT_HART != 0
}

#[inline(always)]
fn set_needs_resched() {
    machine::CpuLocal::current().flags_or(CPU_FLAG_NEEDS_RESCHED);
}

// ===========================================================================
// `sched_holding` is a same-crate `pub(crate) fn` as of P3-1B (mesh sweep) --
// called directly via its crate path instead of its own `extern "C"`
// redeclaration. `__panic_start`/`__panic_end`/`printf` stay `extern`:
// defined in `printf.rs`, out of this wave's scope.
// ===========================================================================
use crate::proc::sched_holding;

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {
    // printf is variadic, so it cannot be declared `safe`.
}

#[inline]
fn neg(errno: u32) -> c_int {
    -(errno as c_int)
}

/// Replicates the C `assert(expr, fmt)` macro expansion (no format args --
/// every call site below passes a literal message). Same pattern as
/// `irq/irq_core.rs`'s `irq_assert`.
fn timer_assert(cond: bool, line: u32, function: &core::ffi::CStr, msg: &core::ffi::CStr) {
    if cond {
        return;
    }
    __panic_start();
    crate::kprintln!(
        "ASSERTION_FAILURE {}:{}: In function '{}':",
        "kernel/timer/timer_core.rs",
        line as c_int,
        crate::printf::Cs(function.as_ptr()),
    );
    crate::kprint!("{}", crate::printf::Cs(msg.as_ptr()));
    crate::kprintln!();
    __panic_end()
}

// ===========================================================================
// jiffies counter (`static uint64 ticks;` in the C original — file-scope,
// never exported). Registered as the IRQ descriptor's `data` pointer in
// `timer_init`, matching the C original's `.data = &ticks` +
// `__atomic_fetch_add((uint64 *)data, ...)` reinterpret; `get_jiffs` reads
// it directly.
// ===========================================================================
static TICKS: AtomicU64 = AtomicU64::new(0);

/// Rust port of `get_jiffs()`.
// @TODO: consider overflow (matches the C original's own TODO comment).
// P3-D3c: `#[no_mangle] extern "C"` dropped -- every reader (`mm/pcache.rs`,
// `lock/rcu_test.rs`, `proc/sched.rs`, `proc/sysproc.rs`) now imports it by
// crate path.
pub(crate) fn get_jiffs() -> u64 {
    TICKS.load(Ordering::SeqCst)
}

// ===========================================================================
// rb_root_opts — key comparison / extraction callbacks for the timer tree.
// ===========================================================================

/// Rust port of `__timer_root_keys_cmp_fun`. Keys are `timer_node*` cast to
/// `u64` (see `__timer_root_get_key_fun`); primary order is `expires`,
/// ties are broken by node address so no two live nodes ever compare equal.
unsafe extern "C" fn timer_root_keys_cmp_fun(key1: u64, key2: u64) -> c_int {
    let node1 = key1 as *const timer_node;
    let node2 = key2 as *const timer_node;
    // SAFETY: both keys are addresses of live `timer_node`s -- the only
    // producer of tree keys is `timer_root_get_key_fun` below, which is
    // only ever called by `rb_insert_color`/`rb_*` on nodes already linked
    // (or being linked) into this tree by `timer_add`.
    let (e1, e2) = unsafe { ((*node1).expires, (*node2).expires) };
    if e1 < e2 {
        -1
    } else if e1 > e2 {
        1
    } else if key1 < key2 {
        -1
    } else if key1 > key2 {
        1
    } else {
        0
    }
}

/// Rust port of `__timer_root_get_key_fun`.
unsafe extern "C" fn timer_root_get_key_fun(node: *mut rb_node) -> u64 {
    timer_assert(!node.is_null(), line!(), c"timer_root_get_key_fun", c"node is NULL");
    // SAFETY: `node` is a live `rb_node` embedded in a `timer_node` (the
    // only rb-tree this callback is ever installed on), non-null per the
    // assert above.
    let tn = (node as *mut u8).wrapping_sub(offset_of!(timer_node, rb)) as *mut timer_node;
    tn as u64
}

static TIMER_ROOT_OPTS: rb_root_opts = rb_root_opts {
    keys_cmp_fun: Some(timer_root_keys_cmp_fun),
    get_key_fun: Some(timer_root_get_key_fun),
};

/// Rust port of `kernel/inc/bintree.h`'s `rb_root_init()` static inline
/// (no external linkage in C, so unreachable from Rust; reimplemented here
/// the same way `bintree.rs` reimplements the rest of that header's
/// `static inline`s it needs).
unsafe fn rb_root_init(root: *mut crate::bindings::rb_root, opts: *mut rb_root_opts) -> *mut crate::bindings::rb_root {
    if root.is_null() || opts.is_null() {
        return ptr::null_mut();
    }
    // SAFETY: `opts` is `&raw const TIMER_ROOT_OPTS` at every call site
    // below -- a live, fully-initialized 'static `rb_root_opts`.
    let o = unsafe { &*opts };
    if o.keys_cmp_fun.is_none() || o.get_key_fun.is_none() {
        return ptr::null_mut();
    }
    // SAFETY: `root` is caller-owned per this function's contract (mirrors
    // the C `rb_root_init`); we only write its two fields.
    unsafe {
        (*root).node = ptr::null_mut();
        (*root).opts = opts;
    }
    root
}

// ===========================================================================
// List-side helpers: `LIST_FIRST_NODE`/`LIST_NEXT_NODE` (kernel/inc/list.h)
// specialised to `timer_node::list_entry`.
// ===========================================================================

#[inline(always)]
fn list_entry_to_node(head: *mut list_node_t, entry: *mut list_node_t) -> *mut timer_node {
    if entry.is_null() || entry == head {
        return ptr::null_mut();
    }
    machine::list_container_of::<timer_node>(entry, offset_of!(timer_node, list_entry))
}

/// Mirrors `LIST_FIRST_NODE(&timer->list_head, struct timer_node, list_entry)`.
///
/// # Safety
/// `timer` must point at a live, initialized `timer_root`.
unsafe fn first_node(timer: *mut timer_root) -> *mut timer_node {
    // SAFETY: caller contract.
    let head = unsafe { &raw mut (*timer).list_head };
    list_entry_to_node(head, machine::list_entry_next(head))
}

/// Mirrors `LIST_NEXT_NODE(&timer->list_head, node, list_entry)`.
///
/// # Safety
/// `timer` must point at a live, initialized `timer_root`; `node` must be
/// a live node currently linked into `timer`'s list.
unsafe fn next_node(timer: *mut timer_root, node: *mut timer_node) -> *mut timer_node {
    // SAFETY: caller contract.
    let head = unsafe { &raw mut (*timer).list_head };
    let entry = unsafe { &raw mut (*node).list_entry };
    list_entry_to_node(head, machine::list_entry_next(entry))
}

/// Mirrors `__timer_update_next_tick()`.
///
/// # Safety
/// `timer` must point at a live, initialized `timer_root`.
unsafe fn update_next_tick(timer: *mut timer_root) {
    // SAFETY: caller contract.
    let next = unsafe { first_node(timer) };
    unsafe {
        (*timer).next_tick = if next.is_null() { 0 } else { (*next).expires };
    }
}

// ===========================================================================
// clockintr — the raw CLINT timer IRQ handler. See the module doc's
// "Interrupt-context discipline" section: no sleep, no allocation, no lock
// other than the atomic ops below.
// ===========================================================================
unsafe extern "C" fn clockintr(_irq: c_int, data: *mut c_void, _dev: *mut c_void) {
    // Ask for the next timer interrupt. This also clears the interrupt
    // request. `__jiff_ticks` is about a tenth of a second.
    // SAFETY: `__jiff_ticks` has a single boot-time writer (see the module
    // doc), read-only from every hart/interrupt context thereafter.
    let jiff_ticks = unsafe { __jiff_ticks };
    machine::write_stimecmp(machine::read_time() + jiff_ticks);

    if is_boot_hart() {
        // SAFETY: `data` is `&raw mut TICKS` (an `AtomicU64`), set once in
        // `timer_init` and never changed; `AtomicU64` has the same size and
        // alignment as the `uint64` the C original reinterpreted via
        // `__atomic_fetch_add((uint64 *)data, ...)` (cf. `irq/irq_core.rs`'s
        // `count_atomic`).
        let ticks = unsafe { &*(data as *const AtomicU64) };
        ticks.fetch_add(1, Ordering::SeqCst);
        crate::timer::sched_timer::sched_timer_tick();
    }
    if sched_holding() == 0 {
        set_needs_resched();
    }
}

// ===========================================================================
// Public API — exact C names/signatures preserved (called from
// `sched_timer.rs`, `lock/{mutex,rwsem,completion,semaphore}.rs` via their
// own `extern` declarations, and `proc/sysproc.rs` for `get_jiffs`).
// ===========================================================================

/// Rust port of `timer_init()`.
///
/// # Safety
/// `timer` must be null or point at caller-owned, writable storage for a
/// `timer_root` (it is unconditionally zeroed and reinitialized).
// P3-1B: only caller is `timer/sched_timer.rs::sched_timer_init` (already a
// direct crate-path `use`, not an extern block) -- demoted.
pub(crate) unsafe fn timer_init(timer: *mut timer_root) {
    if timer.is_null() {
        return;
    }
    // SAFETY: `timer` is caller-owned per this function's contract; this
    // mirrors the C `memset(timer, 0, sizeof(struct timer_root))`.
    unsafe { ptr::write_bytes(timer as *mut u8, 0, core::mem::size_of::<timer_root>()) };

    // SAFETY: `timer` was just zeroed above and is exclusively owned here.
    unsafe {
        rb_root_init(&raw mut (*timer).root, &raw const TIMER_ROOT_OPTS as *mut rb_root_opts);
        machine::list_entry_init(&raw mut (*timer).list_head);
        (*timer).next_tick = 0;
        (*timer).current_tick = 0;
        (*timer).__bindgen_anon_1.set_valid(1);
    }
    TICKS.store(0, Ordering::SeqCst);
    // SAFETY: `timer` is exclusively owned here, before any other hart can
    // reach it (it is not yet published anywhere).
    KSpinlock::from_bindings(unsafe { &raw mut (*timer).lock }).init(c"timer_lock".as_ptr());

    let mut timer_irq_desc = IrqDesc {
        handler: Some(clockintr),
        data: (&raw const TICKS) as *mut c_void,
        dev: ptr::null_mut(),
        irq: 0,
        count: 0,
        // SAFETY: `rcu_head` is plain-old-data; `register_irq_handler`
        // overwrites it unconditionally (see `irq/irq_core.rs`).
        rcu_head: unsafe { core::mem::zeroed() },
    };
    // SAFETY: `timer_irq_desc` is a live, fully-initialized `IrqDesc` for
    // the duration of this call; `__clint_timer_irqno` has a single
    // boot-time writer (see the module doc), read-only thereafter.
    let ret = unsafe {
        let irqno = __clint_timer_irqno as c_int;
        register_irq_handler(irqno, &raw mut timer_irq_desc)
    };
    timer_assert(ret == 0, line!(), c"timer_init", c"timer_init: Failed to register timer IRQ handler");
}

/// Rust port of `timer_node_init()`.
///
/// The C original accepts `retry_limit` but never stores it into
/// `node->retry_limit` (a pre-existing dead-parameter bug, confirmed by
/// inspection of `kernel/timer/timer.c` -- the field is left at its
/// `memset`-zeroed value of `0`). Because `timer_tick()` removes a node
/// once `node->retry >= node->retry_limit`, and `retry` starts at `0` and
/// is incremented *before* that comparison, every timer node is actually
/// removed after its **first** callback invocation regardless of the
/// `retry_limit` the caller passed in (`TIMER_DEFAULT_RETRY_LIMIT` = 3 for
/// `sched_timer_set`, `1` for `sched_timer.c`'s own workqueue path -- both
/// end up behaving like `1`). This port preserves that exact behavior
/// (1:1 fidelity per Wave 8's charter); flagged here and in the Wave 8
/// report as a pre-existing bug, not fixed in this wave.
///
/// # Safety
/// `node` must be null or point at caller-owned, writable storage for a
/// `timer_node` (it is unconditionally zeroed and reinitialized).
// P3-1B: only caller is `timer/sched_timer.rs` (direct crate-path `use`) --
// demoted.
pub(crate) unsafe fn timer_node_init(
    node: *mut timer_node,
    expires: u64,
    callback: Option<unsafe extern "C" fn(*mut timer_node)>,
    data: *mut c_void,
    _retry_limit: c_int,
) {
    if node.is_null() {
        return;
    }
    // SAFETY: `node` is caller-owned per this function's contract; mirrors
    // the C `memset(node, 0, sizeof(struct timer_node))`.
    unsafe {
        ptr::write_bytes(node as *mut u8, 0, core::mem::size_of::<timer_node>());
        machine::rb_node_init(&raw mut (*node).rb);
        machine::list_entry_init(&raw mut (*node).list_entry);
        (*node).expires = expires;
        (*node).callback = callback;
        (*node).data = data;
    }
}

/// Rust port of `timer_add()`.
///
/// # Safety
/// `timer` must be null or a live, initialized `timer_root`; `node` must
/// be null or a live, initialized `timer_node` (via [`timer_node_init`])
/// not currently linked into any `timer_root`.
// P3-1B: only caller is `timer/sched_timer.rs` (direct crate-path `use`) --
// demoted.
pub(crate) unsafe fn timer_add(timer: *mut timer_root, node: *mut timer_node) -> c_int {
    if timer.is_null() || node.is_null() {
        return neg(crate::bindings::EINVAL);
    }
    // SAFETY: non-null per the check above; caller contract guarantees
    // liveness for the duration of this call.
    if unsafe { (*node).callback }.is_none() {
        return neg(crate::bindings::EINVAL);
    }

    // SAFETY: `timer` is non-null and live per the caller contract.
    let _g = KSpinlock::from_bindings(unsafe { &raw mut (*timer).lock }).lock();
    // SAFETY: see above.
    unsafe {
        if (*timer).__bindgen_anon_1.valid() == 0 {
            return neg(crate::bindings::EINVAL);
        }
        if (*timer).current_tick >= (*node).expires {
            // Timer already expired.
            return neg(crate::bindings::EINVAL);
        }
    }

    // SAFETY: `timer`/`node` are live per the caller contract; `node` is
    // not linked into any tree yet (also part of the caller contract).
    let inserted = unsafe { crate::rbtree::rb_insert_color(&raw mut (*timer).root, &raw mut (*node).rb) };
    if inserted.is_null() {
        return neg(crate::bindings::ETXTBSY);
    }
    // SAFETY: see above.
    if inserted != unsafe { &raw mut (*node).rb } {
        return neg(crate::bindings::EEXIST);
    }

    // SAFETY: `node` is now linked into `timer`'s tree by the insert above.
    let prev = unsafe { crate::bintree::rb_prev_node(&raw mut (*node).rb) };
    if prev.is_null() {
        // SAFETY: `timer`/`node` are live; `list_head` is a valid list
        // sentinel (initialized by `timer_init`).
        unsafe {
            machine::list_entry_insert_after(&raw mut (*timer).list_head, &raw mut (*node).list_entry);
            (*timer).next_tick = (*node).expires;
        }
    } else {
        // SAFETY: `prev` is a live `rb_node` embedded in a `timer_node`
        // already linked into `timer`'s list (rb-tree and list are kept in
        // the same order by this function's own invariant).
        let prev_node = (prev as *mut u8).wrapping_sub(offset_of!(timer_node, rb)) as *mut timer_node;
        unsafe {
            machine::list_entry_insert_after(&raw mut (*prev_node).list_entry, &raw mut (*node).list_entry);
        }
    }
    // SAFETY: see above.
    unsafe { (*node).timer = timer };
    0
}

/// Mirrors `__timer_remove_unlocked()`. Caller must already hold
/// `(*timer).lock`.
///
/// # Safety
/// `timer` must be a live, initialized `timer_root`; `node` must be a live
/// node currently linked into `timer`.
unsafe fn timer_remove_unlocked(timer: *mut timer_root, node: *mut timer_node) {
    // SAFETY: caller contract.
    unsafe {
        crate::rbtree::rb_delete_node_color(&raw mut (*timer).root, &raw mut (*node).rb);
        machine::list_entry_detach(&raw mut (*node).list_entry);
        (*node).timer = ptr::null_mut();
        update_next_tick(timer);
    }
}

/// Rust port of `timer_remove()`.
///
/// # Safety
/// `node` must point at a live `timer_node` (matches the C original, which
/// also does not null-check `node` itself -- only `node->timer`).
// P3-1B: only caller is `timer/sched_timer.rs` (direct crate-path `use`) --
// demoted.
pub(crate) unsafe fn timer_remove(node: *mut timer_node) {
    // SAFETY: caller contract.
    let timer = unsafe { (*node).timer };
    if timer.is_null() {
        return;
    }
    // SAFETY: `timer` is non-null, therefore live (only ever set to a live
    // `timer_root` by `timer_add`, cleared by removal).
    let _g = KSpinlock::from_bindings(unsafe { &raw mut (*timer).lock }).lock();
    // SAFETY: `node` is live per the caller contract; `timer` is the tree
    // it was linked into (just read from `node->timer` above), still held
    // locked by `_g`.
    unsafe { timer_remove_unlocked(timer, node) };
}

/// Rust port of `timer_tick()`. See the module doc's "Interrupt-context
/// discipline" section for who actually calls this (thread context only,
/// via `sched_timer::__do_timer_tick`).
///
/// # Safety
/// `timer` must be null or a live, initialized `timer_root`.
// P3-1B: only caller is `timer/sched_timer.rs::__do_timer_tick` (direct
// crate-path `use`) -- demoted.
pub(crate) unsafe fn timer_tick(timer: *mut timer_root, ticks: u64) {
    if timer.is_null() || ticks == 0 {
        return;
    }
    // SAFETY: `timer` is non-null per the check above.
    if unsafe { (*timer).__bindgen_anon_1.valid() } == 0 {
        return;
    }

    let _g = KSpinlock::from_bindings(unsafe { &raw mut (*timer).lock }).lock();
    // SAFETY: `timer` is live for the remainder of this function, guarded
    // by `_g`.
    unsafe {
        if (*timer).next_tick == 0 {
            return;
        }
        if (*timer).current_tick >= ticks {
            return;
        }
        (*timer).current_tick = ticks;
        if (*timer).next_tick > ticks {
            // No timer expired.
            return;
        }
    }

    // SAFETY: `timer` remains live and locked (`_g`) for the whole loop.
    let mut pos = unsafe { first_node(timer) };
    while !pos.is_null() {
        let node = pos;
        // SAFETY: `node` is live and currently linked into `timer`.
        let next = unsafe { next_node(timer, node) };
        // SAFETY: see above.
        if unsafe { (*node).expires } > ticks {
            break;
        }
        // SAFETY: see above.
        let callback = unsafe { (*node).callback };
        match callback {
            None => {
                crate::kprintln!("Warning: Timer expired without callback");
                // SAFETY: `node->timer` is still `timer` -- `node` has not
                // been removed yet.
                let nt = unsafe { (*node).timer };
                unsafe { timer_remove_unlocked(nt, node) };
            }
            Some(cb) => {
                // Because many callback functions wake up a thread, and the
                // thread will remove the timer node from the timer_root, we
                // try to call the callback function multiple times until
                // the retry limit is reached or the node is removed by the
                // thread. (See `timer_node_init`'s doc comment: the C
                // original's dead `retry_limit` parameter means this always
                // removes after the first call in practice -- preserved
                // here for 1:1 fidelity.)
                // SAFETY: `node` is live.
                unsafe { (*node).retry += 1 };
                let expired = unsafe { (*node).retry >= (*node).retry_limit };
                if expired {
                    let nt = unsafe { (*node).timer };
                    unsafe { timer_remove_unlocked(nt, node) };
                }
                // SAFETY: `cb` was supplied by the registrant along with
                // `node`'s other fields (via `timer_node_init`) and is
                // safe to call with `node`; `node` itself remains valid
                // memory even if just removed from the tree/list above
                // (removal does not free it -- ownership stays with the
                // caller of `timer_add`).
                unsafe { cb(node) };
            }
        }
        pos = next;
    }
}
