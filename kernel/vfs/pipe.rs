//! Pipe implementation — Rust port of `kernel/vfs/pipe.c` (Phase 2
//! Wave 14, see `docs/rustify/phase2_plan.md`).
//!
//! Provides the pipe ring buffer (`pipe_alloc`/`pipe_close`,
//! `pipe_read`/`pipe_write`, `pipe_set_flags`/`pipe_get_flags`) and the
//! `vfs_file_ops` adapter (`pipe_open` installs it) that
//! [`super::file`] dispatches through. `kernel/vfs/file.c` (this
//! wave's sibling file) only calls the small public surface
//! (`pipe_alloc`/`pipe_open`/`pipe_close`); the ring-buffer internals
//! below are private to this file, same as the C original's `static`
//! functions.
//!
//! # The `nread`/`nwrite` visibility contract (read this before
//! touching this file)
//!
//! `pipe.nread`/`pipe.nwrite` are monotonically increasing byte
//! counters (`uint`, wrap at 2^32 by design — every size computation
//! here uses wrapping subtraction, matching C's well-defined unsigned
//! overflow). Each counter has exactly one *writer* (the reader thread
//! owns `nread`, the writer thread owns `nwrite`) and one *cross-hart
//! reader* (the writer peeks `nread` to compute free space, the reader
//! peeks `nwrite` to compute available data). Every peek across that
//! boundary goes through [`load_acquire_u32`]/[`store_release_u32`]
//! (`Acquire` load / `Release` store, matching the C `smp_load_acquire`/
//! `smp_store_release` macros exactly) — **this is a hard external
//! contract, not an internal implementation detail**: `kernel/tty/tty.rs`
//! (`load_acquire_u32`, `tty_poll`'s `nwrite - nread` check) and
//! `kernel/tty/pty.rs` already peek `(*pi).nread`/`(*pi).nwrite`
//! lock-free from outside this module (added in Wave 11, before this
//! module existed in Rust), relying on exactly this
//! Release/Acquire-paired visibility. This port preserves the C
//! original's field layout (`pipe` is a real bindgen type, not
//! redefined here) and the exact ordering discipline at every
//! `nread`/`nwrite` access site, so those pre-existing external readers
//! keep working unmodified. The one *within-lock* plain (non-atomic)
//! read at each side (`pipe_read` reads `pi->nread` directly while
//! holding `reader_lock`, `pipe_write` reads `pi->nwrite` directly
//! while holding `writer_lock`) is preserved 1:1 from the C — it is
//! sound because only the lock-holding side ever writes that counter,
//! matching the C original's identical reasoning.
//!
//! # Blocking / wakeup protocol
//!
//! This is the subtle part of the C original and is preserved exactly,
//! including its apparent lock-inversion: a reader blocked on empty
//! data waits on `nread_queue`, but that queue is protected by
//! `writer_lock`, not `reader_lock` ([`pipe_wait_writer`] acquires
//! `writer_lock` before checking conditions and sleeping) — symmetric
//! for a writer blocked on a full pipe ([`pipe_wait_reader`] acquires
//! `reader_lock`, sleeps on `nwrite_queue`). The point is: the queue a
//! thread parks on is protected by *the lock the other side already
//! holds while producing/consuming*, so the producer's own
//! `tq_wakeup_all` call (made while holding its own lock, e.g.
//! `pipe_write`'s `tq_wakeup_all(&pi->nread_queue, ...)` under
//! `writer_lock`) can never race a waiter registering on that same
//! queue. Both wait helpers additionally re-check the wake condition
//! immediately after acquiring their lock (documented inline as a
//! lost-wakeup fix already present in the C original — a producer can
//! slip in between the consumer's `spin_unlock` and the wait helper's
//! `spin_lock`), so this is not a naive condvar port.
//!
//! `pipe_read`/`pipe_write` use the exact-same `let mut g =
//! Some(lock.lock()); ...; g = None;` / re-`Some(lock.lock())` pattern
//! `kernel/lock/mutex.rs`'s `mutex_lock` uses for its own `tq_wait`
//! loop (`let _g = KSpinlock::from_bindings(...).lock(); ... tq_wait(...)
//! ...` — `tq_wait` itself releases/reacquires the passed spinlock
//! around the actual sleep; the guard only brackets the *outer*
//! lock/unlock pair at each of this function's several acquire points).
//!
//! # Style notes (rust-skills)
//!
//! Every `unsafe` block is scoped to the smallest expression that needs
//! it, with a `SAFETY:` comment at each non-obvious site. `bool` in the
//! headers this file binds against is the project's own `int`-sized
//! `typedef enum { false = 0, true = 1 } bool` (see `vfs/file.rs`'s
//! module doc for the full rationale) — every hand-declared extern
//! taking a C `bool` uses `c_int`, matching `kernel/tty/tty.rs`/
//! `kernel/tty/pty.rs`/`kernel/console.rs`'s pre-existing `pipe_read`/
//! `pipe_write`/`pipe_alloc`/`pipe_close`/`pipe_set_flags` externs —
//! this file's real definitions keep those exact signatures so the
//! already-landed Wave 11/12 callers link and behave unchanged. Where
//! this file *installs* a function into the bindgen `vfs_file_ops`
//! vtable (`PIPE_FILE_OPS`), the real bindgen-generated field type
//! (`bool_` = `c_uint`) is used instead, since that type is fixed by
//! the struct definition, not by this file's own convention.
//!
//! One pre-existing behavioral quirk found while porting (preserved
//! 1:1, not fixed — see the code comment at [`clear_writable`]):
//! `PIPE_CLEAR_WRITABLE`/`PIPE_CLEAR_READABLE` only report "safe to
//! free" when the flags word was *exactly* the single bit being
//! cleared, so if a non-blocking flag (`PIPE_FLAGS_NONBLOCK_RD`/`_WR`,
//! set by `kernel/console.rs`'s `pipe_set_flags` on TTY-owned pipes)
//! happens to be set, `pipe_close` on that pipe never frees its
//! backing buffer. A second, more clearly latent bug: on a
//! `vm_copyin`/first-chunk failure in `pipe_write`, the final `total -
//! (tmp_len - tmp_pos)` accounting can go negative even though nothing
//! was actually written yet (`total` has not been incremented for the
//! failed chunk, but `tmp_len` already reflects it) — both are
//! documented at their code sites and preserved exactly.

