//! Core TTY line discipline.
//!
//! Rust port of `kernel/tty/tty.c` (Phase 2 Wave 11, see
//! `docs/rustify/phase2_plan.md`). Implements terminal allocation,
//! reference counting, line-discipline processing (canonical / raw),
//! echo, output post-processing, signal generation, and ioctl
//! handling.
//!
//! Each tty owns an input pipe (driver -> reader) and an output pipe
//! (writer -> driver). The line discipline sits between the driver and
//! the pipes: [`tty_input`] processes incoming characters through the
//! discipline before pushing them into the input pipe, while
//! [`tty_write`] performs output post-processing before pushing data
//! into the output pipe.
//!
//! # Locking / concurrency
//!
//! `tty->lock` (a [`crate::sync::KSpinlock`]) protects `termios`,
//! `winsize`, and the raw-mode ring buffer (`raw_buf`/`raw_r`/`raw_w`).
//! [`tty_input`] and [`tty_echo_char`] both need to call into
//! `pipe_write()`, which can sleep (the pipe may be full) — exactly
//! like the C original, the lock is dropped immediately before any such
//! call and re-acquired immediately after; the state machine below
//! keeps that drop/reacquire pattern at the same points the C did, via
//! an owned [`crate::sync::KSpinGuard`] that gets `drop()`-ed and
//! re-`lock()`-ed explicitly rather than nested/hidden in a helper.
//!
//! `tty_input()` is called only from thread context (never from an
//! interrupt handler directly): `console.rs`'s `consoleintr()` (real
//! interrupt context) only stages bytes into a lock-free ring buffer;
//! a dedicated kernel thread (`console_tty_input_thread`) drains that
//! ring and calls `tty_input()`. `pty.rs`'s `pty_master_write()` (the
//! other caller) also always runs in the writing thread's context. So
//! `tty_input()` sleeping under the covers (via `pipe_write`) is sound
//! — there is no interrupt-context caller to violate a no-sleep rule.
//!
//! `tty_read()`'s raw-mode path parks on `tty->raw_wait` via `tq_wait`,
//! which atomically drops `tty->lock`, sleeps, and re-acquires it on
//! wakeup or signal delivery — identical to the C original's
//! `tq_wait(&tty->raw_wait, &tty->lock, NULL)`.

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_short, c_void};
use core::mem::MaybeUninit;

use crate::bindings::{pid_t, pipe, session, slab_cache_t, spinlock_t, termios, thread, tty, tty_ops};
use crate::sync::{KSpinGuard, KSpinlock};
// P3-D2a: proc/thread_queue.rs primitives, reached as plain crate-path
// items instead of `extern "C"` redeclarations.
use crate::proc::{tq_init, tq_wait, tq_wakeup_all};
// P3-D2b: `signal_pending`/`kill_proc` (proc/signal.rs) and `pgroup_kill`
// (proc/pgroup.rs) are plain crate-path items now that their
// `#[no_mangle]` exports are gone (identical signatures).
use crate::proc::{kill_proc, pgroup_kill, signal_pending};

use super::session::{session_get_fg_pgid, session_set_ctrl_tty, session_set_fg_pgid};
use super::termios::termios_init_default;
// P3-1C mesh sweep: vfs/pipe.rs is in scope for this wave; converted from
// `extern "C"` redeclarations to plain crate-path items (identical
// signatures).
use crate::vfs::pipe::{pipe_alloc, pipe_close, pipe_read, pipe_write};

// ===========================================================================
// Native uabi `struct termios` + `struct winsize` — P3-4b nativization
// (user directive: remove the C-compatible interfaces; userspace-ABI
// scrutiny class). `Termios`/`Winsize` are the canonical KERNEL-SIDE
// definitions of `kernel/inc/uabi/termios.h`'s `struct termios`/
// `struct winsize`: `build.rs` blocklists the bindgen emissions and
// re-exports them as `crate::bindings::termios`/`winsize` (facade
// `pub use`, N2 pattern).
//
// *** USERSPACE ABI — HANDLE WITH P3-4 SCRUTINY *** The C header STAYS:
// user/ programs (sh's line editing via tcgetattr/tcsetattr wrappers)
// compile against uabi/termios.h, and the kernel copies both records BY
// VALUE across the boundary (TCGETS/TCSETS* and TIOCGWINSZ/TIOCSWINSZ
// ioctls — `either_copyout`/`either_copyin` in vfs_syscall.rs and the
// tty ioctl paths below). A layout slip here silently breaks shell
// line editing/raw mode. The byte-exact asserts below pin the natives
// to the header. HOST determination: no host-side tool consumes any
// uabi header (grep-verified), so the gcc probe is target-only.
//
// DERIVE DECISION (P3-4b): Copy + Clone, exactly as the
// pre-nativization bindgen output derived (plain scalar/array fields).
// `Tty` (below) embeds both BY VALUE — its own derive line needs
// build.rs's `NativeTypeCallbacks` Copy=Yes answers for them.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + toolchain-gcc `_Static_assert` probe (rv64gc/lp64d —
// scratchpad p3_4b_uabi_probe.c); both agree on every value asserted
// below (NCCS == 16 confirmed by the same probe).
// ===========================================================================

/// Native uabi `struct termios` (`kernel/inc/uabi/termios.h`) — the
/// terminal mode record exchanged with userspace via TCGETS/TCSETS*.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Termios {
    /// Input mode flags.
    pub c_iflag: crate::bindings::tcflag_t,
    /// Output mode flags.
    pub c_oflag: crate::bindings::tcflag_t,
    /// Control mode flags.
    pub c_cflag: crate::bindings::tcflag_t,
    /// Local mode flags.
    pub c_lflag: crate::bindings::tcflag_t,
    /// Control characters (`NCCS == 16`).
    pub c_cc: [crate::bindings::cc_t; 16],
    /// Input speed.
    pub c_ispeed: crate::bindings::speed_t,
    /// Output speed.
    pub c_ospeed: crate::bindings::speed_t,
}

/// Native uabi `struct winsize` (`kernel/inc/uabi/termios.h`) — the
/// window-size record exchanged with userspace via TIOCGWINSZ/TIOCSWINSZ.
#[repr(C)]
#[derive(Copy, Clone)]
pub struct Winsize {
    /// Rows, in characters.
    pub ws_row: crate::bindings::uint16,
    /// Columns, in characters.
    pub ws_col: crate::bindings::uint16,
    /// Horizontal size, in pixels.
    pub ws_xpixel: crate::bindings::uint16,
    /// Vertical size, in pixels.
    pub ws_ypixel: crate::bindings::uint16,
}

