//! Pure-Rust port of `kernel/proc/signal.c`.
//!
//! Owns the canonical signal handling ABI (`signal_init`, `kill`, `sigaction`,
//! `sigprocmask`, `handle_signal`, etc.), called directly by sibling Rust
//! modules -- the `xv6_sigport_*` C-ABI alias layer that used to front
//! these (in the now-deleted `signal_ffi.rs`) was collapsed in the
//! P3-1B2 sweep.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case, dead_code)]

// See `crate::u`'s doc comment (kernel/lib.rs) for the macro's contract.
use crate::u;

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicU32, AtomicU64, Ordering};

use crate::bindings::{
    ksiginfo as ksiginfo_t, list_node_t, sigacts as sigacts_t, sigaction as sigaction_t,
    sigpending as sigpending_t, sigset_t, slab_alloc, slab_cache_init, slab_cache_t,
    slab_free, spin_init, spin_lock, spin_unlock, spinlock_t, stack as stack_t,
    thread, thread_group, thread_signal_t, thread_state,
};
use crate::lock::spinlock::spin_holding;
use crate::machine::{cpuid, CpuLocal};
use crate::proc::proc_shims::{
    xv6_current_thread, xv6_panic, xv6_pid_rlock, xv6_pid_runlock,
};
use crate::proc::proc_assert_holding;
// P3-D2a: the other scheduler wake/yield entry points join
// `scheduler_wakeup_stopped` as plain crate-path imports (their former
// `extern "C"` redeclarations in the block below are gone).
use crate::proc::{scheduler_wakeup, scheduler_wakeup_interruptible, scheduler_yield};
use crate::proc::scheduler_wakeup_stopped;
use crate::proc::tg_dequeue_signal;
use crate::proc::tg_signal_send;
use crate::proc::tg_sigpending_empty;
use crate::ipi::ipi_send_single;
use crate::proc::access::{
    KsigInfoAccess, SchedEntityRef, SigPendingRef, SigactsAccess, SpinLockRef, ThreadAccess,
    ThreadGroupAccess, ThreadSignalAccess, ThreadSignalPtrRef, atomic_dec_unless_i32,
    atomic_inc_i32, is_err, list_container_of as container_of,
    list_node_detach_raw, list_node_init_raw, list_node_is_empty_raw, list_node_next_raw,
    list_node_push_back_raw, write_out,
};
use crate::kstd::{Errno, KResult};

// ---------- constants ----------
const SLAB_FLAG_STATIC: u64 = 1 << 1;

const NSIG: c_int = 32;
const SIGHUP: c_int = 1;
const SIGINT: c_int = 2;
const SIGQUIT: c_int = 3;
const SIGILL: c_int = 4;
const SIGTRAP: c_int = 5;
const SIGABRT: c_int = 6;
const SIGBUS: c_int = 7;
const SIGFPE: c_int = 8;
const SIGKILL: c_int = 9;
const SIGUSR1: c_int = 10;
const SIGSEGV: c_int = 11;
const SIGUSR2: c_int = 12;
const SIGPIPE: c_int = 13;
const SIGALRM: c_int = 14;
const SIGTERM: c_int = 15;
const SIGSTKFLT: c_int = 16;
const SIGCHLD: c_int = 17;
const SIGCONT: c_int = 18;
const SIGSTOP: c_int = 19;
const SIGTSTP: c_int = 20;
const SIGTTIN: c_int = 21;
const SIGTTOU: c_int = 22;
const SIGURG: c_int = 23;
const SIGXCPU: c_int = 24;
const SIGXFSZ: c_int = 25;
const SIGVTALRM: c_int = 26;
const SIGPROF: c_int = 27;
const SIGWINCH: c_int = 28;
const SIGIO: c_int = 29;
const SIGPWR: c_int = 30;
const SIGSYS: c_int = 31;

const SA_NOCLDSTOP: c_int = 0x00000001;
const SA_SIGINFO: c_int = 0x00000004;
const SA_NODEFER: c_int = 0x00000020;
const SA_RESETHAND: c_int = 0x00000040;

const SS_DISABLE: c_int = 0x4;

const SIG_BLOCK: c_int = 1;
const SIG_UNBLOCK: c_int = 2;
const SIG_SETMASK: c_int = 3;

const CLONE_SIGHAND: u64 = 0x0200000000;
const CLONE_THREAD: u64 = 0x1000000000;

const EINVAL: c_int = 22;
const ESRCH: c_int = 3;
const EAGAIN: c_int = 11;
const EPERM: c_int = 1;
const EINTR: c_int = 4;

const MAX_SIGINFO_PER_SIGNAL: c_int = 8;

// sig_defact values (enum)
const SIG_ACT_INVALID: c_int = -1;
const SIG_ACT_IGN: c_int = 0;
const SIG_ACT_TERM: c_int = 1;
const SIG_ACT_CORE: c_int = 2;
const SIG_ACT_STOP: c_int = 3;
const SIG_ACT_CONT: c_int = 4;

const PAGE_SIZE: u64 = 4096;

const THREAD_FLAG_KILLED: u64 = 2;
const THREAD_FLAG_SIGPENDING: u64 = 4;
const THREAD_FLAG_USER_SPACE: u64 = 5;

const THREAD_UNUSED: thread_state = 0;
const THREAD_INTERRUPTIBLE: thread_state = 2;
const THREAD_KIILABLE: thread_state = 3;
const THREAD_TIMER: thread_state = 4;
const THREAD_KIILABLE_TIMER: thread_state = 5;
const THREAD_UNINTERRUPTIBLE: thread_state = 6;
const THREAD_WAKENING: thread_state = 7;
const THREAD_RUNNING: thread_state = 8;
const THREAD_STOPPED: thread_state = 9;
const THREAD_ZOMBIE: thread_state = 11;

const CPU_FLAG_NEEDS_RESCHED: u64 = 1;
const IPI_REASON_RESCHEDULE: c_int = 2;

// ---------- extern decls ----------
unsafe extern "C" {
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;
    fn memmove(d: *mut c_void, s: *const c_void, n: usize) -> *mut c_void;

    // pid lookups
    safe fn get_pid_thread(pid: c_int) -> *mut thread;
    safe fn __proctab_get_initproc() -> *mut thread;
    safe fn thread_tgid(p: *mut thread) -> c_int;

    // tcb_lock
    safe fn tcb_lock(p: *mut thread);
    safe fn tcb_unlock(p: *mut thread);

    // exit
    safe fn exit(code: c_int) -> !;

    // pgroup
    safe fn pgroup_kill(pgid: c_int, signum: c_int) -> c_int;

    // RCU

    // thread group helpers (Rust ports in thread_group.rs)
}

// P3-1B mesh sweep: both are same-crate `pub(crate)` items as of this wave
// (`irq/trap.rs`, only caller anywhere in the tree is this file) --
// referenced via a typed thin wrapper instead of an `extern "C"`
// redeclaration. `irq::trap::Ucontext` and this file's own `UContext`
// (below) are two independently-written, layout-identical `#[repr(C)]`
// mirrors of the same C `ucontext_t` -- reinterpreting the pointer is
// sound, same precedent as `sysproc.rs`'s `thread_clone`/`exec.rs`'s
// `vfork_done` wrappers from this same wave.
#[inline(always)]
fn push_sigframe(p: *mut thread, signo: c_int, sa: *mut sigaction_t, info: *mut ksiginfo_t) -> c_int {
    crate::irq::trap::push_sigframe(p, signo, sa, info)
}
#[inline(always)]
fn restore_sigframe(p: *mut thread, ret_uc: *mut UContext) -> c_int {
    crate::irq::trap::restore_sigframe(p, ret_uc as *mut c_void as *mut _)
}

// ---------- ucontext_t (not in bindings - declare locally) ----------
#[repr(C)]
#[derive(Copy, Clone)]
struct McGpState {
    regs: [u64; 32],
}
#[repr(C)]
#[derive(Copy, Clone)]
struct McFpState {
    f: [u64; 32],
    fcsr: u32,
    _pad: u32,
}
#[repr(C)]
#[derive(Copy, Clone)]
struct MContext {
    sc_regs: McGpState,
    sc_fpregs: McFpState,
}
#[repr(C)]
#[derive(Copy, Clone)]
struct UContext {
    uc_link: *mut UContext,
    uc_sigmask: sigset_t,
    uc_stack: stack_t,
    uc_mcontext: MContext,
}