#![allow(non_camel_case_types, non_upper_case_globals, non_snake_case)]

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_short, c_void};
use core::mem::MaybeUninit;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use crate::bindings::{
    bool_, pipe, slab_cache_t, thread, thread_state_THREAD_INTERRUPTIBLE, tq_t, vfs_file,
    vfs_file_ops, vfs_inode, vm, EAGAIN, EINTR, SLAB_FLAG_STATIC,
};
use crate::proc::proc_shims::xv6_current_thread;
// P3-D2a: proc/thread_queue.rs primitives, reached as plain crate-path
// items instead of `extern "C"` redeclarations.
use crate::proc::{tq_init, tq_wait, tq_wakeup_all};
use crate::sync::KSpinlock;

// ===========================================================================
// Externs — every cross-module C-ABI symbol this file calls, declared
// locally per this crate's established convention (see `vfs/inode.rs`'s
// / `vfs/file.rs`'s externs-block docs).
// ===========================================================================

unsafe extern "C" {
    // proc module.
    safe fn killed(p: *mut thread) -> c_int;
    safe fn signal_pending(p: *mut thread) -> c_int;

    // lock/spinlock.rs — init only (lock/unlock go through
    // `crate::sync::KSpinlock` RAII, see the module doc).
    safe fn spin_init(l: *mut crate::bindings::spinlock_t, name: *mut c_char);

    // mm/vm.rs.
    safe fn vm_copyin(vm_ptr: *mut vm, dst: *mut c_void, srcva: u64, len: u64) -> c_int;
    safe fn vm_copyout(vm_ptr: *mut vm, dstva: u64, src: *const c_void, len: u64) -> c_int;

    // mm/kalloc.rs.
    safe fn kalloc() -> *mut c_void;
    safe fn kfree(pa: *mut c_void);

    // mm/slab.rs.
    safe fn slab_cache_init(
        cache: *mut slab_cache_t,
        name: *mut c_char,
        obj_size: usize,
        flags: u64,
    ) -> c_int;
    safe fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void;
    safe fn slab_free(obj: *mut c_void);
}

// `kassert!`'s canonical home is `crate::kstd`/crate root
// (P3-CS1 centralization). P3-CS14: `pipe_alloc` now builds a
// `KResult<*mut pipe>` internally and encodes it to the ABI-fixed `ERR_PTR`
// exactly once at the `extern "C"` boundary via `result_to_errptr`; only the
// error-return encoding changed (the `slab_free` unwind stays byte-identical).
use crate::kassert;
use crate::kstd::{result_to_errptr, Errno, KResult};

// ===========================================================================
// Small helpers.
// ===========================================================================

#[inline(always)]
const fn neg(e: u32) -> c_int {
    -(e as c_int)
}