// P3-4b hardcoded layout proof — the USERSPACE byte contract
// (`uabi/termios.h` `struct termios`/`struct winsize`), every field.
// Values captured from the pre-nativization bindgen output via the
// temporary in-tree `offset_of!` gate and cross-checked by the target
// gcc probe.
const _: () = {
    assert!(core::mem::size_of::<Termios>() == 40, "termios size (USERSPACE ABI)");
    assert!(core::mem::align_of::<Termios>() == 4, "termios alignment");
    assert!(core::mem::offset_of!(Termios, c_iflag) == 0, "termios.c_iflag offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Termios, c_oflag) == 4, "termios.c_oflag offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Termios, c_cflag) == 8, "termios.c_cflag offset (USERSPACE ABI)");
    assert!(
        core::mem::offset_of!(Termios, c_lflag) == 12,
        "termios.c_lflag offset (USERSPACE ABI)"
    );
    assert!(core::mem::offset_of!(Termios, c_cc) == 16, "termios.c_cc offset (USERSPACE ABI)");
    assert!(
        core::mem::offset_of!(Termios, c_ispeed) == 32,
        "termios.c_ispeed offset (USERSPACE ABI)"
    );
    assert!(
        core::mem::offset_of!(Termios, c_ospeed) == 36,
        "termios.c_ospeed offset (USERSPACE ABI)"
    );
    assert!(core::mem::size_of::<Winsize>() == 8, "winsize size (USERSPACE ABI)");
    assert!(core::mem::align_of::<Winsize>() == 2, "winsize alignment");
    assert!(core::mem::offset_of!(Winsize, ws_row) == 0, "winsize.ws_row offset (USERSPACE ABI)");
    assert!(core::mem::offset_of!(Winsize, ws_col) == 2, "winsize.ws_col offset (USERSPACE ABI)");
    assert!(
        core::mem::offset_of!(Winsize, ws_xpixel) == 4,
        "winsize.ws_xpixel offset (USERSPACE ABI)"
    );
    assert!(
        core::mem::offset_of!(Winsize, ws_ypixel) == 6,
        "winsize.ws_ypixel offset (USERSPACE ABI)"
    );
};

// ===========================================================================
// Native tty types — P3-N8 nativization (user directive: remove the
// C-compatible interfaces). `Tty`/`TtyOps` are the canonical native
// definitions of `kernel/inc/tty/tty_types.h`'s `struct tty`/`struct
// tty_ops`: `build.rs` blocklists the bindgen emissions and re-exports
// these types as `crate::bindings::tty`/`tty_ops` (facade `pub use`, N2
// pattern). The C header stays unchanged (no C consumers remain — the
// kernel tree has zero `.c` files).
//
// `termios`/`winsize` stayed bindgen-emitted through N8..P3-4a
// (userspace-ABI layout contracts, the P3-4 scrutiny class) and are
// native since P3-4b — [`Termios`]/[`Winsize`] above; `Tty`'s embedded
// fields re-pointed transparently via their `crate::bindings` paths.
//
// Layout evidence: temporary in-tree `offset_of!` gate on the live
// bindgen forms + cross-compiler value probe (toolchain gcc,
// rv64gc/lp64d — scratchpad p3n8_probe_values.c); both agree on every
// value asserted below (no pipe-style divergence). Note `lock`
// occupies [0,24) and `termios` starts at 24: the C
// `__ALIGNED_CACHELINE` rides the `spinlock_t` typedef and affects only
// the field's START alignment (here offset 0), not its size — both
// compilers agree, and the struct-level align(64) carries the C
// record's own 64-byte alignment, exactly as bindgen emitted.
//
// DERIVE DECISION (P3-N8): both types derive Copy/Clone exactly as the
// pre-nativization bindgen emissions did (`tty`'s embedded
// `spinlock_t`/`tq_t`/`termios`/`winsize` are all Copy). Nothing in the
// remaining bindgen output embeds either type by value (verified — all
// uses are pointers).
// ===========================================================================

/// Native `struct tty_ops` (`kernel/inc/tty/tty_types.h`) — the
/// per-terminal line-discipline/driver operations table (fn-pointer
/// `Option` forms reproduced verbatim; trait-ification is P3-10).
#[repr(C)]
#[derive(Copy, Clone)]
pub struct TtyOps {
    pub open: Option<unsafe extern "C" fn(tty: *mut Tty) -> c_int>,
    pub close: Option<unsafe extern "C" fn(tty: *mut Tty)>,
    pub hangup: Option<unsafe extern "C" fn(tty: *mut Tty)>,
    pub throttle: Option<unsafe extern "C" fn(tty: *mut Tty)>,
    pub unthrottle: Option<unsafe extern "C" fn(tty: *mut Tty)>,
    pub stop: Option<unsafe extern "C" fn(tty: *mut Tty)>,
    pub start: Option<unsafe extern "C" fn(tty: *mut Tty)>,
    pub discard_input: Option<unsafe extern "C" fn(tty: *mut Tty)>,
    pub read: Option<unsafe extern "C" fn(tty: *mut Tty, buf: *mut c_char, nr: usize) -> isize>,
    pub write: Option<unsafe extern "C" fn(tty: *mut Tty, buf: *const c_char, nr: usize) -> isize>,
    pub rx: Option<unsafe extern "C" fn(tty: *mut Tty, buf: *const c_char, nr: usize) -> isize>,
    pub tx: Option<unsafe extern "C" fn(tty: *mut Tty, buf: *mut c_char, nr: usize) -> isize>,
    pub set_termios: Option<unsafe extern "C" fn(tty: *mut Tty, new_termios: *mut termios)>,
    pub set_winsize:
        Option<unsafe extern "C" fn(tty: *mut Tty, new_winsize: *mut crate::bindings::winsize)>,
    pub ioctl: Option<
        unsafe extern "C" fn(tty: *mut Tty, cmd: crate::bindings::uint64, arg: *mut c_void) -> c_int,
    >,
}

/// Native `struct tty` (`kernel/inc/tty/tty_types.h`) — one terminal:
/// termios/winsize state, the canonical-mode pipes, the raw-mode ring
/// buffer, and the owning session.
#[repr(C, align(64))]
#[derive(Copy, Clone)]
pub struct Tty {
    pub lock: spinlock_t,
    pub termios: termios,
    pub winsize: crate::bindings::winsize,
    pub ops: *mut TtyOps,
    pub ref_count: c_int,
    pub input_pipe: *mut pipe,
    pub output_pipe: *mut pipe,
    pub raw_buf: [c_char; 256],
    pub raw_r: crate::bindings::uint,
    pub raw_w: crate::bindings::uint,
    pub raw_wait: crate::bindings::tq_t,
    pub driver_data: *mut c_void,
    pub session: *mut session,
    pub name: [c_char; 64],
}

