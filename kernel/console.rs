//! Console input and output to UART.
//!
//! Rust port of `kernel/console.c` (Phase 2 Wave 4, see
//! `docs/rustify/phase2_plan.md`). Routes writes through the TTY layer
//! for line discipline, signal generation (Ctrl+C -> SIGINT), and
//! termios support; falls back to SBI for early boot output before UART
//! init, and to a raw ring buffer for input before the TTY is
//! allocated. Converts `\n` to `\r\n` for proper terminal display.
//!
//! # MMIO
//!
//! None directly — all hardware access goes through `crate::uart`
//! (`uartputc_sync`/`uartputs_nb`/`uart_tx_wait`) or `crate::sbi`
//! (`sbi_console_putchar`/`sbi_console_getchar`). The Wave 4 plan's "4
//! volatile sites in console" count refers to this file's C original
//! having none of its own — the 4 sites are `uart.rs`'s 2 (shared
//! `read_reg`/`write_reg` helpers) reached indirectly from here via
//! `consputc`/`consolewrite`, i.e. this file adds no *new* volatile
//! surface; it is pure dispatch/line-discipline logic on top of
//! `uart.rs`'s MMIO layer.
//!
//! # Locking / concurrency
//!
//! * `CONS_LOCK` (`cons` in the C original) guards the raw fallback
//!   input ring buffer used before the TTY is allocated.
//! * `console_tty` (here: `CONSOLE_TTY`, an `AtomicPtr`) is written
//!   exactly once, late in `consoledevinit()`, and only ever read
//!   afterward. The C original used a plain (non-atomic) pointer for
//!   this — including from `consoleintr()`, which can run in interrupt
//!   context concurrently with `consoledevinit()`'s IRQ-handler
//!   registration (the C code registers the IRQ *before* setting
//!   `console_tty`, so a UART interrupt landing in that narrow window
//!   sees an unset TTY, which the fallback path already handles
//!   correctly). `AtomicPtr` with `Relaxed` reproduces the exact same
//!   observable behaviour while removing the data race that the C
//!   pointer read/write technically was.
//! * `TTY_INBUF_W`/`TTY_INBUF_R` mirror the C `__ATOMIC_RELEASE`
//!   (producer, `consoleintr`) / `__ATOMIC_ACQUIRE` (consumer,
//!   `console_tty_input_thread`) orderings exactly — this is a
//!   single-producer/single-consumer ring buffer publishing byte
//!   arrival across the interrupt/thread boundary.
//! * `consoleintr` never sleeps (interrupt context): the TTY path only
//!   stages a byte into the lock-free `tty_inbuf` ring; a dedicated
//!   kernel thread (`console_tty_input_thread`) drains it into
//!   `tty_input()`, which may sleep (e.g. `pipe_write`).

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_short, c_void};
use core::mem::MaybeUninit;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, Ordering};

use crate::bindings::{
    cdev_t, device_t, mode_t, pipe, session, tty,
};
// P3-1D mesh sweep: dev/cdev.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration. P3-10c: `CdevOps` is the ops-table trait
// this file's `ConsoleCdevOps` implements.
use crate::dev::cdev::{Cdev, CdevOps};
use crate::kstd::{Errno, KResult};
// P3-D2a: proc/sched.rs sleep/wake entry points, reached as plain
// crate-path items instead of `extern "C"` redeclarations. `sleep` is
// now reached through `SpinLockGuard::sleep_on` (lock-owns-data), so
// `sleep_on_chan` is no longer imported directly here.
use crate::proc::Scheduler;
// N-R7 (lock-owns-data): the console's raw-fallback input buffer + its
// lock are now a single data-owning `SpinLock<ConsInner>` (was a bare
// `static mut spinlock_t` beside four `static mut` state fields).
use crate::sync::SpinLock;

// ===========================================================================
// Externs.
// ===========================================================================

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

// N-R7h: the raw `spin_lock`/`spin_unlock` facade wrappers that used to
// sit here were driven by exactly one site -- `consolewrite`'s manual
// lock of the console tty's `lock` to read `termios`. That tty is now a
// lock-owns-data `SpinLock<TtyInner>` (`kernel/tty/tty.rs`), so the site
// takes `(*tty_ptr).inner.lock()` (an RAII guard) instead, and these
// wrappers -- along with their `spinlock_t` import -- are retired.

// P3-D3b: `kthread_create` (proc/thread.rs) is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone; reached via the `crate::proc`
// glob re-export.
// P3-D3c: `timer/sched_timer.rs`'s `sleep_ms` is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone -- crate-path import.
use crate::timer::sched_timer::SchedTimer;

// P3-D3a: `either_copyin`/`either_copyout` (mm/vm.rs) are ordinary (safe)
// Rust fns now that their `#[no_mangle]` exports are gone; reached as
// crate-path items instead of the `extern "C"` redeclarations that used
// to sit in the block above (identical signatures).
use crate::mm::{either_copyin, either_copyout};

// P3-D2b: the proc-object entry points (proc/{signal,pid}.rs) are plain
// crate-path items now that their `#[no_mangle]` exports are gone
// (identical signatures).
use crate::proc::ProcTable;