// `uabi/fcntl.h`.
const O_RDONLY: c_int = 0o0;
const O_ACCMODE: c_int = 0o3 | 0o10000000; // `O_SEARCH` = `O_PATH`, matches `vfs/file.rs`.

// `vfs/pipe.h`'s `PIPE_FLAGS_*` bit indices and `PIPESIZE`.
const PIPE_FLAGS_READABLE: i32 = 1;
const PIPE_FLAGS_WRITABLE: i32 = 2;
const PIPE_FLAGS_NONBLOCK_RD: i32 = 3;
const PIPE_FLAGS_NONBLOCK_WR: i32 = 4;
const PIPE_FLAGS_RW: c_int = (1 << PIPE_FLAGS_READABLE) | (1 << PIPE_FLAGS_WRITABLE);
const PIPE_NONBLOCK_MASK: i32 = (1 << PIPE_FLAGS_NONBLOCK_RD) | (1 << PIPE_FLAGS_NONBLOCK_WR);
const PIPESIZE: usize = crate::bindings::PGSIZE as usize;

// `uabi/poll.h`.
const POLLIN: c_short = 0x0001;
const POLLOUT: c_short = 0x0004;
const POLLERR: c_short = 0x0008;
const POLLHUP: c_short = 0x0010;
const POLLNVAL: c_short = 0x0020;
const POLLRDNORM: c_short = 0x0040;
const POLLWRNORM: c_short = 0x0100;

/// `smp_load_acquire`/`smp_store_release` reinterpreted for the `uint`
/// (`u32`) pipe counters -- same-size signed/unsigned reinterpret, the
/// loaded bit pattern is identical either way, only the carrying type
/// differs. This is `vfs/pipe.rs`'s own copy of the identical helper
/// `kernel/tty/tty.rs` keeps `pub(super)` for its own module -- see this
/// file's module doc for why both modules independently peek the same
/// fields with the same ordering.
#[inline(always)]
fn load_acquire_u32(p: *const u32) -> u32 {
    crate::machine::smp_load_acquire_i32(p as *const c_int) as u32
}
#[inline(always)]
fn store_release_u32(p: *mut u32, v: u32) {
    crate::machine::smp_store_release_i32(p as *mut c_int, v as c_int)
}

/// # Safety
/// `pi` must point to a live, aligned `pipe`.
#[inline(always)]
fn flags_atomic<'a>(pi: *mut pipe) -> &'a AtomicI32 {
    // SAFETY: `flags` is a plain C `int` field, same size/align as
    // `AtomicI32`; caller ensures `pi` is a live, aligned `pipe`.
    unsafe { &*(ptr::addr_of_mut!((*pi).flags) as *const AtomicI32) }
}
#[inline(always)]
fn pipe_writable(pi: *mut pipe) -> bool {
    flags_atomic(pi).load(Ordering::Acquire) & (1 << PIPE_FLAGS_WRITABLE) != 0
}
#[inline(always)]
fn pipe_readable(pi: *mut pipe) -> bool {
    flags_atomic(pi).load(Ordering::Acquire) & (1 << PIPE_FLAGS_READABLE) != 0
}
#[inline(always)]
fn pipe_nonblock_rd(pi: *mut pipe) -> bool {
    flags_atomic(pi).load(Ordering::Acquire) & (1 << PIPE_FLAGS_NONBLOCK_RD) != 0
}
#[inline(always)]
fn pipe_nonblock_wr(pi: *mut pipe) -> bool {
    flags_atomic(pi).load(Ordering::Acquire) & (1 << PIPE_FLAGS_NONBLOCK_WR) != 0
}

/// Mirrors `PIPE_CLEAR_WRITABLE`. **Preserved 1:1 including a latent
/// quirk**: this only returns `true` (safe to free) when the flags word
/// was *exactly* `1 << PIPE_FLAGS_WRITABLE` before the clear -- i.e. if
/// any nonblock bit (`kernel/console.rs`'s `pipe_set_flags`, used on
/// TTY-owned pipes) happens to be set, this reports "not yet safe to
/// free" even after both readable and writable are actually clear,
/// leaking the pipe's backing page/slab object. Anonymous pipes from
/// `pipe()` never touch the nonblock bits, so this only affects
/// TTY-internal pipes, which are not expected to ever `pipe_close()` in
/// practice; flagged rather than fixed, matching this port's fidelity
/// mandate.
#[inline(always)]
fn clear_writable(pi: *mut pipe) -> bool {
    let bit = 1i32 << PIPE_FLAGS_WRITABLE;
    let old = flags_atomic(pi).fetch_and(!bit, Ordering::SeqCst);
    old == bit
}
/// Mirrors `PIPE_CLEAR_READABLE` -- see [`clear_writable`].
#[inline(always)]
fn clear_readable(pi: *mut pipe) -> bool {
    let bit = 1i32 << PIPE_FLAGS_READABLE;
    let old = flags_atomic(pi).fetch_and(!bit, Ordering::SeqCst);
    old == bit
}

