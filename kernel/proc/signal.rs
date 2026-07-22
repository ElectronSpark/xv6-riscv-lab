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
    sigpending as sigpending_t, sigset_t, slab_cache_t,
    spinlock_t, stack as stack_t,
    thread, thread_group, thread_signal_t, thread_state,
};
// P3-D3c: the spinlock primitives used to be imported from
// `crate::bindings` (bindgen extern decls resolved at link time against
// lock/spinlock.rs's `#[no_mangle]` exports -- now gone). Same genuinely
// `unsafe fn` signatures at their crate path; every call site already
// sits in an `unsafe` context.
use crate::lock::spinlock::{spin_init, spin_lock, spin_unlock};

// P3-D3a: `slab_alloc`/`slab_free`/`slab_cache_init` used to be imported
// from `crate::bindings` (bindgen extern decls resolved at link time
// against mm/slab.rs's `#[no_mangle]` exports — now gone). Thin unsafe
// cast adapters keep this file's bindgen `slab_cache_t` call-site types
// (same layout as `crate::mm::slab::SlabCache`, see `cffi::raw`'s
// identical note); genuinely `unsafe fn`, exactly like the old bindgen
// declarations, so every call site keeps its existing `unsafe` context.
/// # Safety
/// See `crate::mm::slab::slab_cache_init`'s contract.
#[inline]
unsafe fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
}
/// # Safety
/// `cache` must originate from `slab_cache_init` above.
#[inline]
unsafe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache)
}
/// # Safety
/// `obj` must originate from `slab_alloc` above.
#[inline]
unsafe fn slab_free(obj: *mut c_void) {
    crate::mm::slab_free(obj)
}
use crate::lock::spinlock::spin_holding;

// ===========================================================================
// Native uabi signal trio — P3-4b nativization (user directive: remove
// the C-compatible interfaces; userspace-ABI scrutiny class).
// `SigVal`/`SigInfo`/`SigStack` are the canonical KERNEL-SIDE
// definitions of `kernel/inc/uabi/signal.h`'s `union sigval`/`struct
// siginfo`/`struct stack` (sigaltstack): `build.rs` blocklists the
// bindgen emissions and re-exports them as
// `crate::bindings::{sigval,siginfo,stack}` (facade `pub use`, N2
// pattern). The `siginfo_t`/`stack_t` typedefs stay bindgen-emitted and
// resolve through the facades unchanged.
//
// *** USERSPACE ABI — HANDLE WITH P3-4 SCRUTINY *** The C header STAYS:
// user/ programs (testsig.c's SA_SIGINFO handlers, sigaltstack users)
// compile against uabi/signal.h, and the kernel delivers `siginfo` BY
// VALUE onto the user stack when it builds a signal frame
// (`either_copyout` of `siginfo_t` next to the ucontext in
// kernel/irq/trap.rs's sig_deliver path; `sigaltstack(2)` copies
// `stack_t` both ways). A layout slip here silently corrupts every
// SA_SIGINFO delivery. The byte-exact asserts below pin the natives to
// the header. HOST determination: no host-side tool consumes any uabi
// header (mkfs.c includes only types.h/ondisk.h/param.h,
// grep-verified), so the gcc probe is target-only (unlike P3-4a's
// two-arch ondisk gate).
//
// UNION FIDELITY (N2/N5 precedent): `sigval` is a real C union (int |
// void*); bindgen emitted it as a real Rust `union` with Copy/Clone —
// reproduced exactly below, both arms at offset 0, 8/8. `siginfo` has
// NO anonymous members — its union member is the NAMED field
// `si_value: sigval` (plus 4 bytes of tail padding after `si_status`
// that the field offsets pin) — so no `__bindgen_anon` shells exist
// and no consumer re-pointing is needed.
//
// DERIVE DECISION (P3-4b): all three Copy + Clone, exactly as the
// pre-nativization bindgen output derived. The native `KsigInfo`
// (below) embeds `siginfo_t` and the native `ThreadSignal` embeds
// `stack` BY VALUE — their derive lines need build.rs's
// `NativeTypeCallbacks` Copy=Yes answers for these.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4b_uabi_probe.c); both agree on every value asserted
// below.
// ===========================================================================

/// Native uabi `union sigval` (`kernel/inc/uabi/signal.h`) — the
/// application-specific value carried inside [`SigInfo`].
#[repr(C)]
#[derive(Copy, Clone)]
pub union SigVal {
    pub sival_int: c_int,
    pub sival_ptr: *mut c_void,
}

/// Native uabi `struct siginfo` / `siginfo_t` (`kernel/inc/uabi/
/// signal.h`) — the record delivered by value onto the user stack for
/// `SA_SIGINFO` handlers.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct SigInfo {
    /// Signal number.
    pub si_signo: c_int,
    /// If nonzero, errno value from errno.h.
    pub si_errno: c_int,
    /// Additional info (depends on signal).
    pub si_code: c_int,
    /// Sending process ID.
    pub si_pid: c_int,
    /// Address that caused the fault.
    pub si_addr: *mut c_void,
    /// Exit value.
    pub si_status: c_int,
    /// Application-specific value.
    pub si_value: SigVal,
}

/// Native uabi `struct stack` / `stack_t` (`kernel/inc/uabi/signal.h`)
/// — the `sigaltstack(2)` record, copied both ways across the user
/// boundary and embedded by value in [`ThreadSignal::sig_stack`].
#[repr(C)]
#[derive(Copy, Clone)]
pub struct SigStack {
    /// Stack base pointer.
    pub ss_sp: *mut c_void,
    /// Flags (`SS_AUTOREARM`/`SS_ONSTACK`/`SS_DISABLE`).
    pub ss_flags: c_int,
    /// Stack size.
    pub ss_size: usize,
}

// P3-4b hardcoded layout proof — the USERSPACE byte contract
// (`uabi/signal.h` `union sigval`/`struct siginfo`/`struct stack`),
// every field. Values captured from the pre-nativization bindgen
// output via the temporary in-tree `offset_of!` gate and cross-checked
// by the target gcc probe (which also pins both `sigval` arms at
// offset 0).
const _: () = {
    assert!(core::mem::size_of::<SigVal>() == 8, "sigval size (USERSPACE ABI)");
    assert!(core::mem::align_of::<SigVal>() == 8, "sigval alignment");
    assert!(core::mem::size_of::<SigInfo>() == 40, "siginfo size (USERSPACE ABI)");
    assert!(core::mem::align_of::<SigInfo>() == 8, "siginfo alignment");
    assert!(core::mem::offset_of!(SigInfo, si_signo) == 0, "siginfo.si_signo offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(SigInfo, si_errno) == 4, "siginfo.si_errno offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(SigInfo, si_code) == 8, "siginfo.si_code offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(SigInfo, si_pid) == 12, "siginfo.si_pid offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(SigInfo, si_addr) == 16, "siginfo.si_addr offset (USERSPACE ABI)");
    assert!(
        core::mem::offset_of!(SigInfo, si_status) == 24,
        "siginfo.si_status offset (USERSPACE ABI)"
    );
    assert!(
        core::mem::offset_of!(SigInfo, si_value) == 32,
        "siginfo.si_value offset (USERSPACE ABI)"
    );
    assert!(core::mem::size_of::<SigStack>() == 24, "stack size (USERSPACE ABI)");
    assert!(core::mem::align_of::<SigStack>() == 8, "stack alignment");
    assert!(core::mem::offset_of!(SigStack, ss_sp) == 0, "stack.ss_sp offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(SigStack, ss_flags) == 8, "stack.ss_flags offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(SigStack, ss_size) == 16, "stack.ss_size offset (USERSPACE ABI)");
};

// ---------------------------------------------------------------------------
// Native layouts — Wave P3-N4 (signal type family).
//
// These ARE the kernel-wide Rust definitions of `kernel/inc/uabi/signal.h`'s
// `struct sigaction` (+ its anonymous handler union), `kernel/inc/
// signal_types.h`'s `struct sigacts`/`struct sigpending`/`struct ksiginfo`,
// and `kernel/inc/proc/thread_group_types.h`'s `struct tg_shared_pending`
// now: `build.rs` blocklists the bindgen-generated forms and injects
// `pub use crate::proc::signal::... as ...;` facade re-exports, so every
// `crate::bindings::{sigaction,sigacts,sigacts_t,sigpending,sigpending_t,
// ksiginfo,tg_shared_pending}` path across the crate resolves here. The
// `siginfo_t` embedded *by value* (`KsigInfo::info`) was still-bindgen
// through N4..P3-4a (the sanctioned mixed-tier pattern, P3-N2/N3) and
// is native since P3-4b ([`SigInfo`] above) — the alias re-pointed
// transparently, exactly as this note predicted. `SigPending` is
// likewise still embedded by value by the remaining bindgen
// `thread_signal` (`sig_pending: [sigpending_t; 32]`), which is why
// `build.rs`'s `NativeTypeCallbacks` answers Copy=Yes for it. The C
// `_Atomic int` member (`sigacts.refcount`) stays a plain `c_int` field,
// exactly as bindgen lowered it; atomic access goes through pointer
// views, unchanged.
// ---------------------------------------------------------------------------