/// `struct irq_desc`/`register_irq_handler`/`PLIC_IRQ_OFFSET` now live in
/// `kernel/irq/irq_core.rs` (Phase 2 Wave 5 — irq/irq.c port). Previously
/// hand-declared locally here (`trap.h` still isn't in `wrapper.h`, same
/// bindgen-coverage rationale as before) with a placeholder `rcu_head_next:
/// *mut c_void` standing in for the trailing `rcu_head_t` field — that
/// placeholder was undersized (a real `rcu_head_t` is 40 bytes, not 8),
/// which meant the C-side `*desc = *in_desc` copy in `register_irq_handler`
/// technically over-read past this file's on-stack `IrqDesc` by 32 bytes
/// (never observed as a real bug: `__alloc_irq_desc` immediately
/// zero-overwrites `rcu_head` after the copy). `irq_core::IrqDesc` uses the
/// real bindgen `rcu_head`/`device_t` types, so it's byte-for-byte
/// layout-compatible with the C struct and the over-read is gone.
use crate::irq::irq_core::{IrqCore, IrqDesc, PLIC_IRQ_OFFSET};

// P3-1C mesh sweep: tty/tty.rs and vfs/pipe.rs are in scope for this wave,
// so these become plain crate-path imports instead of `extern "C"`
// redeclarations (identical signatures). ABI-truth note: `tty_alloc`/
// `tty_read`/`tty_ioctl`/`tty_poll`/`tty_input`/`tty_output` are real
// `unsafe extern "C" fn`s -- this file's former mirror declared them
// `safe`, but every call site here already sits inside a broader
// `unsafe { }` block (verified per call site), so no behavior changes.
use crate::tty::tty::Tty;
use crate::tty::session::{Session, SessionTable};
use crate::vfs::pipe::Pipe;

const EINTR: c_int = 4;
const EFAULT: c_int = 14;
const ENOTTY: c_int = 25;

/// `S_IFCHR` (`kernel/inc/uabi/stat.h`).
const S_IFCHR: u32 = 0o020_000;

const CONSOLE_MAJOR: c_int = 1;
const CONSOLE_MINOR: c_int = 1;

const BACKSPACE: c_int = 0x100;

/// `C(x)` macro: `Control-x`.
#[inline(always)]
const fn ctrl(x: u8) -> c_int {
    (x as c_int) - ('@' as c_int)
}

// ===========================================================================
// Panic helper — replicates the C `assert(expr, fmt, arg)` macro
// expansion (see `kernel/sbi.rs::required_extension_missing` for the
// established pattern this mirrors).
// ===========================================================================

unsafe extern "C" {
}

// P3-2 (zero-C wave): `console_assert_errno`/`console_assert_plain` used to
// take their assertion message as a runtime `&CStr` (with the errno variant
// embedding a `%d` inside that string) -- `core::fmt`'s `format_args!` (and
// therefore `kprintln!`/`kprint!`) requires a literal format string, so a
// generic "assert with a caller-chosen dynamic format" helper has no
// equivalent. Both had exactly 3 call sites total (all in
// `consoledevinit()` below), so they were inlined at each call site instead
// of preserved as a shared helper -- see the `macro-rules-hygiene`/
// `own-*` rust-skills guidance against over-abstracting a 3-call-site
// pattern behind a signature that can no longer be generic over its
// message.

// `is_err_or_null`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::is_err_or_null;

// ===========================================================================
// State.
// ===========================================================================

/// Flag to track if UART has been initialized. Before UART init, we use
/// SBI for console output.
static UART_INITIALIZED: AtomicBool = AtomicBool::new(false);

/// The console TTY -- allocated during `consoledevinit()`. Before this
/// is set, `consoleintr()` falls back to the raw buffer. See the
/// module-level doc comment for the race analysis this `AtomicPtr`
/// replaces (a plain pointer in the C original).
static CONSOLE_TTY: AtomicPtr<tty> = AtomicPtr::new(core::ptr::null_mut());

#[inline(always)]
fn console_tty() -> *mut tty {
    CONSOLE_TTY.load(Ordering::Relaxed)
}

// ===========================================================================
// consputc / consputs -- send character(s) to the console.
// ===========================================================================

/// Send one character to the uart. Called by printf(), and to echo
/// input characters, but not from write().
pub(crate) extern "C" fn consputc(c: c_int) {
    if !UART_INITIALIZED.load(Ordering::Relaxed) {
        // Use SBI for early console output before UART is ready.
        if c == BACKSPACE {
            crate::sbi::Sbi::console_putchar('\u{8}' as c_int);
            crate::sbi::Sbi::console_putchar(' ' as c_int);
            crate::sbi::Sbi::console_putchar('\u{8}' as c_int);
        } else {
            // Convert \n to \r\n for proper terminal output.
            if c == '\n' as c_int {
                crate::sbi::Sbi::console_putchar('\r' as c_int);
            }
            crate::sbi::Sbi::console_putchar(c);
        }
        return;
    }

    // Use synchronous UART output (safe for interrupt context).
    if c == BACKSPACE {
        // If the user typed backspace, overwrite with a space.
        crate::uart::Uart::uartputc_sync('\u{8}' as c_int);
        crate::uart::Uart::uartputc_sync(' ' as c_int);
        crate::uart::Uart::uartputc_sync('\u{8}' as c_int);
    } else {
        if c == '\n' as c_int {
            crate::uart::Uart::uartputc_sync('\r' as c_int);
        }
        crate::uart::Uart::uartputc_sync(c);
    }
}

