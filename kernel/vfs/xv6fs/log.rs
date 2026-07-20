//! xv6fs write-ahead log — Rust port of `kernel/vfs/xv6fs/log.c`
//! (Phase 2 Wave 19, sub-wave B; see `super` module doc and
//! `docs/rustify/phase2_plan.md`).
//!
//! Per-superblock logging for crash recovery: a transaction contains
//! updates from multiple FS operations, and only commits when there are
//! no FS operations active, ensuring atomicity. This is xv6fs's
//! crash-consistency mechanism — **block-count/outstanding-op accounting
//! and commit ordering are ported exactly**; a fidelity bug here either
//! deadlocks concurrent writers (log-full wait never wakes) or corrupts
//! the filesystem across a simulated crash.
//!
//! # Locking (see the C original's own file-header comment)
//!
//! 1. `vfs_superblock` rwsem (if held by caller)
//! 2. `vfs_inode` mutex (if held by caller)
//! 3. `log->lock` spinlock (acquired by `begin_op`/`end_op`)
//! 4. buffer mutex (acquired by `bread` during commit)
//!
//! CRITICAL: [`xv6fs_begin_op`] may sleep waiting for log space via the
//! per-log wait queue (`log->wait_queue`, a `tq_t` — chosen over a global
//! `sleep_on_chan()` specifically to avoid contention on the global sleep
//! lock, per the C original's own struct doc). Callers holding the
//! superblock write lock should be aware this can block file I/O
//! operations that need the same log.
//!
//! # `end_op`'s drain-and-wake pattern (wake under the lock)
//!
//! [`xv6fs_end_op`] uses `tq_bulk_move` to drain every waiter into a
//! stack-local temp queue and then calls `tq_wakeup_all` on it — both
//! steps *while holding `log->lock`*. The wake stays inside the critical
//! section on purpose: a waiter's `tq_wait` uses `log->lock` as its guard,
//! so an interrupted (signal-woken) waiter re-checks `is_enqueued()` under
//! that same lock. If the wake ran *after* releasing `log->lock`, a signal
//! could wake a waiter in the window after it was migrated to `temp_queue`
//! but before `tq_wakeup_all` popped it; the waiter would then find itself
//! enqueued on `temp_queue` rather than `log->wait_queue` and panic while
//! self-removing (and race the committer's lock-free `temp_queue` drain).
//! Keeping the wake under the lock serializes the two, closing the race.
//! The `temp_queue.counter > 0` guard is retained (an empty temp queue
//! after an uncontended commit is the common case).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_int, c_void};
use core::ptr;

use crate::bindings::{buf, spinlock_t, tq_t, xv6fs_logheader, xv6fs_superblock};
use crate::proc::proc_shims::{xv6_current_thread, xv6_panic, xv6_thread_state_set};
// P3-D2a: proc/thread_queue.rs primitives, reached as plain crate-path
// items instead of `extern "C"` redeclarations.
use crate::proc::{tq_bulk_move, tq_init, tq_wakeup_all};
// N-R7b: the log's in-memory control state is now a data-owning
// `SpinLock<LogInner>` (lock-owns-data); `tq_wait` is reached through the
// guard's `wait_on` instead of a bare crate-path call.
use crate::sync::SpinLock;