/// Native replacement for the anonymous C union inside `struct sigaction`
/// (`kernel/inc/uabi/signal.h`): bindgen's `sigaction__bindgen_ty_1`,
/// two fn-pointer arms, 8/8. Field forms reproduce bindgen's exactly
/// (`Option<unsafe extern "C" fn ...>`).
#[repr(C)]
#[derive(Copy, Clone)]
pub union SigActionHandler {
    pub sa_handler: Option<unsafe extern "C" fn(arg1: c_int)>,
    pub sa_sigaction: Option<
        unsafe extern "C" fn(
            arg1: c_int,
            arg2: *mut crate::bindings::siginfo_t,
            arg3: *mut c_void,
        ),
    >,
}

/// `struct sigaction` (`kernel/inc/uabi/signal.h`). The anonymous union
/// became the named `handler` field (consumers re-pointed from bindgen's
/// `__bindgen_anon_1`, N3 precedent).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct SigAction {
    pub handler: SigActionHandler,
    pub sa_mask: sigset_t,
    pub sa_flags: c_int,
}

/// `struct sigacts` (`kernel/inc/signal_types.h`). The align(64) is the
/// `spinlock_t` *typedef*'s `__ALIGNED_CACHELINE` riding in through the
/// first member — the record type `RawSpinlock` is 24/8, so the embedder
/// carries the alignment itself, exactly as bindgen emitted it
/// (`#[repr(C)] #[repr(align(64))]`).
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct SigActs {
    pub lock: spinlock_t,
    pub sa: [SigAction; 33],
    pub sa_sigterm: sigset_t,
    pub sa_sigstop: sigset_t,
    pub sa_sigcont: sigset_t,
    pub sa_sigignore: sigset_t,
    pub refcount: c_int,
}

/// `struct sigpending` (`kernel/inc/signal_types.h`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct SigPending {
    pub queue: list_node_t,
}

/// `struct ksiginfo` (`kernel/inc/signal_types.h`).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct KsigInfo {
    pub list_entry: list_node_t,
    pub receiver: *mut thread,
    pub sender: *mut thread,
    pub signo: c_int,
    pub info: crate::bindings::siginfo_t,
}

/// `struct tg_shared_pending` (`kernel/inc/proc/thread_group_types.h`) —
/// embedded by value by the native `ThreadGroup` (P3-N3), whose
/// `crate::bindings::tg_shared_pending` path now resolves here.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct TgSharedPending {
    pub sig_pending_mask: sigset_t,
    pub sig_pending: [SigPending; 32],
}

/// `struct thread_signal` / `thread_signal_t` (`kernel/inc/
/// signal_types.h`) — per-thread signal state, embedded by value by the
/// native `Thread` (P3-N9). `crate::bindings::{thread_signal,
/// thread_signal_t}` resolve here via the build.rs facade. The
/// `sig_pending` array embeds the native `SigPending` (N4, transparent
/// re-point); `sig_stack` (`crate::bindings::stack`,
/// kernel/inc/uabi/signal.h) was still-bindgen through N9..P3-4a (the
/// uabi class, sanctioned mixed-tier pattern) and is native since
/// P3-4b ([`SigStack`] above) — the facade re-pointed the field
/// transparently, and the 584/8 asserts below still pin the layout.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct ThreadSignal {
    pub sig_mask: sigset_t,
    pub sig_saved_mask: sigset_t,
    pub sig_pending_mask: sigset_t,
    pub sig_pending: [SigPending; 32],
    pub sig_ucontext: u64,
    pub sig_stack: crate::bindings::stack,
    pub esignal: u64,
    pub stop_signal: c_int,
    pub term_signal: c_int,
}

// P3-N4 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (kernel_bindings.rs: `pub struct
// sigaction { __bindgen_anon_1: sigaction__bindgen_ty_1, sa_mask:
// sigset_t, sa_flags: c_int }`, `pub struct sigacts { lock: spinlock_t,
// sa: [sigaction; 33], sa_sigterm/sa_sigstop/sa_sigcont/sa_sigignore:
// sigset_t, refcount: c_int }` (`#[repr(align(64))]`), `pub struct
// sigpending { queue: list_node_t }`, `pub struct ksiginfo { list_entry:
// list_node_t, receiver/sender: *mut thread, signo: c_int, info:
// siginfo_t }`, `pub struct tg_shared_pending { sig_pending_mask:
// sigset_t, sig_pending: [sigpending_t; 32] }`) and independently
// confirmed by a riscv64-unknown-elf-gcc `_Static_assert` probe
// (rv64gc/lp64d) against the headers above — see the P3-N4 wave record:
// sigaction 24/8 offsets 0/0/8/16, sigacts 896/64 offsets
// 0/24/816/824/832/840/848, sigpending 16/8, ksiginfo 80/8 offsets
// 0/16/24/32/40, tg_shared_pending 520/8 offsets 0/8 (siginfo_t itself
// proven 40/8 by the same probe).
const _: () = {
    assert!(core::mem::size_of::<SigActionHandler>() == 8, "sigaction handler union size");
    assert!(core::mem::align_of::<SigActionHandler>() == 8, "sigaction handler union alignment");
    assert!(core::mem::size_of::<SigAction>() == 24, "sigaction size");
    assert!(core::mem::align_of::<SigAction>() == 8, "sigaction alignment");
    assert!(core::mem::offset_of!(SigAction, handler) == 0, "sigaction.handler offset");
    assert!(core::mem::offset_of!(SigAction, sa_mask) == 8, "sigaction.sa_mask offset");
    assert!(core::mem::offset_of!(SigAction, sa_flags) == 16, "sigaction.sa_flags offset");
    assert!(core::mem::size_of::<SigActs>() == 896, "sigacts size");
    assert!(core::mem::align_of::<SigActs>() == 64, "sigacts alignment");
    assert!(core::mem::offset_of!(SigActs, lock) == 0, "sigacts.lock offset");
    assert!(core::mem::offset_of!(SigActs, sa) == 24, "sigacts.sa offset");
    assert!(core::mem::offset_of!(SigActs, sa_sigterm) == 816, "sigacts.sa_sigterm offset");
    assert!(core::mem::offset_of!(SigActs, sa_sigstop) == 824, "sigacts.sa_sigstop offset");
    assert!(core::mem::offset_of!(SigActs, sa_sigcont) == 832, "sigacts.sa_sigcont offset");
    assert!(core::mem::offset_of!(SigActs, sa_sigignore) == 840, "sigacts.sa_sigignore offset");
    assert!(core::mem::offset_of!(SigActs, refcount) == 848, "sigacts.refcount offset");
    assert!(core::mem::size_of::<SigPending>() == 16, "sigpending size");
    assert!(core::mem::align_of::<SigPending>() == 8, "sigpending alignment");
    assert!(core::mem::offset_of!(SigPending, queue) == 0, "sigpending.queue offset");
    assert!(core::mem::size_of::<crate::bindings::siginfo_t>() == 40, "siginfo_t size");
    assert!(core::mem::align_of::<crate::bindings::siginfo_t>() == 8, "siginfo_t alignment");
    assert!(core::mem::size_of::<KsigInfo>() == 80, "ksiginfo size");
    assert!(core::mem::align_of::<KsigInfo>() == 8, "ksiginfo alignment");
    assert!(core::mem::offset_of!(KsigInfo, list_entry) == 0, "ksiginfo.list_entry offset");
    assert!(core::mem::offset_of!(KsigInfo, receiver) == 16, "ksiginfo.receiver offset");
    assert!(core::mem::offset_of!(KsigInfo, sender) == 24, "ksiginfo.sender offset");
    assert!(core::mem::offset_of!(KsigInfo, signo) == 32, "ksiginfo.signo offset");
    assert!(core::mem::offset_of!(KsigInfo, info) == 40, "ksiginfo.info offset");
    assert!(core::mem::size_of::<TgSharedPending>() == 520, "tg_shared_pending size");
    assert!(core::mem::align_of::<TgSharedPending>() == 8, "tg_shared_pending alignment");
    assert!(
        core::mem::offset_of!(TgSharedPending, sig_pending_mask) == 0,
        "tg_shared_pending.sig_pending_mask offset"
    );
    assert!(
        core::mem::offset_of!(TgSharedPending, sig_pending) == 8,
        "tg_shared_pending.sig_pending offset"
    );
    // P3-N9 — values captured from the pre-nativization bindgen output
    // via the temporary in-tree `offset_of!` gate and independently
    // confirmed by the riscv64-unknown-elf-gcc `_Static_assert` probe
    // (rv64gc/lp64d — scratchpad p3n9_static_assert_probe.c); both agree
    // on every value (no pipe-style divergence, N5 precedent checked).
    assert!(core::mem::size_of::<ThreadSignal>() == 584, "thread_signal size");
    assert!(core::mem::align_of::<ThreadSignal>() == 8, "thread_signal alignment");
    assert!(core::mem::offset_of!(ThreadSignal, sig_mask) == 0, "thread_signal.sig_mask offset");
    assert!(
        core::mem::offset_of!(ThreadSignal, sig_saved_mask) == 8,
        "thread_signal.sig_saved_mask offset"
    );
    assert!(
        core::mem::offset_of!(ThreadSignal, sig_pending_mask) == 16,
        "thread_signal.sig_pending_mask offset"
    );
    assert!(
        core::mem::offset_of!(ThreadSignal, sig_pending) == 24,
        "thread_signal.sig_pending offset"
    );
    assert!(
        core::mem::offset_of!(ThreadSignal, sig_ucontext) == 536,
        "thread_signal.sig_ucontext offset"
    );
    assert!(
        core::mem::offset_of!(ThreadSignal, sig_stack) == 544,
        "thread_signal.sig_stack offset"
    );
    assert!(core::mem::size_of::<crate::bindings::stack>() == 24, "stack_t size");
    assert!(core::mem::align_of::<crate::bindings::stack>() == 8, "stack_t alignment");
    assert!(core::mem::offset_of!(ThreadSignal, esignal) == 568, "thread_signal.esignal offset");
    assert!(
        core::mem::offset_of!(ThreadSignal, stop_signal) == 576,
        "thread_signal.stop_signal offset"
    );
    assert!(
        core::mem::offset_of!(ThreadSignal, term_signal) == 580,
        "thread_signal.term_signal offset"
    );
};
use crate::machine::{cpuid, CpuLocal};
use crate::proc::proc_shims::{
    xv6_current_thread, xv6_panic, xv6_pid_rlock, xv6_pid_runlock,
};
// P3-D2a: the other scheduler wake/yield entry points join
// `scheduler_wakeup_stopped` as plain crate-path imports (their former
// `extern "C"` redeclarations in the block below are gone).
use crate::proc::Scheduler;
use crate::ipi::ipi_send_single;
use crate::proc::access::{
    KsigInfoAccess, SchedEntityRef, SigPendingRef, SigactsAccess, SpinLockRef, ThreadAccess,
    ThreadGroupAccess, ThreadSignalAccess, ThreadSignalPtrRef, atomic_dec_unless_i32,
    atomic_inc_i32, is_err, list_container_of as container_of,
    list_node_detach_raw, list_node_init_raw, list_node_is_empty_raw, list_node_next_raw,
    list_node_push_back_raw, write_out,
};
use crate::kstd::{Errno, KResult};
// NO-STANDALONE-FN: `make_ksiginfo` is now the `KsigInfoAccess::make`
// associated fn (access.rs); already imported via `KsigInfoAccess` above.

