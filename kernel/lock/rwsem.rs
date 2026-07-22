//! Rust port of `kernel/lock/rwsem.c`. Preserves C ABI.

#![allow(non_camel_case_types, non_snake_case)]


use core::ffi::{c_char, c_int, c_void};
use core::ptr::{addr_of_mut, null_mut};

use crate::u;
use crate::bindings::{
    rwsem_t, spinlock_t, thread, timer_node, tq_t,
    thread_state_THREAD_INTERRUPTIBLE, thread_state_THREAD_UNINTERRUPTIBLE,
    EAGAIN, EDEADLK, EINTR, EINVAL, ETIMEDOUT,
    RWLOCK_PRIO_WRITE,
};
use crate::machine;
use crate::sync::KSpinlock;

// P3-D2a: the thread-queue primitives (kernel/proc/thread_queue.rs) are
// ordinary Rust fns, reached as plain crate-path items instead of
// `extern "C"` redeclarations.
// NO-STANDALONE-FN: the `tq_init`/`tq_size`/`tq_wait`/`tq_wait_cb`/
// `tq_wakeup`/`tq_wakeup_all` free-fn delegators were deleted; each call
// site builds a `TqRef` handle via `from_ptr` and invokes the method.
use crate::proc::access::TqRef;

// P3-D2b: `signal_pending` (proc/signal.rs) is a plain crate-path item
// now that its `#[no_mangle]` export is gone. The old redeclaration here
// said `-> u32`; the real fn returns `bool` (same 0/1 in `a0` under the
// old C ABI), so the call sites drop their `!= 0`.

// P3-D3c: `sched_timer_set`/`sched_timer_done` are genuinely `unsafe fn`
// in `crate::timer::sched_timer` now that their `#[no_mangle]` exports are
// gone; this file's original extern declarations asserted `pub safe fn`
// (usual FFI-facade convention). The thin wrappers below preserve that
// safe facade for the unchanged call sites.
/// SAFETY: see [`crate::timer::sched_timer::sched_timer_set`]'s contract.
fn sched_timer_set(tn: *mut timer_node, ticks: u64) -> c_int {
    unsafe { crate::timer::sched_timer::sched_timer_set(tn, ticks) }
}
/// SAFETY: see [`crate::timer::sched_timer::sched_timer_done`]'s contract.
fn sched_timer_done(tn: *mut timer_node) {
    unsafe { crate::timer::sched_timer::sched_timer_done(tn) }
}
use crate::lock::spinlock::RawSpinlock;

/// See `completion.rs`'s identical note: `crate::lock::spinlock::RawSpinlock::init`
/// takes `name: *mut c_char`; this file's original extern declaration
/// typed it `*const c_char` (call site only ever passes a `'static`
/// string-literal pointer, never written through).
#[inline]
fn spin_init(lk: *mut spinlock_t, name: *const c_char) {
    // SAFETY: `name` is only read by the callee despite the `*mut`
    // parameter; the sole call site passes a `'static` string literal.
    unsafe { crate::lock::spinlock::RawSpinlock::init(lk, name as *mut c_char) };
}

// ---------------------------------------------------------------------------
// Native layout — Wave P3-3A.
//
// `crate::bindings::rwsem_t` (`kernel/inc/lock/rwlock_types.h`'s `struct
// rwsem`) has ~90 non-`lock/` referents (`KRwSem::raw` is embedded in
// bindgen structs across mm/proc/vfs), so the C-ABI entry points below
// and `KRwSem`/its guards' public field type keep `*mut rwsem_t`
// unchanged. `RawRwsem` is this file's own native, layout-identical
// working type: every private helper operates on it directly. `lock`
// reuses `spinlock::RawSpinlock`; `read_queue`/`write_queue` stay the
// bindgen `tq_t` (proc-owned, out of this wave's scope).
// P3-N2: `RawRwsem` IS the kernel-wide Rust definition of `rwsem_t`
// now (`build.rs` blocklists the bindgen `rwsem`/`rwsem_t` and
// re-exports this type under both names), so bindgen structs embedding
// an rwsem by value (`vm.lock`, `vfs_inode.rw_lock`, ...) resolve here
// and require the same `Copy`/`Clone` derives the bindgen struct had.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct RawRwsem {
    pub(crate) lock: crate::lock::spinlock::RawSpinlock,
    pub(crate) readers: c_int,
    pub(crate) holder_pid: c_int,
    pub(crate) read_queue: tq_t,
    pub(crate) write_queue: tq_t,
    pub(crate) name: *const c_char,
    pub(crate) flags: u64,
}