fn reader_lock(pi: *mut pipe) -> KSpinlock {
    // SAFETY: `pi` is a live pipe (every call site's precondition).
    KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*pi).reader_lock) })
}
fn writer_lock(pi: *mut pipe) -> KSpinlock {
    // SAFETY: `pi` is a live pipe (every call site's precondition).
    KSpinlock::from_bindings(unsafe { ptr::addr_of_mut!((*pi).writer_lock) })
}

// ===========================================================================
// Pipe cache + init/alloc/close/flags.
// ===========================================================================

#[repr(transparent)]
struct PipeSlabCell(UnsafeCell<MaybeUninit<slab_cache_t>>);
// SAFETY: written in full by `slab_cache_init` (called once from
// `pipe_init`, before any `slab_alloc`/`slab_free` on this cache) and
// otherwise only touched through the C slab allocator's own
// internally-synchronized entry points -- same precedent as
// `vfs/file.rs`'s `FileSlabCell`.
unsafe impl Sync for PipeSlabCell {}
static __PIPE_CACHE: PipeSlabCell = PipeSlabCell(UnsafeCell::new(MaybeUninit::zeroed()));
#[inline]
fn pipe_slab() -> *mut slab_cache_t {
    __PIPE_CACHE.0.get() as *mut slab_cache_t
}

pub(crate) extern "C" fn pipe_init() {
    let ret = slab_cache_init(
        pipe_slab(),
        c"pipe_cache".as_ptr() as *mut c_char,
        core::mem::size_of::<pipe>(),
        SLAB_FLAG_STATIC as u64,
    );
    kassert!(ret == 0, "Failed to initialize pipe_cache slab cache");
}

pub(crate) extern "C" fn pipe_alloc(flags: c_int) -> *mut pipe {
    result_to_errptr(pipe_alloc_inner(flags))
}

fn pipe_alloc_inner(flags: c_int) -> KResult<*mut pipe> {
    let pi = slab_alloc(pipe_slab()) as *mut pipe;
    if pi.is_null() {
        return Err(Errno::NoMem);
    }

    let data = kalloc();
    if data.is_null() {
        slab_free(pi as *mut c_void);
        return Err(Errno::NoMem);
    }

    // SAFETY: `pi` was just slab-allocated and is exclusively owned by
    // this call; no other thread can observe it until this function
    // returns it to its caller.
    unsafe {
        crate::machine::smp_store_release_i32(ptr::addr_of_mut!((*pi).flags), PIPE_FLAGS_RW | flags);
        (*pi).nwrite = 0;
        (*pi).nread = 0;
        (*pi).data = data as *mut c_char;
        spin_init(
            ptr::addr_of_mut!((*pi).reader_lock),
            c"vfs_pipe_reader".as_ptr() as *mut c_char,
        );
        spin_init(
            ptr::addr_of_mut!((*pi).writer_lock),
            c"vfs_pipe_writer".as_ptr() as *mut c_char,
        );
        tq_init(
            ptr::addr_of_mut!((*pi).nread_queue),
            c"pipe_nread_queue".as_ptr(),
            ptr::null_mut(),
        );
        tq_init(
            ptr::addr_of_mut!((*pi).nwrite_queue),
            c"pipe_nwrite_queue".as_ptr(),
            ptr::null_mut(),
        );
    }
    Ok(pi)
}

pub(crate) extern "C" fn pipe_close(pi: *mut pipe, writable: c_int) {
    let freed;
    if writable != 0 {
        let g = writer_lock(pi).lock();
        freed = clear_writable(pi);
        // SAFETY: non-null `pi`.
        tq_wakeup_all(unsafe { ptr::addr_of_mut!((*pi).nread_queue) }, -1, 0);
        drop(g);
    } else {
        let g = reader_lock(pi).lock();
        freed = clear_readable(pi);
        // SAFETY: non-null `pi`.
        tq_wakeup_all(unsafe { ptr::addr_of_mut!((*pi).nwrite_queue) }, -1, 0);
        drop(g);
    }
    if freed {
        // SAFETY: `pi->data` was allocated by `kalloc()` in `pipe_alloc`
        // and is being freed exactly once (the both-clear check above
        // guards against a double free between the two closer sides).
        unsafe { kfree((*pi).data as *mut c_void) };
        slab_free(pi as *mut c_void);
    }
}