/// Zero-sized marker carrying the pid/current-keyed signal ABI entry points
/// as associated functions.
///
/// NO-STANDALONE-FN: these entries have no single `thread`/`sigacts`
/// receiver to hang a method on — each resolves `current()` or looks up a
/// pid at run time — so, per the wave rule, they become associated
/// functions on this marker rather than remaining free functions. The
/// thread-keyed entries (`killed`/`signal_pending`/`kill_proc`/… ) instead
/// became [`ThreadAccess`] methods.
pub(crate) struct Signal;

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
}
// P3-D3c: `proc/exit.rs`'s `exit` is a plain (safe) Rust fn now that its
// `#[no_mangle]` export is gone -- imported via its private sibling module
// path (the `crate::proc` glob would work too, but the direct path is
// unambiguous by construction).
use crate::proc::Proc;

// P3-D3b: `tcb_lock`/`tcb_unlock` (kernel/proc/thread.rs) are plain safe
// Rust fns now that their `#[no_mangle]` exports are gone; reached by
// crate path.
// NO-STANDALONE-FN: `tcb_lock`/`tcb_unlock`/`proc_assert_holding` are handle
// methods on `ThreadAccess`; this file routes them through its `ThreadAccess::ta_of(p)`
// choke point (which constructs the handle) instead of the deleted free fns.

// P3-D2b: the pid lookups (proc/pid.rs), `thread_tgid`
// (proc/thread_group.rs) and `pgroup_kill` (proc/pgroup.rs) are plain
// crate-path items now that their `#[no_mangle]` exports are gone
// (identical signatures) -- they used to be `extern "C"` redeclarations
// in the block above.
use crate::proc::{ProcTable, Pgroup};

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

// NO-STANDALONE-FN: these were file-local free-fn accessor/predicate
// shims over a `*mut thread` (null-guard + delegate to `ThreadAccess`);
// mechanically relocated into a private `ThreadAccess` impl (same
// bodies, byte-identical null-handling -- see wave NO-STANDALONE-FN
// follow-up, signal.rs). `recalc_sigpending_tsk`/`siginfo_queue_len`/
// `dequeue_signal_update_pending_nolock` join them here (first param is
// the thread they operate on); every call site already held a
// proven-non-null `p` (checked or `self.as_ptr()`), so relocation is
// behavior-identical.
impl<'a> ThreadAccess<'a> {
    #[inline]
    fn ta_of(p: *mut thread) -> ThreadAccess<'a> {
        // SAFETY: `ta_of` is this file's choke point for `ThreadAccess::assume`; every
        // call site in this file checks `p.is_null()` immediately before calling
        // `ta_of` (see e.g. THREAD_SLEEPING/THREAD_AWOKEN/thread_state_get and the
        // THREAD_* macros above).
        unsafe { ThreadAccess::assume(p) }
    }

    #[inline]
    fn thread_state_get(p: *mut thread) -> thread_state {
        if p.is_null() { return THREAD_UNUSED; }
        ThreadAccess::ta_of(p).state_load()
    }

    #[inline]
    fn thread_state_set(p: *mut thread, s: thread_state) {
        if p.is_null() { return; }
        ThreadAccess::ta_of(p).state_store(s);
    }

    #[inline]
    fn flags_load_ptr(p: *mut thread) -> u64 {
        if p.is_null() { return 0; }
        ThreadAccess::ta_of(p).flags_load()
    }

    #[inline]
    fn flag_test(p: *mut thread, bit: u64) -> bool {
        if p.is_null() { return false; }
        ThreadAccess::ta_of(p).flags_test_bit(bit)
    }

    #[inline]
    fn flag_set(p: *mut thread, bit: u64) {
        if p.is_null() { return; }
        ThreadAccess::ta_of(p).flags_set_bit(bit);
    }

    #[inline]
    fn flag_clear(p: *mut thread, bit: u64) {
        if p.is_null() { return; }
        ThreadAccess::ta_of(p).flags_clear_bit(bit);
    }

    #[inline]
    fn THREAD_KILLED(p: *mut thread) -> bool { if p.is_null() { false } else { ThreadAccess::ta_of(p).flags_test_bit(THREAD_FLAG_KILLED) } }

    #[inline]
    fn THREAD_SIGPENDING(p: *mut thread) -> bool { if p.is_null() { false } else { ThreadAccess::ta_of(p).flags_test_bit(THREAD_FLAG_SIGPENDING) } }

    #[inline]
    fn THREAD_USER_SPACE(p: *mut thread) -> bool { if p.is_null() { false } else { ThreadAccess::ta_of(p).flags_test_bit(THREAD_FLAG_USER_SPACE) } }

    #[inline]
    fn THREAD_SET_KILLED(p: *mut thread) { if !p.is_null() { ThreadAccess::ta_of(p).flags_set_bit(THREAD_FLAG_KILLED) } }

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
    fn THREAD_SET_SIGPENDING(p: *mut thread) { if !p.is_null() { ThreadAccess::ta_of(p).flags_set_bit(THREAD_FLAG_SIGPENDING) } }

    #[inline]
    fn THREAD_CLEAR_SIGPENDING(p: *mut thread) { if !p.is_null() { ThreadAccess::ta_of(p).flags_clear_bit(THREAD_FLAG_SIGPENDING) } }

    #[inline]
    fn THREAD_IS_INTERRUPTIBLE(s: thread_state) -> bool { s == THREAD_INTERRUPTIBLE }

    #[inline]
    fn THREAD_SLEEPING(p: *mut thread) -> bool {
        if p.is_null() { return false; }
        let s = ThreadAccess::ta_of(p).state_load();
        s == THREAD_INTERRUPTIBLE || s == THREAD_UNINTERRUPTIBLE
            || s == THREAD_KIILABLE || s == THREAD_TIMER || s == THREAD_KIILABLE_TIMER
    }

    #[inline]
    fn THREAD_AWOKEN(p: *mut thread) -> bool {
        if p.is_null() { return false; }
        let s = ThreadAccess::ta_of(p).state_load();
        s == THREAD_RUNNING || s == THREAD_WAKENING
    }

    #[inline]
    fn THREAD_STOPPED_p(p: *mut thread) -> bool {
        if p.is_null() { return false; }
        ThreadAccess::ta_of(p).state_load() == THREAD_STOPPED
    }

    fn recalc_sigpending_tsk(p: *mut thread) -> bool {
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
            ThreadAccess::THREAD_SET_SIGPENDING(p);
            return true;
        }
        false
    }

    fn siginfo_queue_len(p: *mut thread, signo: c_int) -> c_int {
        // SAFETY: `siginfo_queue_len`'s sole call site (below, inside `__signal_send`'s
        // `raw_sig_abi!` block) passes the same `p` already checked non-null at
        // `__signal_send`'s entry.
        unsafe { ThreadSignalAccess::assume_thread(p) }
            .sig_pending_ref_index((signo - 1) as usize)
            .queue_len()
    }

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
            let info = container_of::<ksiginfo_t>((*head).next, KsigInfo::ksi_list_entry_offset());
            list_node_detach_raw(&raw mut (*info).list_entry);
            if list_node_is_empty_raw(head) {
                sigdelset(&mut (*p).signal.sig_pending_mask, signo);
            }
            Ok(info)
        }
    }
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

