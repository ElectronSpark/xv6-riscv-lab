//! Formatted console output -- printf, panic.
//!
//! Rust port of `kernel/printf.c` (Phase 2 Wave 4, see
//! `docs/rustify/phase2_plan.md`). Top of the console/panic chain:
//! `xv6_panic`/callers -> this file -> `console.rs` -> `uart.rs`.
//!
//! # Variadic ABI (Step 0 of the Wave 4 plan)
//!
//! `printf(char *, ...)` is a C-ABI variadic function. Empirically
//! verified against this workspace's pinned **stable** `rustc 1.95.0**:
//! *defining* a variadic function (`c_variadic`/`VaList`) still requires
//! `#![feature(c_variadic)]`, which is nightly-only and rejected outright
//! on stable (`error[E0554]: #![feature] may not be used on the stable
//! release channel`). *Calling* an externally-declared C variadic
//! function from Rust (`unsafe extern "C" { fn printf(fmt: *const
//! c_char, ...) -> c_int; }`) has always been stable and is used
//! extensively elsewhere in this crate (`sbi.rs`, `lock/spinlock.rs`,
//! this file's own `__panic_start`).
//!
//! So: `printf()`'s C-variadic *entry point* stays a minimal C shim
//! (`kernel/printf_shim.c`) — the last-resort C remnant in this
//! otherwise-Rust module, to be deleted once `c_variadic` stabilizes.
//! The shim is as small as it can be: it does `va_start`/`va_end` and
//! three one-line `va_arg` fetcher functions (`__printf_va_arg_int` /
//! `__printf_va_arg_u64` / `__printf_va_arg_str`, matching the three
//! distinct `va_arg(ap, T)` call shapes the original C `printf()`
//! used), then hands the format string and an opaque `va_list *` to
//! [`printf_rust`] below. `printf_rust` never interprets the `va_list`
//! itself (its concrete layout is a C/target ABI detail Rust has no
//! stable way to model without `c_variadic`) -- it only holds the
//! pointer and calls back through the three fetchers whenever the
//! format string calls for the next argument. Every format-string
//! conversion the original C `printf()` supported is preserved exactly
//! (`%d %ld %lld %u %lu %llu %x %lx %llx %p %s %c` -- wait, there is no
//! `%c` in the original; see [`printf_rust`]'s match arms for the exact
//! list carried over: `%d %ld %lld %u %lu %llu %x %lx %llx %p %s %%`,
//! plus the `-`/field-width prefix and the `%*s` padding special case),
//! no more.
//!
//! `snprintf`/`sprintf`/`vprintf` do not exist anywhere in
//! `kernel/printf.c` or its header (`kernel/inc/printf.h` declares only
//! `printf`) -- confirmed by a full-tree grep before starting this
//! port -- so there is no wider variadic surface to replicate.
//!
//! # Panic machinery
//!
//! `__panic_start`/`__panic_end`/`panic_state`/`panic_disable_bt`/
//! `trigger_panic`/`panic_msg_lock`/`panic_msg_unlock`/`printfinit` all
//! keep their exact C names: `proc/proc_shims.rs`'s `xv6_panic` and ~14
//! other `kernel/proc/*.rs` files declare `unsafe extern "C" {
//! safe fn __panic_start(); safe fn __panic_end() -> !; }` and link
//! against these definitions unchanged.

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicBool, Ordering};

use crate::bindings::spinlock_t;
use crate::machine;

// ===========================================================================
// Externs.
// ===========================================================================