// P3-N8 hardcoded layout proof — values captured from the
// pre-nativization bindgen output via the temporary in-tree
// `offset_of!` gate and cross-checked by the gcc probe (see the module
// note above).
const _: () = {
    assert!(core::mem::size_of::<TtyOps>() == 120, "tty_ops size");
    assert!(core::mem::align_of::<TtyOps>() == 8, "tty_ops alignment");
    assert!(core::mem::offset_of!(TtyOps, open) == 0, "tty_ops.open offset");
    assert!(core::mem::offset_of!(TtyOps, close) == 8, "tty_ops.close offset");
    assert!(core::mem::offset_of!(TtyOps, hangup) == 16, "tty_ops.hangup offset");
    assert!(core::mem::offset_of!(TtyOps, throttle) == 24, "tty_ops.throttle offset");
    assert!(core::mem::offset_of!(TtyOps, unthrottle) == 32, "tty_ops.unthrottle offset");
    assert!(core::mem::offset_of!(TtyOps, stop) == 40, "tty_ops.stop offset");
    assert!(core::mem::offset_of!(TtyOps, start) == 48, "tty_ops.start offset");
    assert!(core::mem::offset_of!(TtyOps, discard_input) == 56, "tty_ops.discard_input offset");
    assert!(core::mem::offset_of!(TtyOps, read) == 64, "tty_ops.read offset");
    assert!(core::mem::offset_of!(TtyOps, write) == 72, "tty_ops.write offset");
    assert!(core::mem::offset_of!(TtyOps, rx) == 80, "tty_ops.rx offset");
    assert!(core::mem::offset_of!(TtyOps, tx) == 88, "tty_ops.tx offset");
    assert!(core::mem::offset_of!(TtyOps, set_termios) == 96, "tty_ops.set_termios offset");
    assert!(core::mem::offset_of!(TtyOps, set_winsize) == 104, "tty_ops.set_winsize offset");
    assert!(core::mem::offset_of!(TtyOps, ioctl) == 112, "tty_ops.ioctl offset");

    assert!(core::mem::size_of::<Tty>() == 512, "tty size");
    assert!(core::mem::align_of::<Tty>() == 64, "tty alignment");
    assert!(core::mem::offset_of!(Tty, lock) == 0, "tty.lock offset");
    assert!(core::mem::offset_of!(Tty, termios) == 24, "tty.termios offset");
    assert!(core::mem::offset_of!(Tty, winsize) == 64, "tty.winsize offset");
    assert!(core::mem::offset_of!(Tty, ops) == 72, "tty.ops offset");
    assert!(core::mem::offset_of!(Tty, ref_count) == 80, "tty.ref_count offset");
    assert!(core::mem::offset_of!(Tty, input_pipe) == 88, "tty.input_pipe offset");
    assert!(core::mem::offset_of!(Tty, output_pipe) == 96, "tty.output_pipe offset");
    assert!(core::mem::offset_of!(Tty, raw_buf) == 104, "tty.raw_buf offset");
    assert!(core::mem::offset_of!(Tty, raw_r) == 360, "tty.raw_r offset");
    assert!(core::mem::offset_of!(Tty, raw_w) == 364, "tty.raw_w offset");
    assert!(core::mem::offset_of!(Tty, raw_wait) == 368, "tty.raw_wait offset");
    assert!(core::mem::offset_of!(Tty, driver_data) == 416, "tty.driver_data offset");
    assert!(core::mem::offset_of!(Tty, session) == 424, "tty.session offset");
    assert!(core::mem::offset_of!(Tty, name) == 432, "tty.name offset");
};

// ===========================================================================
// Externs.
// ===========================================================================

// P3-D3c: `printf.rs`'s panic plumbing fns are plain (safe) Rust fns now
// that their `#[no_mangle]` exports are gone -- crate-path imports.
use crate::printf::{__panic_end, __panic_start};

unsafe extern "C" {
    pub safe fn safestrcpy(s: *mut c_char, t: *const c_char, n: usize) -> *mut c_char;

}

// P3-D3a: `either_copyin`/`either_copyout` (mm/vm.rs) are ordinary (safe)
// Rust fns now that their `#[no_mangle]` exports are gone; reached as
// crate-path items instead of the `extern "C"` redeclarations that used
// to sit in the block above (identical signatures).
use crate::mm::{either_copyin, either_copyout};

// `slab_alloc`/`slab_free`/`slab_cache_init` are genuinely `unsafe fn` in
// `crate::mm::slab`; this file's original extern declarations asserted
// `pub safe fn` (usual FFI facade) and typed the cache pointer as the
// bindgen `slab_cache_t` rather than `crate::mm::slab::SlabCache` (same
// layout, "locally convenient pointer type" idiom — see `cffi::raw`'s
// identical note). Thin cast + safe-facade wrappers preserve both.
/// SAFETY: see [`crate::mm::slab::slab_cache_init`]'s contract.
#[inline]
fn slab_cache_init(
    cache: *mut slab_cache_t, name: *mut c_char, obj_size: usize, flags: u64,
) -> c_int {
    unsafe {
        crate::mm::slab_cache_init(cache as *mut crate::mm::slab::SlabCache, name, obj_size, flags)
    }
}
/// SAFETY: `cache` must originate from `slab_cache_init` above.
#[inline]
fn slab_alloc(cache: *mut slab_cache_t) -> *mut c_void {
    unsafe { crate::mm::slab_alloc(cache as *mut crate::mm::slab::SlabCache) }
}
/// SAFETY: `obj` must originate from `slab_alloc` above.
#[inline]
fn slab_free(obj: *mut c_void) {
    unsafe { crate::mm::slab_free(obj) };
}

// ===========================================================================
// Constants.
// ===========================================================================

const ENOMEM: c_int = 12;
const EFAULT: c_int = 14;
const EINVAL: c_int = 22;
const EINTR: c_int = 4;
const ENOTTY: c_int = 25;
const EPERM: c_int = 1;

/// `c_cc` indices actually used by the line discipline
/// (`kernel/inc/uabi/termios.h`).
const VINTR: usize = 0;
const VQUIT: usize = 1;
const VERASE: usize = 2;
const VKILL: usize = 3;
const VEOF: usize = 4;
const VSUSP: usize = 9;

// c_iflag
const ISTRIP: u32 = 0x0020;
const INLCR: u32 = 0x0040;
const IGNCR: u32 = 0x0080;
const ICRNL: u32 = 0x0100;

// c_oflag
const OPOST: u32 = 0x0001;
const ONLCR: u32 = 0x0002;