/// Send a string to the console. For use by `puts()` and optimized
/// printing. Reuses [`consputc`]'s per-character logic verbatim
/// (`UART_INITIALIZED` is read-mostly after boot, so re-checking it
/// per byte costs nothing observable and keeps this a single code
/// path instead of the C original's two near-duplicate loops).
///
/// # Safety
/// `s` must point to at least `n` readable bytes.
pub(crate) unsafe extern "C" fn consputs(s: *const c_char, n: c_int) {
    // N-METH (goal #2): the raw `for i in 0..n { *s.offset(i) }` pointer
    // walk became one `from_raw_parts` + slice iteration -- `n` per-byte
    // raw derefs collapse to a single up-front `unsafe`, mirroring the
    // tty.rs `tty_input` conversion in the N-METH model (da51c6c).
    // SAFETY: caller guarantees `s` has at least `n` readable bytes (fn
    // doc); `n.max(0)` reproduces the old loop's empty range for `n <= 0`.
    let bytes = unsafe { core::slice::from_raw_parts(s as *const u8, n.max(0) as usize) };
    for &b in bytes {
        consputc(b as c_int);
    }
}

// ===========================================================================
// Raw fallback input buffer (used before the TTY is allocated).
// ===========================================================================

const INPUT_BUF_SIZE: usize = 128;

/// The raw fallback input ring buffer and its indices (the C `cons`
/// struct's `buf`/`r`/`w`/`e`). All four fields are guarded by the same
/// single lock, so they live together inside one [`SpinLock`] that
/// *owns* them (N-R7 lock-owns-data): every access goes through a guard
/// that `Deref`s to `&ConsInner`/`&mut ConsInner`, so the borrow checker
/// enforces "only while the lock is held" and no raw `static mut` access
/// (nor its `unsafe`) remains.
struct ConsInner {
    /// Input ring buffer.
    buf: [u8; INPUT_BUF_SIZE],
    /// Read index.
    r: u32,
    /// Write index.
    w: u32,
    /// Edit index.
    e: u32,
}

/// The console's raw-fallback input buffer + lock, as one data-owning
/// primitive. `SpinLock::new` const-initialises the embedded lock to the
/// exact post-`spin_init` state (`cons` name, `locked = 0`), so this is
/// a plain `static`; `consoleinit` still calls `CONS.init()` at the old
/// `spin_init(&CONS_LOCK)` site to keep the explicit init point.
static CONS: SpinLock<ConsInner> = SpinLock::new(
    c"cons",
    ConsInner { buf: [0; INPUT_BUF_SIZE], r: 0, w: 0, e: 0 },
);

/// Sleep/wakeup channel for `consoleread` waiting on `consoleintr` to
/// deliver a line into `CONS`. The C original used `&cons.r` as this
/// token; here the single fixed address of the `CONS` static serves the
/// same role -- it is only ever paired between `consoleread`'s
/// [`SpinLockGuard::sleep_on`] and `consoleintr`'s `wakeup_on_chan`
/// below (nothing else in the tree references it), so any consistent
/// address is a faithful, race-free replacement.
#[inline(always)]
fn cons_chan() -> *mut c_void {
    core::ptr::addr_of!(CONS) as *mut c_void
}

// ===========================================================================
// consolewrite -- user write()s to the console go here.
// ===========================================================================
//
// When the TTY is active, output post-processing (OPOST/ONLCR) is
// controlled by the TTY's termios flags. Bytes are sent directly to
// the UART via interrupt-driven uartputc -- the same approach Linux
// uses (user writes go straight to the driver, only echo goes through
// the TTY output pipe / drain thread).