// ===========================================================================
// Native xv6fs log types — P3-N8 nativization (user directive: remove
// the C-compatible interfaces). `Xv6fsLogHeader`/`Xv6fsLog` are the
// canonical native definitions of `kernel/vfs/xv6fs/xv6fs_private.h`'s
// `struct xv6fs_logheader`/`struct xv6fs_log`: `build.rs` blocklists
// the bindgen emissions and re-exports these types as
// `crate::bindings::xv6fs_logheader`/`xv6fs_log` (facade `pub use`, N2
// pattern). The C header stays unchanged (no C consumers remain — the
// kernel tree has zero `.c` files).
//
// *** ON-DISK LAYOUT — HANDLE WITH P3-4 SCRUTINY *** `xv6fs_logheader`
// is BOTH the in-memory and the ON-DISK log-header record: this file's
// `__xv6fs_write_head` casts the log-start buffer-cache block's `data`
// directly to `*mut xv6fs_logheader` and `bwrite`s it (and
// `__xv6fs_read_head` reads it back the same way), so the struct IS
// the persistent crash-recovery format: `n` (i32 LE) at byte 0,
// `block[240]` (i32 LE each) at byte 4, 964 bytes total, no padding.
// The byte-exact asserts below pin every one of those facts; the array
// length 240 reproduces bindgen's emission verbatim (`XV6FS_LOGSIZE`
// == param.h's `LOGSIZE` == 240; changing LOGSIZE would already be an
// on-disk format break in C, and would now also trip the size assert).
// The full fs corruption battery (commit/recovery paths, stressfs ×2)
// is the behavioral gate for this type.
//
// DERIVE DECISIONS (P3-N8): both derived Copy/Clone in the
// pre-nativization bindgen output (`xv6fs_logheader` is a plain-int
// POD; `xv6fs_log`'s embedded `spinlock_t`/`tq_t` natives are Copy) —
// kept, exactly as emitted.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + cross-compiler value probe (toolchain gcc,
// rv64gc/lp64d — scratchpad p3n8_probe_values.c); both agree on every
// value asserted below (no pipe-style divergence).
// ===========================================================================

/// Native `struct xv6fs_logheader` (`kernel/vfs/xv6fs/xv6fs_private.h`)
/// — the log header, used **both on-disk and in-memory** to track
/// logged blocks (see the ON-DISK note in the section comment above).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Xv6fsLogHeader {
    pub n: c_int,
    pub block: [c_int; 240],
}

/// Native `struct xv6fs_log` (`kernel/vfs/xv6fs/xv6fs_private.h`) —
/// per-superblock logging state for crash recovery (in-memory only;
/// only its `lh` member's *contents* reach disk, via
/// `__xv6fs_write_head`).
///
/// N-R7b: **superseded in the live code path by [`SpinLock<LogInner>`]**
/// (lock-owns-data). This full-fat struct — with the `lock: spinlock_t`
/// baked in as a plain field — is retained ONLY as the target of the
/// `crate::bindings::xv6fs_log` facade re-export in `kernel/bindings.rs`
/// (a file outside this wave's edit scope), so that facade keeps
/// resolving. Nothing constructs or stores an `Xv6fsLog` anymore; the
/// mounted superblock holds a `SpinLock<LogInner>` instead. The layout
/// asserts below still pin its historical shape.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct Xv6fsLog {
    pub lock: spinlock_t,
    /// Per-log wait queue for `begin_op` waiters.
    pub wait_queue: tq_t,
    pub start: c_int,
    pub size: c_int,
    pub outstanding: c_int,
    pub committing: c_int,
    pub dev: c_int,
    pub lh: Xv6fsLogHeader,
}

// P3-N8 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc probe (see the module
// note above). The `Xv6fsLogHeader` asserts are the ON-DISK byte
// contract.
const _: () = {
    assert!(core::mem::size_of::<Xv6fsLogHeader>() == 964, "xv6fs_logheader size (ON-DISK)");
    assert!(core::mem::align_of::<Xv6fsLogHeader>() == 4, "xv6fs_logheader alignment");
    assert!(core::mem::offset_of!(Xv6fsLogHeader, n) == 0, "xv6fs_logheader.n offset (ON-DISK)");
    assert!(
        core::mem::offset_of!(Xv6fsLogHeader, block) == 4,
        "xv6fs_logheader.block offset (ON-DISK)"
    );
    assert!(core::mem::size_of::<c_int>() == 4, "xv6fs_logheader element width (ON-DISK)");

    assert!(core::mem::size_of::<Xv6fsLog>() == 1088, "xv6fs_log size");
    assert!(core::mem::align_of::<Xv6fsLog>() == 64, "xv6fs_log alignment");
    assert!(core::mem::offset_of!(Xv6fsLog, lock) == 0, "xv6fs_log.lock offset");
    assert!(core::mem::offset_of!(Xv6fsLog, wait_queue) == 24, "xv6fs_log.wait_queue offset");
    assert!(core::mem::offset_of!(Xv6fsLog, start) == 72, "xv6fs_log.start offset");
    assert!(core::mem::offset_of!(Xv6fsLog, size) == 76, "xv6fs_log.size offset");
    assert!(core::mem::offset_of!(Xv6fsLog, outstanding) == 80, "xv6fs_log.outstanding offset");
    assert!(core::mem::offset_of!(Xv6fsLog, committing) == 84, "xv6fs_log.committing offset");
    assert!(core::mem::offset_of!(Xv6fsLog, dev) == 88, "xv6fs_log.dev offset");
    assert!(core::mem::offset_of!(Xv6fsLog, lh) == 92, "xv6fs_log.lh offset");
};