pub(crate) extern "C" fn pipe_set_flags(pi: *mut pipe, flags: c_int) {
    let set = flags & PIPE_NONBLOCK_MASK;
    let a = flags_atomic(pi);
    let mut old = a.load(Ordering::Acquire);
    loop {
        let new = (old & !PIPE_NONBLOCK_MASK) | set;
        match a.compare_exchange(old, new, Ordering::SeqCst, Ordering::SeqCst) {
            Ok(_) => return,
            Err(cur) => old = cur,
        }
    }
}

pub(crate) extern "C" fn pipe_get_flags(pi: *mut pipe) -> c_int {
    flags_atomic(pi).load(Ordering::Acquire) & PIPE_NONBLOCK_MASK
}

// ===========================================================================
// Wait helpers -- see the module doc's "Blocking / wakeup protocol"
// section before changing anything here.
// ===========================================================================

/// Mirrors `__pipe_wait_writer`: a *reader* blocked on an empty pipe
/// waits here, on `nread_queue`, protected by `writer_lock` (not
/// `reader_lock`) -- intentional, see the module doc.
fn pipe_wait_writer(pi: *mut pipe) -> c_int {
    let cur = xv6_current_thread();
    let g = writer_lock(pi).lock();
    if !pipe_writable(pi) || killed(cur) != 0 {
        drop(g);
        // Return 0 to let the caller re-check and detect EOF properly.
        return 0;
    }
    // Re-check data availability after the reader->writer lock
    // transition: a writer may have slipped in during the gap between
    // `pipe_read`'s `spin_unlock(&pi->reader_lock)` and this function's
    // `spin_lock(&pi->writer_lock)`, written data, woken `nread_queue`
    // (nobody on it yet), and left. Without this re-check the reader
    // would sleep even though data is already available -- a classic
    // lost wakeup.
    // SAFETY: non-null `pi`.
    if load_acquire_u32(unsafe { ptr::addr_of!((*pi).nwrite) })
        != load_acquire_u32(unsafe { ptr::addr_of!((*pi).nread) })
    {
        drop(g);
        return 0; // Data available; caller will re-check under reader_lock.
    }
    crate::machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
    // SAFETY: non-null `pi`; `g` is held here, matching `tq_wait`'s
    // "release the passed lock before sleeping, reacquire before
    // returning" contract (same idiom as `kernel/lock/mutex.rs`'s
    // `mutex_lock`, see the module doc).
    unsafe {
        tq_wait(
            ptr::addr_of_mut!((*pi).nread_queue),
            ptr::addr_of_mut!((*pi).writer_lock),
            ptr::null_mut(),
        )
    };
    drop(g);
    if signal_pending(cur) != 0 {
        return neg(EINTR);
    }
    // Return 0 to re-check conditions (the wakeup may be from close or
    // from new data).
    0
}

/// Mirrors `__pipe_wait_reader` -- symmetric to [`pipe_wait_writer`], see
/// the module doc.
fn pipe_wait_reader(pi: *mut pipe) -> c_int {
    let cur = xv6_current_thread();
    let g = reader_lock(pi).lock();
    if !pipe_readable(pi) || killed(cur) != 0 {
        drop(g);
        return 0;
    }
    // Re-check space availability after the writer->reader lock
    // transition -- same lost-wakeup avoidance as `pipe_wait_writer`.
    // SAFETY: non-null `pi`.
    let nwrite = load_acquire_u32(unsafe { ptr::addr_of!((*pi).nwrite) });
    let nread = load_acquire_u32(unsafe { ptr::addr_of!((*pi).nread) });
    if PIPESIZE - nwrite.wrapping_sub(nread) as usize > 0 {
        drop(g);
        return 0; // Space available; caller will re-check under writer_lock.
    }
    crate::machine::thread_state_set(cur, thread_state_THREAD_INTERRUPTIBLE);
    // SAFETY: see `pipe_wait_writer`.
    unsafe {
        tq_wait(
            ptr::addr_of_mut!((*pi).nwrite_queue),
            ptr::addr_of_mut!((*pi).reader_lock),
            ptr::null_mut(),
        )
    };
    drop(g);
    if signal_pending(cur) != 0 {
        return neg(EINTR);
    }
    0
}

// ===========================================================================
// Standalone pipe read/write -- the core pipe I/O routines. They
// operate on a bare `struct pipe *` and know nothing about VFS file
// structures. `user` selects user-space vs kernel-space buffer
// handling.
// ===========================================================================