/// # Safety
/// `buffer` must point to at least `n` readable bytes if `user_src` is
/// false, or be a valid userspace address range otherwise (checked by
/// `either_copyin`).
unsafe fn consolewrite(
    _cdev: *mut cdev_t,
    user_src: crate::bindings::bool_,
    buffer: *const c_void,
    n: usize,
) -> c_int {
    let src = buffer as u64;
    let mut kbuf = [0u8; 64];
    let mut outbuf = [0u8; 128]; // room for ONLCR expansion (worst case 2x)
    let mut written: usize = 0;

    // Check OPOST flags from the TTY (if active).
    let mut do_onlcr = true; // default: convert \n -> \r\n
    let tty_ptr = console_tty();
    if !tty_ptr.is_null() {
        // SAFETY: `tty_ptr` was published by `consoledevinit()` and is
        // never freed for the kernel's lifetime. N-R7h (lock-owns-data):
        // the tty's `termios` now lives inside `SpinLock<TtyInner>`;
        // `(*tty_ptr).inner.lock()` acquires the same per-tty lock this
        // used to take manually, and the guard `Deref`s to read `termios`.
        unsafe {
            let tty = (*tty_ptr).inner.lock();
            let oflag = tty.termios.c_oflag;
            do_onlcr = (oflag & crate::bindings::OPOST) != 0 && (oflag & crate::bindings::ONLCR) != 0;
            drop(tty);
        }
    }

    while written < n {
        let mut batch_size = n - written;
        if batch_size > 64 {
            batch_size = 64;
        }

        for i in 0..batch_size {
            // SAFETY: `user_src` selects user vs. kernel copy semantics;
            // `either_copyin` validates the userspace range itself.
            let r = unsafe {
                either_copyin(
                    (&raw mut kbuf[i]) as *mut c_void,
                    user_src as c_int,
                    src + (written + i) as u64,
                    1,
                )
            };
            if r < 0 {
                return if written > 0 { written as c_int } else { -EFAULT };
            }
        }

        // Expand into output buffer.
        // N-METH (goal #2): the `for i in 0..batch_size { ...kbuf[i]... }`
        // index walk over the just-filled batch became a slice iteration,
        // the same shape as the tty.rs `tty_write` NL->CRNL conversion in
        // the N-METH model (da51c6c).
        let mut olen = 0usize;
        for &b in &kbuf[..batch_size] {
            if do_onlcr && b == b'\n' {
                outbuf[olen] = b'\r';
                olen += 1;
            }
            outbuf[olen] = b;
            olen += 1;
        }

        // Submit output, waiting interruptibly when the TX buffer is full.
        let mut sent = 0usize;
        while sent < olen {
            // SAFETY: `outbuf[sent..olen]` is a valid readable slice.
            let accepted = unsafe {
                crate::uart::Uart::uartputs_nb(
                    outbuf[sent..olen].as_ptr() as *const c_char,
                    (olen - sent) as c_int,
                )
            };
            sent += accepted.max(0) as usize;
            if sent < olen {
                let ret = crate::uart::Uart::uart_tx_wait();
                if ret != 0 {
                    // Interrupted by signal -- return partial write
                    // count. We consumed this batch from userspace, so
                    // count it.
                    written += batch_size;
                    return if written > 0 { written as c_int } else { -EINTR };
                }
            }
        }
        written += batch_size;
    }

    written as c_int
}

// ===========================================================================
// consoleread -- user read()s from the console go here.
// ===========================================================================
//
// When the TTY is active, reads go through the TTY line discipline.
// Before TTY init, falls back to the raw console buffer.

/// # Safety
/// `buffer` must point to at least `n` writable bytes if `user_dst` is
/// false, or be a valid userspace address range otherwise (checked by
/// `either_copyout`).
unsafe fn consoleread(
    _cdev: *mut cdev_t,
    user_dst: crate::bindings::bool_,
    buffer: *mut c_void,
    n: usize,
) -> c_int {
    let tty_ptr = console_tty();
    if !tty_ptr.is_null() {
        // SAFETY: see `consolewrite`.
        return unsafe {
            Tty::read(tty_ptr, buffer as *mut c_char, n as u64, user_dst as c_int)
        } as c_int;
    }

    // --- Early boot fallback (before TTY is allocated) ---
    //
    // N-R7 lock-owns-data: the raw buffer + its lock are now `CONS`
    // (`SpinLock<ConsInner>`). The guard is acquired fresh at the top of
    // each outer iteration and dropped before the userspace copy, so the
    // lock is held exactly during collection and released during the
    // copy -- byte-identical to the C original's
    // `acquire ... release-before-copyout ... reacquire-if-continuing`
    // hold pattern, but expressed as guard drop/reacquire. No raw
    // `static mut` access, hence no `unsafe`, remains on this path
    // (`killed`/`current_thread_ptr`/`either_copyout` are all safe fns).
    let target = n;
    let mut n = n;
    let mut dst = buffer as u64;
    let mut got_newline = false;
    let mut got_eof = false;

    while n > 0 && !got_newline && !got_eof {
        let mut cons = CONS.lock();
        let mut kbuf = [0u8; 64];
        let mut batch_count = 0usize;

        // Collect up to 64 chars (or until newline/EOF) while holding
        // the lock.
        while n > 0 && batch_count < 64 && !got_newline && !got_eof {
            // Wait until the interrupt handler has put some input into
            // cons.buf.
            while cons.r == cons.w {
                if crate::proc::access::ThreadAccess::from_ptr(crate::machine::Riscv::current_thread_ptr()).map_or(0, |ta| ta.killed()) != 0 {
                    drop(cons);
                    // Copy any pending data before returning.
                    if batch_count > 0 {
                        if either_copyout(
                            user_dst as c_int,
                            dst,
                            kbuf.as_mut_ptr() as *mut c_void,
                            batch_count as u64,
                        ) < 0
                        {
                            return (target - n) as c_int; // bytes read so far
                        }
                        dst += batch_count as u64;
                        n -= batch_count;
                    }
                    return (target - n) as c_int;
                }
                // Atomically release `CONS`, block on `cons_chan()`, and
                // reacquire before returning -- the guard is still valid
                // (lock re-held) afterward. Mirrors C `sleep(&cons.r,
                // &cons.lock)`; `consoleintr` wakes this exact channel.
                cons.sleep_on(cons_chan());
            }

            let c = cons.buf[(cons.r % INPUT_BUF_SIZE as u32) as usize];
            cons.r += 1;

            if c as c_int == ctrl(b'D') {
                // End-of-file.
                if n < target || batch_count > 0 {
                    // Save ^D for next time, to make sure caller gets a
                    // 0-byte result.
                    cons.r -= 1;
                }
                got_eof = true;
                break;
            }

            kbuf[batch_count] = c;
            batch_count += 1;
            n -= 1;

            if c == b'\n' {
                // A whole line has arrived.
                got_newline = true;
                break;
            }
        }

        // Release lock before copying to userspace.
        drop(cons);

        // Copy batch to userspace (without holding the spinlock).
        if batch_count > 0 {
            if either_copyout(
                user_dst as c_int,
                dst,
                kbuf.as_mut_ptr() as *mut c_void,
                batch_count as u64,
            ) < 0
            {
                // Copy failed, return what we've read so far.
                return (target - n - batch_count) as c_int;
            }
            dst += batch_count as u64;
        }
        // Loop condition re-tests `n > 0 && !got_newline && !got_eof`;
        // if it holds we reacquire a fresh guard at the top next
        // iteration (the C original's "reacquire to continue").
    }

    (target - n) as c_int
}