unsafe extern "C" {
    pub safe fn spin_init(lk: *mut spinlock_t, name: *const c_char);
    pub safe fn spin_lock(lk: *mut spinlock_t);
    pub safe fn spin_unlock(lk: *mut spinlock_t);

    /// `printf()` itself: defined in `kernel/printf_shim.c` (the
    /// variadic C-ABI entry point), forwards to [`printf_rust`]. Every
    /// other file in the tree that calls `printf(...)` (C or Rust)
    /// keeps working unchanged -- this is the same symbol name/ABI as
    /// the original `kernel/printf.c::printf()`.
    pub safe fn printf(fmt: *const c_char, ...) -> c_int;

    /// `ipi/ipi.c` (not yet ported).
    fn ipi_send_all(reason: c_int) -> c_int;

    /// `kernel/backtrace.c` (not yet ported; needs Wave 1, already
    /// done -- backtrace itself is a later wave).
    fn print_backtrace(context: u64, stack_start: u64, stack_end: u64);

    /// FDT-configured kernel physical memory bounds
    /// (`kernel/inc/mm/memlayout.h`'s `KERNBASE`/`PHYSTOP` macros),
    /// mirrored here exactly like `mm/page.rs`/`mm/vm_pgtab.rs`'s
    /// identical extern pair.
    pub safe static __physical_memory_start: u64;
    pub safe static __physical_memory_end: u64;
}

// The three va_list fetchers implemented in `kernel/printf_shim.c` --
// see the module doc comment above. `ap` is an opaque pointer to the
// shim's live `va_list`; only the shim ever dereferences it.
unsafe extern "C" {
    fn __printf_va_arg_int(ap: *mut c_void) -> i64;
    fn __printf_va_arg_u64(ap: *mut c_void) -> u64;
    fn __printf_va_arg_str(ap: *mut c_void) -> *const c_char;
}

#[inline(always)]
fn va_int(ap: *mut c_void) -> i64 {
    // SAFETY: `ap` is the live `va_list *` forwarded unchanged from
    // `printf_shim.c`'s `printf()`; this is called at most once per
    // format-string conversion, in the same left-to-right order the C
    // original consumed arguments, so each call fetches the next
    // genuine vararg.
    unsafe { __printf_va_arg_int(ap) }
}
#[inline(always)]
fn va_u64(ap: *mut c_void) -> u64 {
    // SAFETY: see `va_int`.
    unsafe { __printf_va_arg_u64(ap) }
}
#[inline(always)]
fn va_str(ap: *mut c_void) -> *const c_char {
    // SAFETY: see `va_int`.
    unsafe { __printf_va_arg_str(ap) }
}

/// Read the frame pointer (`s0`). Mirrors the C `r_fp()` static inline
/// (`kernel/inc/riscv.h`); no equivalent exists yet in `machine.rs`
/// (only used by this file's panic path), so it's a local primitive
/// following the same `core::arch::asm!` + SAFETY-comment convention
/// as every other CSR/register read in the crate.
#[inline(always)]
fn read_fp() -> u64 {
    let fp: u64;
    // SAFETY: `mv {0}, s0` reads a general-purpose register with no
    // memory effects and cannot trap.
    unsafe {
        core::arch::asm!("mv {0}, s0", out(reg) fp, options(nomem, nostack, preserves_flags));
    }
    fp
}

/// `IPI_REASON_CRASH` (`kernel/inc/smp/ipi.h`).
const IPI_REASON_CRASH: c_int = 0;

/// `PAGE_SHIFT` (`kernel/inc/param.h`), mirrored the same way
/// `start.rs`/`machine.rs` already duplicate it locally.
const PAGE_SHIFT: u32 = 12;

// ===========================================================================
// panic_state -- global panic flag, checked by spinlock's deadlock path.
// ===========================================================================

/// Mirrors the C `static volatile int __global_panic_state`.
static GLOBAL_PANIC_STATE: AtomicBool = AtomicBool::new(false);

pub(crate) extern "C" fn panic_state() -> c_int {
    GLOBAL_PANIC_STATE.load(Ordering::Acquire) as c_int
}

// ===========================================================================
// pr -- lock to avoid interleaving concurrent printf()s.
// ===========================================================================