// c_lflag
const ISIG: u32 = 0x0001;
const ICANON: u32 = 0x0002;
const ECHO: u32 = 0x0008;
const ECHOE: u32 = 0x0010;
const ECHOK: u32 = 0x0020;

const SIGINT: c_int = 2;
const SIGQUIT: c_int = 3;
const SIGTSTP: c_int = 20;

/// `kernel/inc/tty/tty_types.h`.
const TTY_RAW_BUF_SIZE: u32 = 256;

/// Sentinel raw byte pushed through [`tty_echo_char`] for the erase
/// (backspace) visual sequence (`BS SPC BS`). Distinct from the literal
/// `'\b'` (0x08) byte, which is one of the *input* erase triggers.
const BACKSPACE: i32 = 0x100;

/// `kernel/inc/uabi/termios.h` ioctl request numbers.
const TCGETS: u64 = 0x5401;
const TCSETS: u64 = 0x5402;
const TCSETSW: u64 = 0x5403;
const TCSETSF: u64 = 0x5404;
const TIOCGWINSZ: u64 = 0x5413;
const TIOCSWINSZ: u64 = 0x5414;
const TIOCGPGRP: u64 = 0x540F;
const TIOCSPGRP: u64 = 0x5410;
const TIOCSCTTY: u64 = 0x540E;

/// `kernel/inc/uabi/termios.h`.
const DEFAULT_ROWS: u16 = 24;
const DEFAULT_COLS: u16 = 80;

/// `kernel/inc/uabi/poll.h`.
const POLLIN: c_short = 0x0001;
const POLLOUT: c_short = 0x0004;
const POLLRDNORM: c_short = 0x0040;
const POLLWRNORM: c_short = 0x0100;

// `is_err`/`err_ptr`'s canonical home is `crate::kstd` (P3-CS2
// centralization).
use crate::kstd::{err_ptr, is_err};

// ===========================================================================
// Panic helper -- replicates the C `assert(expr, fmt, arg)` macro
// expansion (same pattern as `kernel/tty/tty_dev.rs::tty_dev_assert_errno`).
// ===========================================================================

// P3-2 (zero-C wave): `tty_assert_errno` used to take its assertion
// message as a runtime `&CStr` embedding a `%d` -- `core::fmt`'s
// `format_args!` requires a literal format string, so (matching
// `tty_dev.rs`'s equivalent fix) its one call site below was inlined
// directly instead of kept as a helper.

// ===========================================================================
// Slab cache.
// ===========================================================================

/// `UnsafeCell<MaybeUninit<T>>` + `unsafe impl Sync` for one file-scope
/// lock/cache storage cell. Same pattern as `kernel/kobject.rs`'s
/// `SyncCell<T>` (private there, so re-declared locally here — the
/// established crate convention, see e.g. `irq_core.rs`'s `CacheCell`,
/// `pcache.rs`'s `SyncCell`, `workqueue.rs`'s `CacheCell`).
struct SyncCell<T>(UnsafeCell<MaybeUninit<T>>);
// SAFETY: every instance below is mutated exclusively through the C
// kernel's own synchronised entry points (`slab_cache_init`/`slab_alloc`
// internally lock; `spin_init`/`KSpinlock` guard the raw lock storage).
unsafe impl<T> Sync for SyncCell<T> {}
impl<T> SyncCell<T> {
    const fn uninit() -> Self {
        SyncCell(UnsafeCell::new(MaybeUninit::uninit()))
    }
    #[inline(always)]
    fn get(&self) -> *mut T {
        self.0.get() as *mut T
    }
}

static TTY_CACHE: SyncCell<slab_cache_t> = SyncCell::uninit();

pub(crate) extern "C" fn tty_init() {
    // SAFETY: `TTY_CACHE` is written here (its first use) before any
    // other `tty_*` entry point can run (mirrors the C original's
    // `start_kernel.c`-ordered single call); `slab_cache_init`
    // unconditionally overwrites every field of the cache descriptor.
    let ret = unsafe {
        slab_cache_init(
            TTY_CACHE.get(),
            c"tty_cache".as_ptr() as *mut c_char,
            core::mem::size_of::<tty>(),
            crate::bindings::SLAB_FLAG_STATIC as u64,
        )
    };
    if ret != 0 {
        __panic_start();
        crate::kprintln!(
            "ASSERTION_FAILURE {}:{}: In function '{}':",
            "kernel/tty/tty.rs",
            line!(),
            "tty_init",
        );
        crate::kprintln!("tty_init: failed to init tty_cache slab, errno={}", ret);
        __panic_end();
    }
}

// ===========================================================================
// Allocation / reference counting.
// ===========================================================================

/// Allocate a new `tty`, along with its input/output pipes.
///
/// # Safety
/// `name` must be a valid, NUL-terminated C string for the duration of
/// this call. `ops`, if non-null, must remain live for as long as the
/// returned `tty` (every `tty_*` dispatcher checks `ops` for null
/// before use, and the pointer is stored, not copied-from).
pub(crate) unsafe extern "C" fn tty_alloc(name: *const c_char, ops: *mut tty_ops) -> *mut tty {
    let raw = slab_alloc(TTY_CACHE.get()) as *mut tty;
    if raw.is_null() {
        return err_ptr(-ENOMEM);
    }

    // Two blocking pipes: input (driver->user) and output (user->driver).
    let inp = pipe_alloc(0);
    if is_err(inp) {
        slab_free(raw as *mut c_void);
        // Propagate the encoded ERR_PTR value, reinterpreted as `*mut
        // tty` -- mirrors the C original's `return (struct tty *)inp;`.
        return inp as *mut tty;
    }
    let outp = pipe_alloc(0);
    if is_err(outp) {
        pipe_close(inp, 1);
        pipe_close(inp, 0);
        slab_free(raw as *mut c_void);
        return outp as *mut tty;
    }

    // SAFETY: `raw` is freshly allocated, exclusively-owned slab memory;
    // every field used elsewhere in this file is written below before
    // `raw` is returned to any other code (matches the C original's
    // field-by-field initialization -- no blanket zeroing was ever done
    // there either).
    unsafe {
        KSpinlock::from_bindings(&raw mut (*raw).lock).init(c"tty".as_ptr());
        termios_init_default(&raw mut (*raw).termios);
        (*raw).winsize.ws_row = DEFAULT_ROWS;
        (*raw).winsize.ws_col = DEFAULT_COLS;
        (*raw).winsize.ws_xpixel = 0;
        (*raw).winsize.ws_ypixel = 0;
        (*raw).ops = ops;
        (*raw).ref_count = 1;
        (*raw).input_pipe = inp;
        (*raw).output_pipe = outp;
        (*raw).raw_r = 0;
        (*raw).raw_w = 0;
        tq_init(&raw mut (*raw).raw_wait, c"tty_raw".as_ptr(), core::ptr::null_mut());
        (*raw).driver_data = core::ptr::null_mut();
        (*raw).session = core::ptr::null_mut();
        safestrcpy((*raw).name.as_mut_ptr(), name, (*raw).name.len());
    }

    raw
}