/// Zero-sized [`CdevOps`] implementor for the console cdev (P3-10c; was
/// the `static mut cdev_ops_t CONSOLE_CDEV_OPS` fn-pointer table,
/// runtime-populated in `consoledevinit` -- now an immutable `Sync`
/// static, same soundness payoff as the P3-10a ptmx conversion; the six
/// former `extern "C"` callbacks are trait methods now, `open_file` is
/// not overridden so the trait default reproduces the old `None` slot).
struct ConsoleCdevOps;

/// The single shared instance `consoledevinit` installs.
static CONSOLE_CDEV_OPS: ConsoleCdevOps = ConsoleCdevOps;

impl CdevOps for ConsoleCdevOps {
    unsafe fn open(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }

    unsafe fn release(&self, _cdev: *mut cdev_t) -> KResult<()> {
        Ok(())
    }

    unsafe fn read(&self, cdev: *mut cdev_t, user: bool, buf: *mut c_void, count: usize)
        -> KResult<c_int> {
        // SAFETY: forwarded caller contract (see `consoleread`'s doc).
        Errno::cint_result(unsafe { consoleread(cdev, user as crate::bindings::bool_, buf, count) })
    }

    unsafe fn write(&self, cdev: *mut cdev_t, user: bool, buf: *const c_void, count: usize)
        -> KResult<c_int> {
        // SAFETY: forwarded caller contract (see `consolewrite`'s doc).
        Errno::cint_result(unsafe { consolewrite(cdev, user as crate::bindings::bool_, buf, count) })
    }

    /// Console ioctl -- delegates to the TTY layer for
    /// termios/TIOCSPGRP/etc.
    unsafe fn ioctl(&self, _cdev: *mut cdev_t, cmd: u64, arg: *mut c_void) -> KResult<c_int> {
        let tty_ptr = console_tty();
        if tty_ptr.is_null() {
            return Err(Errno::NotTy);
        }
        // SAFETY: `tty_ptr` is a live, permanently-allocated TTY once
        // published (see `console_tty`); `arg`'s validity is forwarded
        // from the dispatch contract.
        Errno::cint_result(unsafe { Tty::ioctl(tty_ptr, cmd, arg) })
    }

    /// Console poll -- check whether console has data ready for
    /// reading/writing. Delegates to `tty_poll`, which inspects
    /// `raw_buf` (raw mode) or `input_pipe` (canonical mode).
    unsafe fn poll(&self, _cdev: *mut cdev_t, events: c_short) -> Option<c_int> {
        let tty_ptr = console_tty();
        if tty_ptr.is_null() {
            return Some(0);
        }
        // SAFETY: see `ioctl` above.
        Some(unsafe { Tty::poll(tty_ptr, events) })
    }
}

/// Zero-sized [`DeviceOps`] implementor for the console's underlying
/// `device_t` (P3-10c; was the post-`cdev_register` `device_ops_t`
/// override `{ open: None, release: None, ioctl: Some(console_dev_ioctl) }`,
/// which replaced the cdev forwarder vtable to expose ioctl at the
/// `dev_ioctl` level -- reached from `vfs_ioctl` on the direct-device
/// fast path).
///
/// `open`/`release` reproduce the old override's `None` slots exactly:
/// device-level `open` has *no dispatch site anywhere in the tree*
/// (`device_get` only takes a reference), and a `None` `release` made
/// `dev_call_release`'s kassert panic -- both bodies therefore panic if
/// ever reached, exactly as the old `None` slots would have (the
/// console device is permanent: never unregistered, refcount never
/// reaches zero).
struct ConsoleDevOps;

/// The single shared instance `consoledevinit` installs (after
/// `cdev_register`, replacing the cdev forwarder -- the historical
/// install order, preserved).
static CONSOLE_DEV_OPS: ConsoleDevOps = ConsoleDevOps;

impl crate::dev::dev::DeviceOps for ConsoleDevOps {
    unsafe fn open(&self, _dev: *mut device_t) -> KResult<()> {
        // Old slot: `None`; no dispatcher exists (see type doc).
        crate::kstd::xv6_panic(c"console: device-level open not supported".as_ptr())
    }

    unsafe fn release(&self, _dev: *mut device_t) -> KResult<()> {
        // Old slot: `None`; `dev_call_release` would have panicked.
        crate::kstd::xv6_panic(c"console: device-level release not supported".as_ptr())
    }