// ---------- sigset helpers ----------
#[inline]
fn sigbad(signo: c_int) -> bool { signo < 1 || signo > NSIG }
#[inline]
fn sig_mask_bit(signo: c_int) -> sigset_t {
    if sigbad(signo) { 0 } else { 1u64 << (signo - 1) }
}
#[inline]
fn sigismember(set: sigset_t, signo: c_int) -> i32 {
    let m = sig_mask_bit(signo);
    if m == 0 { return -1; }
    if (set & m) != 0 { 1 } else { 0 }
}
#[inline]
fn sigaddset(set: &mut sigset_t, signo: c_int) {
    let m = sig_mask_bit(signo);
    if m != 0 { *set |= m; }
}
#[inline]
fn sigdelset(set: &mut sigset_t, signo: c_int) {
    let m = sig_mask_bit(signo);
    if m != 0 { *set &= !m; }
}
#[inline]
fn sigemptyset(set: &mut sigset_t) { *set = 0; }

#[inline]
fn bits_ffs_g(mut x: u64) -> c_int {
    // 1-based index of lowest set bit, 0 if none
    if x == 0 { return 0; }
    let mut n: c_int = 1;
    while (x & 1) == 0 { x >>= 1; n += 1; }
    n
}

// ---------- thread flags / state helpers ----------
#[inline]
fn ta_of<'a>(p: *mut thread) -> ThreadAccess<'a> {
    // SAFETY: `ta_of` is this file's choke point for `ThreadAccess::assume`; every
    // call site in this file checks `p.is_null()` immediately before calling
    // `ta_of` (see e.g. THREAD_SLEEPING/THREAD_AWOKEN/thread_state_get and the
    // THREAD_* macros above).
    unsafe { ThreadAccess::assume(p) }
}

#[inline]
fn thread_state_get(p: *mut thread) -> thread_state {
    if p.is_null() { return THREAD_UNUSED; }
    ta_of(p).state_load()
}
#[inline]
fn thread_state_set(p: *mut thread, s: thread_state) {
    if p.is_null() { return; }
    ta_of(p).state_store(s);
}
#[inline]
fn flags_load(p: *mut thread) -> u64 {
    if p.is_null() { return 0; }
    ta_of(p).flags_load()
}
#[inline]
fn flag_test(p: *mut thread, bit: u64) -> bool {
    if p.is_null() { return false; }
    ta_of(p).flags_test_bit(bit)
}
#[inline]
fn flag_set(p: *mut thread, bit: u64) {
    if p.is_null() { return; }
    ta_of(p).flags_set_bit(bit);
}
#[inline]
fn flag_clear(p: *mut thread, bit: u64) {
    if p.is_null() { return; }
    ta_of(p).flags_clear_bit(bit);
}

#[inline]
fn THREAD_KILLED(p: *mut thread) -> bool { if p.is_null() { false } else { ta_of(p).flags_test_bit(THREAD_FLAG_KILLED) } }
#[inline]
fn THREAD_SIGPENDING(p: *mut thread) -> bool { if p.is_null() { false } else { ta_of(p).flags_test_bit(THREAD_FLAG_SIGPENDING) } }
#[inline]
fn THREAD_USER_SPACE(p: *mut thread) -> bool { if p.is_null() { false } else { ta_of(p).flags_test_bit(THREAD_FLAG_USER_SPACE) } }
#[inline]
fn THREAD_SET_KILLED(p: *mut thread) { if !p.is_null() { ta_of(p).flags_set_bit(THREAD_FLAG_KILLED) } }

/// Records the signal number that is about to terminate `p`, for the
/// POSIX wait-status encoding built by `xv6_exit_reap_zombie`
/// (`kernel/proc/proc_shims.rs`; see the encode-site comment there).
/// First cause wins: a no-op once `signal.term_signal` is already nonzero,
/// so whichever fatal signal is detected first keeps the attribution even
/// if `THREAD_SET_KILLED` is (harmlessly) called again later by a
/// different detection path. Call this alongside every `THREAD_SET_KILLED`
/// call site that has a concrete `signo` in hand.
#[inline]
fn set_term_signal_first(p: *mut thread, signo: c_int) {
    if p.is_null() || signo <= 0 {
        return;
    }
    // SAFETY: `p` is checked non-null immediately above; every call site
    // below shares the same live-thread precondition as the `THREAD_*`
    // helpers in this file (the thread is either `current()` or a target
    // already dereferenced by the caller for the matching
    // `THREAD_SET_KILLED` call).
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    if ts.term_signal() == 0 {
        ts.set_term_signal(signo);
    }
}
#[inline]
fn THREAD_SET_SIGPENDING(p: *mut thread) { if !p.is_null() { ta_of(p).flags_set_bit(THREAD_FLAG_SIGPENDING) } }
#[inline]
fn THREAD_CLEAR_SIGPENDING(p: *mut thread) { if !p.is_null() { ta_of(p).flags_clear_bit(THREAD_FLAG_SIGPENDING) } }

#[inline]
fn THREAD_IS_INTERRUPTIBLE(s: thread_state) -> bool { s == THREAD_INTERRUPTIBLE }
#[inline]
fn THREAD_SLEEPING(p: *mut thread) -> bool {
    if p.is_null() { return false; }
    let s = ta_of(p).state_load();
    s == THREAD_INTERRUPTIBLE || s == THREAD_UNINTERRUPTIBLE
        || s == THREAD_KIILABLE || s == THREAD_TIMER || s == THREAD_KIILABLE_TIMER
}
#[inline]
fn THREAD_AWOKEN(p: *mut thread) -> bool {
    if p.is_null() { return false; }
    let s = ta_of(p).state_load();
    s == THREAD_RUNNING || s == THREAD_WAKENING
}
#[inline]
fn THREAD_STOPPED_p(p: *mut thread) -> bool {
    if p.is_null() { return false; }
    ta_of(p).state_load() == THREAD_STOPPED
}

#[inline]
fn set_needs_resched() {
    CpuLocal::current().flags_or(CPU_FLAG_NEEDS_RESCHED);
}

// ---------- list helpers ----------
#[inline]
fn ksi_list_entry_offset() -> usize { core::mem::offset_of!(ksiginfo_t, list_entry) }

#[inline]
fn list_first_ksi(head: *mut list_node_t) -> *mut ksiginfo_t {
    if list_node_is_empty_raw(head) { return ptr::null_mut(); }
    container_of::<ksiginfo_t>(list_node_next_raw(head), ksi_list_entry_offset())
}

// ---------- slab pool storage ----------
#[repr(transparent)]
struct CacheCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: both `SIGACTS_POOL` and `KSIGINFO_POOL` are written in full
// by `slab_cache_init` (called once during signal subsystem init,
// before any `slab_alloc` on either cache) and otherwise only accessed
// through the C slab allocator's own internally-synchronized
// primitives — same pattern as `thread_group.rs`'s `TG_POOL`.
unsafe impl Sync for CacheCell {}
static SIGACTS_POOL: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));
static KSIGINFO_POOL: CacheCell = CacheCell(UnsafeCell::new(MaybeUninit::zeroed()));
#[inline]
fn sigacts_pool() -> *mut slab_cache_t { SIGACTS_POOL.0.get() as *mut slab_cache_t }
#[inline]
fn ksiginfo_pool() -> *mut slab_cache_t { KSIGINFO_POOL.0.get() as *mut slab_cache_t }

// SAFETY: `raw_sig_abi!` is the single unsafe-scope marker used throughout
// this file to wrap the actually-unsafe portion of a signal-ABI function
// body: raw `(*ptr).field` access on `*mut thread` / `*mut sigacts_t` /
// `*mut ksiginfo_t` / `*mut UContext` etc., plus calls to
// non-`safe`-qualified externs (`memset`, `memmove`, `spin_init`,
// `slab_alloc`, `slab_free`, `push_sigframe`, `restore_sigframe`) and to
// `spin_holding` (an `unsafe extern "C" fn`, see `lock::spinlock`).
// Each call site's pointer validity is established by the surrounding
// code in that same function rather than restated here: an `is_null()`
// check (or `xv6_panic` on null) a few lines above, or — for the `p: *mut
// thread` / `sa: *mut sigacts_t` parameters most functions take — the
// documented calling convention that callers pass a live thread/sigacts
// pointer (frequently the current thread via `xv6_current_thread()`, or
// a pointer already validated by the caller). Mutations of `*sa` /
// `(*p).signal.*` additionally rely on `sigacts_lock`/`tcb_lock` (or the
// equivalent lock documented at the call site) being held across the
// access, matching the C original's locking discipline.
macro_rules! raw_sig_abi {
    ($($body:tt)*) => {{
        #[allow(unused_unsafe)]
        u! { $($body)* }
    }};
}