/// The lock-protected in-memory log control state (N-R7b) — everything
/// [`Xv6fsLog`] held **except** the `lock` field, which now lives in the
/// enclosing [`SpinLock`] (`raw`). Stored as `SpinLock<LogInner>` in
/// [`Xv6fsSuperblock::log`](super::superblock::Xv6fsSuperblock); every
/// begin/end/log_write field touch goes through the guard (safe), while
/// the commit/recovery I/O helpers reach it via
/// [`SpinLock::data_ptr`] under the `committing`-flag protocol (they
/// cannot hold the spinlock across sleeping `bread`/`bwrite`).
///
/// `lh` is the ON-DISK [`Xv6fsLogHeader`] record, rides here unchanged
/// (same `#[repr(C)]` bytes `__xv6fs_write_head` streams to disk); the
/// asserts below pin its offset within this in-memory struct.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct LogInner {
    /// Per-log wait queue for `begin_op` waiters (init'd against the
    /// enclosing `SpinLock`'s lock in [`xv6fs_initlog`]).
    pub wait_queue: tq_t,
    pub start: c_int,
    pub size: c_int,
    pub outstanding: c_int,
    pub committing: c_int,
    pub dev: c_int,
    pub lh: Xv6fsLogHeader,
}

// N-R7b interior-layout proof for the de-locked inner state. Independent
// of the enclosing `SpinLock`'s own layout; the point is to keep the
// ON-DISK `lh` sub-record 4-aligned and contiguous with the control
// scalars (identical to `Xv6fsLog` minus the leading 24-byte lock).
const _: () = {
    assert!(core::mem::align_of::<LogInner>() == 8, "LogInner alignment");
    assert!(core::mem::offset_of!(LogInner, wait_queue) == 0, "LogInner.wait_queue offset");
    assert!(core::mem::offset_of!(LogInner, start) == 48, "LogInner.start offset");
    assert!(core::mem::offset_of!(LogInner, size) == 52, "LogInner.size offset");
    assert!(core::mem::offset_of!(LogInner, outstanding) == 56, "LogInner.outstanding offset");
    assert!(core::mem::offset_of!(LogInner, committing) == 60, "LogInner.committing offset");
    assert!(core::mem::offset_of!(LogInner, dev) == 64, "LogInner.dev offset");
    assert!(core::mem::offset_of!(LogInner, lh) == 68, "LogInner.lh offset (ON-DISK sub-record)");
    assert!(core::mem::size_of::<LogInner>() == 1032, "LogInner size");
};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

// P3-D2b: `signal_pending` (proc/signal.rs) is a plain crate-path item
// now that its `#[no_mangle]` export is gone. The old redeclaration here
// said `-> bool_` (c_uint); the real fn returns `bool` (same 0/1 in `a0`
// under the old C ABI), so the call sites drop their `!= 0`.
use crate::proc::signal_pending;

unsafe extern "C" {
    // string.rs.
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

}