    /// Console `device_t` ioctl -- same delegation as
    /// [`ConsoleCdevOps::ioctl`] but reached from `dev_ioctl` via
    /// `vfs_ioctl`.
    unsafe fn ioctl(&self, _dev: *mut device_t, cmd: u64, arg: *mut c_void) -> Option<c_int> {
        let tty_ptr = console_tty();
        if tty_ptr.is_null() {
            return Some(-ENOTTY);
        }
        // SAFETY: `tty_ptr` is a live, permanently-allocated TTY once
        // published (see `console_tty`).
        Some(unsafe { Tty::ioctl(tty_ptr, cmd, arg) })
    }
}

// ===========================================================================
// consoleintr -- the console input interrupt handler.
// ===========================================================================
//
// uartintr() calls this for each input character.
//
// When the TTY layer is active, characters are staged in tty_inbuf and
// a kernel thread feeds them to tty_input().
//
// Before TTY init, falls back to the raw console buffer with basic
// editing.

const TTY_INBUF_SIZE: u32 = 256;
static TTY_INBUF: [TtyInbufCell; TTY_INBUF_SIZE as usize] = {
    const INIT: TtyInbufCell = TtyInbufCell::new(0);
    // `[INIT; N]` with `INIT` a `const` item works for non-`Copy`
    // types too (the repeat expression re-evaluates the constant per
    // slot; no `Copy`/`Clone` bound needed), which is why
    // `TtyInbufCell` deliberately does not derive either.
    [INIT; TTY_INBUF_SIZE as usize]
};
/// Write index (interrupt producer).
static TTY_INBUF_W: AtomicU32 = AtomicU32::new(0);
/// Read index (drain-thread consumer).
static TTY_INBUF_R: AtomicU32 = AtomicU32::new(0);

/// Single-byte cell with interior mutability, sized to match the C
/// `volatile char tty_inbuf[]`.
struct TtyInbufCell(core::cell::UnsafeCell<u8>);
// SAFETY: every access goes through `TTY_INBUF_W`/`TTY_INBUF_R`
// (`AtomicU32`, `Release`/`Acquire`), which establishes a
// single-producer/single-consumer happens-before edge per slot exactly
// like the C `__atomic_store_n(..., RELEASE)` / `__atomic_load_n(...,
// ACQUIRE)` pair it replaces; the byte itself never needs its own
// atomic instructions.
unsafe impl Sync for TtyInbufCell {}
impl TtyInbufCell {
    const fn new(v: u8) -> Self {
        Self(core::cell::UnsafeCell::new(v))
    }
}

pub(crate) extern "C" fn consoleintr(c: c_int) {
    // ---- TTY path: stage in ring buffer for deferred processing ----
    let tty_ptr = console_tty();
    if !tty_ptr.is_null() {
        // ^P still handled here for kernel debugging.
        if c == ctrl(b'P') {
            ProcTable::dump();
            return;
        }
        let w = TTY_INBUF_W.load(Ordering::Relaxed);
        let next = (w + 1) % TTY_INBUF_SIZE;
        if next != TTY_INBUF_R.load(Ordering::Acquire) {
            // SAFETY: single producer (this function, serialised by the
            // UART interrupt handler); the slot at `w` is not read by
            // the consumer until the `Release` store below publishes
            // the new `w`.
            unsafe {
                *TTY_INBUF[w as usize].0.get() = c as u8;
            }
            TTY_INBUF_W.store(next, Ordering::Release);
        } // else: drop if full
        return;
    }

    // ---- Early boot fallback (raw buffer) ----
    //
    // N-R7 lock-owns-data: `CONS.lock()` acquires the guard; `cons`
    // `DerefMut`s to `&mut ConsInner`, so `cons.buf`/`cons.r`/`cons.w`/
    // `cons.e` are plain safe field accesses. The guard drops (unlocks)
    // at the end of the function -- byte-identical to the old
    // `spin_lock ... spin_unlock` bracket, with no `unsafe`.
    let mut cons = CONS.lock();

    match c {
        _ if c == ctrl(b'P') => {
            // Print process list.
            ProcTable::dump();
        }
        _ if c == ctrl(b'U') => {
            // Kill line.
            while cons.e != cons.w
                && cons.buf[((cons.e - 1) % INPUT_BUF_SIZE as u32) as usize] != b'\n'
            {
                cons.e -= 1;
                consputc(BACKSPACE);
            }
        }
        _ if c == ctrl(b'H') || c == 0x7f => {
            // Backspace / Delete key.
            if cons.e != cons.w {
                cons.e -= 1;
                consputc(BACKSPACE);
            }
        }
        _ => {
            if c != 0 && cons.e.wrapping_sub(cons.r) < INPUT_BUF_SIZE as u32 {
                let mut c = c;
                if c == '\r' as c_int {
                    c = '\n' as c_int;
                }

                // Echo back to the user.
                if c == 0x1b {
                    consputc('[' as c_int); // Escape sequence start
                } else if c == '\t' as c_int {
                    consputc(' ' as c_int); // Convert tab to space
                } else if (c < 32 || c > 126) && c != '\n' as c_int {
                    consputc('?' as c_int); // Non-printable
                } else {
                    consputc(c);
                }

                // Store for consumption by consoleread().
                let idx = (cons.e % INPUT_BUF_SIZE as u32) as usize;
                cons.buf[idx] = c as u8;
                cons.e += 1;

                if c == '\n' as c_int
                    || c == '\t' as c_int
                    || c == ctrl(b'D')
                    || cons.e.wrapping_sub(cons.r) == INPUT_BUF_SIZE as u32
                {
                    // Wake up consoleread() if a whole line (or
                    // end-of-file) has arrived.
                    cons.w = cons.e;
                    Scheduler::wakeup_on_chan(cons_chan());
                }
            }
        }
    }
    // `cons` drops here -> unlock.
}