// ============================================================================
// signo_default_action
// ============================================================================
#[no_mangle]
pub extern "C" fn signo_default_action(signo: c_int) -> c_int {
    match signo {
        SIGCHLD | SIGURG | SIGWINCH => SIG_ACT_IGN,
        SIGALRM | SIGUSR1 | SIGUSR2 | SIGHUP | SIGINT | SIGIO | SIGKILL
        | SIGPIPE | SIGPROF | SIGPWR | SIGSTKFLT | SIGTERM | SIGVTALRM => SIG_ACT_TERM,
        SIGSTOP | SIGTSTP | SIGTTIN | SIGTTOU => SIG_ACT_STOP,
        SIGCONT => SIG_ACT_CONT,
        SIGABRT | SIGBUS | SIGILL | SIGQUIT | SIGSEGV | SIGSYS | SIGTRAP
        | SIGXCPU | SIGXFSZ | SIGFPE => SIG_ACT_CORE,
        _ => SIG_ACT_INVALID,
    }
}

// ============================================================================
// recalc_sigpending_tsk / recalc_sigpending
// ============================================================================
#[no_mangle]
pub extern "C" fn recalc_sigpending_tsk(p: *mut thread) -> bool {
    if p.is_null() { return false; }
    // SAFETY: `p` is checked non-null at the top of `recalc_sigpending_tsk` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    if ta.sigacts_ptr().is_null() { return false; }
    // SAFETY: `p` is checked non-null at the top of `recalc_sigpending_tsk` above.
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    let mut pending = ts.sig_pending_mask_atomic_load();
    let blocked = ts.sig_mask();
    let tg = ta.thread_group_ptr();
    if !tg.is_null() {
        // SAFETY: `tg` is checked non-null immediately above.
        pending |= unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref().sig_pending_mask_atomic_load();
    }
    if (pending & !blocked) != 0 {
        THREAD_SET_SIGPENDING(p);
        return true;
    }
    false
}

#[no_mangle]
pub extern "C" fn recalc_sigpending() {
    let p = xv6_current_thread();
    if p.is_null() { return; }
    // SAFETY: `p` is checked non-null at the top of `recalc_sigpending` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let sa = ta.sigacts_ptr();
    if sa.is_null() { return; }
    sigacts_lock_impl(sa);
    if !recalc_sigpending_tsk(p) {
        THREAD_CLEAR_SIGPENDING(p);
    }
    sigacts_unlock_impl(sa);
}

// ============================================================================
// sigpending_init / sigpending_destroy / sigpending_clone / sigstack_init
// ============================================================================
#[no_mangle]
pub extern "C" fn sigpending_init(p: *mut thread) {
    if p.is_null() { return; }
    // SAFETY: `p` is checked non-null at the top of `sigpending_init` above.
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    for i in 0..(NSIG as usize) {
        ts.sig_pending_ref_index(i).queue_ref().init();
    }
}

#[no_mangle]
pub extern "C" fn sigpending_destroy(p: *mut thread) {
    if p.is_null() { return; }
    // SAFETY: `p` is checked non-null at the top of `sigpending_destroy` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let sa = ta.sigacts_ptr();
    if !sa.is_null() {
        // SAFETY: `sa` is checked non-null immediately above (`if !sa.is_null()`).
        debug_assert!(unsafe { SigactsAccess::assume(sa) }.lock_ref().holding());
    }
    // SAFETY: `p` is checked non-null at the top of `sigpending_destroy` above.
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    for i in 0..(NSIG as usize) {
        if !ts.sig_pending_ref_index(i).queue_ref().is_empty() {
            xv6_panic(c"sigpending_destroy: pending signals not empty".as_ptr());
        }
    }
    if ts.sig_pending_mask() != 0 {
        xv6_panic(c"sigpending_destroy: pending mask not zero".as_ptr());
    }
}

#[no_mangle]
pub extern "C" fn sigpending_clone(dst: *mut thread_signal_t,
                                          src: *mut thread_signal_t,
                                          clone_flags: u64,
                                          esignal: c_int) {
    // SAFETY: `dst` is an `extern "C"` parameter documented (by this function's C-era
    // contract, mirrored in all Rust callers) to be the address of a live
    // `thread_signal_t` sub-struct; never null in practice.
    let dst = unsafe { ThreadSignalPtrRef::assume(dst) };
    // SAFETY: `src` is an `extern "C"` parameter documented (by this function's C-era
    // contract, mirrored in all Rust callers) to be the address of a live
    // `thread_signal_t` sub-struct; never null in practice.
    let src = unsafe { ThreadSignalPtrRef::assume(src) };
    dst.set_sig_mask(src.sig_mask());
    dst.set_sig_saved_mask(src.sig_saved_mask());
    if (clone_flags & CLONE_THREAD) != 0 {
        dst.set_esignal(0);
    } else {
        dst.set_esignal(esignal as u64);
    }
}

#[no_mangle]
pub extern "C" fn sigstack_init(stack: *mut stack_t) {
    if stack.is_null() { return; }
    raw_sig_abi! {
        (*stack).ss_sp = ptr::null_mut();
        (*stack).ss_flags = SS_DISABLE;
        (*stack).ss_size = 0;
    }
}

// ============================================================================
// ksiginfo_alloc / ksiginfo_free
// ============================================================================
#[no_mangle]
pub extern "C" fn ksiginfo_alloc()-> *mut ksiginfo_t  {
    raw_sig_abi! {
        let ksi = slab_alloc(ksiginfo_pool()) as *mut ksiginfo_t;
        if ksi.is_null() { return ptr::null_mut(); }
        memset(ksi as *mut c_void, 0, core::mem::size_of::<ksiginfo_t>());
        let ka = KsigInfoAccess::assume(ksi);
        ka.list_entry_ref().init();
        ka.set_sender(ptr::null_mut());
        ksi
    }
}

#[no_mangle]
pub extern "C" fn ksiginfo_free(ksi: *mut ksiginfo_t) {
    if !ksi.is_null() {
        raw_sig_abi! { slab_free(ksi as *mut c_void); }
    }
}

// ============================================================================
// sigacts_lock/unlock/holding
// ============================================================================
fn sigacts_lock_ref<'a>(sa: *mut sigacts_t) -> Option<SpinLockRef<'a>> {
    SigactsAccess::from_ptr(sa).map(|s| s.lock_ref())
}
fn sigacts_lock_impl(sa: *mut sigacts_t) {
    sigacts_lock_ref(sa).expect("BUG: sigacts_lock called with null/invalid sigacts pointer").lock();
}
fn sigacts_unlock_impl(sa: *mut sigacts_t) {
    sigacts_lock_ref(sa).expect("BUG: sigacts_unlock called with null/invalid sigacts pointer").unlock();
}
fn sigacts_holding_impl(sa: *mut sigacts_t) -> c_int {
    sigacts_lock_ref(sa).expect("BUG: sigacts_holding called with null/invalid sigacts pointer").holding() as c_int
}

#[no_mangle]
pub extern "C" fn sigacts_lock(sa: *mut sigacts_t) { sigacts_lock_impl(sa) }
#[no_mangle]
pub extern "C" fn sigacts_unlock(sa: *mut sigacts_t) { sigacts_unlock_impl(sa) }
#[no_mangle]
pub extern "C" fn sigacts_holding(sa: *mut sigacts_t) -> c_int { sigacts_holding_impl(sa) }