// P3-N2 hardcoded layout proof — values captured from the
// pre-nativization bindgen output (`#[repr(C)] #[repr(align(64))]
// pub struct rwsem { lock: spinlock_t, readers: c_int, holder_pid:
// pid_t, read_queue: tq_t, write_queue: tq_t, name: *const c_char,
// flags: uint64 }`) and independently confirmed by a
// riscv64-unknown-elf-gcc `_Static_assert` probe against
// `kernel/inc/lock/rwsem_types.h` (size 192, align 64,
// offsets 0/24/28/32/80/128/136).
const _: () = {
    assert!(core::mem::size_of::<RawRwsem>() == 192, "rwsem_t size (144 payload bytes padded to 3 cachelines)");
    assert!(core::mem::align_of::<RawRwsem>() == 64, "rwsem_t alignment (from embedded spinlock_t)");
    assert!(core::mem::offset_of!(RawRwsem, lock) == 0, "rwsem.lock offset");
    assert!(core::mem::offset_of!(RawRwsem, readers) == 24, "rwsem.readers offset");
    assert!(core::mem::offset_of!(RawRwsem, holder_pid) == 28, "rwsem.holder_pid offset");
    assert!(core::mem::offset_of!(RawRwsem, read_queue) == 32, "rwsem.read_queue offset");
    assert!(core::mem::offset_of!(RawRwsem, write_queue) == 80, "rwsem.write_queue offset");
    assert!(core::mem::offset_of!(RawRwsem, name) == 128, "rwsem.name offset");
    assert!(core::mem::offset_of!(RawRwsem, flags) == 136, "rwsem.flags offset");
};

// NO-STANDALONE-FN sweep: every relocatable free fn in this file (internal
// field/wait-decision helpers and the public C-style API alike) is now an
// associated fn on `impl RawRwsem`, keyed by raw-pointer parameter, not by
// `&self`/`&RawRwsem` (see the module-scope N-R1 note below: `RawRwsem` is
// `Copy` with no `UnsafeCell`, so a `&RawRwsem` receiver would
// `readonly`/`noalias`-miscompile the `while reader_should_wait`/
// `writer_should_wait` spin/wait loops -- a permanent hang). Every moved
// body is byte-identical to its old free-fn form; only the wrapping `impl`
// block, the fn name (leading `rwsem_` stripped from the public API), and
// call-site `Self::`/`RawRwsem::` qualification changed.
impl RawRwsem {
    /// Reinterpret the bindgen `*mut rwsem_t` as the native mirror.
    ///
    /// SAFETY: layout equivalence is proven at compile time by the
    /// assertions above; the caller provides a valid, non-dangling
    /// `*mut rwsem_t`.
    #[inline(always)]
    fn as_native(l: *mut rwsem_t) -> *mut RawRwsem {
        l as *mut RawRwsem
    }

    // -----------------------------------------------------------------
    // Helpers — centralised unsafe field accessors
    // -----------------------------------------------------------------