static mut PR_LOCK: spinlock_t = spinlock_t {
    locked: 0,
    name: core::ptr::null_mut(),
    cpu: core::ptr::null_mut(),
};
/// Mirrors the C `pr.locking` (an `int` read/written via
/// `__atomic_{load,store}_n`). Starts `false` (BSS-zero in C, since
/// `printfinit()` -- called well after early boot -- is what flips it
/// on); disabled for the rest of a panicked kernel's life by
/// `__panic_start` to avoid recursive deadlock on `PR_LOCK` itself.
static PR_LOCKING: AtomicBool = AtomicBool::new(false);

/// Mirrors the C `static int nnewline` -- a `printf()`-local static in
/// the original, hoisted to module scope (identical storage duration
/// and visibility; C function-local `static` has file-scope lifetime).
static NNEWLINE: AtomicBool = AtomicBool::new(false);

// ===========================================================================
// Formatting helpers. Operate on a caller-owned `&mut [u8]` + running
// length instead of the C `char *outbuf, int *outlen` pointer pair --
// plain safe slice indexing, no unsafe needed (the buffer is always the
// fixed-size local array in `printf_rust`, never escapes).
// ===========================================================================

const DIGITS: &[u8; 16] = b"0123456789abcdef";

fn printint(xx: i64, base: i64, sign: bool, outbuf: &mut [u8], outlen: &mut usize) {
    let mut buf = [0u8; 16];
    let mut i = 0usize;

    let neg = sign && xx < 0;
    // `wrapping_neg` mirrors the C `x = -xx` (assigned into an
    // `unsigned long long`): even for `xx == i64::MIN`, where plain
    // negation would overflow, the C original's negate-then-reinterpret
    // recovers the correct unsigned magnitude via two's-complement
    // wraparound; `wrapping_neg` reproduces that bit pattern exactly
    // without Rust's debug-mode overflow panic.
    let mut x: u64 = if neg { xx.wrapping_neg() as u64 } else { xx as u64 };

    loop {
        buf[i] = DIGITS[(x % base as u64) as usize];
        i += 1;
        x /= base as u64;
        if x == 0 {
            break;
        }
    }
    if neg {
        buf[i] = b'-';
        i += 1;
    }
    while i > 0 {
        i -= 1;
        outbuf[*outlen] = buf[i];
        *outlen += 1;
    }
}

fn printptr(mut x: u64, outbuf: &mut [u8], outlen: &mut usize) {
    outbuf[*outlen] = b'0';
    *outlen += 1;
    outbuf[*outlen] = b'x';
    *outlen += 1;
    for _ in 0..16 {
        outbuf[*outlen] = DIGITS[(x >> 60) as usize];
        *outlen += 1;
        x <<= 4;
    }
}

fn print_padding(len: i32, outbuf: &mut [u8], outlen: &mut usize) {
    for _ in 0..len {
        outbuf[*outlen] = b' ';
        *outlen += 1;
    }
}

fn print_timestamp(outbuf: &mut [u8], outlen: &mut usize) {
    let stime = machine::read_time();
    outbuf[*outlen] = b'[';
    *outlen += 1;
    printint(stime as i64, 10, false, outbuf, outlen);
    outbuf[*outlen] = b']';
    *outlen += 1;
    outbuf[*outlen] = b' ';
    *outlen += 1;
}

/// Flush `outbuf[..*outlen]` to the console and reset the length.
#[inline]
fn flush(outbuf: &mut [u8], outlen: &mut usize) {
    // SAFETY: `outbuf[..*outlen]` is always initialised (every byte
    // was just written by this same function before `*outlen` grew
    // past it).
    unsafe {
        crate::console::consputs(outbuf.as_ptr() as *const c_char, *outlen as c_int);
    }
    *outlen = 0;
}

// ===========================================================================
// printf_rust -- the real formatter, called from the C variadic shim.
// ===========================================================================