// ============================================================================
// sigpending_empty
// ============================================================================
#[no_mangle]
pub extern "C" fn sigpending_empty(p: *mut thread, signo: c_int)-> c_int  {
    if p.is_null() { return -EINVAL; }
    raw_sig_abi! {
        let ta = ThreadAccess::assume(p);
        let sa = ta.sigacts_ptr();
        if !sa.is_null() && spin_holding(&raw mut (*sa).lock) == 0 {
            xv6_panic(c"sigpending_empty: sigacts not held".as_ptr());
        }

        if signo == 0 {
            let ts = ThreadSignalAccess::assume_thread(p);
            for i in 1..=NSIG {
                let head = ts.sig_pending_ref_index((i - 1) as usize).queue_ptr();
                while !list_node_is_empty_raw(head) {
                    let ksi = container_of::<ksiginfo_t>(list_node_next_raw(head), ksi_list_entry_offset());
                    KsigInfoAccess::assume(ksi).list_entry_ref().detach();
                    ksiginfo_free(ksi);
                }
            }
            ts.set_sig_pending_mask(0);
            THREAD_CLEAR_SIGPENDING(p);
            return 0;
        }

        if sigbad(signo) { return -EINVAL; }

        let ts = ThreadSignalAccess::assume_thread(p);
        let head = ts.sig_pending_ref_index((signo - 1) as usize).queue_ptr();
        while !list_node_is_empty_raw(head) {
            let ksi = container_of::<ksiginfo_t>(list_node_next_raw(head), ksi_list_entry_offset());
            KsigInfoAccess::assume(ksi).list_entry_ref().detach();
            ksiginfo_free(ksi);
        }
        ts.and_sig_pending_mask(!sig_mask_bit(signo));
        recalc_sigpending_tsk(p);
        0
    }
}

// ============================================================================
// __sig_reset_act_mask / __sig_setdefault
// ============================================================================
fn sig_reset_act_mask(sa: *mut sigacts_t, signo: c_int) {
    raw_sig_abi! {
        sigdelset(&mut (*sa).sa_sigterm, signo);
        sigdelset(&mut (*sa).sa_sigignore, signo);
        if signo != SIGSTOP { sigdelset(&mut (*sa).sa_sigstop, signo); }
        if signo != SIGCONT { sigdelset(&mut (*sa).sa_sigcont, signo); }
    }
}

fn sig_setdefault(sa: *mut sigacts_t, signo: c_int)-> c_int  {
    if sa.is_null() || sigbad(signo) { return -EINVAL; }
    raw_sig_abi! {
        let defact = signo_default_action(signo);
        if defact == SIG_ACT_INVALID { return 0; }
        sig_reset_act_mask(sa, signo);
        match defact {
            SIG_ACT_IGN => sigaddset(&mut (*sa).sa_sigignore, signo),
            SIG_ACT_CONT => sigaddset(&mut (*sa).sa_sigcont, signo),
            SIG_ACT_STOP => sigaddset(&mut (*sa).sa_sigstop, signo),
            SIG_ACT_TERM | SIG_ACT_CORE | SIG_ACT_INVALID => {
                sigaddset(&mut (*sa).sa_sigterm, signo);
            }
            _ => return -EINVAL,
        }
        SigactsAccess::assume(sa).action_ref(signo).clear_handler_and_metadata();
        0
    }
}

// ============================================================================
// sigacts_init / sigacts_exec / sigacts_dup / sigacts_put / signal_init
// ============================================================================
#[no_mangle]
pub extern "C" fn sigacts_init()-> *mut sigacts_t  {
    raw_sig_abi! {
        let sa = slab_alloc(sigacts_pool()) as *mut sigacts_t;
        if sa.is_null() { return ptr::null_mut(); }
        memset(sa as *mut c_void, 0, core::mem::size_of::<sigacts_t>());
        (*sa).sa_sigterm = 0;
        (*sa).sa_sigstop = 0;
        (*sa).sa_sigcont = 0;
        (*sa).sa_sigignore = 0;
        spin_init(&raw mut (*sa).lock, c"sigacts_lock".as_ptr() as *mut c_char);
        (*sa).refcount = 1;
        for i in 1..=NSIG {
            if sig_setdefault(sa, i) != 0 {
                xv6_panic(c"sigacts_init: failed to set default action".as_ptr());
            }
        }
        sa
    }
}

#[no_mangle]
pub extern "C" fn sigacts_exec(sa: *mut sigacts_t) {
    if sa.is_null() { return; }
    raw_sig_abi! {
        sigacts_lock(sa);
        for i in 1..=NSIG {
            let act = SigactsAccess::assume(sa).action_ref(i);
            let is_dfl = act.is_default();
            let is_ign = act.is_ignore();
            if !is_dfl && !is_ign {
                sig_setdefault(sa, i);
            }
            act.set_flags(0);
            act.set_mask(0);
        }
        sigacts_unlock(sa);
    }
}

#[no_mangle]
pub extern "C" fn sigacts_dup(psa: *mut sigacts_t, clone_flags: u64)-> *mut sigacts_t  {
    if psa.is_null() { return ptr::null_mut(); }
    raw_sig_abi! {
        if (clone_flags & CLONE_SIGHAND) != 0 {
            atomic_inc_i32(&raw mut (*psa).refcount);
            return psa;
        }
        let sa = slab_alloc(sigacts_pool()) as *mut sigacts_t;
        if !sa.is_null() {
            sigacts_lock(psa);
            memmove(sa as *mut c_void, psa as *const c_void, core::mem::size_of::<sigacts_t>());
            sigacts_unlock(psa);
            spin_init(&raw mut (*sa).lock, c"sigacts_lock".as_ptr() as *mut c_char);
            (*sa).refcount = 1;
        }
        sa
    }
}

#[no_mangle]
pub extern "C" fn sigacts_put(sa: *mut sigacts_t) {
    if sa.is_null() { return; }
    raw_sig_abi! {
        if !atomic_dec_unless_i32(&raw mut (*sa).refcount, 1) {
            slab_free(sa as *mut c_void);
        }
    }
}

// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn signal_init() {
    raw_sig_abi! {
        slab_cache_init(sigacts_pool(),
            c"sigacts".as_ptr() as *mut c_char,
            core::mem::size_of::<sigacts_t>(),
            SLAB_FLAG_STATIC);
        slab_cache_init(ksiginfo_pool(),
            c"ksiginfo".as_ptr() as *mut c_char,
            core::mem::size_of::<ksiginfo_t>(),
            SLAB_FLAG_STATIC);
    }
}

// ============================================================================
// __signal_send / signal_send / signal_pending / signal_pending_locked / signal_notify
// ============================================================================
fn siginfo_queue_len(p: *mut thread, signo: c_int) -> c_int {
    // SAFETY: `siginfo_queue_len`'s sole call site (below, inside `__signal_send`'s
    // `raw_sig_abi!` block) passes the same `p` already checked non-null at
    // `__signal_send`'s entry.
    unsafe { ThreadSignalAccess::assume_thread(p) }
        .sig_pending_ref_index((signo - 1) as usize)
        .queue_len()
}

#[no_mangle]
pub extern "C" fn __signal_send(p: *mut thread, info: *mut ksiginfo_t)-> c_int  {
    if p.is_null() || info.is_null() { return -EINVAL; }
    raw_sig_abi! {
        let signo = (*info).signo;
        if sigbad(signo) { return -EINVAL; }
        let pstate = thread_state_get(p);
        if pstate == THREAD_UNUSED || pstate == THREAD_ZOMBIE || THREAD_KILLED(p) {
            return -ESRCH;
        }
        let ta = ThreadAccess::assume(p);
        let sa = ta.sigacts_ptr();
        if sa.is_null() { return -EINVAL; }

        sigacts_lock(sa);

        if sigismember((*sa).sa_sigignore, signo) > 0 {
            sigacts_unlock(sa);
            return 0;
        }

        let act = &raw mut (*sa).sa[signo as usize];
        if ((*act).sa_flags & SA_SIGINFO) != 0 {
            // SA_SIGINFO: queue per-signal, with cap
            let qlen = siginfo_queue_len(p, signo);
            if qlen >= MAX_SIGINFO_PER_SIGNAL {
                let head = ThreadSignalAccess::assume_thread(p)
                    .sig_pending_ref_index((signo - 1) as usize)
                    .queue_ptr();
                if !list_node_is_empty_raw(head) {
                    let old = container_of::<ksiginfo_t>(list_node_next_raw(head), ksi_list_entry_offset());
                    KsigInfoAccess::assume(old).list_entry_ref().detach();
                    ksiginfo_free(old);
                }
            }
            let ksi = ksiginfo_alloc();
            if !ksi.is_null() {
                let ka = KsigInfoAccess::assume(ksi);
                ka.copy_from(info);
                ka.list_entry_ref().init();
                let head = ThreadSignalAccess::assume_thread(p)
                    .sig_pending_ref_index((signo - 1) as usize)
                    .queue_ptr();
                ka.list_entry_ref().push_back(head);
            }
            // fall through
        }

        sigaddset(&mut (*p).signal.sig_pending_mask, signo);
        recalc_sigpending_tsk(p);

        let is_stop = sigismember((*sa).sa_sigstop, signo) > 0
            && sigismember((*p).signal.sig_mask, signo) == 0;
        let is_cont = sigismember((*sa).sa_sigcont, signo) > 0
            && sigismember((*p).signal.sig_mask, signo) == 0;
        let is_term = sigismember((*sa).sa_sigterm, signo) > 0;
        let sigmask = (*p).signal.sig_mask;

        sigacts_unlock(sa);

        if is_stop {
            tcb_lock(p);
            let pst = thread_state_get(p);
            if THREAD_IS_INTERRUPTIBLE(pst) {
                tcb_unlock(p);
                scheduler_wakeup(p);
            } else if pst == THREAD_RUNNING {
                tcb_unlock(p);
                let se = ta.sched_entity_ptr();
                if !se.is_null() {
                    let target = SchedEntityRef::assume(se).cpu_id_load_acquire();
                    if target != cpuid() {
                        ipi_send_single(target, IPI_REASON_RESCHEDULE);
                    } else {
                        set_needs_resched();
                    }
                }
            } else {
                tcb_unlock(p);
            }
        }
        if is_cont {
            scheduler_wakeup_stopped(p);
        }
        if is_term {
            THREAD_SET_KILLED(p);
            set_term_signal_first(p, signo);
            if THREAD_STOPPED_p(p) {
                scheduler_wakeup_stopped(p);
            }
        }

        let pmp = &raw const (*p).signal.sig_pending_mask as *const AtomicU64;
        let pending_unmasked = (*pmp).load(Ordering::Acquire) & !sigmask;
        if pending_unmasked != 0 {
            tcb_lock(p);
            signal_notify(p);
            tcb_unlock(p);
        }
        0
    }
}