/// # Safety
/// `t`, if non-null, must be a `tty` previously returned by
/// [`tty_alloc`], not already freed, and not concurrently accessed.
pub(crate) unsafe extern "C" fn tty_free(t: *mut tty) {
    if t.is_null() {
        return;
    }
    // SAFETY: caller guarantees `t` is a live, exclusively-owned `tty`
    // (fn doc); the pipe pointers were set by `tty_alloc` and never
    // mutated afterward except here.
    unsafe {
        let inp = (*t).input_pipe;
        if !inp.is_null() {
            pipe_close(inp, 1);
            pipe_close(inp, 0);
        }
        let outp = (*t).output_pipe;
        if !outp.is_null() {
            pipe_close(outp, 1);
            pipe_close(outp, 0);
        }
        slab_free(t as *mut c_void);
    }
}

/// # Safety
/// `t` must be a live, non-null `tty` (mirrors the C original, which
/// also unconditionally dereferences `t->lock`).
pub(crate) unsafe extern "C" fn tty_ref(t: *mut tty) {
    // SAFETY: see fn doc.
    let guard = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
    unsafe {
        (*t).ref_count += 1;
    }
    drop(guard);
}

/// # Safety
/// `t` must be a live, non-null `tty` previously returned by
/// [`tty_alloc`] (mirrors the C original's unconditional dereference).
pub(crate) unsafe extern "C" fn tty_unref(t: *mut tty) {
    let should_free;
    {
        // SAFETY: see fn doc.
        let guard = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
        unsafe {
            (*t).ref_count -= 1;
            should_free = (*t).ref_count <= 0;
        }
        drop(guard);
    }
    if should_free {
        // SAFETY: `t` is caller-guaranteed live (fn doc); a ref_count
        // that just dropped to <= 0 means this call owns the last
        // reference, matching the C original's `tty_free(tty)` call.
        unsafe { tty_free(t) };
    }
}

// ===========================================================================
// Line-discipline flag helpers.
//
// Every helper below requires `t->lock` to already be held by the
// caller (they only ever run inside a locked critical section in this
// file) -- mirrors the C `static inline` helpers of the same name,
// which carried the same implicit contract.
// ===========================================================================

#[inline]
unsafe fn l_canon(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_lflag & ICANON != 0 }
}
#[inline]
unsafe fn l_echo(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_lflag & ECHO != 0 }
}
#[inline]
unsafe fn l_echoe(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_lflag & ECHOE != 0 }
}
#[inline]
unsafe fn l_echok(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_lflag & ECHOK != 0 }
}
#[inline]
unsafe fn l_isig(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_lflag & ISIG != 0 }
}
#[inline]
unsafe fn i_icrnl(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_iflag & ICRNL != 0 }
}
#[inline]
unsafe fn i_igncr(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_iflag & IGNCR != 0 }
}
#[inline]
unsafe fn i_inlcr(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_iflag & INLCR != 0 }
}
#[inline]
unsafe fn i_istrip(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_iflag & ISTRIP != 0 }
}
#[inline]
unsafe fn o_opost(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_oflag & OPOST != 0 }
}
#[inline]
unsafe fn o_onlcr(t: *mut tty) -> bool {
    unsafe { (*t).termios.c_oflag & ONLCR != 0 }
}

// ===========================================================================
// Echo a character to the output pipe.
// ===========================================================================

/// Echo one character (or the `BACKSPACE` erase sequence) to the
/// output pipe.
///
/// # Locking
/// `guard` must be the caller's currently-held `t->lock` guard. This
/// function drops it before calling `pipe_write()` (which can sleep if
/// the output pipe is full) and returns a freshly re-acquired guard --
/// the caller must use the returned guard from this point on, exactly
/// mirroring the C original's "drop the spinlock before calling
/// pipe_write(), re-lock after" comment.
///
/// # Safety
/// `t` must be a live `tty` whose `output_pipe` is non-null.
unsafe fn tty_echo_char(t: *mut tty, c: i32, guard: KSpinGuard) -> KSpinGuard {
    let mut echobuf = [0u8; 4];
    let mut n = 0usize;

    if c == BACKSPACE {
        // BS SPC BS
        echobuf[n] = b'\x08';
        n += 1;
        echobuf[n] = b' ';
        n += 1;
        echobuf[n] = b'\x08';
        n += 1;
    } else {
        // SAFETY: `t` is caller-guaranteed live (fn doc); `t->lock` is
        // still held at this point (guard not yet dropped).
        if unsafe { o_opost(t) && o_onlcr(t) } && c == b'\n' as i32 {
            echobuf[n] = b'\r';
            n += 1;
        }
        echobuf[n] = c as u8;
        n += 1;
    }

    // Drop the lock before calling pipe_write() -- it can sleep.
    drop(guard);
    // SAFETY: `t` is caller-guaranteed live (fn doc).
    unsafe { pipe_write((*t).output_pipe, echobuf.as_ptr() as *const c_char, n as u64, 0) };
    // SAFETY: `t` is caller-guaranteed live (fn doc).
    unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() }
}

// ===========================================================================
// Signal generation (foreground process group).
// ===========================================================================

/// Send a signal to the terminal's foreground process group, or to the
/// current process if there is no session / foreground group.
///
/// # Safety
/// `t` must be a live `tty`. Must be called with `t->lock` **not**
/// held (it dereferences `t->session` without the lock, matching the
/// C original -- see the call sites in [`tty_input`], all of which
/// drop the lock immediately before calling this).
unsafe fn tty_signal_fg_pgroup(t: *mut tty, signum: c_int) {
    // SAFETY: see fn doc.
    let sess = unsafe { (*t).session };
    if !sess.is_null() {
        let fg = session_get_fg_pgid(sess);
        if fg > 0 {
            pgroup_kill(fg, signum);
            return;
        }
    }
    // Fallback: no session or no fg group -- signal current process.
    let cur = crate::machine::current_thread_ptr();
    kill_proc(cur, signum);
}

// ===========================================================================
// Line-discipline input processing.
// ===========================================================================

