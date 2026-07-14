//! Formatted console output -- `kprint!`/`kprintln!`, panic.
//!
//! Rust port of `kernel/printf.c` (Phase 2 Wave 4), completed by Wave
//! P3-2 (the "zero-C" milestone). Top of the console/panic chain:
//! `xv6_panic`/callers -> this file -> `console.rs` -> `uart.rs`.
//!
//! # From C-variadic `printf()` to `core::fmt` (Wave P3-2)
//!
//! The original console output primitive was a C-ABI variadic
//! `printf(char *, ...)`. Because *defining* a C-variadic function needs
//! `#![feature(c_variadic)]` (nightly-only; rejected on this workspace's
//! pinned stable `rustc 1.95.0` with `error[E0554]`), the variadic entry
//! point used to live in a tiny C shim (`kernel/printf_shim.c`) that did
//! `va_start`/`va_end` + three `va_arg` fetchers and handed an opaque
//! `va_list *` back to a Rust format-string parser (`printf_rust`).
//!
//! Wave P3-2 removed all of that. `core::fmt::Arguments` needs no
//! variadic ABI at all -- `format_args!` builds the argument list at each
//! macro-expansion call site -- so every `printf(fmt, args...)` call site
//! crate-wide was migrated to the native [`kprint!`]/[`kprintln!`] macros
//! below (which format through [`Console`], a `core::fmt::Write` sink
//! that pushes bytes down the exact same [`crate::console::consputs`]
//! path the old `printf_rust` used). With no caller left, `printf_rust`,
//! its three `va_arg` fetchers, and `printf_shim.c` itself were all
//! deleted -- `printf_shim.c` was the last C file anywhere in the
//! kernel, so its deletion is the zero-C milestone.
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

use core::ffi::{c_char, c_int};
use core::fmt::Write as _;
use core::sync::atomic::{AtomicBool, Ordering};

use crate::bindings::spinlock_t;
use crate::machine;
// P3-1D mesh sweep: ipi.rs/backtrace.rs are in scope for this wave;
// signatures are identical, so these become plain crate-path imports
// instead of `extern "C"` redeclarations.
use crate::ipi::ipi_send_all;
use crate::backtrace::print_backtrace;

// ===========================================================================
// Externs.
// ===========================================================================