#[no_mangle]
pub extern "C" fn signal_send(pid: c_int, info: *mut ksiginfo_t)-> c_int  {
    if pid < 0 || info.is_null() { return -EINVAL; }
    // SAFETY: `info` is checked non-null immediately above.
    if sigbad(unsafe { KsigInfoAccess::assume(info) }.signo()) { return -EINVAL; }
    let _rcu = crate::lock::rcu::KRcuRead::new();
    let p = get_pid_thread(pid);
    if is_err(p) || p.is_null() {
        return -ESRCH;
    }
    // SAFETY: `p` is checked non-null (`is_err(p) || p.is_null()`) immediately above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let tg = ta.thread_group_ptr();
    // SAFETY: `tg` is proven non-null by the short-circuiting `&&` (`!tg.is_null() &&
    // ...`).
    if !tg.is_null() && unsafe { ThreadGroupAccess::assume(tg) }.is_kernel() != 0 {
        __signal_send(p, info)
    } else if matches!(ThreadGroupAccess::from_ptr(tg), Some(tga) if tga.tgid() == pid) {
        tg_signal_send(tg, info)
    } else {
        __signal_send(p, info)
    }
}

#[no_mangle]
pub extern "C" fn signal_pending(p: *mut thread) -> bool {
    if p.is_null() { return false; }
    if !THREAD_SIGPENDING(p) { return false; }
    let cur = xv6_current_thread();
    // SAFETY: `p` is checked non-null at the top of `signal_pending` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    if p != cur || ta.sigacts_ptr().is_null() { return true; }
    let sa = ta.sigacts_ptr();
    sigacts_lock_impl(sa);
    let has_pending = recalc_sigpending_tsk(p);
    if !has_pending { THREAD_CLEAR_SIGPENDING(p); }
    sigacts_unlock_impl(sa);
    has_pending
}

#[no_mangle]
pub extern "C" fn signal_pending_locked(p: *mut thread, sa: *mut sigacts_t) -> bool {
    if p.is_null() || sa.is_null() { return false; }
    if !THREAD_SIGPENDING(p) { return false; }
    // SAFETY: `p` is checked non-null at the top of `signal_pending_locked` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    // SAFETY: `p` is checked non-null at the top of `signal_pending_locked` above.
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    let mut pending = ts.sig_pending_mask_atomic_load();
    let blocked = ts.sig_mask();
    let tg = ta.thread_group_ptr();
    if !tg.is_null() {
        // SAFETY: `tg` is checked non-null immediately above.
        pending |= unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref().sig_pending_mask_atomic_load();
    }
    (pending & !blocked) != 0
}

#[no_mangle]
pub extern "C" fn signal_notify(p: *mut thread)-> c_int  {
    if p.is_null() { return -EINVAL; }
    raw_sig_abi! {
        proc_assert_holding(p);
        if THREAD_AWOKEN(p) { return 0; }
        if !THREAD_SLEEPING(p) { return -EAGAIN; }
        if thread_state_get(p) == THREAD_INTERRUPTIBLE {
            tcb_unlock(p);
            scheduler_wakeup_interruptible(p);
            tcb_lock(p);
            return 0;
        }
        -EAGAIN
    }
}

#[no_mangle]
pub extern "C" fn signal_terminated(p: *mut thread) -> bool {
    if p.is_null() { return false; }
    // SAFETY: `p` is checked non-null at the top of `signal_terminated` above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let sa = ta.sigacts_ptr();
    if sa.is_null() { return false; }
    sigacts_lock_impl(sa);
    // SAFETY: `p` is checked non-null at the top of `signal_terminated` above.
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    let masked = ts.sig_pending_mask() & !ts.sig_mask();
    // SAFETY: `sa` is checked non-null immediately above (`if sa.is_null() { return
    // false; }`).
    let t = (masked & unsafe { SigactsAccess::assume(sa) }.sigterm()) != 0;
    sigacts_unlock_impl(sa);
    t
}

// ============================================================================
// signal_test_clear_stopped
// ============================================================================
#[no_mangle]
pub extern "C" fn signal_test_clear_stopped(p: *mut thread)-> bool  {
    if p.is_null() { return false; }
    raw_sig_abi! {
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if sa.is_null() { return THREAD_STOPPED_p(p); }
        sigacts_lock(sa);
        let sigmask = (*p).signal.sig_mask;
        let sigstop_mask = (*sa).sa_sigstop;
        let sigcont_mask = (*sa).sa_sigcont;
        let masked = (*p).signal.sig_pending_mask & !sigmask;
        let pending_stopped = masked & sigstop_mask;
        let pending_cont = masked & sigcont_mask;

        if pending_cont != 0 {
            let mut user_handler = false;
            for signo in 1..=NSIG {
                if sigismember(sigcont_mask, signo) > 0 && sigismember(pending_cont, signo) > 0 {
                    let act = &raw const (*sa).sa[signo as usize];
                    let h = (*act).__bindgen_anon_1.sa_handler;
                    let is_dfl = h.is_none();
                    let is_ign = matches!(h, Some(f) if (f as *const () as usize) == 1);
                    if !is_dfl && !is_ign { user_handler = true; break; }
                }
            }
            (*p).signal.sig_pending_mask &= !sigstop_mask;
            if !user_handler {
                (*p).signal.sig_pending_mask &= !pending_cont;
            }
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);
            return false;
        }
        if pending_stopped != 0 {
            (*p).signal.sig_pending_mask &= !pending_stopped;
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);
            return true;
        }
        sigacts_unlock(sa);
        THREAD_STOPPED_p(p)
    }
}

// ============================================================================
// signal_restore
// ============================================================================
#[no_mangle]
pub extern "C" fn signal_restore(p: *mut thread, context: *mut c_void)-> c_int  {
    if p.is_null() || context.is_null() { return -EINVAL; }
    raw_sig_abi! {
        let ctx = context as *mut UContext;
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if sa.is_null() { return -EINVAL; }
        sigacts_lock(sa);
        (*p).signal.sig_stack = (*ctx).uc_stack;
        (*p).signal.sig_ucontext = (*ctx).uc_link as u64;
        if (*p).signal.sig_ucontext == 0 {
            (*p).signal.sig_mask = (*p).signal.sig_saved_mask;
        } else {
            (*p).signal.sig_mask = (*ctx).uc_sigmask;
            (*p).signal.sig_mask |= (*p).signal.sig_saved_mask;
        }
        sigdelset(&mut (*p).signal.sig_mask, SIGKILL);
        sigdelset(&mut (*p).signal.sig_mask, SIGSTOP);
        sigdelset(&mut (*sa).sa_sigignore, SIGKILL);
        sigdelset(&mut (*sa).sa_sigignore, SIGSTOP);
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);
        0
    }
}