/// Receive raw characters from the driver.
///
/// Called from thread context only (see the module doc). Applies input
/// flags, canonical editing, echo, and signal generation, then pushes
/// completed data into the input pipe. Returns the number of
/// characters consumed.
///
/// # Safety
/// `t` must be a live `tty`. `buf` must point to at least `count`
/// readable bytes.
pub(crate) unsafe extern "C" fn tty_input(t: *mut tty, buf: *const c_char, count: u64) -> i64 {
    let n = count as usize;
    let lock_ptr = unsafe { &raw mut (*t).lock };
    // SAFETY: `t` is caller-guaranteed live (fn doc).
    let mut guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };

    for i in 0..n {
        // SAFETY: caller guarantees `buf[..count]` is readable (fn doc).
        let raw_byte = unsafe { *buf.add(i) } as u8;
        let mut c = raw_byte as i32;

        // ---------- input flag processing ----------

        // SAFETY: `t` is live; `guard` is held.
        if unsafe { i_istrip(t) } {
            c &= 0x7F;
        }

        if c == b'\r' as i32 {
            // SAFETY: `t` is live; `guard` is held.
            if unsafe { i_igncr(t) } {
                continue;
            }
            // SAFETY: `t` is live; `guard` is held.
            if unsafe { i_icrnl(t) } {
                c = b'\n' as i32;
            }
        } else if c == b'\n' as i32 {
            // SAFETY: `t` is live; `guard` is held.
            if unsafe { i_inlcr(t) } {
                c = b'\r' as i32;
            }
        }

        // ---------- signal characters ----------

        // SAFETY: `t` is live; `guard` is held.
        if unsafe { l_isig(t) } {
            // SAFETY: `t` is live; `guard` is held; reading the whole
            // small `c_cc` array by value is a plain memcpy.
            let cc = unsafe { (*t).termios.c_cc };

            if c == cc[VINTR] as i32 {
                drop(guard);
                // SAFETY: `t` is live; lock dropped above (fn doc of
                // `tty_signal_fg_pgroup`).
                unsafe { tty_signal_fg_pgroup(t, SIGINT) };
                // SAFETY: `t` is live.
                guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };
                // SAFETY: `t` is live; `guard` is held.
                if unsafe { l_echo(t) } {
                    // SAFETY: `t` is live.
                    guard = unsafe { tty_echo_char(t, '^' as i32, guard) };
                    guard = unsafe { tty_echo_char(t, 'C' as i32, guard) };
                    guard = unsafe { tty_echo_char(t, '\n' as i32, guard) };
                }
                continue;
            }
            if c == cc[VQUIT] as i32 {
                drop(guard);
                // SAFETY: as above.
                unsafe { tty_signal_fg_pgroup(t, SIGQUIT) };
                guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };
                if unsafe { l_echo(t) } {
                    guard = unsafe { tty_echo_char(t, '^' as i32, guard) };
                    guard = unsafe { tty_echo_char(t, '\\' as i32, guard) };
                    guard = unsafe { tty_echo_char(t, '\n' as i32, guard) };
                }
                continue;
            }
            if c == cc[VSUSP] as i32 {
                drop(guard);
                unsafe { tty_signal_fg_pgroup(t, SIGTSTP) };
                guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };
                if unsafe { l_echo(t) } {
                    guard = unsafe { tty_echo_char(t, '^' as i32, guard) };
                    guard = unsafe { tty_echo_char(t, 'Z' as i32, guard) };
                    guard = unsafe { tty_echo_char(t, '\n' as i32, guard) };
                }
                continue;
            }
        }

        // ---------- canonical mode editing ----------

        // SAFETY: `t` is live; `guard` is held.
        if unsafe { l_canon(t) } {
            // SAFETY: `t` is live; `guard` is held.
            let cc = unsafe { (*t).termios.c_cc };

            // Erase (backspace / DEL).
            if c == cc[VERASE] as i32 || c == b'\x08' as i32 {
                // SAFETY: `t` is live; `guard` is held.
                if unsafe { l_echoe(t) } {
                    guard = unsafe { tty_echo_char(t, BACKSPACE, guard) };
                }
                continue;
            }

            // Kill line (^U).
            if c == cc[VKILL] as i32 {
                if unsafe { l_echok(t) } {
                    guard = unsafe { tty_echo_char(t, '\n' as i32, guard) };
                }
                continue;
            }

            // EOF (^D) -- push a zero-length "line" to unblock readers.
            if c == cc[VEOF] as i32 {
                drop(guard);
                let nul: u8 = 0;
                // SAFETY: `t` is live; lock dropped above.
                unsafe {
                    pipe_write((*t).input_pipe, &nul as *const u8 as *const c_char, 0, 0);
                }
                guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };
                continue;
            }
        }

        // ---------- echo ----------

        if unsafe { l_echo(t) } {
            guard = unsafe { tty_echo_char(t, c, guard) };
        }

        // ---------- push character to consumer ----------

        if unsafe { !l_canon(t) } {
            // Raw mode: store in the direct ring buffer and wake any
            // reader immediately (single-character latency).
            // SAFETY: `t` is live; `guard` is held.
            unsafe {
                let next = ((*t).raw_w + 1) & (TTY_RAW_BUF_SIZE - 1);
                if next != ((*t).raw_r & (TTY_RAW_BUF_SIZE - 1)) {
                    let idx = ((*t).raw_w & (TTY_RAW_BUF_SIZE - 1)) as usize;
                    (*t).raw_buf[idx] = c as u8 as c_char;
                    (*t).raw_w += 1;
                }
            }
            // tq_wakeup_all is safe under spinlock.
            unsafe { tq_wakeup_all(&raw mut (*t).raw_wait, 0, 0) };
        } else {
            // Canonical mode: push into input pipe.
            let ch = c as u8 as c_char;
            drop(guard);
            unsafe { pipe_write((*t).input_pipe, &ch as *const c_char, 1, 0) };
            guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };
        }
    }

    drop(guard);
    n as i64
}

// ===========================================================================
// Read (user -> input pipe).
// ===========================================================================