pub(crate) extern "C" fn pipe_read(pi: *mut pipe, buf: *mut c_char, count: u64, user: c_int) -> i64 {
    let count = count as usize;
    let pr = xv6_current_thread();
    let mut total: usize = 0;
    let mut tmp: [u8; 128] = [0; 128];
    let mut tmp_pos: usize = 0;
    let mut tmp_len: usize = 0;
    let nonblock = pipe_nonblock_rd(pi);

    'out: {
        while total < count {
            let mut g = Some(reader_lock(pi).lock());
            while tmp_len == 0 {
                // SAFETY: non-null `pi`.
                let nwrite = load_acquire_u32(unsafe { ptr::addr_of!((*pi).nwrite) });
                // SAFETY: non-null `pi`; `nread` is exclusively owned by
                // the reader side while `reader_lock` is held (only the
                // reader ever writes it), matching the C original's
                // plain (non-atomic) read here -- see the module doc.
                let nread_old = unsafe { (*pi).nread };
                let readable = nwrite.wrapping_sub(nread_old) as usize;
                if readable == 0 {
                    if !pipe_writable(pi) {
                        // Writer closed and no data left: EOF.
                        g = None;
                        break 'out;
                    }
                    if killed(pr) != 0 {
                        g = None;
                        return -1;
                    }
                    // POSIX short-read: if data was already copied to
                    // the caller, return it now instead of blocking for
                    // more. Essential for interactive/streaming
                    // consumers (terminals, telnet, etc.).
                    if total > 0 {
                        g = None;
                        break 'out;
                    }
                    if nonblock {
                        g = None;
                        return neg(EAGAIN) as i64;
                    }
                    // SAFETY: non-null `pi`.
                    tq_wakeup_all(unsafe { ptr::addr_of_mut!((*pi).nwrite_queue) }, 0, 0);
                    g = None;

                    let ret = pipe_wait_writer(pi);
                    if ret < 0 {
                        if total > 0 {
                            break 'out;
                        }
                        return ret as i64;
                    }
                    // Return 0 to re-check conditions (wakeup may be
                    // from close or data).
                    g = Some(reader_lock(pi).lock());
                } else {
                    let read_size = (count - total).min(readable).min(tmp.len());
                    let nread = nread_old.wrapping_add(read_size as u32);
                    let nread_idx = (nread_old as usize) % PIPESIZE;

                    // SAFETY: `pi->data` is a live `PIPESIZE`-byte ring
                    // buffer owned by this pipe; `nread_idx`/`read_size`
                    // stay in bounds because `readable` (derived from
                    // `nwrite - nread`) never exceeds `PIPESIZE`.
                    unsafe {
                        let data = (*pi).data as *const u8;
                        if nread_idx + read_size <= PIPESIZE {
                            ptr::copy_nonoverlapping(data.add(nread_idx), tmp.as_mut_ptr(), read_size);
                        } else {
                            let first_part = PIPESIZE - nread_idx;
                            ptr::copy_nonoverlapping(data.add(nread_idx), tmp.as_mut_ptr(), first_part);
                            ptr::copy_nonoverlapping(
                                data,
                                tmp.as_mut_ptr().add(first_part),
                                read_size - first_part,
                            );
                        }
                    }
                    // SAFETY: non-null `pi`.
                    store_release_u32(unsafe { ptr::addr_of_mut!((*pi).nread) }, nread);
                    tmp_len = read_size;
                }
            }
            g = None;

            // Copy data out (user or kernel).
            let copy_size = (tmp_len - tmp_pos).min(count - total);
            if user != 0 {
                // SAFETY: `pr` is the live current thread; `buf + total`
                // is the caller-supplied user-space destination -- the
                // caller (a syscall path) is responsible for its
                // validity, matching every other `user`-gated copy in
                // this crate.
                let ret = unsafe {
                    vm_copyout(
                        (*pr).vm,
                        (buf as u64).wrapping_add(total as u64),
                        tmp.as_ptr().add(tmp_pos) as *const c_void,
                        copy_size as u64,
                    )
                };
                if ret < 0 {
                    break 'out;
                }
            } else {
                // SAFETY: `buf` is a valid kernel destination of at
                // least `count` bytes (caller contract when `user ==
                // false`, matching every kernel-space pipe caller in
                // the tree, e.g. `kernel/tty/pty.rs`).
                unsafe {
                    ptr::copy_nonoverlapping(
                        tmp.as_ptr().add(tmp_pos),
                        (buf as *mut u8).add(total),
                        copy_size,
                    );
                }
            }
            total += copy_size;
            tmp_pos += copy_size;
            if tmp_pos >= tmp_len {
                tmp_pos = 0;
                tmp_len = 0;
            }
        }
    }

    {
        let _g = reader_lock(pi).lock();
        // SAFETY: non-null `pi`.
        tq_wakeup_all(unsafe { ptr::addr_of_mut!((*pi).nwrite_queue) }, 0, 0);
    }
    total as i64
}

