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
//! # `end_op`'s wake-outside-lock pattern
//!
//! [`xv6fs_end_op`] uses `tq_bulk_move` to drain every waiter into a
//! stack-local temp queue *while holding `log->lock`*, then calls
//! `tq_wakeup_all` on the temp queue *after releasing* `log->lock` — this
//! avoids a lock convoy where every woken thread immediately piles up
//! trying to reacquire `log->lock` before it's even been released.
//! Ported exactly, including the `temp_queue.counter > 0` guard before
//! bothering to wake (an empty temp queue after an uncontended commit is
//! the common case).

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::ffi::{c_char, c_int, c_void};
use core::ptr;

use crate::bindings::{buf, spinlock_t, thread, tq_t, xv6fs_log, xv6fs_logheader, xv6fs_superblock};

// ===========================================================================
// Externs — see `superblock.rs`'s module doc for the convention.
// ===========================================================================

unsafe extern "C" {
    // proc module.
    safe fn xv6_panic(msg: *const c_char) -> !;
    safe fn xv6_current_thread() -> *mut thread;
    safe fn xv6_thread_state_set(p: *mut thread, s: c_int);
    safe fn signal_pending(p: *mut thread) -> crate::bindings::bool_;

    // string.rs.
    safe fn memmove(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;

    // lock module (100% Rust).
    safe fn spin_lock(l: *mut spinlock_t);
    safe fn spin_unlock(l: *mut spinlock_t);
    safe fn spin_init(l: *mut spinlock_t, name: *mut c_char);

    // proc/thread_queue.rs (100% Rust).
    safe fn tq_init(q: *mut tq_t, name: *const c_char, lock: *mut spinlock_t);
    safe fn tq_wait(q: *mut tq_t, lock: *mut spinlock_t, rdata: *mut u64) -> c_int;
    safe fn tq_bulk_move(to: *mut tq_t, from: *mut tq_t) -> c_int;
    safe fn tq_wakeup_all(q: *mut tq_t, error_no: c_int, rdata: u64) -> c_int;

    // kernel/bio.c (classic xv6 buffer cache, unchanged C).
    safe fn bread(dev: u32, blockno: u32) -> *mut buf;
    safe fn brelse(b: *mut buf);
    safe fn bwrite(b: *mut buf);
    safe fn bwrite_async(b: *mut buf);
    safe fn bsync();
    safe fn bpin(b: *mut buf);
    safe fn bunpin(b: *mut buf);
}

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
/// `log` must point to a live `xv6fs_log`.
unsafe fn __xv6fs_install_trans(log: *mut xv6fs_log, recovering: bool) {
    unsafe {
        let mut tail: i32 = 0;
        while tail < (*log).lh.n {
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
            tail += 1;
        }

        // Flush all async writes to disk.
        if !recovering && (*log).lh.n > 0 {
            bsync();
        }
    }
}

/// Mirrors `__xv6fs_read_head()`. Read the log header from disk into the
/// in-memory log header.
///
/// # Safety
/// `log` must point to a live `xv6fs_log`.
unsafe fn __xv6fs_read_head(log: *mut xv6fs_log) {
    unsafe {
        let b = bread((*log).dev as u32, (*log).start as u32);
        let lh = (*b).data as *const xv6fs_logheader;
        (*log).lh.n = (*lh).n;
        let mut i = 0usize;
        while (i as i32) < (*log).lh.n {
            (*log).lh.block[i] = (*lh).block[i];
            i += 1;
        }
        brelse(b);
    }
}

/// Mirrors `__xv6fs_write_head()`. Write in-memory log header to disk.
/// This is the true point at which the current transaction commits.
///
/// # Safety
/// `log` must point to a live `xv6fs_log`.
unsafe fn __xv6fs_write_head(log: *mut xv6fs_log) {
    unsafe {
        let b = bread((*log).dev as u32, (*log).start as u32);
        let hb = (*b).data as *mut xv6fs_logheader;
        (*hb).n = (*log).lh.n;
        let mut i = 0usize;
        while (i as i32) < (*log).lh.n {
            (*hb).block[i] = (*log).lh.block[i];
            i += 1;
        }
        bwrite(b);
        brelse(b);
    }
}

/// # Safety
/// `log` must point to a live `xv6fs_log`.
unsafe fn __xv6fs_recover_from_log(log: *mut xv6fs_log) {
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
/// `log` must point to a live `xv6fs_log`.
unsafe fn __xv6fs_write_log(log: *mut xv6fs_log) {
    unsafe {
        let mut tail: i32 = 0;
        while tail < (*log).lh.n {
            let to = bread((*log).dev as u32, ((*log).start + tail + 1) as u32); // log block
            let from = bread((*log).dev as u32, (*log).lh.block[tail as usize] as u32); // cache block
            memmove((*to).data as *mut c_void, (*from).data as *const c_void, super::BSIZE as usize);
            bwrite_async(to); // mark dirty, will flush at end
            brelse(from);
            brelse(to);
            tail += 1;
        }

        // Flush all log writes before writing header.
        if (*log).lh.n > 0 {
            bsync();
        }
    }
}

/// # Safety
/// `log` must point to a live `xv6fs_log`.
unsafe fn __xv6fs_commit(log: *mut xv6fs_log) {
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
#[no_mangle]
pub extern "C" fn xv6fs_initlog(xv6_sb: *mut xv6fs_superblock) {
    // SAFETY: `xv6_sb` is live (caller's contract, mount-time setup with
    // no concurrent access yet).
    unsafe {
        let log = ptr::addr_of_mut!((*xv6_sb).log);
        let disk_sb = ptr::addr_of!((*xv6_sb).disk_sb);

        if core::mem::size_of::<xv6fs_logheader>() >= super::BSIZE as usize {
            xv6_panic(c"xv6fs_initlog: too big logheader".as_ptr());
        }

        spin_init(ptr::addr_of_mut!((*log).lock), c"xv6fs_log".as_ptr() as *mut c_char);
        tq_init(ptr::addr_of_mut!((*log).wait_queue), c"xv6fs_log_wait".as_ptr(), ptr::addr_of_mut!((*log).lock));
        (*log).start = (*disk_sb).logstart as c_int;
        (*log).size = (*disk_sb).nlog as c_int;
        (*log).dev = xv6fs_sb_dev(xv6_sb) as c_int;
        (*log).outstanding = 0;
        (*log).committing = 0;
        (*log).lh.n = 0;

        __xv6fs_recover_from_log(log);
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
        let log = ptr::addr_of_mut!((*xv6_sb).log);
        let interruptible = state == THREAD_INTERRUPTIBLE;
        let current = xv6_current_thread();

        spin_lock(ptr::addr_of_mut!((*log).lock));
        loop {
            if (*log).committing != 0 {
                if interruptible && signal_pending(current) != 0 {
                    spin_unlock(ptr::addr_of_mut!((*log).lock));
                    return neg(crate::bindings::EINTR);
                }
                xv6_thread_state_set(current, state);
                let ret = tq_wait(ptr::addr_of_mut!((*log).wait_queue), ptr::addr_of_mut!((*log).lock), ptr::null_mut());
                if interruptible && ret != 0 && signal_pending(current) != 0 {
                    spin_unlock(ptr::addr_of_mut!((*log).lock));
                    return neg(crate::bindings::EINTR);
                }
            } else if (*log).lh.n + ((*log).outstanding + 1) * super::MAXOPBLOCKS > super::XV6FS_LOGSIZE {
                // This op might exhaust log space; wait for commit.
                if interruptible && signal_pending(current) != 0 {
                    spin_unlock(ptr::addr_of_mut!((*log).lock));
                    return neg(crate::bindings::EINTR);
                }
                xv6_thread_state_set(current, state);
                let ret = tq_wait(ptr::addr_of_mut!((*log).wait_queue), ptr::addr_of_mut!((*log).lock), ptr::null_mut());
                if interruptible && ret != 0 && signal_pending(current) != 0 {
                    spin_unlock(ptr::addr_of_mut!((*log).lock));
                    return neg(crate::bindings::EINTR);
                }
            } else {
                (*log).outstanding += 1;
                spin_unlock(ptr::addr_of_mut!((*log).lock));
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
#[no_mangle]
pub extern "C" fn xv6fs_begin_op(xv6_sb: *mut xv6fs_superblock) -> c_int {
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
#[no_mangle]
pub extern "C" fn xv6fs_begin_op_nointr(xv6_sb: *mut xv6fs_superblock) {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe { __xv6fs_begin_op(xv6_sb, THREAD_UNINTERRUPTIBLE) };
}

/// Called at the end of each FS operation. Commits if this was the last
/// outstanding operation.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
#[no_mangle]
pub extern "C" fn xv6fs_end_op(xv6_sb: *mut xv6fs_superblock) {
    // SAFETY: `xv6_sb` is live (caller's contract).
    unsafe {
        let log = ptr::addr_of_mut!((*xv6_sb).log);
        let mut do_commit = false;

        spin_lock(ptr::addr_of_mut!((*log).lock));
        (*log).outstanding -= 1;
        if (*log).committing != 0 {
            xv6_panic(c"xv6fs: log.committing".as_ptr());
        }
        if (*log).outstanding == 0 {
            do_commit = true;
            (*log).committing = 1;
        }
        spin_unlock(ptr::addr_of_mut!((*log).lock));

        if do_commit {
            __xv6fs_commit(log);

            // Collect waiters while holding lock, then wake outside lock
            // to avoid lock convoy (woken threads try to reacquire
            // log->lock) -- see the module doc.
            let mut temp_queue: tq_t = core::mem::zeroed();
            tq_init(ptr::addr_of_mut!(temp_queue), c"xv6fs_log_temp".as_ptr(), ptr::null_mut());

            spin_lock(ptr::addr_of_mut!((*log).lock));
            (*log).committing = 0;
            tq_bulk_move(ptr::addr_of_mut!(temp_queue), ptr::addr_of_mut!((*log).wait_queue));
            spin_unlock(ptr::addr_of_mut!((*log).lock));

            // Wake all outside the lock.
            if temp_queue.counter > 0 {
                tq_wakeup_all(ptr::addr_of_mut!(temp_queue), 0, 0);
            }
        }
    }
}

/// Record the block number for writing. Must be called between begin_op
/// and end_op.
///
/// Kept `#[no_mangle]`/exported per `xv6fs_private.h`'s `extern`
/// declaration.
#[no_mangle]
pub extern "C" fn xv6fs_log_write(xv6_sb: *mut xv6fs_superblock, b: *mut buf) {
    // SAFETY: `xv6_sb`/`b` are live (caller's contract: called between
    // begin_op/end_op).
    unsafe {
        let log = ptr::addr_of_mut!((*xv6_sb).log);

        spin_lock(ptr::addr_of_mut!((*log).lock));
        if (*log).lh.n >= super::XV6FS_LOGSIZE || (*log).lh.n >= (*log).size - 1 {
            xv6_panic(c"xv6fs: too big a transaction".as_ptr());
        }
        if (*log).outstanding < 1 {
            xv6_panic(c"xv6fs: log_write outside of trans".as_ptr());
        }

        let mut i: usize = 0;
        while (i as i32) < (*log).lh.n {
            if (*log).lh.block[i] == (*b).blockno as i32 {
                // Log absorption.
                break;
            }
            i += 1;
        }
        (*log).lh.block[i] = (*b).blockno as i32;
        if i as i32 == (*log).lh.n {
            // Add new block to log?
            bpin(b);
            (*log).lh.n += 1;
        }
        spin_unlock(ptr::addr_of_mut!((*log).lock));
    }
}