/// Read data from the terminal.
///
/// In canonical mode, reads go through the input pipe (blocks until a
/// full line is available). In raw mode, reads drain the tty's direct
/// ring buffer -- each character is available as soon as it arrives.
///
/// # Safety
/// `t` must be a live `tty`. `buf` must point to at least `count`
/// writable bytes if `user == 0`, or be a valid userspace address
/// range otherwise (checked internally by `either_copyout`).
pub(crate) unsafe extern "C" fn tty_read(t: *mut tty, buf: *mut c_char, count: u64, user: c_int) -> i64 {
    let lock_ptr = unsafe { &raw mut (*t).lock };

    let canon = {
        // SAFETY: `t` is caller-guaranteed live (fn doc).
        let _g = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };
        unsafe { l_canon(t) }
    };

    if canon {
        // SAFETY: `t` is live; `pipe_read` validates `buf` itself per
        // the userspace/kernel split encoded by `user`.
        return unsafe { pipe_read((*t).input_pipe, buf, count, user) };
    }

    // ---- Raw mode: read from direct ring buffer ----
    let mut total: i64 = 0;
    let target = count as i64;

    // SAFETY: `t` is caller-guaranteed live (fn doc).
    let mut guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };

    // The C original wraps this in `while ((size_t)total < count) { ...
    // break; }` -- the trailing `break` always executes on the first
    // (only) pass, so the loop is really "run the body once, unless
    // `count == 0`". Written as an `if` here for clarity; semantics
    // preserved 1:1 (single batch, `read(2)`-style return).
    if total < target {
        // Wait for at least one character.
        // SAFETY: `t` is live; `guard` is held.
        while unsafe { (*t).raw_r == (*t).raw_w } {
            let cur = crate::machine::current_thread_ptr();
            crate::machine::thread_state_set(cur, crate::bindings::thread_state_THREAD_INTERRUPTIBLE);
            // `tq_wait` releases `t->lock`, sleeps, and re-acquires it
            // on wake.
            unsafe { tq_wait(&raw mut (*t).raw_wait, lock_ptr, core::ptr::null_mut()) };
            if signal_pending(cur) {
                drop(guard);
                return if total > 0 { total } else { -(EINTR as i64) };
            }
        }

        // Drain available characters, up to count.
        // SAFETY: `t` is live; `guard` is held at loop entry each time.
        while total < target && unsafe { (*t).raw_r != (*t).raw_w } {
            let idx = unsafe { (*t).raw_r & (TTY_RAW_BUF_SIZE - 1) } as usize;
            let ch = unsafe { (*t).raw_buf[idx] };
            unsafe {
                (*t).raw_r += 1;
            }
            drop(guard);

            if user != 0 {
                let r = unsafe {
                    either_copyout(
                        1,
                        (buf as u64).wrapping_add(total as u64),
                        &ch as *const c_char as *mut c_void,
                        1,
                    )
                };
                if r < 0 {
                    return if total > 0 { total } else { -(EFAULT as i64) };
                }
            } else {
                unsafe {
                    *buf.add(total as usize) = ch;
                }
            }
            total += 1;
            guard = unsafe { KSpinlock::from_bindings(lock_ptr).lock() };
        }
    }
    drop(guard);

    total
}

// ===========================================================================
// Poll (check readiness without blocking).
// ===========================================================================

/// Reuses the already-audited `smp_load_acquire_i32` atomic-load helper
/// (`machine.rs`) for the `uint` (`u32`) pipe counters -- same-size
/// signed/unsigned reinterpret, the loaded bit pattern is identical
/// either way, only the carrying type differs. Shared with
/// `kernel/tty/ptmx.rs` via `pub(super)`.
#[inline]
pub(super) unsafe fn load_acquire_u32(p: *const u32) -> u32 {
    // SAFETY: caller provides a valid, aligned `*const u32` (matches
    // `smp_load_acquire_i32`'s own contract, modulo the reinterpret).
    unsafe { crate::machine::smp_load_acquire_i32(p as *const c_int) as u32 }
}

/// Check whether the terminal has data available.
///
/// In canonical mode, checks the input pipe for buffered line data. In
/// raw mode, checks the direct ring buffer. Output (`POLLOUT`) is
/// always reported ready since UART writes never block the caller.
///
/// # Safety
/// `t` must be a live `tty`.
pub(crate) unsafe extern "C" fn tty_poll(t: *mut tty, events: c_short) -> c_int {
    let mut revents: c_short = 0;

    // SAFETY: `t` is caller-guaranteed live (fn doc).
    let _guard = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };

    if events & (POLLIN | POLLRDNORM) != 0 {
        // SAFETY: `t` is live; `_guard` is held.
        if unsafe { l_canon(t) } {
            // Canonical mode: data ready if input pipe has bytes.
            // SAFETY: `t` is live.
            let pi = unsafe { (*t).input_pipe };
            // SAFETY: `pi` is a live pipe owned by `t` for its lifetime.
            let nwrite = unsafe { load_acquire_u32(&raw const (*pi).nwrite) };
            let nread = unsafe { load_acquire_u32(&raw const (*pi).nread) };
            if nwrite.wrapping_sub(nread) > 0 {
                revents |= events & (POLLIN | POLLRDNORM);
            }
        } else {
            // Raw mode: data ready if ring buffer is non-empty.
            // SAFETY: `t` is live; `_guard` is held.
            if unsafe { (*t).raw_r != (*t).raw_w } {
                revents |= events & (POLLIN | POLLRDNORM);
            }
        }
    }

    if events & (POLLOUT | POLLWRNORM) != 0 {
        // Output to the driver is always accepted.
        revents |= events & (POLLOUT | POLLWRNORM);
    }

    revents as c_int
}

// ===========================================================================
// Write (output pipe <- user).
// ===========================================================================

/// Write data to the terminal.
///
/// Applies output post-processing (OPOST / ONLCR) and pushes the
/// result into the output pipe.
///
/// # Safety
/// `t` must be a live `tty`. `buf` must point to at least `count`
/// readable bytes if `user == 0`, or be a valid userspace address
/// range otherwise (checked internally by `either_copyin`).
pub(crate) unsafe extern "C" fn tty_write(t: *mut tty, buf: *const c_char, count: u64, user: c_int) -> i64 {
    let need_opost = {
        // SAFETY: `t` is caller-guaranteed live (fn doc).
        let _g = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
        unsafe { o_opost(t) && o_onlcr(t) }
    };

    if !need_opost {
        // Fast path -- no post-processing.
        // SAFETY: `t` is live; `pipe_write` validates `buf` itself.
        return unsafe { pipe_write((*t).output_pipe, buf, count, user) };
    }

    // Slow path -- scan for '\n' and insert '\r' before it.
    let mut written: u64 = 0;
    while written < count {
        let mut batch = count - written;
        if batch > 32 {
            // leave room for \r expansion (kbuf is 64 bytes)
            batch = 32;
        }
        let batch_usize = batch as usize;

        let mut kbuf = [0u8; 64];
        if user != 0 {
            let r = unsafe {
                either_copyin(
                    kbuf.as_mut_ptr() as *mut c_void,
                    1,
                    (buf as u64).wrapping_add(written),
                    batch,
                )
            };
            if r < 0 {
                return if written > 0 { written as i64 } else { -(EFAULT as i64) };
            }
        } else {
            // SAFETY: caller guarantees `buf[..count]` is readable
            // kernel memory when `user == 0` (fn doc); `written + batch
            // <= count` by construction of `batch` above.
            unsafe {
                core::ptr::copy_nonoverlapping(
                    (buf as *const u8).add(written as usize),
                    kbuf.as_mut_ptr(),
                    batch_usize,
                );
            }
        }

        // Expand NL -> CRNL into a second buffer.
        let mut outbuf = [0u8; 128];
        let mut olen = 0usize;
        for j in 0..batch_usize {
            if kbuf[j] == b'\n' {
                outbuf[olen] = b'\r';
                olen += 1;
            }
            outbuf[olen] = kbuf[j];
            olen += 1;
        }

        let ret = unsafe { pipe_write((*t).output_pipe, outbuf.as_ptr() as *const c_char, olen as u64, 0) };
        if ret < 0 {
            return if written > 0 { written as i64 } else { ret };
        }

        written += batch;
    }

    written as i64
}