pub(crate) extern "C" fn pipe_write(pi: *mut pipe, buf: *const c_char, count: u64, user: c_int) -> i64 {
    let count = count as usize;
    let pr = xv6_current_thread();
    let mut total: usize = 0;
    let mut tmp: [u8; 128] = [0; 128];
    let mut tmp_pos: usize = 0;
    let mut tmp_len: usize = 0;
    let nonblock = pipe_nonblock_wr(pi);

    'out: {
        while total < count {
            if tmp_len == 0 {
                tmp_len = (count - total).min(tmp.len());
                if tmp_len == 0 {
                    break 'out;
                }
                if user != 0 {
                    // SAFETY: see `pipe_read`'s identical `vm_copyout`
                    // rationale.
                    let ret = unsafe {
                        vm_copyin(
                            (*pr).vm,
                            tmp.as_mut_ptr() as *mut c_void,
                            (buf as u64).wrapping_add(total as u64),
                            tmp_len as u64,
                        )
                    };
                    if ret < 0 {
                        // NOTE (preserved 1:1 from the C original): at
                        // this point `total` has NOT yet been
                        // incremented by `tmp_len` (that happens right
                        // below, unconditionally, once we fall through
                        // this block) -- but `tmp_len` already holds the
                        // size of this never-attempted chunk and
                        // `tmp_pos` is still 0. The shared tail below
                        // computes `total - (tmp_len - tmp_pos)`, which
                        // on this exact path subtracts a chunk that was
                        // never added to `total`, and can return a
                        // negative count that is neither the number of
                        // bytes written nor a valid negative errno. This
                        // is a latent bug in the C original (see the
                        // module doc); reproduced exactly rather than
                        // fixed.
                        break 'out;
                    }
                } else {
                    // SAFETY: `buf` is a valid kernel source of at
                    // least `count` bytes (caller contract when `user
                    // == false`).
                    unsafe {
                        ptr::copy_nonoverlapping((buf as *const u8).add(total), tmp.as_mut_ptr(), tmp_len);
                    }
                }
            }
            total += tmp_len;
            let mut g = Some(writer_lock(pi).lock());
            while tmp_len > tmp_pos {
                // SAFETY: non-null `pi`.
                let nread = load_acquire_u32(unsafe { ptr::addr_of!((*pi).nread) });
                if !pipe_readable(pi) || killed(pr) != 0 {
                    g = None;
                    return -1;
                }
                // SAFETY: non-null `pi`; `nwrite` is exclusively owned
                // by the writer side while `writer_lock` is held --
                // see `pipe_read`'s identical `nread` rationale.
                let nwrite_old = unsafe { (*pi).nwrite };
                let writable = PIPESIZE - nwrite_old.wrapping_sub(nread) as usize;
                if writable == 0 {
                    if nonblock {
                        g = None;
                        if total > tmp_len {
                            break 'out;
                        }
                        return neg(EAGAIN) as i64;
                    }
                    // SAFETY: non-null `pi`.
                    tq_wakeup_all(unsafe { ptr::addr_of_mut!((*pi).nread_queue) }, 0, 0);
                    g = None;

                    let ret = pipe_wait_reader(pi);
                    if ret < 0 {
                        if total > tmp_len {
                            break 'out;
                        }
                        return ret as i64;
                    }
                    g = Some(writer_lock(pi).lock());
                } else {
                    let write_size = (tmp_len - tmp_pos).min(writable);
                    let nwrite = nwrite_old.wrapping_add(write_size as u32);
                    let nwrite_idx = (nwrite_old as usize) % PIPESIZE;

                    // SAFETY: see `pipe_read`'s identical ring-buffer
                    // bounds rationale (`writable` never exceeds
                    // `PIPESIZE`).
                    unsafe {
                        let data = (*pi).data as *mut u8;
                        if nwrite_idx + write_size <= PIPESIZE {
                            ptr::copy_nonoverlapping(tmp.as_ptr().add(tmp_pos), data.add(nwrite_idx), write_size);
                        } else {
                            let first_part = PIPESIZE - nwrite_idx;
                            ptr::copy_nonoverlapping(tmp.as_ptr().add(tmp_pos), data.add(nwrite_idx), first_part);
                            ptr::copy_nonoverlapping(
                                tmp.as_ptr().add(tmp_pos + first_part),
                                data,
                                write_size - first_part,
                            );
                        }
                    }
                    // SAFETY: non-null `pi`.
                    store_release_u32(unsafe { ptr::addr_of_mut!((*pi).nwrite) }, nwrite);
                    tmp_pos += write_size;
                }
            }
            g = None;
            tmp_pos = 0;
            tmp_len = 0;
        }
    }

    {
        let _g = writer_lock(pi).lock();
        // SAFETY: non-null `pi`.
        tq_wakeup_all(unsafe { ptr::addr_of_mut!((*pi).nread_queue) }, 0, 0);
    }
    (total as i64) - (tmp_len as i64 - tmp_pos as i64)
}