unsafe extern "C" {
    pub safe fn spin_init(lk: *mut spinlock_t, name: *const c_char);
    pub safe fn spin_lock(lk: *mut spinlock_t);
    pub safe fn spin_unlock(lk: *mut spinlock_t);

    /// FDT-configured kernel physical memory bounds
    /// (`kernel/inc/mm/memlayout.h`'s `KERNBASE`/`PHYSTOP` macros),
    /// mirrored here exactly like `mm/page.rs`/`mm/vm_pgtab.rs`'s
    /// identical extern pair.
    pub safe static __physical_memory_start: u64;
    pub safe static __physical_memory_end: u64;
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
// kprint! / kprintln! -- core::fmt::Write-based console output (Wave P3-2,
// "the zero-C milestone"). These replaced the C-variadic `printf()`
// entirely: `#![feature(c_variadic)]` is nightly-only (see the module doc
// comment), but Rust's native `core::fmt::Arguments` machinery needs no
// variadic ABI at all -- `format_args!` builds the argument list at the
// macro-expansion call site, so there is no `va_list` to fetch through a C
// shim.
//
// Output path: byte-for-byte the same as `printf_rust`'s `flush()` above --
// [`Console::write_str`] forwards straight to [`crate::console::consputs`]
// (no intermediate buffering; `consputs` itself is already the primitive
// `puts`/panic output use). No allocation anywhere in this path.
//
// Locking/timestamp semantics are mirrored exactly from `printf_rust`
// above, at the same granularity (once per `kprint!`/`kprintln!` call,
// matching "once per `printf()` call" in the C-derived version):
//   - `PR_LOCKING`/`PR_LOCK`: identical acquire/release around the whole
//     call (skipped pre-`printfinit()` and for the panic path, which
//     disables locking via `__panic_start` exactly as before).
//   - `NNEWLINE`: the same swap-then-maybe-print-timestamp dance, and the
//     same "any '\n' byte anywhere in this call's output clears the flag"
//     rule (not just a trailing '\n') -- see [`Console::write_str`].
//
// Call-site migration rule (see the P3-2 worker report): every original
// `printf(fmt, args...)` call site became exactly one `kprint!`/`kprintln!`
// call at the same position, with the same argument count/order -- calls
// were never merged or split, specifically so this timestamp-per-call
// granularity stays byte-identical to the pre-migration boot log (modulo
// the timestamp values/addresses themselves).
// ===========================================================================

/// The kernel console output sink for [`kprint!`]/[`kprintln!`]. Implements
/// [`core::fmt::Write`] by forwarding each formatted chunk, unbuffered,
/// through the exact same [`crate::console::consputs`] path `printf_rust`'s
/// `flush()` uses -- no allocation, no new buffering, so output is
/// byte-identical to the legacy `printf()` for the same content.
pub struct Console;

impl core::fmt::Write for Console {
    fn write_str(&mut self, s: &str) -> core::fmt::Result {
        if !s.is_empty() {
            // SAFETY: `s` is a valid `&str` (borrow-checked, UTF-8, backed
            // by real memory for its full length); `consputs` only reads
            // exactly `s.len()` bytes starting at `s.as_ptr()`, matching
            // the slice's own bounds.
            unsafe {
                crate::console::consputs(s.as_ptr() as *const c_char, s.len() as c_int);
            }
            // Mirrors printf_rust's per-byte `if cx == b'\n' {
            // NNEWLINE.store(false, ...) }` inside its format loop: ANY
            // newline byte in this chunk clears the flag, even if this
            // chunk has trailing non-newline bytes after it (an
            // intentionally-preserved quirk of the original -- see the
            // module doc above).
            if s.as_bytes().contains(&b'\n') {
                NNEWLINE.store(false, Ordering::Release);
            }
        }
        Ok(())
    }
}

/// Backend for the [`kprint!`]/[`kprintln!`] macros. Not normally called
/// directly -- use the macros, which build the [`core::fmt::Arguments`]
/// via `format_args!`.
pub fn _kprint(args: core::fmt::Arguments<'_>) {
    // Same locking discipline as printf_rust: skip the lock entirely
    // before printfinit() / after a panic disables it (PR_LOCKING is
    // cleared by __panic_start with Release ordering; this Acquire load
    // observes that on any core).
    let locking = PR_LOCKING.load(Ordering::Acquire);
    if locking {
        spin_lock(&raw mut PR_LOCK);
    }

    // Same fresh-line timestamp dance as printf_rust: print once per
    // call, only if the previous call (on any core, thanks to the
    // shared atomic) ended with a newline byte.
    if !NNEWLINE.swap(true, Ordering::Acquire) {
        let _ = write!(Console, "[{}] ", machine::read_time());
    }

    let _ = Console.write_fmt(args);

    if locking {
        spin_unlock(&raw mut PR_LOCK);
    }
}

/// Format and print to the kernel console, exactly like the C-derived
/// `printf()` above but via `core::fmt` (no C-variadic ABI, no allocation).
/// See the module section doc above for the exact locking/timestamp
/// semantics this preserves.
#[macro_export]
macro_rules! kprint {
    ($($arg:tt)*) => {
        $crate::printf::_kprint(::core::format_args!($($arg)*))
    };
}

/// Like [`kprint!`], with a trailing newline appended to the format
/// string. The format string must be a string literal (matching every
/// call site this macro replaces -- the original C `printf()` calls it
/// supersedes always passed a literal format string too).
#[macro_export]
macro_rules! kprintln {
    () => {
        $crate::kprint!("\n")
    };
    ($fmt:literal) => {
        $crate::kprint!(::core::concat!($fmt, "\n"))
    };
    // `$($arg:tt)*` (rather than `$($arg:expr),*`) so that named/width
    // arguments -- e.g. the dynamic-width `"{:w$}", "", w = indent` idiom
    // used to reproduce C's `%*s` padding in `mm/vm_pgtab.rs`'s page-table
    // dump -- forward through to `format_args!` unchanged; a plain
    // `$arg:expr` matcher cannot capture `w = expr`.
    ($fmt:literal, $($arg:tt)*) => {
        $crate::kprint!(::core::concat!($fmt, "\n"), $($arg)*)
    };
}

/// `%s`-equivalent `Display` adapter for a raw NUL-terminated C string
/// pointer, matching `printf_rust`'s `%s` semantics exactly: a null
/// pointer prints as `(null)`; otherwise the string's bytes are copied
/// through (falling back to a byte-for-byte, non-UTF-8-validated pass
/// if the string isn't valid UTF-8 -- the legacy `%s` handler never
/// validated encoding either, it just walked bytes to the NUL).
pub struct Cs(pub *const c_char);

impl core::fmt::Display for Cs {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        let p = if self.0.is_null() { c"(null)".as_ptr() } else { self.0 };
        // SAFETY: `p` is either the caller-supplied NUL-terminated C
        // string (every `Cs` construction site's contract, matching the
        // original `%s` vararg contract) or the `"(null)"` literal above.
        let cstr = unsafe { core::ffi::CStr::from_ptr(p) };
        match cstr.to_str() {
            Ok(s) => f.write_str(s),
            Err(_) => {
                for &b in cstr.to_bytes() {
                    f.write_char(b as char)?;
                }
                Ok(())
            }
        }
    }
}

/// `%p`-equivalent `Display` adapter, matching `printptr`'s exact output:
/// `0x` followed by all 16 lowercase hex nibbles of the 64-bit value
/// (always full width, never trimmed) -- deliberately not using
/// `core::fmt`'s built-in `{:p}` (`Pointer`) formatting, whose exact
/// zero-padding behavior isn't part of any stability guarantee this
/// kernel wants to depend on for boot-log byte-fidelity.
pub struct Ptr(pub u64);

impl core::fmt::Display for Ptr {
    fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
        write!(f, "0x{:016x}", self.0)
    }
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
            crate::kprintln!(
                "[Core: {}] No thread context, fp={}",
                machine::cpuid() as i64,
                Ptr(fp),
            );
            if BT_ENABLED.load(Ordering::Relaxed) {
                // Use KERNBASE to PHYSTOP as conservative stack bounds.
                print_backtrace(fp, __physical_memory_start, __physical_memory_end);
            }
            return;
        }
        crate::kprintln!(
            "[Core: {}] In thread {} ({}) at {}",
            machine::cpuid() as i64,
            (*p).pid,
            Cs((*p).name.as_ptr()),
            Ptr(fp),
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

    // Send IPI to all harts (including self) to crash. `ipi_send_all`
    // takes a reason code; no preconditions beyond "callable from any
    // context", which holds here (we are already committed to halting).
    ipi_send_all(IPI_REASON_CRASH);

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