/// # Safety
/// `fmt` must be a valid NUL-terminated string. `ap` must be a live
/// `va_list *` (opaque to Rust) produced by `printf_shim.c`'s
/// `printf()`, positioned at the first vararg, and valid for the
/// duration of this call.
#[no_mangle]
pub unsafe extern "C" fn printf_rust(fmt: *const c_char, ap: *mut c_void) -> c_int {
    let mut outbuf = [0u8; 512];
    let mut outlen: usize = 0;

    // Use atomic load to see if panic disabled locking.
    let locking = PR_LOCKING.load(Ordering::Acquire);
    if locking {
        spin_lock(&raw mut PR_LOCK);
    }

    if !NNEWLINE.swap(true, Ordering::Acquire) {
        print_timestamp(&mut outbuf, &mut outlen);
    }

    let mut i: isize = 0;
    loop {
        // SAFETY: `fmt` is NUL-terminated (caller contract); this loop
        // never reads past the terminator.
        let cx = unsafe { *fmt.offset(i) } as u8;
        if cx == 0 {
            break;
        }
        if cx != b'%' {
            outbuf[outlen] = cx;
            outlen += 1;
            if cx == b'\n' {
                NNEWLINE.store(false, Ordering::Release);
            }
            if outlen >= 500 {
                flush(&mut outbuf, &mut outlen);
            }
            i += 1;
            continue;
        }
        i += 1;

        // Parse optional '-' flag (left-align) and field width.
        let mut left_align = false;
        let mut field_width: i32 = 0;
        // SAFETY: see above.
        if unsafe { *fmt.offset(i) as u8 } == b'-' {
            left_align = true;
            i += 1;
        }
        // SAFETY: see above.
        while {
            let d = unsafe { *fmt.offset(i) as u8 };
            d.is_ascii_digit()
        } {
            // SAFETY: see above.
            let d = unsafe { *fmt.offset(i) as u8 };
            field_width = field_width * 10 + (d - b'0') as i32;
            i += 1;
        }
        let start_pos = outlen;

        // SAFETY: see above.
        let mut c0 = unsafe { *fmt.offset(i) as u8 };
        let mut c1 = 0u8;
        if c0 != 0 {
            // SAFETY: see above.
            c1 = unsafe { *fmt.offset(i + 1) as u8 };
        }
        if c0 != 0 && c1 != 0 && c0 == b'*' && c1 == b's' {
            let len = va_int(ap) as i32;
            print_padding(len, &mut outbuf, &mut outlen);
            i += 1;
            c0 = c1;
            // SAFETY: see above.
            c1 = unsafe { *fmt.offset(i + 1) as u8 };
        }
        let c2 = if c1 != 0 {
            // SAFETY: see above.
            unsafe { *fmt.offset(i + 2) as u8 }
        } else {
            0u8
        };

        if c0 == b'd' {
            printint(va_int(ap), 10, true, &mut outbuf, &mut outlen);
        } else if c0 == b'l' && c1 == b'd' {
            printint(va_u64(ap) as i64, 10, true, &mut outbuf, &mut outlen);
            i += 1;
        } else if c0 == b'l' && c1 == b'l' && c2 == b'd' {
            printint(va_u64(ap) as i64, 10, true, &mut outbuf, &mut outlen);
            i += 2;
        } else if c0 == b'u' {
            printint(va_int(ap), 10, false, &mut outbuf, &mut outlen);
        } else if c0 == b'l' && c1 == b'u' {
            printint(va_u64(ap) as i64, 10, false, &mut outbuf, &mut outlen);
            i += 1;
        } else if c0 == b'l' && c1 == b'l' && c2 == b'u' {
            printint(va_u64(ap) as i64, 10, false, &mut outbuf, &mut outlen);
            i += 2;
        } else if c0 == b'x' {
            printint(va_int(ap), 16, false, &mut outbuf, &mut outlen);
        } else if c0 == b'l' && c1 == b'x' {
            printint(va_u64(ap) as i64, 16, false, &mut outbuf, &mut outlen);
            i += 1;
        } else if c0 == b'l' && c1 == b'l' && c2 == b'x' {
            printint(va_u64(ap) as i64, 16, false, &mut outbuf, &mut outlen);
            i += 2;
        } else if c0 == b'p' {
            printptr(va_u64(ap), &mut outbuf, &mut outlen);
        } else if c0 == b's' {
            let mut s = va_str(ap);
            if s.is_null() {
                s = c"(null)".as_ptr();
            }
            let mut j: isize = 0;
            loop {
                // SAFETY: `s` is either the caller's NUL-terminated
                // string vararg or the `"(null)"` literal above.
                let ch = unsafe { *s.offset(j) };
                if ch == 0 {
                    break;
                }
                outbuf[outlen] = ch as u8;
                outlen += 1;
                j += 1;
                if outlen >= 500 {
                    flush(&mut outbuf, &mut outlen);
                }
            }
        } else if c0 == b'%' {
            outbuf[outlen] = b'%';
            outlen += 1;
        } else if c0 == 0 {
            break;
        } else {
            // Print unknown % sequence to draw attention.
            outbuf[outlen] = b'%';
            outlen += 1;
            outbuf[outlen] = c0;
            outlen += 1;
        }

        // Apply left-aligned field width padding.
        if left_align && field_width > 0 {
            let mut written = (outlen - start_pos) as i32;
            while written < field_width {
                outbuf[outlen] = b' ';
                outlen += 1;
                written += 1;
            }
        }
        if outlen >= 500 {
            flush(&mut outbuf, &mut outlen);
        }

        i += 1;
    }

    // Flush remaining buffer.
    if outlen > 0 {
        flush(&mut outbuf, &mut outlen);
    }

    if locking {
        spin_unlock(&raw mut PR_LOCK);
    }

    0
}