// ===========================================================================
// VFS file_ops adapters -- thin wrappers extracting `struct pipe *`
// from the VFS file and delegating to the standalone pipe_read/
// pipe_write above.
// ===========================================================================

extern "C" fn pipe_file_read(file: *mut vfs_file, buf: *mut c_char, count: usize, user: bool_) -> isize {
    // SAFETY: non-null `file` (only reachable via `PIPE_FILE_OPS`
    // dispatch from `vfs/file.rs`, which already validated `file`).
    let pi = unsafe { (*file).__bindgen_anon_1.pipe };
    pipe_read(pi, buf, count as u64, user as c_int) as isize
}

extern "C" fn pipe_file_write(
    file: *mut vfs_file,
    buf: *const c_char,
    count: usize,
    user: bool_,
) -> isize {
    // SAFETY: see `pipe_file_read`.
    let pi = unsafe { (*file).__bindgen_anon_1.pipe };
    pipe_write(pi, buf, count as u64, user as c_int) as isize
}

extern "C" fn pipe_file_release(_inode: *mut vfs_inode, file: *mut vfs_file) -> c_int {
    // Pipes have no inode.
    // SAFETY: non-null `file`.
    let pi = unsafe { (*file).__bindgen_anon_1.pipe };
    if !pi.is_null() {
        // SAFETY: non-null `file`.
        let writable = (unsafe { (*file).f_flags } & O_ACCMODE) != O_RDONLY;
        pipe_close(pi, writable as c_int);
        // SAFETY: non-null `file`.
        unsafe { (*file).__bindgen_anon_1.pipe = ptr::null_mut() };
    }
    0
}

/// Mirrors `__pipe_file_poll`: check pipe readiness for I/O without
/// blocking. Returns the subset of `events` that are ready.
extern "C" fn pipe_file_poll(file: *mut vfs_file, events: c_short) -> c_int {
    // SAFETY: non-null `file`.
    let pi = unsafe { (*file).__bindgen_anon_1.pipe };
    if pi.is_null() {
        return POLLNVAL as c_int;
    }

    // SAFETY: non-null `pi`.
    let nwrite = load_acquire_u32(unsafe { ptr::addr_of!((*pi).nwrite) });
    let nread = load_acquire_u32(unsafe { ptr::addr_of!((*pi).nread) });
    let readable = nwrite.wrapping_sub(nread) as usize;
    let writable = PIPESIZE - readable;

    let mut revents: c_short = 0;
    if events & (POLLIN | POLLRDNORM) != 0 && (readable > 0 || !pipe_writable(pi)) {
        revents |= events & (POLLIN | POLLRDNORM);
    }
    if events & (POLLOUT | POLLWRNORM) != 0 && (writable > 0 && pipe_readable(pi)) {
        revents |= events & (POLLOUT | POLLWRNORM);
    }
    if !pipe_readable(pi) {
        revents |= POLLERR;
    }
    if !pipe_writable(pi) {
        revents |= POLLHUP;
    }
    revents as c_int
}

static PIPE_FILE_OPS: vfs_file_ops = vfs_file_ops {
    read: Some(pipe_file_read),
    write: Some(pipe_file_write),
    llseek: None,
    release: Some(pipe_file_release),
    fsync: None,
    fflush: None,
    poll: Some(pipe_file_poll),
    ioctl: None,
    fault: None,
};

pub(crate) extern "C" fn pipe_open(file: *mut vfs_file, pi: *mut pipe, f_flags: c_int) {
    // SAFETY: `file` is a freshly allocated `vfs_file` (every caller's
    // precondition, e.g. `vfs_pipealloc` in `vfs/file.rs`).
    unsafe {
        (*file).f_flags = f_flags;
        (*file).__bindgen_anon_1.pipe = pi;
        (*file).ops = ptr::addr_of!(PIPE_FILE_OPS) as *mut vfs_file_ops;
    }
}