// ===========================================================================
// consoleinit / consoledevinit.
// ===========================================================================

pub(crate) extern "C" fn consoleinit() {
    // Initialise `CONS`'s embedded lock at the historical
    // `spin_init(&CONS_LOCK)` site. Runs once, before any other hart or
    // interrupt handler can touch `CONS`. (`SpinLock::new` already
    // const-initialised the same fields, so this is belt-and-suspenders,
    // preserving the explicit init point.)
    CONS.init();

    // Try to initialize UART hardware. Returns 1 if successful (QEMU),
    // 0 if deferred (real hardware uses SBI).
    if crate::uart::Uart::uartinit() != 0 {
        // Mark UART as initialized - switch from SBI to UART output.
        UART_INITIALIZED.store(true, Ordering::Relaxed);
    }
    // If uartinit returned 0, keep UART_INITIALIZED false to continue
    // using SBI.
}

/// SBI console input polling thread. On non-QEMU platforms where UART
/// hardware isn't used, we poll SBI for input.
extern "C" fn sbi_console_poll_thread(_arg1: u64, _arg2: u64) {
    loop {
        // Read all available characters in a batch.
        let mut got_input = false;
        for _ in 0..32 {
            // Read up to 32 chars per cycle.
            let c = crate::sbi::Sbi::console_getchar();
            if c >= 0 {
                consoleintr(c);
                got_input = true;
            } else {
                break; // No more input available.
            }
        }

        if !got_input {
            // No input available, sleep briefly to avoid busy-waiting.
            // Use 1ms for responsive interactive typing.
            SchedTimer::sleep_ms(1);
        }
        // If we got input, immediately check for more without
        // sleeping.
    }
}

/// TTY input feeder thread.
///
/// Drains the `tty_inbuf` ring buffer (filled by `consoleintr` in
/// interrupt context) and feeds characters to `tty_input()`, which can
/// safely sleep.
extern "C" fn console_tty_input_thread(_arg1: u64, _arg2: u64) {
    let tty_ptr = console_tty();
    loop {
        let mut r = TTY_INBUF_R.load(Ordering::Acquire);
        let mut w = TTY_INBUF_W.load(Ordering::Acquire);

        if r == w {
            // Nothing to process -- poll every 1 ms.
            SchedTimer::sleep_ms(1);
            continue;
        }

        // Drain available characters into the TTY line discipline.
        while r != w {
            // SAFETY: `r` was published by the producer's `Release`
            // store to `TTY_INBUF_W`, observed here via the `Acquire`
            // load above (or the re-load below), establishing a
            // happens-before edge for this slot.
            let ch = unsafe { *TTY_INBUF[r as usize].0.get() };
            r = (r + 1) % TTY_INBUF_SIZE;
            TTY_INBUF_R.store(r, Ordering::Release);

            // SAFETY: `tty_ptr` was captured once above; it is
            // permanently allocated once published by
            // `consoledevinit()`, and this thread is only started
            // after that publication.
            unsafe {
                Tty::input(tty_ptr, &ch as *const u8 as *const c_char, 1);
            }

            // Re-read w in case more arrived.
            w = TTY_INBUF_W.load(Ordering::Acquire);
        }
    }
}

/// TTY output drain thread.
///
/// Drains the TTY output pipe and sends bytes to the UART/SBI console.
/// Only echo output (from `tty_echo_char` in the line discipline) flows
/// through this pipe. User writes go directly to UART via
/// `consolewrite`.
///
/// Data arriving from the pipe has already been post-processed by
/// `tty_echo_char` (OPOST/ONLCR), so bytes are output verbatim -- no
/// additional `\n` -> `\r\n` conversion is performed.
extern "C" fn console_tty_drain_thread(_arg1: u64, _arg2: u64) {
    let tty_ptr = console_tty();
    let mut buf = [0u8; 64];
    loop {
        // `tty_output` blocks if no data is available.
        // SAFETY: see `console_tty_input_thread`.
        let n = unsafe { Tty::output(tty_ptr, buf.as_mut_ptr() as *mut c_char, buf.len() as u64) };
        if n <= 0 {
            SchedTimer::sleep_ms(1);
            continue;
        }
        // N-METH (goal #2): drain the filled prefix by slice iteration
        // instead of a manual `for i in 0..n { buf[i] }` index walk.
        for &byte in &buf[..n as usize] {
            if !UART_INITIALIZED.load(Ordering::Relaxed) {
                crate::sbi::Sbi::console_putchar(byte as c_int);
            } else {
                crate::uart::Uart::uartputc_sync(byte as c_int);
            }
        }
    }
}

static mut CONSOLE_CDEV: MaybeUninit<cdev_t> = MaybeUninit::zeroed();