// ============================================================================
// sigpending_init / sigpending_destroy / sigpending_clone / sigstack_init
// ============================================================================
impl<'a> ThreadAccess<'a> {
    pub(crate) fn sigpending_init(&self) {
        let p = self.as_ptr();
        // SAFETY: `p` is `self`'s non-null pointer.
        let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
        for i in 0..(NSIG as usize) {
            ts.sig_pending_ref_index(i).queue_ref().init();
        }
    }

    pub(crate) fn sigpending_destroy(&self) {
        let p = self.as_ptr();
        let sa = self.sigacts_ptr();
        if !sa.is_null() {
            // SAFETY: `sa` is checked non-null immediately above (`if !sa.is_null()`).
            debug_assert!(unsafe { SigactsAccess::assume(sa) }.lock_ref().holding());
        }
        // SAFETY: `p` is `self`'s non-null pointer.
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
}

// NO-STANDALONE-FN: the per-thread signal-state clone became an associated
// fn on the native [`ThreadSignal`] type; the sigaltstack reset became an
// associated fn on the native [`SigStack`] type.
impl ThreadSignal {
    pub(crate) fn sigpending_clone(dst: *mut thread_signal_t,
                                   src: *mut thread_signal_t,
                                   clone_flags: u64,
                                   esignal: c_int) {
        // SAFETY: `dst` is documented (by this function's C-era contract,
        // mirrored in all Rust callers) to be the address of a live
        // `thread_signal_t` sub-struct; never null in practice.
        let dst = unsafe { ThreadSignalPtrRef::assume(dst) };
        // SAFETY: same contract as `dst` above for `src`.
        let src = unsafe { ThreadSignalPtrRef::assume(src) };
        dst.set_sig_mask(src.sig_mask());
        dst.set_sig_saved_mask(src.sig_saved_mask());
        if (clone_flags & CLONE_THREAD) != 0 {
            dst.set_esignal(0);
        } else {
            dst.set_esignal(esignal as u64);
        }
    }
}

impl SigStack {
    pub(crate) fn init(stack: *mut stack_t) {
        if stack.is_null() { return; }
        raw_sig_abi! {
            (*stack).ss_sp = ptr::null_mut();
            (*stack).ss_flags = SS_DISABLE;
            (*stack).ss_size = 0;
        }
    }
}

// ============================================================================
// ksiginfo_alloc / ksiginfo_free
// ============================================================================
// NO-STANDALONE-FN: the ksiginfo slab constructor/destructor became
// associated fns on the native [`KsigInfo`] type (`alloc`/`free`); `make`
// forwards to `access.rs`'s builder (real definition; out of this wave's
// scope). Every cross-module caller (thread_group.rs) uses `KsigInfo::`.
impl KsigInfo {
    pub(crate) fn alloc() -> *mut ksiginfo_t {
        raw_sig_abi! {
            let ksi = slab_alloc(KsigInfo::ksiginfo_pool()) as *mut ksiginfo_t;
            if ksi.is_null() { return ptr::null_mut(); }
            memset(ksi as *mut c_void, 0, core::mem::size_of::<ksiginfo_t>());
            let ka = KsigInfoAccess::assume(ksi);
            ka.list_entry_ref().init();
            ka.set_sender(ptr::null_mut());
            ksi
        }
    }

    pub(crate) fn free(ksi: *mut ksiginfo_t) {
        if !ksi.is_null() {
            raw_sig_abi! { slab_free(ksi as *mut c_void); }
        }
    }
    #[inline]
    fn ksi_list_entry_offset() -> usize { core::mem::offset_of!(ksiginfo_t, list_entry) }

    #[inline]
    fn list_first_ksi(head: *mut list_node_t) -> *mut ksiginfo_t {
        if list_node_is_empty_raw(head) { return ptr::null_mut(); }
        container_of::<ksiginfo_t>(list_node_next_raw(head), KsigInfo::ksi_list_entry_offset())
    }

    /// A lazy [`Iterator`] over one sigpending queue (a `list_node_t` sentinel
    /// `head` with embedded `ksiginfo` entries linked via `list_entry`),
    /// yielding `*mut ksiginfo_t` for each element.
    ///
    /// Mirrors `thread_group.rs`'s builder (8b455e4): a
    /// `list_foreach_node_safe`-equivalent walk —
    /// [`crate::list::ListIterator::next`] caches each node's successor
    /// *before* yielding the current node, so a loop body may `detach` **and
    /// free** the yielded entry (the drain used by `sigpending_empty`). Every
    /// touch of a node is a raw pointer load through the iterator's cursor —
    /// no `&ksiginfo` reference is formed. A null `head` yields nothing.
    /// Every caller drains under the owning `sigacts` lock, so the walk is not
    /// a lock-free/Freeze-in-loop read.
    #[inline]
    fn siginfo_queue<'a>(head: *mut list_node_t) -> crate::list::ListIterator<'a, ksiginfo_t> {
        // SAFETY: every caller passes a live `signal.sig_pending[..].queue`
        // sentinel head (a `list_node_t` embedded by value in an already-valid
        // `thread_signal`), and `KsigInfo::ksi_list_entry_offset()` is
        // `ksiginfo::list_entry`'s real member offset, upholding
        // `ListIterator::new`'s contract. A null head yields nothing.
        unsafe { crate::list::ListIterator::new(head, KsigInfo::ksi_list_entry_offset()) }
    }

    #[inline]
    fn ksiginfo_pool() -> *mut slab_cache_t { KSIGINFO_POOL.0.get() as *mut slab_cache_t }
}

// ============================================================================
// sigacts_lock/unlock/holding
// ============================================================================

// P3-D2b: `sigacts_holding` (+ its `_impl`) deleted -- zero callers
// anywhere in the tree (kernel/inc/signal.h's legacy prototype does not
// count, 9d35f95 precedent).

// ============================================================================
// sigpending_empty
// ============================================================================
impl<'a> ThreadAccess<'a> {
    pub(crate) fn sigpending_empty(&self, signo: c_int) -> c_int {
    let p = self.as_ptr();
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
                for ksi in KsigInfo::siginfo_queue(head) {
                    KsigInfoAccess::assume(ksi).list_entry_ref().detach();
                    KsigInfo::free(ksi);
                }
            }
            ts.set_sig_pending_mask(0);
            ThreadAccess::THREAD_CLEAR_SIGPENDING(p);
            return 0;
        }

        if sigbad(signo) { return -EINVAL; }

        let ts = ThreadSignalAccess::assume_thread(p);
        let head = ts.sig_pending_ref_index((signo - 1) as usize).queue_ptr();
        for ksi in KsigInfo::siginfo_queue(head) {
            KsigInfoAccess::assume(ksi).list_entry_ref().detach();
            KsigInfo::free(ksi);
        }
        ts.and_sig_pending_mask(!sig_mask_bit(signo));
        ThreadAccess::recalc_sigpending_tsk(p);
        0
    }
    }
}