// ============================================================================
// sigaction
// ============================================================================
#[no_mangle]
pub extern "C" fn sigaction(signum: c_int, act: *mut sigaction_t,
                                   oldact: *mut sigaction_t)-> c_int  {
    if signum < 1 || signum > NSIG { return -EINVAL; }
    if signum == SIGKILL || signum == SIGSTOP { return -EINVAL; }
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() { xv6_panic(c"sigaction: current returned NULL".as_ptr()); }
        if !THREAD_USER_SPACE(p) { return -EPERM; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        sigacts_lock(sa);
        if !oldact.is_null() {
            SigactsAccess::assume(sa).copy_action_to(signum, oldact);
        }
        if !act.is_null() {
            let mut clear_pending = false;
            sig_reset_act_mask(sa, signum);
            let input_act = crate::proc::access::SigActionAccess::assume(act);
            let is_dfl = input_act.is_default();
            let is_ign = input_act.is_ignore();

            if is_ign {
                sigaddset(&mut (*sa).sa_sigignore, signum);
                SigactsAccess::assume(sa).copy_action_from(signum, act);
                clear_pending = true;
            } else if is_dfl {
                if sig_setdefault(sa, signum) != 0 {
                    sigacts_unlock(sa);
                    return -EINVAL;
                }
                if sigismember((*sa).sa_sigignore, signum) > 0 {
                    clear_pending = true;
                }
                let pending_term = (*p).signal.sig_pending_mask
                    & (*sa).sa_sigterm & !(*p).signal.sig_mask;
                if pending_term != 0 {
                    THREAD_SET_KILLED(p);
                    set_term_signal_first(p, bits_ffs_g(pending_term));
                }
            } else {
                SigactsAccess::assume(sa).copy_action_from(signum, act);
                let stored_act = SigactsAccess::assume(sa).action_ref(signum);
                stored_act.and_mask(!sig_mask_bit(SIGKILL));
                stored_act.and_mask(!sig_mask_bit(SIGSTOP));
            }
            if clear_pending {
                if sigpending_empty(p, signum) != 0 {
                    sigacts_unlock(sa);
                    return -EINVAL;
                }
                let tg_ptr = ta.thread_group_ptr();
                if !tg_ptr.is_null() {
                    tg_sigpending_empty(tg_ptr, signum);
                }
            }
        }
        sigacts_unlock(sa);
        0
    }
}

// ============================================================================
// sigprocmask
// ============================================================================
#[no_mangle]
pub extern "C" fn sigprocmask(how: c_int, set: *const sigset_t,
                                     oldset: *mut sigset_t)-> c_int  {
    if !set.is_null() && how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK {
        return -EINVAL;
    }
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() { xv6_panic(c"sigprocmask: current returned NULL".as_ptr()); }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if sa.is_null() { xv6_panic(c"sigprocmask: sigacts NULL".as_ptr()); }
        sigacts_lock(sa);
        if !oldset.is_null() { *oldset = (*p).signal.sig_mask; }
        if !set.is_null() {
            let s = *set;
            if how == SIG_SETMASK {
                (*p).signal.sig_saved_mask = s;
                (*p).signal.sig_mask = s;
            } else if how == SIG_BLOCK {
                (*p).signal.sig_saved_mask |= s;
                (*p).signal.sig_mask |= s;
            } else if how == SIG_UNBLOCK {
                (*p).signal.sig_saved_mask &= !s;
                (*p).signal.sig_mask &= !s;
            }
        }
        sigdelset(&mut (*p).signal.sig_saved_mask, SIGKILL);
        sigdelset(&mut (*p).signal.sig_saved_mask, SIGSTOP);
        sigdelset(&mut (*p).signal.sig_mask, SIGKILL);
        sigdelset(&mut (*p).signal.sig_mask, SIGSTOP);
        recalc_sigpending_tsk(p);
        let mut pending = (*p).signal.sig_pending_mask;
        let tg = ta.thread_group_ptr();
        if !tg.is_null() {
            pending |= (*tg).shared_pending.sig_pending_mask;
        }
        let pending_unmasked = pending & !(*p).signal.sig_mask;
        let pending_term = pending_unmasked & (*sa).sa_sigterm;
        if pending_term != 0 {
            THREAD_SET_KILLED(p);
            set_term_signal_first(p, bits_ffs_g(pending_term));
        }
        sigacts_unlock(sa);
        if pending_unmasked != 0 {
            tcb_lock(p);
            signal_notify(p);
            tcb_unlock(p);
        }
        0
    }
}

// ============================================================================
// sigpending (query)
// ============================================================================
#[no_mangle]
pub extern "C" fn sigpending(p: *mut thread, set: *mut sigset_t) -> c_int {
    if set.is_null() { return -EINVAL; }
    if p.is_null() { xv6_panic(c"sigpending: thread NULL".as_ptr()); }
    // SAFETY: `p` is proven non-null by the `xv6_panic` (diverging) null check
    // immediately above.
    let ta = unsafe { ThreadAccess::assume(p) };
    let sa = ta.sigacts_ptr();
    if sa.is_null() { xv6_panic(c"sigpending: sigacts NULL".as_ptr()); }
    sigacts_lock_impl(sa);
    // SAFETY: `p` is proven non-null by the `xv6_panic` (diverging) null check above.
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    let mask = ts.sig_mask();
    let mut pending = ts.sig_pending_mask();
    let tg = ta.thread_group_ptr();
    // SAFETY: `tg` is proven non-null by the enclosing `if !tg.is_null() { ... }`.
    if !tg.is_null() { pending |= unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref().sig_pending_mask(); }
    write_out(set, mask & pending);
    sigacts_unlock_impl(sa);
    0
}

// ============================================================================
// sigreturn
// ============================================================================
#[no_mangle]
pub extern "C" fn sigreturn()-> c_int  {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() { xv6_panic(c"sigreturn: current NULL".as_ptr()); }
        if !THREAD_USER_SPACE(p) { return -EPERM; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        sigacts_lock(sa);
        if (*p).signal.sig_ucontext == 0 {
            sigacts_unlock(sa);
            return -EINVAL;
        }
        sigacts_unlock(sa);
        let mut uc: UContext = core::mem::zeroed();
        if restore_sigframe(p, &raw mut uc) != 0 {
            // Corrupted/invalid saved signal context: not reachable via the
            // normal signal-delivery path (so no `THREAD_SET_KILLED` call
            // has run for this death yet), but every real POSIX kernel
            // treats a broken user stack/context the same way it treats a
            // stray bad memory access -- attribute it to SIGSEGV so
            // `xv6_exit_reap_zombie`'s wait-status encode (proc_shims.rs)
            // produces a well-formed WIFSIGNALED status instead of falling
            // through to the WIFEXITED branch with this call's meaningless
            // `-1` argument.
            THREAD_SET_KILLED(p);
            set_term_signal_first(p, SIGSEGV);
            exit(-1);
        }
        if signal_restore(p, &raw mut uc as *mut c_void) != 0 {
            xv6_panic(c"sigreturn: signal_restore failed".as_ptr());
        }
        0
    }
}

// ============================================================================
// __dequeue_signal_update_pending_nolock + __deliver_signal + handle_signal
// ============================================================================
fn dequeue_signal_update_pending_nolock(p: *mut thread, signo: c_int,
                                               act: *mut sigaction_t)-> KResult<*mut ksiginfo_t>  {
    if p.is_null() || act.is_null() { return Err(Errno::Inval); }
    raw_sig_abi! {
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if !sa.is_null() && spin_holding(&raw mut (*sa).lock) == 0 {
            xv6_panic(c"dequeue_signal: sigacts not held".as_ptr());
        }
        if sigbad(signo) { return Err(Errno::Inval); }
        let sq = &raw mut (*p).signal.sig_pending[(signo - 1) as usize];
        let head = &raw mut (*sq).queue;
        if ((*act).sa_flags & SA_SIGINFO) == 0 {
            sigdelset(&mut (*p).signal.sig_pending_mask, signo);
            return Ok(ptr::null_mut());
        }
        if list_node_is_empty_raw(head) {
            sigdelset(&mut (*p).signal.sig_pending_mask, signo);
            return Ok(ptr::null_mut());
        }
        let info = container_of::<ksiginfo_t>((*head).next, ksi_list_entry_offset());
        list_node_detach_raw(&raw mut (*info).list_entry);
        if list_node_is_empty_raw(head) {
            sigdelset(&mut (*p).signal.sig_pending_mask, signo);
        }
        Ok(info)
    }
}