    #[inline(always)]
    fn lk_ptr(l: *mut RawRwsem) -> *mut spinlock_t {
        // SAFETY: structurally valid rwsem.
        u! { addr_of_mut!((*l).lock) as *mut spinlock_t }
    }
    #[inline(always)]
    fn rq_ptr(l: *mut RawRwsem) -> *mut tq_t {
        // SAFETY: see `Self::lk_ptr`.
        u! { addr_of_mut!((*l).read_queue) }
    }
    #[inline(always)]
    fn wq_ptr(l: *mut RawRwsem) -> *mut tq_t {
        // SAFETY: see `Self::lk_ptr`.
        u! { addr_of_mut!((*l).write_queue) }
    }
    #[inline(always)]
    fn get_readers(l: *mut RawRwsem) -> c_int {
        // SAFETY: caller holds `l->lock`.
        u! { (*l).readers }
    }
    #[inline(always)]
    fn set_readers(l: *mut RawRwsem, v: c_int) {
        // SAFETY: caller holds `l->lock`.
        u! { (*l).readers = v; }
    }
    #[inline(always)]
    fn get_holder(l: *mut RawRwsem) -> c_int {
        // SAFETY: caller holds `l->lock`.
        u! { (*l).holder_pid }
    }
    #[inline(always)]
    fn set_holder(l: *mut RawRwsem, v: c_int) {
        // SAFETY: caller holds `l->lock`.
        u! { (*l).holder_pid = v; }
    }
    #[inline(always)]
    fn get_flags(l: *mut RawRwsem) -> u64 {
        // SAFETY: read-only of a constant-after-init field.
        u! { (*l).flags }
    }
    #[inline(always)]
    fn set_name_flags(l: *mut RawRwsem, name: *const c_char, flags: u64) {
        // SAFETY: caller has exclusive access at init time.
        u! { (*l).name = name; (*l).flags = flags; }
    }

    // -----------------------------------------------------------------
    // Wait-decision helpers (caller holds `l->lock`)
    // -----------------------------------------------------------------

    fn reader_should_wait(l: *mut RawRwsem) -> bool {
        if Self::get_readers(l) == 0 {
            return Self::get_holder(l) != -1;
        }
        if (Self::get_flags(l) & RWLOCK_PRIO_WRITE as u64) != 0 && TqRef::from_ptr(Self::wq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.size()) > 0 {
            return true;
        }
        false
    }

    fn writer_should_wait(l: *mut RawRwsem, pid: c_int) -> bool {
        let h = Self::get_holder(l);
        if h == pid { return false; }
        if h != -1 { return true; }
        if Self::get_readers(l) > 0 { return true; }
        false
    }

    // Caller holds `l->lock`.
    fn wake_readers(l: *mut RawRwsem) {
        let _ = TqRef::from_ptr(Self::rq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wakeup_all(0, 0));
    }

    // Caller holds `l->lock`.
    fn wake_writer(l: *mut RawRwsem) {
        // Null-queue path unreachable; empty valid queue -> null (former
        // `tq_wakeup`). `thread_pid` filters null/ERR to -1 identically.
        let next = TqRef::from_ptr(Self::wq_ptr(l)).map_or(core::ptr::null_mut(), |r| r.wakeup_one(0, 0));
        let pid = machine::Riscv::thread_pid(next);
        if pid != -1 {
            Self::set_holder(l, pid);
        }
    }