// ============================================================================
// __sig_reset_act_mask / __sig_setdefault
// ============================================================================
// P3 RUSTIFY-PROC: `__sig_reset_act_mask` / `__sig_setdefault` are now
// inherent methods on the `SigactsAccess` handle (an inherent impl for a
// same-crate type may live in any module). Bodies read/write the four
// default-action sigset masks through the handle's raw-load getters/setters
// (no `&sigacts`/`&Freeze` formed), so they are Freeze-safe by construction.
// Every caller already holds the sigacts lock. The former `sa.is_null()`
// early-return in `setdefault` is subsumed by the handle's non-null
// invariant (every call site builds the handle from a pointer that
// `sigacts_lock` already dereferenced).
impl<'a> SigactsAccess<'a> {
    /// Clears `signo` from the term/ignore (always) and stop/cont (unless
    /// `signo` is `SIGSTOP`/`SIGCONT` respectively) default-action masks.
    fn reset_act_mask(&self, signo: c_int) {
        let m = sig_mask_bit(signo);
        if m == 0 { return; }
        self.set_sigterm(self.sigterm() & !m);
        self.set_sigignore(self.sigignore() & !m);
        if signo != SIGSTOP { self.set_sigstop(self.sigstop() & !m); }
        if signo != SIGCONT { self.set_sigcont(self.sigcont() & !m); }
    }

    /// Installs `signo`'s POSIX default action: resets its per-mask bits,
    /// sets the mask matching the default disposition, and clears the
    /// handler/flags/mask of the `sa[signo]` slot.
    fn setdefault(&self, signo: c_int) -> c_int {
        if sigbad(signo) { return -EINVAL; }
        let defact = Signal::signo_default_action(signo);
        if defact == SIG_ACT_INVALID { return 0; }
        self.reset_act_mask(signo);
        let m = sig_mask_bit(signo);
        match defact {
            SIG_ACT_IGN => self.set_sigignore(self.sigignore() | m),
            SIG_ACT_CONT => self.set_sigcont(self.sigcont() | m),
            SIG_ACT_STOP => self.set_sigstop(self.sigstop() | m),
            SIG_ACT_TERM | SIG_ACT_CORE | SIG_ACT_INVALID => {
                self.set_sigterm(self.sigterm() | m);
            }
            _ => return -EINVAL,
        }
        self.action_ref(signo).clear_handler_and_metadata();
        0
    }
}

// ============================================================================
// sigacts_init / sigacts_exec / sigacts_dup / sigacts_put / signal_init
// ============================================================================
// NO-STANDALONE-FN: the sigacts slab constructor/duplicator became
// associated fns on the native [`SigActs`] type; the operations on an
// existing sigacts (`exec`/`put`/`lock`/`unlock`) became [`SigactsAccess`]
// methods (the pub `sigacts_lock`/`sigacts_unlock` free delegators were
// deleted — thread_group.rs now calls the handle's `lock`/`unlock`; this
// file keeps the private `sigacts_lock_impl`/`sigacts_unlock_impl`
// null->panic choke points for its own pervasive internal use, the
// `ta_of`/`sigacts_lock_ref` floor).
impl SigActs {
    pub(crate) fn init() -> *mut sigacts_t {
        raw_sig_abi! {
            let sa = slab_alloc(SigActs::sigacts_pool()) as *mut sigacts_t;
            if sa.is_null() { return ptr::null_mut(); }
            memset(sa as *mut c_void, 0, core::mem::size_of::<sigacts_t>());
            (*sa).sa_sigterm = 0;
            (*sa).sa_sigstop = 0;
            (*sa).sa_sigcont = 0;
            (*sa).sa_sigignore = 0;
            spin_init(&raw mut (*sa).lock, c"sigacts_lock".as_ptr() as *mut c_char);
            (*sa).refcount = 1;
            for i in 1..=NSIG {
                if SigactsAccess::assume(sa).setdefault(i) != 0 {
                    xv6_panic(c"sigacts_init: failed to set default action".as_ptr());
                }
            }
            sa
        }
    }

    pub(crate) fn dup(psa: *mut sigacts_t, clone_flags: u64) -> *mut sigacts_t {
        if psa.is_null() { return ptr::null_mut(); }
        raw_sig_abi! {
            if (clone_flags & CLONE_SIGHAND) != 0 {
                atomic_inc_i32(&raw mut (*psa).refcount);
                return psa;
            }
            let sa = slab_alloc(SigActs::sigacts_pool()) as *mut sigacts_t;
            if !sa.is_null() {
                SigActs::sigacts_lock_impl(psa);
                memmove(sa as *mut c_void, psa as *const c_void, core::mem::size_of::<sigacts_t>());
                SigActs::sigacts_unlock_impl(psa);
                spin_init(&raw mut (*sa).lock, c"sigacts_lock".as_ptr() as *mut c_char);
                (*sa).refcount = 1;
            }
            sa
        }
    }
    fn sigacts_lock_ref<'a>(sa: *mut sigacts_t) -> Option<SpinLockRef<'a>> {
        SigactsAccess::from_ptr(sa).map(|s| s.lock_ref())
    }

    fn sigacts_lock_impl(sa: *mut sigacts_t) {
        SigActs::sigacts_lock_ref(sa).expect("BUG: sigacts_lock called with null/invalid sigacts pointer").lock();
    }

    fn sigacts_unlock_impl(sa: *mut sigacts_t) {
        SigActs::sigacts_lock_ref(sa).expect("BUG: sigacts_unlock called with null/invalid sigacts pointer").unlock();
    }

    #[inline]
    fn sigacts_pool() -> *mut slab_cache_t { SIGACTS_POOL.0.get() as *mut slab_cache_t }
}

impl<'a> SigactsAccess<'a> {
    #[inline] pub(crate) fn lock(&self) { self.lock_ref().lock(); }
    #[inline] pub(crate) fn unlock(&self) { self.lock_ref().unlock(); }

    pub(crate) fn exec(&self) {
        let sa = self.as_ptr();
        raw_sig_abi! {
            SigActs::sigacts_lock_impl(sa);
            for i in 1..=NSIG {
                let sacc = SigactsAccess::assume(sa);
                let act = sacc.action_ref(i);
                let is_dfl = act.is_default();
                let is_ign = act.is_ignore();
                if !is_dfl && !is_ign {
                    sacc.setdefault(i);
                }
                act.set_flags(0);
                act.set_mask(0);
            }
            SigActs::sigacts_unlock_impl(sa);
        }
    }

    pub(crate) fn put(&self) {
        let sa = self.as_ptr();
        raw_sig_abi! {
            if !atomic_dec_unless_i32(&raw mut (*sa).refcount, 1) {
                slab_free(sa as *mut c_void);
            }
        }
    }
}