// N-R7b: the hand-rolled `spin_init`/`spin_lock`/`spin_unlock` facade
// wrappers are gone — the log's lock is now embedded in
// `SpinLock<LogInner>`, so every acquire/release is the guard's `lock()`/
// `Drop`, and the diagnostic name is set once by `SpinLock::new`.

// P3-D3c: `bufcache.rs`'s entry points are plain (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; identical signatures, plain `use`.
use crate::bufcache::{bpin, bread, brelse, bsync, bunpin, bwrite, bwrite_async};

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

/// Mirrors `mkdev(m, n)`/`xv6fs_sb_dev()` — hardcoded locally, same
/// rationale/precedent as `superblock.rs`'s own copy.
#[inline(always)]
const fn mkdev(major: i32, minor: i32) -> u32 {
    ((major << 20) | minor) as u32
}
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock` with a non-null
/// `blkdev`.
#[inline(always)]
unsafe fn xv6fs_sb_dev(xv6_sb: *mut xv6fs_superblock) -> u32 {
    unsafe {
        let bdev = (*xv6_sb).blkdev;
        mkdev((*bdev).dev.major, (*bdev).dev.minor)
    }
}

/// `THREAD_INTERRUPTIBLE` (`proc/thread_types.h`) — hardcoded locally per
/// this crate's established convention (see `proc/exit.rs`/`clone.rs`'s
/// own copies).
const THREAD_INTERRUPTIBLE: c_int = 2;
/// `THREAD_UNINTERRUPTIBLE`.
const THREAD_UNINTERRUPTIBLE: c_int = 6;

// ===========================================================================
// Log recovery
// ===========================================================================

/// Mirrors `__xv6fs_install_trans()`. Copy committed blocks from log to
/// their home location.
///
/// # Safety
/// `log` must point to a live `LogInner`.
unsafe fn __xv6fs_install_trans(log: *mut LogInner, recovering: bool) {
    unsafe {
        // `lh.n` is loop-invariant here: the committing-flag protocol makes
        // this committer/recoverer the SOLE accessor of `log`, so capture it
        // once and walk the tail range. Loop STRUCTURE only -- every
        // bread/memmove/bwrite disk op stays byte-identical (e3cb8a0 precedent).
        let n = (*log).lh.n;
        for tail in 0..n {
            let lbuf = bread((*log).dev as u32, ((*log).start + tail + 1) as u32); // read log block
            let dbuf = bread((*log).dev as u32, (*log).lh.block[tail as usize] as u32); // read dst
            memmove((*dbuf).data as *mut c_void, (*lbuf).data as *const c_void, super::BSIZE as usize); // copy block to dst
            if recovering {
                // During recovery, use synchronous writes for safety.
                bwrite(dbuf);
            } else {
                // Normal operation: use async writes, sync at end.
                bwrite_async(dbuf);
                bunpin(dbuf);
            }
            brelse(lbuf);
            brelse(dbuf);
        }

        // Flush all async writes to disk.
        if !recovering && n > 0 {
            bsync();
        }
    }
}

/// Mirrors `__xv6fs_read_head()`. Read the log header from disk into the
/// in-memory log header.
///
/// # Safety
/// `log` must point to a live `LogInner`.
unsafe fn __xv6fs_read_head(log: *mut LogInner) {
    unsafe {
        let b = bread((*log).dev as u32, (*log).start as u32);
        let lh = (*b).data as *const xv6fs_logheader;
        (*log).lh.n = (*lh).n;
        // `lh.n` just set from disk and invariant for this copy -> range walk.
        for i in 0..(*log).lh.n as usize {
            (*log).lh.block[i] = (*lh).block[i];
        }
        brelse(b);
    }
}

/// Mirrors `__xv6fs_write_head()`. Write in-memory log header to disk.
/// This is the true point at which the current transaction commits.
///
/// # Safety
/// `log` must point to a live `LogInner`.
unsafe fn __xv6fs_write_head(log: *mut LogInner) {
    unsafe {
        let b = bread((*log).dev as u32, (*log).start as u32);
        let hb = (*b).data as *mut xv6fs_logheader;
        (*hb).n = (*log).lh.n;
        // `lh.n` invariant for this copy -> range walk (structure only).
        for i in 0..(*log).lh.n as usize {
            (*hb).block[i] = (*log).lh.block[i];
        }
        bwrite(b);
        brelse(b);
    }
}

/// # Safety
/// `log` must point to a live `LogInner`.
unsafe fn __xv6fs_recover_from_log(log: *mut LogInner) {
    unsafe {
        __xv6fs_read_head(log);
        __xv6fs_install_trans(log, true); // if committed, copy from log to disk
        (*log).lh.n = 0;
        __xv6fs_write_head(log); // clear the log
    }
}

// ===========================================================================
// Commit
// ===========================================================================

/// Mirrors `__xv6fs_write_log()`. Copy modified blocks from cache to log.
/// Uses async writes with a final sync for better I/O batching.
///
/// # Safety
/// `log` must point to a live `LogInner`.
unsafe fn __xv6fs_write_log(log: *mut LogInner) {
    unsafe {
        // `lh.n` loop-invariant under the committing-flag protocol (sole
        // accessor) -> capture once, walk the tail range. Structure only.
        let n = (*log).lh.n;
        for tail in 0..n {
            let to = bread((*log).dev as u32, ((*log).start + tail + 1) as u32); // log block
            let from = bread((*log).dev as u32, (*log).lh.block[tail as usize] as u32); // cache block
            memmove((*to).data as *mut c_void, (*from).data as *const c_void, super::BSIZE as usize);
            bwrite_async(to); // mark dirty, will flush at end
            brelse(from);
            brelse(to);
        }

        // Flush all log writes before writing header.
        if n > 0 {
            bsync();
        }
    }
}

/// # Safety
/// `log` must point to a live `LogInner`.
unsafe fn __xv6fs_commit(log: *mut LogInner) {
    unsafe {
        if (*log).lh.n > 0 {
            __xv6fs_write_log(log); // Write modified blocks from cache to log.
            __xv6fs_write_head(log); // Write header to disk -- the real commit.
            __xv6fs_install_trans(log, false); // Now install writes to home locations.
            (*log).lh.n = 0;
            __xv6fs_write_head(log); // Erase the transaction from the log.
        }
    }
}

// ===========================================================================
// Public API
// ===========================================================================

/// Initialize the log for a xv6fs superblock.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_initlog(xv6_sb: *mut xv6fs_superblock) {
    // SAFETY: `xv6_sb` is live (caller's contract, mount-time setup with
    // no concurrent access yet).
    unsafe {
        if core::mem::size_of::<xv6fs_logheader>() >= super::BSIZE as usize {
            xv6_panic(c"xv6fs_initlog: too big logheader".as_ptr());
        }

        let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);
        let log_slot = ptr::addr_of_mut!((*xv6_sb).log);

        // The mount path zero-initialised the whole superblock, so the log
        // slot is all-zero bytes (an unnamed, unlocked spinlock + a zeroed
        // LogInner). Overwrite it with a properly *named* lock (the name the
        // deadlock-panic path prints) around a zeroed LogInner. `LogInner`/
        // `SpinLock` hold no Drop types, so `ptr::write` correctly does not
        // drop the previous all-zero bytes. This is the runtime-alloc
        // analogue of the console pilot's `static SpinLock::new(...)`.
        ptr::write(log_slot, SpinLock::new(c"xv6fs_log", core::mem::zeroed::<LogInner>()));
        let log_lock: &SpinLock<LogInner> = &*log_slot;

        // Populate the control state under the guard (single-threaded at
        // mount) and wire the per-log wait queue to THIS lock, so a later
        // `begin_op` `wait_on(&raw mut inner.wait_queue, ..)` releases and
        // reacquires exactly this spinlock.
        {
            let mut log = log_lock.lock();
            let lock_ptr = log.lock_ptr();
            tq_init(&raw mut log.wait_queue, c"xv6fs_log_wait".as_ptr(), lock_ptr);
            log.start = (*disk_sb).logstart as c_int;
            log.size = (*disk_sb).nlog as c_int;
            log.dev = xv6fs_sb_dev(xv6_sb) as c_int;
            log.outstanding = 0;
            log.committing = 0;
            log.lh.n = 0;
        }

        // Recovery replays/clears the on-disk log via sleeping bread/bwrite,
        // so it cannot hold the spinlock; mount is single-threaded, so the
        // lock-free `data_ptr()` access has no concurrent accessor.
        __xv6fs_recover_from_log(log_lock.data_ptr());
    }
}

/// Common implementation for begin_op with configurable sleep state.
/// `state`: `THREAD_INTERRUPTIBLE` (returns `-EINTR` on signal) or
/// `THREAD_UNINTERRUPTIBLE` (never interrupted, always returns 0).
///
/// # Safety
/// `xv6_sb` must point to a live `xv6fs_superblock`.
unsafe fn __xv6fs_begin_op(xv6_sb: *mut xv6fs_superblock, state: c_int) -> c_int {
    unsafe {
        let log_lock: &SpinLock<LogInner> = &(*xv6_sb).log;
        let interruptible = state == THREAD_INTERRUPTIBLE;
        let current = xv6_current_thread();

        // Acquire once; the guard is re-held across every `wait_on` (which
        // atomically drops+reacquires this same lock), so each loop
        // iteration re-tests `committing`/`lh.n` under the lock. The early
        // `return`s drop the guard on the way out (== the old
        // `spin_unlock`). `wait_on`'s `&mut self` is the freeze/noalias
        // defense that keeps the condition re-read inside the loop.
        let mut log = log_lock.lock();
        loop {
            if log.committing != 0 {
                if interruptible && signal_pending(current) {
                    return neg(crate::bindings::EINTR);
                }
                xv6_thread_state_set(current, state);
                let wq = &raw mut log.wait_queue;
                let ret = log.wait_on(wq, ptr::null_mut());
                if interruptible && ret != 0 && signal_pending(current) {
                    return neg(crate::bindings::EINTR);
                }
            } else if log.lh.n + (log.outstanding + 1) * super::MAXOPBLOCKS > super::XV6FS_LOGSIZE {
                // This op might exhaust log space; wait for commit.
                if interruptible && signal_pending(current) {
                    return neg(crate::bindings::EINTR);
                }
                xv6_thread_state_set(current, state);
                let wq = &raw mut log.wait_queue;
                let ret = log.wait_on(wq, ptr::null_mut());
                if interruptible && ret != 0 && signal_pending(current) {
                    return neg(crate::bindings::EINTR);
                }
            } else {
                log.outstanding += 1;
                break;
            }
        }
        0
    }
}

/// Called at the start of each FS operation (interruptible). Returns
/// `-EINTR` if a signal is pending; the caller has NOT entered the
/// transaction and may propagate the error or return a short count.
///
/// CRITICAL: Must be called BEFORE acquiring any VFS-layer locks
/// (superblock, inode) to avoid deadlock, since this function may sleep
/// waiting for log space.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_begin_op(xv6_sb: *mut xv6fs_superblock) -> c_int {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe { __xv6fs_begin_op(xv6_sb, THREAD_INTERRUPTIBLE) }
}

/// Uninterruptible variant for mid-operation batch boundaries. Use this
/// when a multi-batch operation (e.g. itrunc) has already committed
/// partial work and MUST re-enter a transaction to continue --
/// abandonment would leave the filesystem in an inconsistent state. This
/// variant ignores signals and never fails.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_begin_op_nointr(xv6_sb: *mut xv6fs_superblock) {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe { __xv6fs_begin_op(xv6_sb, THREAD_UNINTERRUPTIBLE) };
}

/// Called at the end of each FS operation. Commits if this was the last
/// outstanding operation.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_end_op(xv6_sb: *mut xv6fs_superblock) {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe {
        let log_lock: &SpinLock<LogInner> = &(*xv6_sb).log;
        let mut do_commit = false;

        {
            let mut log = log_lock.lock();
            log.outstanding -= 1;
            if log.committing != 0 {
                xv6_panic(c"xv6fs: log.committing".as_ptr());
            }
            if log.outstanding == 0 {
                do_commit = true;
                log.committing = 1;
            }
        } // guard dropped -> spin_unlock

        if do_commit {
            // Commit runs LOCK-FREE, serialised by `committing == 1`: while
            // it holds, `begin_op` waiters block and no `log_write` can run
            // (outstanding == 0), so the committer is the sole accessor of
            // the log's in-memory state. It streams the log to disk across
            // sleeping `bread`/`bwrite`, so it must NOT hold the spinlock --
            // hence `data_ptr()`, the escape hatch for exactly this
            // committing-flag-protected access. The lock release above and
            // reacquire below provide the release/acquire barrier that
            // publishes the committer's writes (e.g. `lh.n = 0`) to the next
            // lock holder.
            __xv6fs_commit(log_lock.data_ptr());

            // Drain waiters into a stack-local queue and wake them, all
            // under `log->lock`. The wake MUST happen inside the same
            // critical section as the `tq_bulk_move`: a waiter's `tq_wait`
            // takes `log->lock` as its guard, so a signal-interrupted
            // waiter re-checks `is_enqueued()` under that same lock. Waking
            // outside the lock opened a window where a signal could wake a
            // waiter after it had been migrated to `temp_queue` but before
            // `tq_wakeup_all` popped it: the waiter would then observe
            // itself enqueued on `temp_queue` (not `log->wait_queue`) and
            // panic in `tq_wait_cb`'s self-removal ("Failed to remove
            // interrupted waiter from queue"), while also racing the
            // committer's lock-free drain of `temp_queue`. Waking under the
            // lock serializes both: the waiter either still sits in
            // `log->wait_queue` (migration not yet done) or has already
            // been popped by `tq_wakeup_all` (is_enqueued() == false).
            let mut temp_queue: tq_t = core::mem::zeroed();
            tq_init(&raw mut temp_queue, c"xv6fs_log_temp".as_ptr(), ptr::null_mut());

            let mut log = log_lock.lock();
            log.committing = 0;
            tq_bulk_move(&raw mut temp_queue, &raw mut log.wait_queue);
            if temp_queue.counter > 0 {
                tq_wakeup_all(&raw mut temp_queue, 0, 0);
            }
            // guard dropped -> spin_unlock
        }
    }
}

/// Record the block number for writing. Must be called between begin_op
/// and end_op.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
pub(crate) extern "C" fn xv6fs_log_write(xv6_sb: *mut xv6fs_superblock, b: *mut buf) {
    // SAFETY: `xv6_sb`/`b` are live (caller's contract: called between
    // begin_op/end_op).
    unsafe {
        let log_lock: &SpinLock<LogInner> = &(*xv6_sb).log;

        let mut log = log_lock.lock();
        if log.lh.n >= super::XV6FS_LOGSIZE || log.lh.n >= log.size - 1 {
            xv6_panic(c"xv6fs: too big a transaction".as_ptr());
        }
        if log.outstanding < 1 {
            xv6_panic(c"xv6fs: log_write outside of trans".as_ptr());
        }

        // Log-absorption scan: find an existing slot for this block, else the
        // append index `n`. `n` is loop-invariant under the held guard (no
        // sleep on this path) -> `.position()` over the live slice.
        let n = log.lh.n as usize;
        let blockno = (*b).blockno as i32;
        let i = log.lh.block[..n].iter().position(|&bn| bn == blockno).unwrap_or(n);
        log.lh.block[i] = blockno;
        if i == n {
            // Add new block to log?
            bpin(b);
            log.lh.n += 1;
        }
        // guard dropped -> spin_unlock
    }
}