    fn do_wake_up(l: *mut RawRwsem) {
        if (Self::get_flags(l) & RWLOCK_PRIO_WRITE as u64) != 0 {
            if TqRef::from_ptr(Self::wq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.size()) > 0 {
                Self::wake_writer(l);
            } else if TqRef::from_ptr(Self::rq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.size()) > 0 {
                Self::wake_readers(l);
            }
        } else {
            if TqRef::from_ptr(Self::rq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.size()) > 0 {
                Self::wake_readers(l);
            } else if TqRef::from_ptr(Self::wq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.size()) > 0 {
                Self::wake_writer(l);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Timed-wait context — see `mutex.rs` for the design rationale: one
// unsafe block per callback (the reborrow), all field access via safe
// methods on the ctx struct.
// ---------------------------------------------------------------------------

#[repr(C)]
struct RwsemTimedCtx {
    lock: *mut RawRwsem,
    timer: timer_node,
    timeout_ms: u64,
    timer_armed: bool,
}

impl RwsemTimedCtx {
    #[inline(always)]
    unsafe fn from_raw<'a>(data: *mut c_void) -> Option<&'a mut Self> {
        if data.is_null() { None } else { Some(u! { &mut *(data as *mut Self) }) }
    }
    #[inline(always)] fn lock_ptr(&self) -> *mut RawRwsem { self.lock }
    #[inline(always)] fn timer_node_ptr(&mut self) -> *mut timer_node { addr_of_mut!(self.timer) }
    #[inline(always)] fn timeout(&self) -> u64 { self.timeout_ms }
    #[inline(always)] fn armed(&self) -> bool { self.timer_armed }
    #[inline(always)] fn set_armed(&mut self, v: bool) { self.timer_armed = v; }
}

// FLOOR (left as free fns): address-taken `extern "C"` timer callbacks
// passed *by value* as `Option<unsafe extern "C" fn(...)>` arguments to
// `TqRef::wait_cb` -- same fn-pointer-type-coercion reasoning as
// `lock::spinlock::spin_sleep_cb`/`spin_wake_cb`. Bodies updated to call
// the renamed `RawRwsem::lk_ptr` associated fn.
extern "C" fn rwsem_timed_sleep_cb(data: *mut c_void)-> c_int  { u! {
    // SAFETY: see `RwsemTimedCtx::from_raw`.
    let ctx = match u! { RwsemTimedCtx::from_raw(data) } {
        Some(c) => c, None => return 0,
    };
    let l = ctx.lock_ptr();
    if l.is_null() { return 0; }

    ctx.set_armed(false);
    let timeout_ms = ctx.timeout();
    if timeout_ms > 0 {
        let tn = ctx.timer_node_ptr();
        if sched_timer_set(tn, timeout_ms) == 0 {
            ctx.set_armed(true);
        }
    }
    let status = RawSpinlock::is_holding(RawRwsem::lk_ptr(l));
    if status != 0 { RawSpinlock::unlock(RawRwsem::lk_ptr(l)); }
    status
}}

extern "C" fn rwsem_timed_wake_cb(data: *mut c_void, sleep_cb_status: c_int) { u! {
    // SAFETY: see `RwsemTimedCtx::from_raw`.
    let ctx = match u! { RwsemTimedCtx::from_raw(data) } {
        Some(c) => c, None => return,
    };
    let l = ctx.lock_ptr();
    if l.is_null() { return; }
    if ctx.armed() {
        let tn = ctx.timer_node_ptr();
        sched_timer_done(tn);
        ctx.set_armed(false);
    }
    if sleep_cb_status != 0 { RawSpinlock::lock(RawRwsem::lk_ptr(l)); }
}}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

// N-R1 NOTE — signatures KEPT RAW `*mut rwsem_t` (same finding as
// `completion.rs`, plus a scope constraint): `RawRwsem` is `#[derive(Copy)]`
// + no `UnsafeCell`, so a `&RawRwsem` parameter would `readonly`/`noalias`-
// miscompile the `while reader_should_wait`/`writer_should_wait` loops
// (hang); the sound typed-reference conversion needs the wave-3d
// `UnsafeCell` work first. The direct call sites also live in out-of-scope
// code (`vfs/fs.rs` ~30, `mm/vm.rs`), owned by later waves. What DID change
// (NO-STANDALONE-FN sweep): every one of these free fns is now an
// associated fn on `impl RawRwsem` (leading `rwsem_` stripped from the
// name), still keyed by raw-pointer parameter, NOT `&self`; the redundant
// whole-body `u!` wrappers stay removed from the public entry points.
impl RawRwsem {
    pub(crate) fn init(l: *mut rwsem_t, flags: u64, name: *const c_char) -> c_int {
        if l.is_null() || name.is_null() { return -1; }
        let l = Self::as_native(l);
        spin_init(Self::lk_ptr(l), b"rwsem spinlock\0".as_ptr() as *const c_char);
        Self::set_readers(l, 0);
        if let Some(r) = TqRef::from_ptr(Self::rq_ptr(l)) { r.init(b"rwsem read queue\0".as_ptr() as *const c_char, Self::lk_ptr(l)); }
        if let Some(r) = TqRef::from_ptr(Self::wq_ptr(l)) { r.init(b"rwsem write queue\0".as_ptr() as *const c_char, Self::lk_ptr(l)); }
        Self::set_name_flags(l, name, flags);
        Self::set_holder(l, -1);
        0
    }

    pub(crate) fn acquire_read(l: *mut rwsem_t) -> c_int {
        if l.is_null() { return -1; }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        while Self::reader_should_wait(l) {
            machine::Riscv::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
            let ret = TqRef::from_ptr(Self::rq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait(Self::lk_ptr(l), null_mut()));
            if ret != 0 { return ret; }
        }
        Self::set_readers(l, Self::get_readers(l) + 1);
        0
    }

    pub(crate) fn try_acquire_read(l: *mut rwsem_t) -> c_int {
        if l.is_null() { return -(EINVAL as c_int); }
        let l = Self::as_native(l);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        if !Self::reader_should_wait(l) {
            Self::set_readers(l, Self::get_readers(l) + 1);
            0
        } else {
            -(EAGAIN as c_int)
        }
    }

    pub(crate) fn acquire_read_interruptible(l: *mut rwsem_t) -> c_int {
        if l.is_null() { return -(EINVAL as c_int); }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        while Self::reader_should_wait(l) {
            if crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
            machine::Riscv::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
            let ret = TqRef::from_ptr(Self::rq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait(Self::lk_ptr(l), null_mut()));
            if ret != 0 && crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
        }
        Self::set_readers(l, Self::get_readers(l) + 1);
        0
    }

    pub(crate) fn acquire_read_timed(l: *mut rwsem_t, timeout_ms: u64) -> c_int {
        if l.is_null() { return -(EINVAL as c_int); }
        if timeout_ms == 0 {
            return if Self::try_acquire_read(l) == 0 { 0 } else { -(ETIMEDOUT as c_int) };
        }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let start = machine::Riscv::read_time();
        let timeout_ticks = machine::Riscv::ms_to_rawticks(timeout_ms);
        let tm = machine::Riscv::tick_ms();
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        while Self::reader_should_wait(l) {
            if crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
            let elapsed = machine::Riscv::read_time().wrapping_sub(start);
            if elapsed >= timeout_ticks { return -(ETIMEDOUT as c_int); }
            let remaining_ticks = timeout_ticks - elapsed;
            let mut remaining_ms = if tm == 0 { 1 } else { (remaining_ticks + tm - 1) / tm };
            if remaining_ms == 0 { remaining_ms = 1; }
            let mut ctx = RwsemTimedCtx {
                lock: l,
                // SAFETY: zero-init timer_node mirrors C `.timer = {0}`.
                timer: u! { core::mem::zeroed() },
                timeout_ms: remaining_ms,
                timer_armed: false,
            };
            machine::Riscv::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
            let ret = TqRef::from_ptr(Self::rq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait_cb(
                Some(rwsem_timed_sleep_cb),
                Some(rwsem_timed_wake_cb),
                &mut ctx as *mut _ as *mut c_void,
                null_mut(),
            ));
            if ret != 0 && crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
        }
        Self::set_readers(l, Self::get_readers(l) + 1);
        0
    }

    pub(crate) fn acquire_write(l: *mut rwsem_t) -> c_int {
        if l.is_null() { return -1; }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let pid = machine::Riscv::thread_pid(cur);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        while Self::writer_should_wait(l, pid) {
            machine::Riscv::thread_state_set(cur, thread_state_THREAD_UNINTERRUPTIBLE);
            let ret = TqRef::from_ptr(Self::wq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait(Self::lk_ptr(l), null_mut()));
            if ret != 0 { return ret; }
        }
        Self::set_holder(l, pid);
        0
    }

    pub(crate) fn try_acquire_write(l: *mut rwsem_t) -> c_int {
        if l.is_null() { return -(EINVAL as c_int); }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let pid = machine::Riscv::thread_pid(cur);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        if Self::get_holder(l) == pid { return -(EDEADLK as c_int); }
        if !Self::writer_should_wait(l, pid) {
            Self::set_holder(l, pid);
            0
        } else {
            -(EAGAIN as c_int)
        }
    }

    pub(crate) fn acquire_write_interruptible(l: *mut rwsem_t) -> c_int {
        if l.is_null() { return -(EINVAL as c_int); }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let pid = machine::Riscv::thread_pid(cur);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        if Self::get_holder(l) == pid { return -(EDEADLK as c_int); }
        while Self::writer_should_wait(l, pid) {
            if crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
            machine::Riscv::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
            let ret = TqRef::from_ptr(Self::wq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait(Self::lk_ptr(l), null_mut()));
            if ret != 0 && crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
        }
        Self::set_holder(l, pid);
        0
    }

    pub(crate) fn acquire_write_timed(l: *mut rwsem_t, timeout_ms: u64) -> c_int {
        if l.is_null() { return -(EINVAL as c_int); }
        if timeout_ms == 0 {
            return if Self::try_acquire_write(l) == 0 { 0 } else { -(ETIMEDOUT as c_int) };
        }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let pid = machine::Riscv::thread_pid(cur);
        let start = machine::Riscv::read_time();
        let timeout_ticks = machine::Riscv::ms_to_rawticks(timeout_ms);
        let tm = machine::Riscv::tick_ms();
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        if Self::get_holder(l) == pid { return -(EDEADLK as c_int); }
        while Self::writer_should_wait(l, pid) {
            if crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
            let elapsed = machine::Riscv::read_time().wrapping_sub(start);
            if elapsed >= timeout_ticks { return -(ETIMEDOUT as c_int); }
            let remaining_ticks = timeout_ticks - elapsed;
            let mut remaining_ms = if tm == 0 { 1 } else { (remaining_ticks + tm - 1) / tm };
            if remaining_ms == 0 { remaining_ms = 1; }
            let mut ctx = RwsemTimedCtx {
                lock: l,
                // SAFETY: zero-init timer_node.
                timer: u! { core::mem::zeroed() },
                timeout_ms: remaining_ms,
                timer_armed: false,
            };
            machine::Riscv::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
            let ret = TqRef::from_ptr(Self::wq_ptr(l)).map_or(-(crate::bindings::EINVAL as c_int), |r| r.wait_cb(
                Some(rwsem_timed_sleep_cb),
                Some(rwsem_timed_wake_cb),
                &mut ctx as *mut _ as *mut c_void,
                null_mut(),
            ));
            if ret != 0 && crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) { return -(EINTR as c_int); }
        }
        Self::set_holder(l, pid);
        0
    }

    pub(crate) fn release(l: *mut rwsem_t) {
        if l.is_null() { return; }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        let self_pid = machine::Riscv::thread_pid(cur);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        if Self::get_holder(l) == self_pid && self_pid != -1 {
            Self::set_holder(l, -1);
            Self::do_wake_up(l);
        } else {
            let r = Self::get_readers(l);
            if r > 0 {
                Self::set_readers(l, r - 1);
                if r - 1 == 0 {
                    Self::do_wake_up(l);
                }
            }
        }
    }

    pub(crate) fn is_write_holding(l: *mut rwsem_t) -> bool {
        if l.is_null() { return false; }
        let l = Self::as_native(l);
        let cur = machine::Riscv::current_thread_ptr();
        if cur.is_null() { return false; }
        let pid = machine::Riscv::thread_pid(cur);
        let _g = KSpinlock::from_bindings(Self::lk_ptr(l)).lock();
        Self::get_holder(l) == pid
    }
}

// ===========================================================================
// Rust-native typed handle
// ===========================================================================

use core::marker::PhantomData;

/// Errors from `rwsem_*` operations.
#[derive(Debug, Copy, Clone, PartialEq, Eq)]
pub enum RwSemError {
    Invalid,
    WouldBlock,
    Interrupted,
    TimedOut,
    Other(c_int),
}

impl RawRwsem {
    fn map_rwsem_err(r: c_int) -> RwSemError {
        match -r as u32 {
            EINVAL => RwSemError::Invalid,
            EAGAIN => RwSemError::WouldBlock,
            EINTR => RwSemError::Interrupted,
            ETIMEDOUT => RwSemError::TimedOut,
            _ => RwSemError::Other(r),
        }
    }
}

/// Typed handle to a kernel `rwsem_t`.
#[derive(Clone, Copy)]
pub struct KRwSem {
    raw: *mut rwsem_t,
    _ns: PhantomData<*const ()>,
}

impl KRwSem {
    #[inline(always)]
    pub fn from_ptr(raw: *mut rwsem_t) -> Self {
        Self { raw, _ns: PhantomData }
    }
    #[inline(always)]
    pub fn as_ptr(self) -> *mut rwsem_t { self.raw }

    #[inline]
    pub fn init(self, flags: u64, name: *const c_char) -> Result<(), RwSemError> {
        // SAFETY: handle wraps live storage.
        let r = u! { RawRwsem::init(self.raw, flags, name) };
        if r == 0 { Ok(()) } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    /// Acquire shared (read). Returns RAII guard.
    #[inline]
    pub fn read(self) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::acquire_read(self.raw) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    #[inline]
    pub fn try_read(self) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::try_acquire_read(self.raw) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    #[inline]
    pub fn read_interruptible(self) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::acquire_read_interruptible(self.raw) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    #[inline]
    pub fn read_timed(self, timeout_ms: u64) -> Result<KRwSemReadGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::acquire_read_timed(self.raw, timeout_ms) };
        if r == 0 {
            Ok(KRwSemReadGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    /// Acquire exclusive (write).
    #[inline]
    pub fn write(self) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::acquire_write(self.raw) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    #[inline]
    pub fn try_write(self) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::try_acquire_write(self.raw) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    #[inline]
    pub fn write_interruptible(self) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::acquire_write_interruptible(self.raw) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    #[inline]
    pub fn write_timed(self, timeout_ms: u64) -> Result<KRwSemWriteGuard, RwSemError> {
        // SAFETY: see `init`.
        let r = u! { RawRwsem::acquire_write_timed(self.raw, timeout_ms) };
        if r == 0 {
            Ok(KRwSemWriteGuard { raw: self.raw, _ns: PhantomData })
        } else { Err(RawRwsem::map_rwsem_err(r)) }
    }

    #[inline]
    pub fn is_write_holding(self) -> bool {
        // SAFETY: see `init`.
        u! { RawRwsem::is_write_holding(self.raw) }
    }
}

/// Read-side guard for [`KRwSem`].
#[must_use = "rwsem read lock is released when the guard is dropped"]
pub struct KRwSemReadGuard {
    raw: *mut rwsem_t,
    _ns: PhantomData<*const ()>,
}
impl Drop for KRwSemReadGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard tracks a successful acquire.
        u! { RawRwsem::release(self.raw); }
    }
}

/// Write-side guard for [`KRwSem`].
#[must_use = "rwsem write lock is released when the guard is dropped"]
pub struct KRwSemWriteGuard {
    raw: *mut rwsem_t,
    _ns: PhantomData<*const ()>,
}
impl Drop for KRwSemWriteGuard {
    #[inline(always)]
    fn drop(&mut self) {
        // SAFETY: guard tracks a successful acquire.
        u! { RawRwsem::release(self.raw); }
    }
}