impl Signal {
    // P3-1B: only caller is `start_kernel.rs` (crate-path `use`).
    pub(crate) fn init() {
        raw_sig_abi! {
            slab_cache_init(SigActs::sigacts_pool(),
                c"sigacts".as_ptr() as *mut c_char,
                core::mem::size_of::<sigacts_t>(),
                SLAB_FLAG_STATIC);
            slab_cache_init(KsigInfo::ksiginfo_pool(),
                c"ksiginfo".as_ptr() as *mut c_char,
                core::mem::size_of::<ksiginfo_t>(),
                SLAB_FLAG_STATIC);
        }
    }
    fn signo_default_action(signo: c_int) -> c_int {
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

    fn recalc_sigpending() {
        let p = xv6_current_thread();
        if p.is_null() { return; }
        // SAFETY: `p` is checked non-null at the top of `recalc_sigpending` above.
        let ta = unsafe { ThreadAccess::assume(p) };
        let sa = ta.sigacts_ptr();
        if sa.is_null() { return; }
        SigActs::sigacts_lock_impl(sa);
        if !ThreadAccess::recalc_sigpending_tsk(p) {
            ThreadAccess::THREAD_CLEAR_SIGPENDING(p);
        }
        SigActs::sigacts_unlock_impl(sa);
    }

    fn __signal_send(p: *mut thread, info: *mut ksiginfo_t)-> c_int  {
        if p.is_null() || info.is_null() { return -EINVAL; }
        raw_sig_abi! {
            let signo = (*info).signo;
            if sigbad(signo) { return -EINVAL; }
            let pstate = ThreadAccess::thread_state_get(p);
            if pstate == THREAD_UNUSED || pstate == THREAD_ZOMBIE || ThreadAccess::THREAD_KILLED(p) {
                return -ESRCH;
            }
            let ta = ThreadAccess::assume(p);
            let sa = ta.sigacts_ptr();
            if sa.is_null() { return -EINVAL; }

            SigActs::sigacts_lock_impl(sa);

            if sigismember((*sa).sa_sigignore, signo) > 0 {
                SigActs::sigacts_unlock_impl(sa);
                return 0;
            }

            let act = &raw mut (*sa).sa[signo as usize];
            if ((*act).sa_flags & SA_SIGINFO) != 0 {
                // SA_SIGINFO: queue per-signal, with cap
                let qlen = ThreadAccess::siginfo_queue_len(p, signo);
                if qlen >= MAX_SIGINFO_PER_SIGNAL {
                    let head = ThreadSignalAccess::assume_thread(p)
                        .sig_pending_ref_index((signo - 1) as usize)
                        .queue_ptr();
                    if !list_node_is_empty_raw(head) {
                        let old = container_of::<ksiginfo_t>(list_node_next_raw(head), KsigInfo::ksi_list_entry_offset());
                        KsigInfoAccess::assume(old).list_entry_ref().detach();
                        KsigInfo::free(old);
                    }
                }
                let ksi = KsigInfo::alloc();
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
            ThreadAccess::recalc_sigpending_tsk(p);

            let is_stop = sigismember((*sa).sa_sigstop, signo) > 0
                && sigismember((*p).signal.sig_mask, signo) == 0;
            let is_cont = sigismember((*sa).sa_sigcont, signo) > 0
                && sigismember((*p).signal.sig_mask, signo) == 0;
            let is_term = sigismember((*sa).sa_sigterm, signo) > 0;
            let sigmask = (*p).signal.sig_mask;

            SigActs::sigacts_unlock_impl(sa);

            if is_stop {
                ThreadAccess::ta_of(p).tcb_lock();
                let pst = ThreadAccess::thread_state_get(p);
                if ThreadAccess::THREAD_IS_INTERRUPTIBLE(pst) {
                    ThreadAccess::ta_of(p).tcb_unlock();
                    Scheduler::wakeup_thread(p);
                } else if pst == THREAD_RUNNING {
                    ThreadAccess::ta_of(p).tcb_unlock();
                    let se = ta.sched_entity_ptr();
                    if !se.is_null() {
                        let target = SchedEntityRef::assume(se).cpu_id_load_acquire();
                        if target != cpuid() {
                            ipi_send_single(target, IPI_REASON_RESCHEDULE);
                        } else {
                            Signal::set_needs_resched();
                        }
                    }
                } else {
                    ThreadAccess::ta_of(p).tcb_unlock();
                }
            }
            if is_cont {
                Scheduler::wakeup_stopped(p);
            }
            if is_term {
                ThreadAccess::THREAD_SET_KILLED(p);
                ThreadAccess::set_term_signal_first(p, signo);
                if ThreadAccess::THREAD_STOPPED_p(p) {
                    Scheduler::wakeup_stopped(p);
                }
            }

            let pmp = &raw const (*p).signal.sig_pending_mask as *const AtomicU64;
            let pending_unmasked = (*pmp).load(Ordering::Acquire) & !sigmask;
            if pending_unmasked != 0 {
                ThreadAccess::ta_of(p).tcb_lock();
                Signal::signal_notify(p);
                ThreadAccess::ta_of(p).tcb_unlock();
            }
            0
        }
    }

    fn signal_notify(p: *mut thread)-> c_int  {
        if p.is_null() { return -EINVAL; }
        raw_sig_abi! {
            ThreadAccess::ta_of(p).assert_lock_held();
            if ThreadAccess::THREAD_AWOKEN(p) { return 0; }
            if !ThreadAccess::THREAD_SLEEPING(p) { return -EAGAIN; }
            if ThreadAccess::thread_state_get(p) == THREAD_INTERRUPTIBLE {
                ThreadAccess::ta_of(p).tcb_unlock();
                Scheduler::wakeup_interruptible_thread(p);
                ThreadAccess::ta_of(p).tcb_lock();
                return 0;
            }
            -EAGAIN
        }
    }

    fn signal_restore(p: *mut thread, context: *mut c_void)-> c_int  {
        if p.is_null() || context.is_null() { return -EINVAL; }
        raw_sig_abi! {
            let ctx = context as *mut UContext;
            let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
            let sa = ta.sigacts_ptr();
            if sa.is_null() { return -EINVAL; }
            SigActs::sigacts_lock_impl(sa);
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
            ThreadAccess::recalc_sigpending_tsk(p);
            SigActs::sigacts_unlock_impl(sa);
            0
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
                ThreadAccess::ta_of(p).tcb_lock();
                ThreadAccess::THREAD_SET_KILLED(p);
                ThreadAccess::set_term_signal_first(p, signo);
                ThreadAccess::ta_of(p).tcb_unlock();
                return 0;
            }
            let mut ret = 0;
            let cur = xv6_current_thread();
            if ThreadAccess::THREAD_USER_SPACE(cur) {
                ret = push_sigframe(p, signo, sa, info);
            }
            let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
            let sigacts = ta.sigacts_ptr();
            SigActs::sigacts_lock_impl(sigacts);
            if (action.flags() & SA_NODEFER) == 0 {
                sigaddset(&mut (*p).signal.sig_mask, signo);
            }
            (*p).signal.sig_mask |= action.mask();
            sigdelset(&mut (*p).signal.sig_mask, SIGKILL);
            sigdelset(&mut (*p).signal.sig_mask, SIGSTOP);
            ThreadAccess::recalc_sigpending_tsk(p);
            if (action.flags() & SA_RESETHAND) != 0 {
                if SigactsAccess::assume(sigacts).setdefault(signo) != 0 {
                    xv6_panic(c"deliver_signal: sig_setdefault failed".as_ptr());
                }
            }
            SigActs::sigacts_unlock_impl(sigacts);
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
                    SigActs::sigacts_lock_impl(psa);
                    if ((*psa).sa[SIGCHLD as usize].sa_flags & SA_NOCLDSTOP) != 0 {
                        notify_sigchld = false;
                    }
                    SigActs::sigacts_unlock_impl(psa);
                }
            }
            xv6_pid_runlock();
            if !parent.is_null() {
                Scheduler::wakeup_interruptible_thread(parent);
                if notify_sigchld {
                    ThreadAccess::ta_of(parent).kill_proc(SIGCHLD);
                }
            }
        }
    }

    fn signal_send_to_tgroup(tgid: c_int, info: *mut ksiginfo_t)-> c_int  {
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let leader = ProcTable::get_thread(tgid);
        if is_err(leader) { return -ESRCH; }
        // SAFETY: `leader` is non-null: `get_pid_thread` only ever returns an err-encoded
        // pointer (caught by `is_err(leader)` above) or a live, non-null thread
        // pointer.
        let tg = unsafe { ThreadAccess::assume(leader) }.thread_group_ptr();
        if matches!(ThreadGroupAccess::from_ptr(tg), Some(tga) if tga.tgid() == tgid) {
            return ThreadGroupAccess::from_ptr(tg).map_or(-EINVAL, |tga| tga.signal_send(info));
        }
        // SAFETY: `tg` is proven non-null by the short-circuiting `&&` (`!tg.is_null() &&
        // ...`).
        if !tg.is_null() && !unsafe { ThreadGroupAccess::assume(tg) }.group_leader_ptr().is_null() {
            return ThreadGroupAccess::from_ptr(tg).map_or(-EINVAL, |tga| tga.signal_send(info));
        }
        Signal::__signal_send(leader, info)
    }

    #[inline]
    fn set_needs_resched() {
        CpuLocal::current().flags_or(CPU_FLAG_NEEDS_RESCHED);
    }
}

// ============================================================================
// __signal_send / signal_send / signal_pending / signal_pending_locked / signal_notify
// ============================================================================

impl Signal {
    pub(crate) fn signal_send(pid: c_int, info: *mut ksiginfo_t) -> c_int {
        if pid < 0 || info.is_null() { return -EINVAL; }
        // SAFETY: `info` is checked non-null immediately above.
        if sigbad(unsafe { KsigInfoAccess::assume(info) }.signo()) { return -EINVAL; }
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let p = ProcTable::get_thread(pid);
        if is_err(p) || p.is_null() {
            return -ESRCH;
        }
        // SAFETY: `p` is checked non-null (`is_err(p) || p.is_null()`) immediately above.
        let ta = unsafe { ThreadAccess::assume(p) };
        let tg = ta.thread_group_ptr();
        // SAFETY: `tg` is proven non-null by the short-circuiting `&&` (`!tg.is_null() &&
        // ...`).
        if !tg.is_null() && unsafe { ThreadGroupAccess::assume(tg) }.is_kernel() != 0 {
            Signal::__signal_send(p, info)
        } else if matches!(ThreadGroupAccess::from_ptr(tg), Some(tga) if tga.tgid() == pid) {
            ThreadGroupAccess::from_ptr(tg).map_or(-EINVAL, |tga| tga.signal_send(info))
        } else {
            Signal::__signal_send(p, info)
        }
    }
}

// NO-STANDALONE-FN: `signal_pending`/`killed`/`kill_proc`/`kill_thread`/
// `sigpending`(query)/`sigpending_init`/`sigpending_destroy`/
// `sigpending_empty` used to be free fns keyed on a `*mut thread`; they are
// now `ThreadAccess` methods (inherent impl for a same-crate type may live
// in any module, d086865 precedent). Every former top-of-body `p.is_null()`
// guard's null->default mapping is reproduced at the call site by
// constructing the handle via `ThreadAccess::from_ptr(p)` and folding
// `None` to that default (the wave's accepted handle-construction burden).
impl<'a> ThreadAccess<'a> {
    pub(crate) fn signal_pending(&self) -> bool {
        let p = self.as_ptr();
        if !ThreadAccess::THREAD_SIGPENDING(p) { return false; }
        let cur = xv6_current_thread();
        if p != cur || self.sigacts_ptr().is_null() { return true; }
        let sa = self.sigacts_ptr();
        SigActs::sigacts_lock_impl(sa);
        let has_pending = ThreadAccess::recalc_sigpending_tsk(p);
        if !has_pending { ThreadAccess::THREAD_CLEAR_SIGPENDING(p); }
        SigActs::sigacts_unlock_impl(sa);
        has_pending
    }