pub(crate) extern "C" fn consoledevinit() {
    // SAFETY: `consoledevinit()` runs exactly once, from
    // `start_kernel.c`'s single-hart init sequence, before any other
    // code can observe `CONSOLE_CDEV`.
    unsafe {
        let cdev = CONSOLE_CDEV.as_mut_ptr();
        (*cdev).dev.major = CONSOLE_MAJOR;
        (*cdev).dev.minor = CONSOLE_MINOR;
        (*cdev).dev.devname = c"console".as_ptr();
        (*cdev).dev.devmode = (S_IFCHR | 0o666) as mode_t;
        (*cdev).flags.set_readable(1);
        (*cdev).flags.set_writable(1);
        (*cdev).ops = Some(&CONSOLE_CDEV_OPS);

        let errno = Cdev::register(cdev);
        if errno != 0 {
            __panic_start();
            crate::kprintln!(
                "ASSERTION_FAILURE {}:{}: In function '{}':",
                "kernel/console.rs", line!(), "consoledevinit",
            );
            crate::kprintln!("consoleinit: cdev_register failed: {}", errno);
            __panic_end();
        }

        // Install ioctl on the device_t level (used by vfs_ioctl ->
        // dev_ioctl) -- replaces the cdev forwarder installed by
        // `cdev_register`, matching the historical install order.
        (*cdev).dev.ops = Some(&CONSOLE_DEV_OPS);

        let mut irq_desc = IrqDesc {
            handler: Some(crate::uart::uartintr),
            data: core::ptr::null_mut(),
            dev: &raw mut (*cdev).dev,
            irq: 0,
            count: 0,
            // SAFETY: `rcu_head` is plain-old-data (pointers/u64/a one-bit
            // bitfield); zero is a valid "not yet queued" state —
            // `register_irq_handler` overwrites it unconditionally anyway
            // (see `irq_core::register_irq_handler`).
            rcu_head: unsafe { core::mem::zeroed() },
        };
        let errno = IrqCore::register_irq_handler(IrqCore::plic_irq(crate::uart::__uart0_irqno as c_int), &raw mut irq_desc);
        if errno != 0 {
            __panic_start();
            crate::kprintln!(
                "ASSERTION_FAILURE {}:{}: In function '{}':",
                "kernel/console.rs", line!(), "consoledevinit",
            );
            crate::kprintln!(
                "consoledevinit: register_irq_handler failed, error code: {}",
                errno,
            );
            __panic_end();
        }

        // --- Allocate the console TTY ---
        // P3-10d: `None` = no driver ops (previously a null `tty_ops*`
        // — the console tty deliberately has no `TtyOps` hooks; every
        // tty-core dispatcher's `None` fallback is its behavior).
        let t = Tty::alloc(c"console".as_ptr(), None);
        if is_err_or_null(t) {
            __panic_start();
            crate::kprintln!(
                "ASSERTION_FAILURE {}:{}: In function '{}':",
                "kernel/console.rs", line!(), "consoledevinit",
            );
            crate::kprintln!("consoledevinit: Tty::alloc failed");
            __panic_end();
        }
        CONSOLE_TTY.store(t, Ordering::Relaxed);

        // Make both TTY pipes non-blocking for writes so that
        // tty_input() (called from the feeder thread) and
        // tty_echo_char() never sleep when the pipe is full --
        // characters are silently discarded instead.
        const PIPE_FLAGS_NONBLOCK_WR: c_int = 4;
        Pipe::pipe_set_flags((*t).input_pipe, 1 << PIPE_FLAGS_NONBLOCK_WR);
        Pipe::pipe_set_flags((*t).output_pipe, 1 << PIPE_FLAGS_NONBLOCK_WR);

        // Attach the console TTY to init's session as the controlling
        // terminal.
        let initproc = ProcTable::get_initproc();
        if !initproc.is_null() {
            // N-R6d-2a: `thread.session` is now a generational `Sid`; resolve
            // it to a live `*mut session` under `pid_wlock` (required by
            // `session_lookup`). Init always has its boot session here, so this
            // attaches the console as its controlling terminal exactly as
            // before; a null (no/stale session) simply skips, as the old
            // null-pointer guard did.
            ProcTable::wlock();
            let sess = SessionTable::lookup((*initproc).session).unwrap_or(core::ptr::null_mut());
            if !sess.is_null() {
                Session::set_ctrl_tty(sess, t);
            }
            ProcTable::wunlock();
        }

        // Start the input feeder thread (intr ring buf -> tty_input).
        let feeder = crate::proc::thread::Thread::kthread_create(
            c"tty_input".as_ptr(),
            console_tty_input_thread as *mut c_void,
            0,
            0,
            0,
        );
        if !is_err_or_null(feeder) {
            Scheduler::wakeup(feeder);
        }

        // Start the output drain thread (echo -> UART).
        let drain = crate::proc::thread::Thread::kthread_create(
            c"tty_drain".as_ptr(),
            console_tty_drain_thread as *mut c_void,
            0,
            0,
            0,
        );
        if !is_err_or_null(drain) {
            Scheduler::wakeup(drain);
        }

        // Start SBI polling thread if UART hardware not available.
        if !UART_INITIALIZED.load(Ordering::Relaxed) {
            let p = crate::proc::thread::Thread::kthread_create(
                c"sbi_console".as_ptr(),
                sbi_console_poll_thread as *mut c_void,
                0,
                0,
                0,
            );
            if !is_err_or_null(p) {
                Scheduler::wakeup(p);
            }
        }
    }
}