// ===========================================================================
// Backtrace-on-panic toggle.
// ===========================================================================

/// Mirrors the C `static int __bt_enabled = 1;`.
static BT_ENABLED: AtomicBool = AtomicBool::new(true);

pub(crate) extern "C" fn panic_disable_bt() {
    BT_ENABLED.store(false, Ordering::Relaxed);
}

// ===========================================================================
// __panic_start / __panic_end / trigger_panic.
// ===========================================================================

#[no_mangle]
pub extern "C" fn __panic_start() {
    // Set global panic state FIRST so other cores can see it.
    GLOBAL_PANIC_STATE.store(true, Ordering::Release);

    // Disable printf locking FIRST, before anything else. This
    // prevents recursive deadlock when panic is called due to a
    // deadlock on PR_LOCK itself (spin_acquire timeout on the "pr"
    // lock). Release semantics so other cores see it.
    PR_LOCKING.store(false, Ordering::Release);

    // SET_CPU_CRASHED()
    machine::CpuLocal::current().flags_or(machine::CPU_FLAG_CRASHED);

    panic_msg_lock();
    let fp = read_fp();
    let p = machine::current_thread_ptr();
    // SAFETY: `p` is checked non-null before every dereference below;
    // `kstack`/`pid`/`name`/`kstack_order` are plain scalar/array
    // fields of `struct thread`, valid for the lifetime of a live
    // thread (this can only run on a thread that is currently
    // executing, i.e. not yet freed).
    unsafe {
        if p.is_null() || (*p).kstack == 0 {
            // No thread context - use kernel memory bounds for
            // backtrace.
            printf(
                c"[Core: %ld] No thread context, fp=%p\n".as_ptr(),
                machine::cpuid() as i64,
                fp as *mut c_void,
            );
            if BT_ENABLED.load(Ordering::Relaxed) {
                // Use KERNBASE to PHYSTOP as conservative stack bounds.
                print_backtrace(fp, __physical_memory_start, __physical_memory_end);
            }
            return;
        }
        printf(
            c"[Core: %ld] In thread %d (%s) at %p\n".as_ptr(),
            machine::cpuid() as i64,
            (*p).pid,
            (*p).name.as_ptr(),
            fp as *mut c_void,
        );
        if BT_ENABLED.load(Ordering::Relaxed) {
            let kstack_size = 1u64 << (PAGE_SHIFT + (*p).kstack_order as u32);
            print_backtrace(fp, (*p).kstack, (*p).kstack + kstack_size);
        }
    }
}