    pub(crate) fn killed(&self) -> c_int {
        if ThreadAccess::THREAD_KILLED(self.as_ptr()) { 1 } else { 0 }
    }
}

// P3-D2b: `signal_pending_locked` deleted -- zero callers anywhere in
// the tree.

// P3-D2b: `signal_terminated` deleted -- zero callers anywhere in the
// tree (kernel/inc/signal.h's legacy prototype does not count).

// ============================================================================
// signal_test_clear_stopped
// ============================================================================
// P3-D2b: `signal_test_clear_stopped` deleted -- zero callers anywhere
// in the tree.

// ============================================================================
// signal_restore
// ============================================================================

// ============================================================================
// sigaction
// ============================================================================
impl Signal {
pub(crate) fn sigaction(signum: c_int, act: *mut sigaction_t,
                                   oldact: *mut sigaction_t)-> c_int  {
    if signum < 1 || signum > NSIG { return -EINVAL; }
    if signum == SIGKILL || signum == SIGSTOP { return -EINVAL; }
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() { xv6_panic(c"sigaction: current returned NULL".as_ptr()); }
        if !ThreadAccess::THREAD_USER_SPACE(p) { return -EPERM; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        SigActs::sigacts_lock_impl(sa);
        if !oldact.is_null() {
            SigactsAccess::assume(sa).copy_action_to(signum, oldact);
        }
        if !act.is_null() {
            let mut clear_pending = false;
            SigactsAccess::assume(sa).reset_act_mask(signum);
            let input_act = crate::proc::access::SigActionAccess::assume(act);
            let is_dfl = input_act.is_default();
            let is_ign = input_act.is_ignore();

            if is_ign {
                sigaddset(&mut (*sa).sa_sigignore, signum);
                SigactsAccess::assume(sa).copy_action_from(signum, act);
                clear_pending = true;
            } else if is_dfl {
                if SigactsAccess::assume(sa).setdefault(signum) != 0 {
                    SigActs::sigacts_unlock_impl(sa);
                    return -EINVAL;
                }
                if sigismember((*sa).sa_sigignore, signum) > 0 {
                    clear_pending = true;
                }
                let pending_term = (*p).signal.sig_pending_mask
                    & (*sa).sa_sigterm & !(*p).signal.sig_mask;
                if pending_term != 0 {
                    ThreadAccess::THREAD_SET_KILLED(p);
                    ThreadAccess::set_term_signal_first(p, bits_ffs_g(pending_term));
                }
            } else {
                SigactsAccess::assume(sa).copy_action_from(signum, act);
                let stored_act = SigactsAccess::assume(sa).action_ref(signum);
                stored_act.and_mask(!sig_mask_bit(SIGKILL));
                stored_act.and_mask(!sig_mask_bit(SIGSTOP));
            }
            if clear_pending {
                if ta.sigpending_empty(signum) != 0 {
                    SigActs::sigacts_unlock_impl(sa);
                    return -EINVAL;
                }
                let tg_ptr = ta.thread_group_ptr();
                if let Some(tga) = ThreadGroupAccess::from_ptr(tg_ptr) {
                    tga.sigpending_empty_sig(signum);
                }
            }
        }
        SigActs::sigacts_unlock_impl(sa);
        0
    }
}
}

// ============================================================================
// sigprocmask
// ============================================================================
impl Signal {
pub(crate) fn sigprocmask(how: c_int, set: *const sigset_t,
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
        SigActs::sigacts_lock_impl(sa);
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
        ThreadAccess::recalc_sigpending_tsk(p);
        let mut pending = (*p).signal.sig_pending_mask;
        let tg = ta.thread_group_ptr();
        if !tg.is_null() {
            pending |= (*tg).shared_pending.sig_pending_mask;
        }
        let pending_unmasked = pending & !(*p).signal.sig_mask;
        let pending_term = pending_unmasked & (*sa).sa_sigterm;
        if pending_term != 0 {
            ThreadAccess::THREAD_SET_KILLED(p);
            ThreadAccess::set_term_signal_first(p, bits_ffs_g(pending_term));
        }
        SigActs::sigacts_unlock_impl(sa);
        if pending_unmasked != 0 {
            ThreadAccess::ta_of(p).tcb_lock();
            Signal::signal_notify(p);
            ThreadAccess::ta_of(p).tcb_unlock();
        }
        0
    }
}
}

// ============================================================================
// sigpending (query)
// ============================================================================
impl<'a> ThreadAccess<'a> {
    pub(crate) fn sigpending_query(&self, set: *mut sigset_t) -> c_int {
    if set.is_null() { return -EINVAL; }
    let p = self.as_ptr();
    let sa = self.sigacts_ptr();
    if sa.is_null() { xv6_panic(c"sigpending: sigacts NULL".as_ptr()); }
    SigActs::sigacts_lock_impl(sa);
    // SAFETY: `p` is `self`'s non-null pointer.
    let ts = unsafe { ThreadSignalAccess::assume_thread(p) };
    let mask = ts.sig_mask();
    let mut pending = ts.sig_pending_mask();
    let tg = self.thread_group_ptr();
    // SAFETY: `tg` is proven non-null by the enclosing `if !tg.is_null() { ... }`.
    if !tg.is_null() { pending |= unsafe { ThreadGroupAccess::assume(tg) }.shared_pending_ref().sig_pending_mask(); }
    write_out(set, mask & pending);
    SigActs::sigacts_unlock_impl(sa);
    0
    }
}

// ============================================================================
// sigreturn
// ============================================================================
impl Signal {
pub(crate) fn sigreturn()-> c_int  {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() { xv6_panic(c"sigreturn: current NULL".as_ptr()); }
        if !ThreadAccess::THREAD_USER_SPACE(p) { return -EPERM; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        SigActs::sigacts_lock_impl(sa);
        if (*p).signal.sig_ucontext == 0 {
            SigActs::sigacts_unlock_impl(sa);
            return -EINVAL;
        }
        SigActs::sigacts_unlock_impl(sa);
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
            ThreadAccess::THREAD_SET_KILLED(p);
            ThreadAccess::set_term_signal_first(p, SIGSEGV);
            Proc::exit(-1);
        }
        if Signal::signal_restore(p, &raw mut uc as *mut c_void) != 0 {
            xv6_panic(c"sigreturn: signal_restore failed".as_ptr());
        }
        0
    }
}
}

// ============================================================================
// __dequeue_signal_update_pending_nolock + __deliver_signal + handle_signal
// ============================================================================