fn deliver_signal(p: *mut thread, signo: c_int, info: *mut ksiginfo_t,
                         sa: *mut sigaction_t, repeat: *mut bool)-> c_int  {
    raw_sig_abi! {
        if !repeat.is_null() { *repeat = false; }
        if p.is_null() || sa.is_null() { return -1; }
        let action = crate::proc::access::SigActionAccess::assume(sa);
        if action.is_ignore() { return 0; }
        // SA_SIGINFO path: info must not be null (assert relaxed: skip if null)
        let h_addr = action.handler_addr() as u64;
        if h_addr < PAGE_SIZE {
            crate::kprintln!("deliver_signal: invalid handler addr {} signo {}",
                   crate::printf::Ptr(h_addr as u64), signo);
            tcb_lock(p);
            THREAD_SET_KILLED(p);
            set_term_signal_first(p, signo);
            tcb_unlock(p);
            return 0;
        }
        let mut ret = 0;
        let cur = xv6_current_thread();
        if THREAD_USER_SPACE(cur) {
            ret = push_sigframe(p, signo, sa, info);
        }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sigacts = ta.sigacts_ptr();
        sigacts_lock(sigacts);
        if (action.flags() & SA_NODEFER) == 0 {
            sigaddset(&mut (*p).signal.sig_mask, signo);
        }
        (*p).signal.sig_mask |= action.mask();
        sigdelset(&mut (*p).signal.sig_mask, SIGKILL);
        sigdelset(&mut (*p).signal.sig_mask, SIGSTOP);
        recalc_sigpending_tsk(p);
        if (action.flags() & SA_RESETHAND) != 0 {
            if sig_setdefault(sigacts, signo) != 0 {
                xv6_panic(c"deliver_signal: sig_setdefault failed".as_ptr());
            }
        }
        sigacts_unlock(sigacts);
        ret
    }
}