pub(crate) extern "C" fn trigger_panic() -> ! {
    // SET_CPU_CRASHED()
    machine::CpuLocal::current().flags_or(machine::CPU_FLAG_CRASHED);

    // Mask all interrupts except software interrupt (IPI).
    machine::write_sie(machine::SIE_SSIE);

    // Send IPI to all harts (including self) to crash.
    // SAFETY: `ipi_send_all` is a plain C function taking a reason
    // code; no preconditions beyond "callable from any context",
    // which holds here (we are already committed to halting).
    unsafe {
        ipi_send_all(IPI_REASON_CRASH);
    }

    // Enable interrupts so IPI can be received.
    machine::intr_on();

    loop {
        machine::wfi();
    }
}

#[no_mangle]
pub extern "C" fn __panic_end() -> ! {
    panic_msg_unlock();
    trigger_panic()
}

pub(crate) extern "C" fn printfinit() {
    // SAFETY: runs once, from `start_kernel.c`'s single-hart init
    // sequence, before any concurrent access to `PR_LOCK` is possible.
    unsafe {
        spin_init(&raw mut PR_LOCK, c"pr".as_ptr());
    }
    PR_LOCKING.store(true, Ordering::Release);
}

// ===========================================================================
// panic_msg_lock / panic_msg_unlock -- serialise panic output across
// multiple cores.
// ===========================================================================

static mut PANIC_BT_LOCK: spinlock_t = spinlock_t {
    locked: 0,
    name: c"panic_bt_lock".as_ptr() as *mut c_char,
    cpu: core::ptr::null_mut(),
};

pub(crate) extern "C" fn panic_msg_lock() {
    // SAFETY: `PANIC_BT_LOCK` is a valid, compile-time-initialised
    // spinlock (see its definition above).
    unsafe {
        spin_lock(&raw mut PANIC_BT_LOCK);
    }
}

pub(crate) extern "C" fn panic_msg_unlock() {
    // SAFETY: see `panic_msg_lock`.
    unsafe {
        spin_unlock(&raw mut PANIC_BT_LOCK);
    }
}

// ===========================================================================
// puts / putchar.
//
// P3-1C mesh sweep: kept `#[no_mangle]` as a class, matching P3-1A's
// documented `strdup` precedent -- these are libc-shaped names
// (signature matches the C standard library's `puts`/`putchar` exactly),
// and LLVM's libcall-recognition instcombine pass rewrites
// `printf("literal with no specifiers\n")`/single-char `printf` calls
// into direct calls to `puts`/`putchar` by that exact linker-symbol name,
// completely invisible to any static Rust caller survey (empirically
// confirmed: demoting these produced dozens of `undefined reference to
// 'putchar'/'puts'` link errors from functions whose source never
// mentions either name -- e.g. `irq::trap::kassert_fail`'s trailing
// `printf(c"\n".as_ptr())` got compiled straight into a `putchar('\n')`
// call). No amount of caller-import fixing can remove this dependency;
// only deleting `printf_shim.c`'s C `printf` entirely (a later,
// zero-C-printf wave) would.
// ===========================================================================

/// # Safety
/// `s` must be a valid NUL-terminated string.
#[no_mangle]
pub unsafe extern "C" fn puts(s: *const c_char) {
    let mut p = s;
    // SAFETY: caller guarantees `s` is NUL-terminated.
    unsafe {
        while *p != 0 {
            crate::console::consputc(*p as c_int);
            p = p.add(1);
        }
    }
    crate::console::consputc('\n' as c_int);
}

#[no_mangle]
pub extern "C" fn putchar(c: c_int) {
    crate::console::consputc(c);
}