// ===========================================================================
// Output drain helper.
// ===========================================================================

/// Pull data from the output pipe. Drivers call this (or read the
/// output pipe directly) to obtain post-processed output bytes for
/// transmission.
///
/// # Safety
/// `t` must be a live `tty`. `buf` must point to at least `count`
/// writable kernel bytes.
pub(crate) unsafe extern "C" fn tty_output(t: *mut tty, buf: *mut c_char, count: u64) -> i64 {
    // SAFETY: `t` is caller-guaranteed live (fn doc).
    unsafe { pipe_read((*t).output_pipe, buf, count, 0) }
}

// ===========================================================================
// Ioctl.
// ===========================================================================

/// # Safety
/// `t` must be a live `tty`. `arg`'s required validity depends on
/// `cmd` (a `termios*`/`winsize*`/`pid_t*`/`int*` writable or readable
/// pointer as appropriate) -- same contract as the C original.
pub(crate) unsafe extern "C" fn tty_ioctl(t: *mut tty, cmd: u64, arg: *mut c_void) -> c_int {
    match cmd {
        TCGETS => {
            let tp = arg as *mut termios;
            // SAFETY: `t` is live (fn doc); `tp` is a valid writable
            // `termios*` for this cmd (fn doc).
            let _g = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
            unsafe { *tp = (*t).termios };
            0
        }
        TCSETS | TCSETSW | TCSETSF => {
            let tp = arg as *mut termios;
            {
                // SAFETY: see TCGETS arm.
                let _g = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
                unsafe { (*t).termios = *tp };
            }

            // Notify driver if it cares.
            // SAFETY: `t` is live; `t->ops`, if non-null, points to a
            // live `tty_ops` for `t`'s lifetime (tty_alloc's contract).
            unsafe {
                if let Some(ops) = (*t).ops.as_ref() {
                    if let Some(f) = ops.set_termios {
                        f(t, tp);
                    }
                }
            }

            // TCSETSF: also discard pending input.
            if cmd == TCSETSF {
                {
                    let _g = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
                    // flush raw ring buffer
                    unsafe { (*t).raw_r = (*t).raw_w };
                }
                unsafe {
                    if let Some(ops) = (*t).ops.as_ref() {
                        if let Some(f) = ops.discard_input {
                            f(t);
                        }
                    }
                }
            }

            0
        }
        TIOCGWINSZ => {
            let wsp = arg as *mut crate::bindings::winsize;
            let _g = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
            unsafe { *wsp = (*t).winsize };
            0
        }
        TIOCSWINSZ => {
            let wsp = arg as *mut crate::bindings::winsize;
            {
                let _g = unsafe { KSpinlock::from_bindings(&raw mut (*t).lock).lock() };
                unsafe { (*t).winsize = *wsp };
            }
            unsafe {
                if let Some(ops) = (*t).ops.as_ref() {
                    if let Some(f) = ops.set_winsize {
                        f(t, wsp);
                    }
                }
            }
            0
        }
        TIOCGPGRP => {
            // Return the foreground process group from the session.
            let pgidp = arg as *mut pid_t;
            unsafe { *pgidp = 0 };
            // SAFETY: `t` is live (fn doc).
            let sess = unsafe { (*t).session };
            if !sess.is_null() {
                unsafe { *pgidp = session_get_fg_pgid(sess) };
            }
            0
        }
        TIOCSPGRP => {
            // Set the foreground process group.
            let pgid = unsafe { *(arg as *mut pid_t) };
            if pgid <= 0 {
                return -EINVAL;
            }
            // Caller must be in the same session as the tty.
            let sess = unsafe { (*t).session };
            let cur = crate::machine::current_thread_ptr();
            let same_session = !sess.is_null() && unsafe { (*sess).sid == (*cur).sid };
            if !same_session {
                return -ENOTTY;
            }
            unsafe { session_set_fg_pgid(sess, pgid) };
            0
        }
        TIOCSCTTY => {
            // Set controlling terminal: the calling process must be a
            // session leader and must not already have a controlling
            // terminal (enforced by `session_set_ctrl_tty`).
            let cur = crate::machine::current_thread_ptr();
            let sess = unsafe { (*cur).session };
            if sess.is_null() {
                return -EPERM;
            }
            unsafe { session_set_ctrl_tty(sess, t) };
            0
        }
        _ => {
            // Let the driver handle unknown ioctls.
            // SAFETY: `t` is live.
            unsafe {
                if let Some(ops) = (*t).ops.as_ref() {
                    if let Some(f) = ops.ioctl {
                        return f(t, cmd, arg);
                    }
                }
            }
            -ENOTTY
        }
    }
}

// ===========================================================================
// Open / close via ops.
// ===========================================================================

/// # Safety
/// `t` must be a live `tty` previously returned by [`tty_alloc`].
pub(crate) unsafe extern "C" fn tty_open(t: *mut tty) -> c_int {
    // SAFETY: `t` is caller-guaranteed live (fn doc).
    unsafe { tty_ref(t) };
    unsafe {
        if let Some(ops) = (*t).ops.as_ref() {
            if let Some(f) = ops.open {
                return f(t);
            }
        }
    }
    0
}

/// # Safety
/// `t` must be a live `tty` previously returned by [`tty_alloc`].
pub(crate) unsafe extern "C" fn tty_close(t: *mut tty) {
    unsafe {
        if let Some(ops) = (*t).ops.as_ref() {
            if let Some(f) = ops.close {
                f(t);
            }
        }
    }
    // SAFETY: `t` is caller-guaranteed live (fn doc).
    unsafe { tty_unref(t) };
}

// ===========================================================================
// Hangup.
// ===========================================================================

/// # Safety
/// `t` must be a live `tty`.
pub(crate) unsafe extern "C" fn tty_hangup(t: *mut tty) {
    unsafe {
        if let Some(ops) = (*t).ops.as_ref() {
            if let Some(f) = ops.hangup {
                f(t);
            }
        }
    }
}