impl Signal {
pub(crate) fn handle_signal() {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() { xv6_panic(c"handle_signal: current NULL".as_ptr()); }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        if ta.sigacts_ptr().is_null() { return; }
        let sa = ta.sigacts_ptr();
        let tg = ta.thread_group_ptr();
        loop {
            SigActs::sigacts_lock_impl(sa);
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
            if (masked & sigterm) != 0 || ThreadAccess::THREAD_KILLED(p) {
                if p == ProcTable::get_initproc() && !ThreadAccess::THREAD_KILLED(p) {
                    (*p).signal.sig_pending_mask &= !sigterm;
                    if !tg.is_null() {
                        (*tg).shared_pending.sig_pending_mask &= !sigterm;
                    }
                    ThreadAccess::recalc_sigpending_tsk(p);
                    SigActs::sigacts_unlock_impl(sa);
                    continue;
                }
                ThreadAccess::THREAD_SET_KILLED(p);
                ThreadAccess::set_term_signal_first(p, bits_ffs_g(masked & sigterm));
                SigActs::sigacts_unlock_impl(sa);
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
                        let h = (*act).handler.sa_handler;
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
                    ThreadAccess::recalc_sigpending_tsk(p);
                    SigActs::sigacts_unlock_impl(sa);
                    continue;
                }
                // fall through to delivery
            } else if pending_stop != 0 {
                (*p).signal.sig_pending_mask &= !pending_stop;
                if !tg.is_null() {
                    (*tg).shared_pending.sig_pending_mask &= !pending_stop;
                }
                (*p).signal.stop_signal = bits_ffs_g(pending_stop);
                ThreadAccess::recalc_sigpending_tsk(p);
                SigActs::sigacts_unlock_impl(sa);
                ThreadAccess::ta_of(p).tcb_lock();
                ThreadAccess::thread_state_set(p, THREAD_STOPPED);
                ThreadAccess::ta_of(p).tcb_unlock();
                Signal::notify_parent_child_stopped(p);
                Scheduler::yield_now();
                continue;
            }

            let signo = if masked != 0 { bits_ffs_g(masked) } else { 0 };
            if signo == 0 || signo > NSIG {
                SigActs::sigacts_unlock_impl(sa);
                break;
            }
            if sigismember(sigstop, signo) > 0 {
                SigActs::sigacts_unlock_impl(sa);
                continue;
            }

            let sa_copy: sigaction_t = SigactsAccess::assume(sa).action_copy(signo);
            let from_shared;
            if sigismember((*p).signal.sig_pending_mask, signo) > 0 {
                from_shared = false;
            } else if !tg.is_null() && sigismember((*tg).shared_pending.sig_pending_mask, signo) > 0 {
                from_shared = true;
            } else {
                SigActs::sigacts_unlock_impl(sa);
                continue;
            }

            let mut repeat = false;
            let info: *mut ksiginfo_t;
            if from_shared && !tg.is_null() {
                info = ThreadGroupAccess::from_ptr(tg).map_or(ptr::null_mut(), |tga| tga.dequeue_signal(signo));
            } else {
                info = match ThreadAccess::dequeue_signal_update_pending_nolock(p, signo,
                    &raw const sa_copy as *mut sigaction_t) {
                    Ok(dequeued) => dequeued,
                    Err(_) => xv6_panic(c"handle_signal: dequeue failed".as_ptr()),
                };
            }
            ThreadAccess::recalc_sigpending_tsk(p);
            SigActs::sigacts_unlock_impl(sa);

            if Signal::deliver_signal(p, signo, info,
                &raw const sa_copy as *mut sigaction_t,
                &raw mut repeat) != 0 {
                xv6_panic(c"handle_signal: deliver failed".as_ptr());
            }
            if (sa_copy.sa_flags & SA_SIGINFO) != 0 {
                SigActs::sigacts_lock_impl(sa);
                let unmasked = sigismember((*p).signal.sig_mask, signo) == 0;
                let mut still_pending = sigismember((*p).signal.sig_pending_mask, signo) > 0;
                if !still_pending && !tg.is_null() {
                    still_pending = sigismember((*tg).shared_pending.sig_pending_mask, signo) > 0;
                }
                SigActs::sigacts_unlock_impl(sa);
                if unmasked && still_pending { repeat = true; }
            }
            if !info.is_null() { KsigInfo::free(info); }
            if !repeat { break; }
        }

        Signal::recalc_sigpending();

        if ThreadAccess::THREAD_KILLED(p) { Proc::exit(-1); }
    }
}
}

// ============================================================================
// kill / kill_thread / tgkill / tkill / killed / kill_from_kernel / kill_proc
// ============================================================================
// NO-STANDALONE-FN: the thread-keyed `kill_thread`/`kill_proc` became
// `ThreadAccess` methods; the pid-keyed `kill`/`tgkill`/`tkill` (no single
// receiver — they resolve a pid at run time) became `Signal` associated
// functions.
impl<'a> ThreadAccess<'a> {
    pub(crate) fn kill_thread(&self, signum: c_int) -> c_int {
        let p = self.as_ptr();
        let cur = xv6_current_thread();
        let mut info = KsigInfoAccess::make(signum, cur, ThreadAccess::from_ptr(cur).map_or(-1, |ta| ta.resolve_tgid()));
        let _rcu = crate::lock::rcu::KRcuRead::new();
        Signal::__signal_send(p, &mut info)
    }

    pub(crate) fn kill_proc(&self, signum: c_int) -> c_int {
        let p = self.as_ptr();
        if sigbad(signum) { return -EINVAL; }
        let cur = xv6_current_thread();
        let mut info = KsigInfoAccess::make(signum, cur, ThreadAccess::from_ptr(cur).map_or(0, |ta| ta.resolve_tgid()));
        let _rcu = crate::lock::rcu::KRcuRead::new();
        // SAFETY: `p` is `self`'s non-null pointer.
        let tg = unsafe { ThreadAccess::assume(p) }.thread_group_ptr();
        if !tg.is_null() {
            return ThreadGroupAccess::from_ptr(tg).map_or(-EINVAL, |tga| tga.signal_send(&mut info));
        }
        Signal::__signal_send(p, &mut info)
    }
}

impl Signal {
    pub(crate) fn kill(pid: c_int, signum: c_int) -> c_int {
        if pid < -1 { return Pgroup::kill(-pid, signum); }
        if pid == -1 { return -EINVAL; }
        let cur = xv6_current_thread();
        let mut info = KsigInfoAccess::make(signum, cur, ThreadAccess::from_ptr(cur).map_or(-1, |ta| ta.resolve_tgid()));
        Self::signal_send(pid, &mut info)
    }

    pub(crate) fn tgkill(tgid: c_int, tid: c_int, signum: c_int) -> c_int {
        if tgid < 0 || tid < 0 || sigbad(signum) { return -EINVAL; }
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let p = ProcTable::get_thread(tid);
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
        let mut info = KsigInfoAccess::make(signum, cur, ThreadAccess::from_ptr(cur).map_or(-1, |ta| ta.resolve_tgid()));
        Signal::__signal_send(p, &mut info)
    }

    pub(crate) fn tkill(tid: c_int, signum: c_int) -> c_int {
        if tid < 0 || sigbad(signum) { return -EINVAL; }
        let _rcu = crate::lock::rcu::KRcuRead::new();
        let p = ProcTable::get_thread(tid);
        if is_err(p) { return -ESRCH; }
        let cur = xv6_current_thread();
        let mut info = KsigInfoAccess::make(signum, cur, ThreadAccess::from_ptr(cur).map_or(-1, |ta| ta.resolve_tgid()));
        Signal::__signal_send(p, &mut info)
    }
}

// P3-D2b: `kill_from_kernel` deleted -- zero callers anywhere in the
// tree (kernel/inc/signal.h's legacy prototype does not count).

// ============================================================================
// sigsuspend / sigwait
// ============================================================================
impl Signal {
pub(crate) fn sigsuspend(mask: *const sigset_t)-> c_int  {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() || mask.is_null() { return -EINVAL; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if sa.is_null() { xv6_panic(c"sigsuspend: sigacts NULL".as_ptr()); }
        SigActs::sigacts_lock_impl(sa);
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
            ThreadAccess::recalc_sigpending_tsk(p);
            SigActs::sigacts_unlock_impl(sa);
            return -EINTR;
        }
        ThreadAccess::recalc_sigpending_tsk(p);
        SigActs::sigacts_unlock_impl(sa);
        ThreadAccess::thread_state_set(p, THREAD_INTERRUPTIBLE);
        Scheduler::yield_now();
        -EINTR
    }
}
}

impl Signal {
pub(crate) fn sigwait(set: *const sigset_t, sig: *mut c_int)-> c_int  {
    raw_sig_abi! {
        let p = xv6_current_thread();
        if p.is_null() || set.is_null() || sig.is_null() { return -EINVAL; }
        let ta = ThreadAccess::from_raw(p).unwrap_unchecked();
        let sa = ta.sigacts_ptr();
        if sa.is_null() { xv6_panic(c"sigwait: sigacts NULL".as_ptr()); }
        loop {
            SigActs::sigacts_lock_impl(sa);
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
                            let ksi = container_of::<ksiginfo_t>((*head).next, KsigInfo::ksi_list_entry_offset());
                            list_node_detach_raw(&raw mut (*ksi).list_entry);
                            KsigInfo::free(ksi);
                        }
                        if list_node_is_empty_raw(head) {
                            sigdelset(&mut (*p).signal.sig_pending_mask, signo);
                        }
                    } else if !tg.is_null() && sigismember((*tg).shared_pending.sig_pending_mask, signo) > 0 {
                        let ksi = ThreadGroupAccess::from_ptr(tg).map_or(ptr::null_mut(), |tga| tga.dequeue_signal(signo));
                        if !ksi.is_null() { KsigInfo::free(ksi); }
                    }
                    *sig = signo;
                    ThreadAccess::recalc_sigpending_tsk(p);
                    SigActs::sigacts_unlock_impl(sa);
                    return 0;
                }
            }
            let saved_mask = (*p).signal.sig_mask;
            (*p).signal.sig_mask &= !wait_set;
            sigdelset(&mut (*p).signal.sig_mask, SIGKILL);
            sigdelset(&mut (*p).signal.sig_mask, SIGSTOP);
            ThreadAccess::recalc_sigpending_tsk(p);
            SigActs::sigacts_unlock_impl(sa);

            ThreadAccess::thread_state_set(p, THREAD_INTERRUPTIBLE);
            Scheduler::yield_now();

            SigActs::sigacts_lock_impl(sa);
            (*p).signal.sig_mask = saved_mask;
            ThreadAccess::recalc_sigpending_tsk(p);
            SigActs::sigacts_unlock_impl(sa);

            if ThreadAccess::ta_of(p).killed() != 0 { return -EINTR; }
        }
    }
}
}