fn notify_parent_child_stopped(p: *mut thread) {
    raw_sig_abi! {
        let mut notify_sigchld = true;
        xv6_pid_rlock();
        let parent = ThreadAccess::from_raw(p).unwrap_unchecked().parent_ptr();
        if !parent.is_null() {
            let pta = ThreadAccess::from_raw(parent).unwrap_unchecked();
            let psa = pta.sigacts_ptr();
            if !psa.is_null() {
                sigacts_lock(psa);
                if ((*psa).sa[SIGCHLD as usize].sa_flags & SA_NOCLDSTOP) != 0 {
                    notify_sigchld = false;
                }
                sigacts_unlock(psa);
            }
        }
        xv6_pid_runlock();
        if !parent.is_null() {
            scheduler_wakeup_interruptible(parent);
            if notify_sigchld {
                kill_proc(parent, SIGCHLD);
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn handle_signal() {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() { xv6_panic(c"handle_signal: current NULL".as_ptr()); }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        if ta.sigacts_ptr().is_null() { return; }
        let sa = ta.sigacts_ptr();
        let tg = ta.thread_group_ptr();
        loop {
            sigacts_lock(sa);
            let sigmask = (*p).signal.sig_mask;
            let sigterm = (*sa).sa_sigterm;
            let sigstop = (*sa).sa_sigstop;
            let sigcont = (*sa).sa_sigcont;
            let mut pending = (*p).signal.sig_pending_mask;
            if !tg.is_null() {
                let sp = &raw const (*tg).shared_pending.sig_pending_mask as *const AtomicU64;
                pending |= (*sp).load(Ordering::Acquire);
            }
            let masked = pending & !sigmask;

            // Termination
            if (masked & sigterm) != 0 || THREAD_KILLED(p) {
                if p == __proctab_get_initproc() && !THREAD_KILLED(p) {
                    (*p).signal.sig_pending_mask &= !sigterm;
                    if !tg.is_null() {
                        (*tg).shared_pending.sig_pending_mask &= !sigterm;
                    }
                    recalc_sigpending_tsk(p);
                    sigacts_unlock(sa);
                    continue;
                }
                THREAD_SET_KILLED(p);
                set_term_signal_first(p, bits_ffs_g(masked & sigterm));
                sigacts_unlock(sa);
                break;
            }

            let pending_cont = masked & sigcont;
            let pending_stop = masked & sigstop;

            if pending_cont != 0 {
                (*p).signal.sig_pending_mask &= !sigstop;
                if !tg.is_null() {
                    (*tg).shared_pending.sig_pending_mask &= !sigstop;
                }
                let mut user_handler = false;
                for signo in 1..=NSIG {
                    if sigismember(sigcont, signo) > 0 && sigismember(pending_cont, signo) > 0 {
                        let act = &raw const (*sa).sa[signo as usize];
                        let h = (*act).__bindgen_anon_1.sa_handler;
                        let is_dfl = h.is_none();
                        let is_ign = matches!(h, Some(f) if (f as *const () as usize) == 1);
                        if !is_dfl && !is_ign { user_handler = true; break; }
                    }
                }
                if !user_handler {
                    (*p).signal.sig_pending_mask &= !pending_cont;
                    if !tg.is_null() {
                        (*tg).shared_pending.sig_pending_mask &= !pending_cont;
                    }
                    recalc_sigpending_tsk(p);
                    sigacts_unlock(sa);
                    continue;
                }
                // fall through to delivery
            } else if pending_stop != 0 {
                (*p).signal.sig_pending_mask &= !pending_stop;
                if !tg.is_null() {
                    (*tg).shared_pending.sig_pending_mask &= !pending_stop;
                }
                (*p).signal.stop_signal = bits_ffs_g(pending_stop);
                recalc_sigpending_tsk(p);
                sigacts_unlock(sa);
                tcb_lock(p);
                thread_state_set(p, THREAD_STOPPED);
                tcb_unlock(p);
                notify_parent_child_stopped(p);
                scheduler_yield();
                continue;
            }

            let signo = if masked != 0 { bits_ffs_g(masked) } else { 0 };
            if signo == 0 || signo > NSIG {
                sigacts_unlock(sa);
                break;
            }
            if sigismember(sigstop, signo) > 0 {
                sigacts_unlock(sa);
                continue;
            }

            let sa_copy: sigaction_t = SigactsAccess::assume(sa).action_copy(signo);
            let from_shared;
            if sigismember((*p).signal.sig_pending_mask, signo) > 0 {
                from_shared = false;
            } else if !tg.is_null() && sigismember((*tg).shared_pending.sig_pending_mask, signo) > 0 {
                from_shared = true;
            } else {
                sigacts_unlock(sa);
                continue;
            }

            let mut repeat = false;
            let info: *mut ksiginfo_t;
            if from_shared && !tg.is_null() {
                info = tg_dequeue_signal(tg, signo);
            } else {
                info = match dequeue_signal_update_pending_nolock(p, signo,
                    &raw const sa_copy as *mut sigaction_t) {
                    Ok(dequeued) => dequeued,
                    Err(_) => xv6_panic(c"handle_signal: dequeue failed".as_ptr()),
                };
            }
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);

            if deliver_signal(p, signo, info,
                &raw const sa_copy as *mut sigaction_t,
                &raw mut repeat) != 0 {
                xv6_panic(c"handle_signal: deliver failed".as_ptr());
            }
            if (sa_copy.sa_flags & SA_SIGINFO) != 0 {
                sigacts_lock(sa);
                let unmasked = sigismember((*p).signal.sig_mask, signo) == 0;
                let mut still_pending = sigismember((*p).signal.sig_pending_mask, signo) > 0;
                if !still_pending && !tg.is_null() {
                    still_pending = sigismember((*tg).shared_pending.sig_pending_mask, signo) > 0;
                }
                sigacts_unlock(sa);
                if unmasked && still_pending { repeat = true; }
            }
            if !info.is_null() { ksiginfo_free(info); }
            if !repeat { break; }
        }

        recalc_sigpending();

        if THREAD_KILLED(p) { exit(-1); }
    }
}

// ============================================================================
// kill / kill_thread / tgkill / tkill / killed / kill_from_kernel / kill_proc
// ============================================================================
fn make_ksiginfo(signum: c_int, sender: *mut thread, si_pid: c_int) -> ksiginfo_t {
    crate::proc::access::make_ksiginfo(signum, sender, si_pid)
}

#[no_mangle]
pub extern "C" fn kill(pid: c_int, signum: c_int) -> c_int {
    if pid < -1 { return pgroup_kill(-pid, signum); }
    if pid == -1 { return -EINVAL; }
    let cur = xv6_current_thread();
    let mut info = make_ksiginfo(signum, cur, thread_tgid(cur));
    signal_send(pid, &mut info)
}

#[no_mangle]
pub extern "C" fn kill_thread(p: *mut thread, signum: c_int)-> c_int  {
    let cur = xv6_current_thread();
    let mut info = make_ksiginfo(signum, cur, thread_tgid(cur));
    let _rcu = crate::lock::rcu::KRcuRead::new();
    __signal_send(p, &mut info)
}

#[no_mangle]
pub extern "C" fn tgkill(tgid: c_int, tid: c_int, signum: c_int) -> c_int {
    if tgid < 0 || tid < 0 || sigbad(signum) { return -EINVAL; }
    let _rcu = crate::lock::rcu::KRcuRead::new();
    let p = get_pid_thread(tid);
    if is_err(p) { return -ESRCH; }
    // SAFETY: `p` is non-null: `get_pid_thread` only ever returns an err-encoded
    // pointer (caught by `is_err(p)` above) or a live, non-null thread
    // pointer.
    let tg_ptr = unsafe { ThreadAccess::assume(p) }.thread_group_ptr();
    match ThreadGroupAccess::from_ptr(tg_ptr) {
        Some(tga) if tga.tgid() == tgid => {}
        _ => return -ESRCH,
    }
    let cur = xv6_current_thread();
    let mut info = make_ksiginfo(signum, cur, thread_tgid(cur));
    __signal_send(p, &mut info)
}

#[no_mangle]
pub extern "C" fn tkill(tid: c_int, signum: c_int) -> c_int {
    if tid < 0 || sigbad(signum) { return -EINVAL; }
    let _rcu = crate::lock::rcu::KRcuRead::new();
    let p = get_pid_thread(tid);
    if is_err(p) { return -ESRCH; }
    let cur = xv6_current_thread();
    let mut info = make_ksiginfo(signum, cur, thread_tgid(cur));
    __signal_send(p, &mut info)
}

#[no_mangle]
pub extern "C" fn killed(p: *mut thread) -> c_int {
    if p.is_null() { return 0; }
    if THREAD_KILLED(p) { 1 } else { 0 }
}

fn signal_send_to_tgroup(tgid: c_int, info: *mut ksiginfo_t)-> c_int  {
    let _rcu = crate::lock::rcu::KRcuRead::new();
    let leader = get_pid_thread(tgid);
    if is_err(leader) { return -ESRCH; }
    // SAFETY: `leader` is non-null: `get_pid_thread` only ever returns an err-encoded
    // pointer (caught by `is_err(leader)` above) or a live, non-null thread
    // pointer.
    let tg = unsafe { ThreadAccess::assume(leader) }.thread_group_ptr();
    if matches!(ThreadGroupAccess::from_ptr(tg), Some(tga) if tga.tgid() == tgid) {
        return tg_signal_send(tg, info);
    }
    // SAFETY: `tg` is proven non-null by the short-circuiting `&&` (`!tg.is_null() &&
    // ...`).
    if !tg.is_null() && !unsafe { ThreadGroupAccess::assume(tg) }.group_leader_ptr().is_null() {
        return tg_signal_send(tg, info);
    }
    __signal_send(leader, info)
}

#[no_mangle]
pub extern "C" fn kill_from_kernel(pid: c_int, signum: c_int) -> c_int {
    if sigbad(signum) && signum != 0 { return -EINVAL; }
    let mut info = make_ksiginfo(signum, ptr::null_mut(), 0);
    if signum == 0 {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let p = get_pid_thread(pid);
        if is_err(p) { return -ESRCH; }
        return 0;
    }
    signal_send_to_tgroup(pid, &mut info)
}

#[no_mangle]
pub extern "C" fn kill_proc(p: *mut thread, signum: c_int)-> c_int  {
    if p.is_null() || sigbad(signum) { return -EINVAL; }
    let cur = xv6_current_thread();
    let mut info = make_ksiginfo(signum, cur, if cur.is_null() { 0 } else { thread_tgid(cur) });
    let _rcu = crate::lock::rcu::KRcuRead::new();
    // SAFETY: `p` is checked non-null at the top of `kill_proc` above.
    let tg = unsafe { ThreadAccess::assume(p) }.thread_group_ptr();
    if !tg.is_null() {
        return tg_signal_send(tg, &mut info);
    }
    __signal_send(p, &mut info)
}

// ============================================================================
// sigsuspend / sigwait
// ============================================================================
#[no_mangle]
pub extern "C" fn sigsuspend(mask: *const sigset_t)-> c_int  {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() || mask.is_null() { return -EINVAL; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if sa.is_null() { xv6_panic(c"sigsuspend: sigacts NULL".as_ptr()); }
        sigacts_lock(sa);
        let saved = (*p).signal.sig_mask;
        (*p).signal.sig_saved_mask = (*p).signal.sig_mask;
        (*p).signal.sig_mask = *mask;
        sigdelset(&mut (*p).signal.sig_mask, SIGKILL);
        sigdelset(&mut (*p).signal.sig_mask, SIGSTOP);
        let mut pending_unmasked = (*p).signal.sig_pending_mask & !(*p).signal.sig_mask;
        let tg = ta.thread_group_ptr();
        if !tg.is_null() {
            pending_unmasked |= (*tg).shared_pending.sig_pending_mask & !(*p).signal.sig_mask;
        }
        if pending_unmasked != 0 {
            (*p).signal.sig_mask = saved;
            (*p).signal.sig_saved_mask = saved;
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);
            return -EINTR;
        }
        recalc_sigpending_tsk(p);
        sigacts_unlock(sa);
        thread_state_set(p, THREAD_INTERRUPTIBLE);
        scheduler_yield();
        -EINTR
    }
}

#[no_mangle]
pub extern "C" fn sigwait(set: *const sigset_t, sig: *mut c_int)-> c_int  {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() || set.is_null() || sig.is_null() { return -EINVAL; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if sa.is_null() { xv6_panic(c"sigwait: sigacts NULL".as_ptr()); }
        loop {
            sigacts_lock(sa);
            let wait_set = *set;
            let mut pending_wanted = (*p).signal.sig_pending_mask & wait_set;
            let tg = ta.thread_group_ptr();
            if !tg.is_null() {
                pending_wanted |= (*tg).shared_pending.sig_pending_mask & wait_set;
            }
            if pending_wanted != 0 {
                for signo in 1..=NSIG {
                    if sigismember(pending_wanted, signo) <= 0 { continue; }
                    if sigismember((*p).signal.sig_pending_mask, signo) > 0 {
                        let sq = &raw mut (*p).signal.sig_pending[(signo - 1) as usize];
                        let head = &raw mut (*sq).queue;
                        if !list_node_is_empty_raw(head) {
                            let ksi = container_of::<ksiginfo_t>((*head).next, ksi_list_entry_offset());
                            list_node_detach_raw(&raw mut (*ksi).list_entry);
                            ksiginfo_free(ksi);
                        }
                        if list_node_is_empty_raw(head) {
                            sigdelset(&mut (*p).signal.sig_pending_mask, signo);
                        }
                    } else if !tg.is_null() && sigismember((*tg).shared_pending.sig_pending_mask, signo) > 0 {
                        let ksi = tg_dequeue_signal(tg, signo);
                        if !ksi.is_null() { ksiginfo_free(ksi); }
                    }
                    *sig = signo;
                    recalc_sigpending_tsk(p);
                    sigacts_unlock(sa);
                    return 0;
                }
            }
            let saved_mask = (*p).signal.sig_mask;
            (*p).signal.sig_mask &= !wait_set;
            sigdelset(&mut (*p).signal.sig_mask, SIGKILL);
            sigdelset(&mut (*p).signal.sig_mask, SIGSTOP);
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);

            thread_state_set(p, THREAD_INTERRUPTIBLE);
            scheduler_yield();

            sigacts_lock(sa);
            (*p).signal.sig_mask = saved_mask;
            recalc_sigpending_tsk(p);
            sigacts_unlock(sa);

            if killed(p) != 0 { return -EINTR; }
        }
    }
}
