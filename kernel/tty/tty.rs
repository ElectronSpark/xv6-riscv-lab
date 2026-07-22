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
//! N-R7h (lock-owns-data): the per-tty lock now OWNS the state it
//! guards. [`Tty::inner`] is a [`crate::sync::SpinLock`]`<`[`TtyInner`]`>`
//! embedded at offset 0; the lock protects `termios`, `winsize`,
//! `ref_count`, the raw-mode ring buffer (`raw_buf`/`raw_r`/`raw_w`) and
//! the raw wait queue (`raw_wait`) — every field inside [`TtyInner`]. A
//! `.lock()` returns a [`crate::sync::SpinLockGuard`] that
//! `Deref`s/`DerefMut`s straight to [`TtyInner`]. [`tty_input`] and
//! [`Tty::echo_char`] both need to call into `Pipe::pipe_write()`, which can
//! sleep (the pipe may be full) — exactly like the C original, the guard
//! is `drop()`-ed immediately before any such call and re-`.lock()`-ed
//! immediately after; the state machine below keeps that drop/reacquire
//! pattern at the same points the C did, threading the guard explicitly
//! rather than nesting/hiding it in a helper.
//!
//! The non-guarded fields (`ops`, `input_pipe`, `output_pipe`,
//! `driver_data`, `session`, `name`) stay plain `Tty` fields: they are
//! set once at [`tty_alloc`] and read lock-free thereafter (the pipe
//! pointers are also the `pty.rs` peer's data path; `session` is
//! dereferenced without the lock by [`Tty::signal_fg_pgroup`] and the
//! ioctl paths, matching the C original).
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
//! `tty_read()`'s raw-mode path parks on `raw_wait` via the guard's
//! [`crate::sync::SpinLockGuard::wait_on`] (`tq_wait`), which atomically
//! drops this lock, sleeps, and re-acquires it on wakeup or signal
//! delivery — identical to the C original's
//! `tq_wait(&tty->raw_wait, &tty->lock, NULL)`.

use core::cell::UnsafeCell;
use core::ffi::{c_char, c_int, c_short, c_void};
use core::mem::MaybeUninit;

use crate::bindings::{pid_t, pipe, session, slab_cache_t, termios, thread, tty};
use crate::kstd::{result_to_neg_errno, Errno, KResult};
use crate::sync::{SpinLock, SpinLockGuard};
// P3-D2a: proc/thread_queue.rs primitives, reached as plain crate-path
// items instead of `extern "C"` redeclarations.
// NO-STANDALONE-FN: the `tq_init`/`tq_wakeup_all` free-fn delegators were
// deleted; the two sites below build a `TqRef` handle via `from_ptr` and
// invoke the method. (`tq_wait` here is reached through the guard's
// `wait_on`, not called directly.)
use crate::proc::access::TqRef;
// P3-D2b: `signal_pending`/`kill_proc` (proc/signal.rs) and `pgroup_kill`
// (proc/pgroup.rs) are plain crate-path items now that their
// `#[no_mangle]` exports are gone (identical signatures).
use crate::proc::Pgroup;

use super::session::{
    session_get_fg_pgid, session_lookup, session_set_ctrl_tty, session_set_fg_pgid,
};
use super::termios::termios_init_default;
// P3-1C mesh sweep: vfs/pipe.rs is in scope for this wave; converted from
// `extern "C"` redeclarations to plain crate-path items (identical
// signatures).
use crate::vfs::pipe::Pipe;

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
// C-compatible interfaces). `Tty` is the canonical native definition
// of `kernel/inc/tty/tty_types.h`'s `struct tty`, re-exported as
// `crate::bindings::tty` (facade `pub use`, N2 pattern). The C header
// stays unchanged (no C consumers remain — the kernel tree has zero
// `.c` files). P3-10d: the C-shaped `struct tty_ops` fn-pointer table
// (and its `bindings::tty_ops` facade alias) is gone — [`TtyOps`]
// below is a real Rust trait now.
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
// DERIVE DECISION (P3-N8 / N-R7h): `Tty` no longer derives Copy/Clone --
// as of N-R7h it embeds a `SpinLock<TtyInner>` (an `UnsafeCell`, so not
// `Copy`). That is sound because `Tty` is only ever handled by pointer
// (`*mut tty`): nothing in the tree embeds it by value or copies it
// (verified -- all uses are pointers; `tty_alloc` fills a raw slab
// allocation field-by-field, never by value-copy). `TtyInner` likewise
// derives nothing (it too is only reached through the lock guard). The
// `termios`/`winsize` uabi records keep their own Copy/Clone derives
// (used by-value across the userspace ioctl boundary, unchanged).
// ===========================================================================

/// The per-terminal line-discipline/driver operations vtable — wave
/// P3-10d's replacement for the C-style `struct tty_ops` fn-pointer
/// table (`kernel/inc/tty/tty_types.h`), following the P3-10a
/// `FileOps` pilot pattern.
///
/// Implementors are zero-sized unit structs with a `static` instance
/// installed into [`Tty::ops`] as `Some(&STATIC)`. Exactly one
/// implementor exists today: `PTY_SLAVE_OPS` in `kernel/tty/pty.rs`.
/// The console tty deliberately has *no* ops (`Tty::ops == None` — it
/// always passed a null `tty_ops*` to `tty_alloc`), so every
/// dispatcher's `None` fallback is the console's behavior.
///
/// *Every* method is defaulted, and each default reproduces the old
/// per-slot `None` fallback exactly (the old tables were sparse:
/// the pty slave populated only `open`/`close`/`read`/`write`, and
/// `open`/`close` were behaviorally identical to the fallback):
/// `open` → success, `ioctl` → `ENOTTY`, everything else → no-op.
///
/// The old table's `throttle`/`unthrottle`/`stop`/`start`/`rx`/`tx`
/// slots were deleted outright (P3-10b "orphan stub" precedent): no
/// implementor ever populated them AND no dispatcher ever called them
/// — dead in both directions. `read`/`write` are retained (the pty
/// slave implements them) even though no tty-core path dispatches
/// them yet: slave I/O currently flows through the line-discipline
/// pipes (`tty_read`/`tty_write`), exactly as in the C original.
///
/// Error encoding: `KResult` — `Err(e)` is encoded to the same raw
/// negative `c_int`/`isize` the old callbacks returned (via
/// [`Errno::neg`]) at the dispatch boundaries.
///
/// `Sync` supertrait: instances are shared crate-wide as `&'static`
/// references reachable from any CPU.
pub trait TtyOps: Sync {
    /// Driver open hook, called from [`tty_open`] after the refcount
    /// bump. Default: success (the old `None` fallback returned 0).
    ///
    /// # Safety
    /// `tty` must be the live `Tty` this instance was installed on.
    unsafe fn open(&self, tty: *mut Tty) -> KResult<()> {
        let _ = tty;
        Ok(())
    }

    /// Driver close hook, called from [`tty_close`] before the
    /// refcount drop. Default: no-op.
    ///
    /// # Safety
    /// Same contract as [`TtyOps::open`].
    unsafe fn close(&self, tty: *mut Tty) {
        let _ = tty;
    }

    /// Carrier-loss hook, called from [`tty_hangup`]. Default: no-op.
    ///
    /// # Safety
    /// Same contract as [`TtyOps::open`].
    unsafe fn hangup(&self, tty: *mut Tty) {
        let _ = tty;
    }

    /// Discard-pending-input hook (`TCSETSF`). Default: no-op (the
    /// tty core has already flushed its own raw ring buffer).
    ///
    /// # Safety
    /// Same contract as [`TtyOps::open`].
    unsafe fn discard_input(&self, tty: *mut Tty) {
        let _ = tty;
    }

    /// Slave-side read (pull from the driver). Not dispatched by any
    /// tty-core path today — see the trait doc. Default: `ENOSYS`.
    ///
    /// # Safety
    /// `tty` as in [`TtyOps::open`]; `buf` must point to at least
    /// `nr` writable bytes.
    unsafe fn read(&self, tty: *mut Tty, buf: *mut c_char, nr: usize) -> KResult<usize> {
        let _ = (tty, buf, nr);
        Err(Errno::NoSys)
    }

    /// Slave-side write (push to the driver). Not dispatched by any
    /// tty-core path today — see the trait doc. Default: `ENOSYS`.
    ///
    /// # Safety
    /// `tty` as in [`TtyOps::open`]; `buf` must point to at least
    /// `nr` readable bytes.
    unsafe fn write(&self, tty: *mut Tty, buf: *const c_char, nr: usize) -> KResult<usize> {
        let _ = (tty, buf, nr);
        Err(Errno::NoSys)
    }

    /// Termios-changed notification (`TCSETS*`), called after the tty
    /// core has updated `tty->termios`. Default: no-op.
    ///
    /// # Safety
    /// `tty` as in [`TtyOps::open`]; `new_termios` points to the
    /// caller's already-validated `termios`.
    unsafe fn set_termios(&self, tty: *mut Tty, new_termios: *mut termios) {
        let _ = (tty, new_termios);
    }

    /// Winsize-changed notification (`TIOCSWINSZ`), called after the
    /// tty core has updated `tty->winsize`. Default: no-op.
    ///
    /// # Safety
    /// `tty` as in [`TtyOps::open`]; `new_winsize` points to the
    /// caller's already-validated `winsize`.
    unsafe fn set_winsize(&self, tty: *mut Tty, new_winsize: *mut crate::bindings::winsize) {
        let _ = (tty, new_winsize);
    }

    /// Driver escape hatch for ioctls the tty core doesn't recognize.
    /// Default: `Err(Errno::NotTy)` (the old fallback returned
    /// `-ENOTTY` both when the table had no `ioctl` slot and when
    /// there was no table at all).
    ///
    /// # Safety
    /// `tty` as in [`TtyOps::open`]; `arg`'s required validity depends
    /// on `cmd` (same contract as [`tty_ioctl`]).
    unsafe fn ioctl(
        &self,
        tty: *mut Tty,
        cmd: crate::bindings::uint64,
        arg: *mut c_void,
    ) -> KResult<c_int> {
        let _ = (tty, cmd, arg);
        Err(Errno::NotTy)
    }
}

/// Native `struct tty` (`kernel/inc/tty/tty_types.h`) — one terminal:
/// termios/winsize state, the canonical-mode pipes, the raw-mode ring
/// buffer, and the owning session.
///
/// As of wave P3-10d this layout is NATIVE-OWNED (post-P3-6 there is
/// no bindgen, no wrapper.h, and no C consumer; the header no longer
/// constrains it), and `bindings::tty` is a pure `pub use` facade of
/// this exact type. N-R7h (lock-owns-data) reordered it: the lock and
/// the fields it guards moved into [`Tty::inner`]
/// (`SpinLock<`[`TtyInner`]`>`) at offset 0, the non-guarded fields
/// follow, and the layout asserts below are the new truth (total size
/// unchanged at 512). `ops` is a real Rust trait object,
/// `Option<&'static dyn TtyOps>` (a 16-byte fat pointer; `None` = the
/// console's deliberate no-driver state, previously a null ops pointer).
#[repr(C, align(64))]
pub struct Tty {
    /// N-R7h (lock-owns-data): the per-tty lock now OWNS the state it
    /// guards. `SpinLock<TtyInner>` is `#[repr(C)] { UnsafeCell<
    /// spinlock_t>, UnsafeCell<TtyInner> }`, so `inner` lays out as the
    /// `spinlock_t` (offset 0 — the `lock@0` guarantee the old bare
    /// `lock` field carried, asserted below) immediately followed by the
    /// guarded [`TtyInner`] fields. Every former `KSpinlock::from_bindings(
    /// &raw mut (*t).lock).lock()` + raw `(*t).<field>` site is now
    /// `let tty = (*t).inner.lock(); tty.<field>`.
    pub inner: SpinLock<TtyInner>,
    pub ops: Option<&'static dyn TtyOps>,
    pub input_pipe: *mut pipe,
    pub output_pipe: *mut pipe,
    pub driver_data: *mut c_void,
    pub session: *mut session,
    pub name: [c_char; 64],
}

/// The lock-guarded per-tty state (N-R7h). Everything the per-tty lock
/// protects lives here and is reached only through a
/// [`crate::sync::SpinLockGuard`] (or, at [`tty_alloc`] time, before any
/// concurrent access). `#[repr(C)]` so `SpinLock<TtyInner>` keeps the
/// `spinlock_t`-then-guarded-fields byte layout `console.rs`'s
/// `consolewrite` (which now takes `(*tty).inner.lock()` to read
/// `termios`) and the layout asserts below both rely on.
#[repr(C)]
pub struct TtyInner {
    pub termios: termios,
    pub winsize: crate::bindings::winsize,
    pub ref_count: c_int,
    pub raw_buf: [c_char; 256],
    pub raw_r: crate::bindings::uint,
    pub raw_w: crate::bindings::uint,
    pub raw_wait: crate::bindings::tq_t,
}

// N-R7h lock-owns-data layout proof — `Tty` is native-owned (module
// note above; `bindings::tty` is a pure `pub use` facade of this type,
// no external `offset_of!`/size asserts, no `.S` refs, so this reorder
// is legal). The lock+guarded fields moved into `SpinLock<TtyInner>` at
// offset 0; the non-guarded fields (`ops`/`input_pipe`/`output_pipe`/
// `driver_data`/`session`/`name`) follow. `SpinLock<TtyInner>` =
// `#[repr(C)] { UnsafeCell<spinlock_t>(24), UnsafeCell<TtyInner>(368) }`
// = 392 bytes, laid out as `spinlock_t` then the `TtyInner` fields, so
// `offset_of!(Tty, inner) == 0` preserves the old `lock@0` guarantee
// (the spinlock is `inner`'s first byte). Total size stays 512 (the
// reorder is absorbed by the trailing align(64) padding). `input_pipe`/
// `output_pipe` shift (96/104 -> 408/416) but are read by NAME only
// (the `pty.rs` peer), never by offset — verified: no external observer.
const _: () = {
    assert!(core::mem::size_of::<Option<&'static dyn TtyOps>>() == 16, "tty.ops fat-pointer size");

    // TtyInner (the guarded state) — byte-identical field sequence to
    // the old bare fields, minus the lock (now `inner`'s prefix).
    assert!(core::mem::size_of::<TtyInner>() == 368, "TtyInner size");
    assert!(core::mem::align_of::<TtyInner>() == 8, "TtyInner alignment");
    assert!(core::mem::offset_of!(TtyInner, termios) == 0, "TtyInner.termios offset");
    assert!(core::mem::offset_of!(TtyInner, winsize) == 40, "TtyInner.winsize offset");
    assert!(core::mem::offset_of!(TtyInner, ref_count) == 48, "TtyInner.ref_count offset");
    assert!(core::mem::offset_of!(TtyInner, raw_buf) == 52, "TtyInner.raw_buf offset");
    assert!(core::mem::offset_of!(TtyInner, raw_r) == 308, "TtyInner.raw_r offset");
    assert!(core::mem::offset_of!(TtyInner, raw_w) == 312, "TtyInner.raw_w offset");
    assert!(core::mem::offset_of!(TtyInner, raw_wait) == 320, "TtyInner.raw_wait offset");

    // SpinLock<TtyInner> = spinlock_t(24) + TtyInner(368).
    assert!(core::mem::size_of::<SpinLock<TtyInner>>() == 392, "SpinLock<TtyInner> size");

    assert!(core::mem::size_of::<Tty>() == 512, "tty size");
    assert!(core::mem::align_of::<Tty>() == 64, "tty alignment");
    // The `lock@0` guarantee: `inner` at 0 => the embedded `spinlock_t`
    // is at byte 0, exactly where the old bare `lock` field sat.
    assert!(core::mem::offset_of!(Tty, inner) == 0, "tty.inner (lock@0) offset");
    assert!(core::mem::offset_of!(Tty, ops) == 392, "tty.ops offset");
    assert!(core::mem::offset_of!(Tty, input_pipe) == 408, "tty.input_pipe offset");
    assert!(core::mem::offset_of!(Tty, output_pipe) == 416, "tty.output_pipe offset");
    assert!(core::mem::offset_of!(Tty, driver_data) == 424, "tty.driver_data offset");
    assert!(core::mem::offset_of!(Tty, session) == 432, "tty.session offset");
    assert!(core::mem::offset_of!(Tty, name) == 440, "tty.name offset");
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

/// Sentinel raw byte pushed through [`Tty::echo_char`] for the erase
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
/// P3-10d: `ops` is a [`TtyOps`] trait object now (`None` = no driver
/// hooks, the console's configuration — previously a null `tty_ops*`);
/// the `&'static` bound replaces the old "must remain live for as long
/// as the returned `tty`" prose contract. Plain `unsafe fn` (the
/// `extern "C"` ABI is gone — a fat pointer is not FFI-safe, and every
/// caller is Rust).
///
/// # Safety
/// `name` must be a valid, NUL-terminated C string for the duration of
/// this call.
pub(crate) unsafe fn tty_alloc(name: *const c_char, ops: Option<&'static dyn TtyOps>) -> *mut tty {
    let raw = slab_alloc(TTY_CACHE.get()) as *mut tty;
    if raw.is_null() {
        return err_ptr(-ENOMEM);
    }

    // Two blocking pipes: input (driver->user) and output (user->driver).
    let inp = Pipe::pipe_alloc(0);
    if is_err(inp) {
        slab_free(raw as *mut c_void);
        // Propagate the encoded ERR_PTR value, reinterpreted as `*mut
        // tty` -- mirrors the C original's `return (struct tty *)inp;`.
        return inp as *mut tty;
    }
    let outp = Pipe::pipe_alloc(0);
    if is_err(outp) {
        Pipe::pipe_close(inp, 1);
        Pipe::pipe_close(inp, 0);
        slab_free(raw as *mut c_void);
        return outp as *mut tty;
    }

    // SAFETY: `raw` is freshly allocated, exclusively-owned slab memory;
    // every field used elsewhere in this file is written below before
    // `raw` is returned to any other code (matches the C original's
    // field-by-field initialization -- no blanket zeroing was ever done
    // there either).
    unsafe {
        // N-R7h: build the lock-guarded state, then move a fully
        // initialised `SpinLock<TtyInner>` into the slab slot. Only the
        // wait queue needs a post-construction fix-up (`tq_init` wires it
        // to *this* lock), done under the guard before `raw` is published.
        // `termios_init_default` writes through a pointer, so seed a stack
        // `termios` first. `raw_buf` is zeroed here (the C original left
        // it uninitialised -- harmless, only `raw_r`/`raw_w` index it).
        let mut termios0: termios = core::mem::zeroed();
        termios_init_default(&raw mut termios0);
        let inner = TtyInner {
            termios: termios0,
            winsize: crate::bindings::winsize {
                ws_row: DEFAULT_ROWS,
                ws_col: DEFAULT_COLS,
                ws_xpixel: 0,
                ws_ypixel: 0,
            },
            ref_count: 1,
            raw_buf: [0; 256],
            raw_r: 0,
            raw_w: 0,
            // Placeholder; `tq_init` below initialises it in place against
            // this lock.
            raw_wait: core::mem::zeroed(),
        };
        // Move the constructed lock into the (uninitialised) slab slot.
        core::ptr::write(&raw mut (*raw).inner, SpinLock::new(c"tty", inner));
        // Wire the raw-mode wait queue to this exact lock. Runs before
        // `raw` is returned, so no other hart can race the acquire; the
        // guard hands `tq_init` this lock's `spinlock_t*` (so a later
        // `wait_on(&raw mut tty.raw_wait, ..)` releases/reacquires it).
        {
            let mut g = (*raw).inner.lock();
            let q = &raw mut g.raw_wait;
            let lk = g.lock_ptr();
            if let Some(r) = TqRef::from_ptr(q) { r.init(c"tty_raw".as_ptr(), lk); }
        }
        // Non-guarded fields: set once here, read lock-free thereafter.
        (*raw).ops = ops;
        (*raw).input_pipe = inp;
        (*raw).output_pipe = outp;
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
            Pipe::pipe_close(inp, 1);
            Pipe::pipe_close(inp, 0);
        }
        let outp = (*t).output_pipe;
        if !outp.is_null() {
            Pipe::pipe_close(outp, 1);
            Pipe::pipe_close(outp, 0);
        }
        slab_free(t as *mut c_void);
    }
}

/// # Safety
/// `t` must be a live, non-null `tty` (mirrors the C original, which
/// also unconditionally dereferences `t->lock`).
pub(crate) unsafe extern "C" fn tty_ref(t: *mut tty) {
    // SAFETY: see fn doc -- creating `&(*t).inner` to lock it is the only
    // deref; `ref_count` is then a safe guarded field access.
    let mut guard = unsafe { (*t).inner.lock() };
    guard.ref_count += 1;
}

/// # Safety
/// `t` must be a live, non-null `tty` previously returned by
/// [`tty_alloc`] (mirrors the C original's unconditional dereference).
pub(crate) unsafe extern "C" fn tty_unref(t: *mut tty) {
    let should_free;
    {
        // SAFETY: see fn doc -- `&(*t).inner` to lock is the only deref.
        let mut guard = unsafe { (*t).inner.lock() };
        guard.ref_count -= 1;
        should_free = guard.ref_count <= 0;
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
// N-R7h: these read `termios` flags out of the lock-guarded [`TtyInner`],
// so they took `&TtyInner` (obtained via the held `SpinLockGuard`'s
// `Deref`) instead of a raw `*mut tty` -- a plain safe field read whose
// "lock must be held" contract is proven by the caller holding the guard.
//
// N-METH (goal #1): these `&TtyInner` free fns became inherent
// [`TtyInner`] methods -- the natural receiver is exactly the guarded
// state they read, and every caller already holds a `SpinLockGuard`, so
// `helper(&guard)` becomes the idiomatic `guard.helper()` (method
// resolution auto-derefs the guard to `&TtyInner`). This mirrors the C
// `static inline` helpers of the same name, which carried the same
// implicit "lock held" contract.
// ===========================================================================

impl TtyInner {
    #[inline]
    fn l_canon(&self) -> bool {
        self.termios.c_lflag & ICANON != 0
    }
    #[inline]
    fn l_echo(&self) -> bool {
        self.termios.c_lflag & ECHO != 0
    }
    #[inline]
    fn l_echoe(&self) -> bool {
        self.termios.c_lflag & ECHOE != 0
    }
    #[inline]
    fn l_echok(&self) -> bool {
        self.termios.c_lflag & ECHOK != 0
    }
    #[inline]
    fn l_isig(&self) -> bool {
        self.termios.c_lflag & ISIG != 0
    }
    #[inline]
    fn i_icrnl(&self) -> bool {
        self.termios.c_iflag & ICRNL != 0
    }
    #[inline]
    fn i_igncr(&self) -> bool {
        self.termios.c_iflag & IGNCR != 0
    }
    #[inline]
    fn i_inlcr(&self) -> bool {
        self.termios.c_iflag & INLCR != 0
    }
    #[inline]
    fn i_istrip(&self) -> bool {
        self.termios.c_iflag & ISTRIP != 0
    }
    #[inline]
    fn o_opost(&self) -> bool {
        self.termios.c_oflag & OPOST != 0
    }
    #[inline]
    fn o_onlcr(&self) -> bool {
        self.termios.c_oflag & ONLCR != 0
    }
}

// ===========================================================================
// Echo + signal generation -- internal per-tty helpers.
//
// N-METH (goal #1): the two private helpers that took a raw `*mut tty`
// (`tty_echo_char`, `tty_signal_fg_pgroup`) became inherent [`Tty`]
// methods on `&self`. Because [`Tty`] embeds a `SpinLock` (an
// `UnsafeCell`) it is NOT `Freeze`, so `&self` carries no `noalias`/
// `readonly` and the interior mutation through `self.inner` is exactly
// the `std::sync::Mutex` shared-`&self`/guarded-`&mut` pattern -- sound,
// and no LLVM read-hoist hazard. With `&self` the receiver deref that
// used to be `unsafe { (*t).<field> }` is a plain safe field read
// (`self.output_pipe`, `self.session`, `self.inner`), so `echo_char`
// loses its wide receiver `unsafe` down to the single `pipe_write` call
// and `signal_fg_pgroup` loses `unsafe` entirely.
// ===========================================================================

impl Tty {
    /// Echo one character (or the `BACKSPACE` erase sequence) to the
    /// output pipe.
    ///
    /// # Locking
    /// `guard` must be `self`'s currently-held lock guard. This method
    /// drops it before calling `Pipe::pipe_write()` (which can sleep if the
    /// output pipe is full) and returns a freshly re-acquired guard tied
    /// to `&self` -- the caller must use the returned guard from this
    /// point on, exactly mirroring the C original's "drop the spinlock
    /// before calling Pipe::pipe_write(), re-lock after" comment. The
    /// `'a`-bound receiver/guard makes that re-acquire a *safe*
    /// `self.inner.lock()` (was an `unsafe` raw re-deref).
    fn echo_char<'a>(
        &'a self,
        c: i32,
        guard: SpinLockGuard<'a, TtyInner>,
    ) -> SpinLockGuard<'a, TtyInner> {
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
            // `guard` is still held here (not yet dropped), so reading the
            // OPOST/ONLCR flags out of the guarded state is a safe read.
            if guard.o_opost() && guard.o_onlcr() && c == b'\n' as i32 {
                echobuf[n] = b'\r';
                n += 1;
            }
            echobuf[n] = c as u8;
            n += 1;
        }

        // Drop the lock before calling Pipe::pipe_write() -- it can sleep.
        drop(guard);
        // SAFETY: `self` is a live `Tty` (the `&self` receiver), so
        // `self.output_pipe` is the non-null pipe installed by
        // `tty_alloc`; `echobuf[..n]` is a valid readable buffer.
        unsafe { Pipe::pipe_write(self.output_pipe, echobuf.as_ptr() as *const c_char, n as u64, 0) };
        // Re-acquire the same lock so the returned guard is valid for the
        // caller to continue -- now a safe borrow of `self.inner`.
        self.inner.lock()
    }

    /// Send a signal to the terminal's foreground process group, or to
    /// the current process if there is no session / foreground group.
    ///
    /// # Locking
    /// Must be called with `self`'s lock **not** held (it dereferences
    /// `self.session` lock-free, matching the C original -- see the call
    /// sites in [`tty_input`], all of which drop the guard immediately
    /// before calling this). With `&self` the former
    /// `unsafe { (*t).session }` is now a plain safe field read.
    fn signal_fg_pgroup(&self, signum: c_int) {
        let sess = self.session;
        if !sess.is_null() {
            // SAFETY: `sess` is non-null (checked) and, per the C
            // original's lock-free access, a live `session` for `self`.
            let fg = unsafe { session_get_fg_pgid(sess) };
            if fg > 0 {
                Pgroup::kill(fg, signum);
                return;
            }
        }
        // Fallback: no session or no fg group -- signal current process.
        let cur = crate::machine::current_thread_ptr();
        if let Some(ta) = crate::proc::access::ThreadAccess::from_ptr(cur) { ta.kill_proc(signum); }
    }
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
    // N-METH: bind `&Tty` once (`this`). `Tty` embeds a `SpinLock`
    // (`UnsafeCell`) so it is NOT `Freeze` -- this shared receiver carries
    // no `noalias`/`readonly`, and the mutation of `TtyInner` through
    // `this.inner`'s guard is the standard `Mutex`-shared-`&self` pattern
    // (sound; no LLVM read-hoist hazard). This turns every former
    // `unsafe { (*t).inner.lock() }` re-lock into a safe `this.inner.lock()`
    // and lets the two internal helpers be called as methods
    // (`this.echo_char(..)` / `this.signal_fg_pgroup(..)`); the guard's
    // `'this` lifetime is what makes `echo_char`'s returned guard valid.
    // SAFETY: `t` is caller-guaranteed live (fn doc).
    let this: &Tty = unsafe { &*t };
    let mut guard = this.inner.lock();

    // N-METH (goal #2): the manual `for i in 0..n { *buf.add(i) }` index
    // walk became a slice iteration -- one `from_raw_parts` up front
    // replaces `n` per-byte raw derefs.
    // SAFETY: caller guarantees `buf[..count]` is readable (fn doc); `buf`
    // is not mutated for the duration of this call (input-only).
    let bytes = unsafe { core::slice::from_raw_parts(buf as *const u8, n) };
    for &raw_byte in bytes {
        let mut c = raw_byte as i32;

        // ---------- input flag processing ----------

        if guard.i_istrip() {
            c &= 0x7F;
        }

        if c == b'\r' as i32 {
            if guard.i_igncr() {
                continue;
            }
            if guard.i_icrnl() {
                c = b'\n' as i32;
            }
        } else if c == b'\n' as i32 {
            if guard.i_inlcr() {
                c = b'\r' as i32;
            }
        }

        // ---------- signal characters ----------

        if guard.l_isig() {
            // Reading the whole small `c_cc` array by value is a plain
            // memcpy out of the guarded termios.
            let cc = guard.termios.c_cc;

            if c == cc[VINTR] as i32 {
                drop(guard);
                this.signal_fg_pgroup(SIGINT);
                guard = this.inner.lock();
                if guard.l_echo() {
                    guard = this.echo_char('^' as i32, guard);
                    guard = this.echo_char('C' as i32, guard);
                    guard = this.echo_char('\n' as i32, guard);
                }
                continue;
            }
            if c == cc[VQUIT] as i32 {
                drop(guard);
                this.signal_fg_pgroup(SIGQUIT);
                guard = this.inner.lock();
                if guard.l_echo() {
                    guard = this.echo_char('^' as i32, guard);
                    guard = this.echo_char('\\' as i32, guard);
                    guard = this.echo_char('\n' as i32, guard);
                }
                continue;
            }
            if c == cc[VSUSP] as i32 {
                drop(guard);
                this.signal_fg_pgroup(SIGTSTP);
                guard = this.inner.lock();
                if guard.l_echo() {
                    guard = this.echo_char('^' as i32, guard);
                    guard = this.echo_char('Z' as i32, guard);
                    guard = this.echo_char('\n' as i32, guard);
                }
                continue;
            }
        }

        // ---------- canonical mode editing ----------

        if guard.l_canon() {
            let cc = guard.termios.c_cc;

            // Erase (backspace / DEL).
            if c == cc[VERASE] as i32 || c == b'\x08' as i32 {
                if guard.l_echoe() {
                    guard = this.echo_char(BACKSPACE, guard);
                }
                continue;
            }

            // Kill line (^U).
            if c == cc[VKILL] as i32 {
                if guard.l_echok() {
                    guard = this.echo_char('\n' as i32, guard);
                }
                continue;
            }

            // EOF (^D) -- push a zero-length "line" to unblock readers.
            if c == cc[VEOF] as i32 {
                drop(guard);
                let nul: u8 = 0;
                // SAFETY: `this` is live; lock dropped above.
                unsafe {
                    Pipe::pipe_write(this.input_pipe, &nul as *const u8 as *const c_char, 0, 0);
                }
                guard = this.inner.lock();
                continue;
            }
        }

        // ---------- echo ----------

        if guard.l_echo() {
            guard = this.echo_char(c, guard);
        }

        // ---------- push character to consumer ----------

        if !guard.l_canon() {
            // Raw mode: store in the direct ring buffer and wake any
            // reader immediately (single-character latency). All accesses
            // are safe guarded field reads/writes through `guard`.
            let next = (guard.raw_w + 1) & (TTY_RAW_BUF_SIZE - 1);
            if next != (guard.raw_r & (TTY_RAW_BUF_SIZE - 1)) {
                let idx = (guard.raw_w & (TTY_RAW_BUF_SIZE - 1)) as usize;
                guard.raw_buf[idx] = c as u8 as c_char;
                guard.raw_w += 1;
            }
            // `wakeup_all` is safe under the spinlock (still held via
            // `guard`); form the raw queue pointer, then wake.
            let q = &raw mut guard.raw_wait;
            let _ = TqRef::from_ptr(q).map_or(-EINVAL, |r| r.wakeup_all(0, 0));
        } else {
            // Canonical mode: push into input pipe.
            let ch = c as u8 as c_char;
            drop(guard);
            // SAFETY: `this` is live; lock dropped above.
            unsafe { Pipe::pipe_write(this.input_pipe, &ch as *const c_char, 1, 0) };
            guard = this.inner.lock();
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
    let canon = {
        // SAFETY: `t` is caller-guaranteed live (fn doc).
        let g = unsafe { (*t).inner.lock() };
        g.l_canon()
    };

    if canon {
        // SAFETY: `t` is live; `pipe_read` validates `buf` itself per
        // the userspace/kernel split encoded by `user`.
        return unsafe { Pipe::pipe_read((*t).input_pipe, buf, count, user) };
    }

    // ---- Raw mode: read from direct ring buffer ----
    let mut total: i64 = 0;
    let target = count as i64;

    // SAFETY: `t` is caller-guaranteed live (fn doc).
    let mut guard = unsafe { (*t).inner.lock() };

    // The C original wraps this in `while ((size_t)total < count) { ...
    // break; }` -- the trailing `break` always executes on the first
    // (only) pass, so the loop is really "run the body once, unless
    // `count == 0`". Written as an `if` here for clarity; semantics
    // preserved 1:1 (single batch, `read(2)`-style return).
    if total < target {
        // Wait for at least one character. All ring reads/writes are safe
        // guarded field accesses through `guard`.
        while guard.raw_r == guard.raw_w {
            let cur = crate::machine::current_thread_ptr();
            crate::machine::thread_state_set(cur, crate::bindings::thread_state_THREAD_INTERRUPTIBLE);
            // Park on the raw wait queue. `wait_on` (`tq_wait`) atomically
            // releases THIS lock, sleeps, and re-acquires it on wake --
            // same lost-wakeup-free protocol as the C
            // `tq_wait(&tty->raw_wait, &tty->lock, NULL)`. Raw queue ptr
            // formed first (ends the borrow before the `&mut self` call).
            let q = &raw mut guard.raw_wait;
            guard.wait_on(q, core::ptr::null_mut());
            if crate::proc::access::ThreadAccess::from_ptr(cur).is_some_and(|ta| ta.signal_pending()) {
                drop(guard);
                return if total > 0 { total } else { -(EINTR as i64) };
            }
        }

        // Drain available characters, up to count.
        while total < target && guard.raw_r != guard.raw_w {
            let idx = (guard.raw_r & (TTY_RAW_BUF_SIZE - 1)) as usize;
            let ch = guard.raw_buf[idx];
            guard.raw_r += 1;
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
            // SAFETY: `t` is live; re-acquire the same lock for the next
            // iteration / the trailing drop.
            guard = unsafe { (*t).inner.lock() };
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
    let guard = unsafe { (*t).inner.lock() };

    if events & (POLLIN | POLLRDNORM) != 0 {
        if guard.l_canon() {
            // Canonical mode: data ready if input pipe has bytes.
            // SAFETY: `t` is live; `input_pipe` is a plain (non-guarded)
            // field read through the raw pointer.
            let pi = unsafe { (*t).input_pipe };
            // SAFETY: `pi` is a live pipe owned by `t` for its lifetime.
            let nwrite = unsafe { load_acquire_u32(&raw const (*pi).nwrite) };
            let nread = unsafe { load_acquire_u32(&raw const (*pi).nread) };
            if nwrite.wrapping_sub(nread) > 0 {
                revents |= events & (POLLIN | POLLRDNORM);
            }
        } else {
            // Raw mode: data ready if ring buffer is non-empty (safe
            // guarded field reads through `guard`).
            if guard.raw_r != guard.raw_w {
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
        let g = unsafe { (*t).inner.lock() };
        g.o_opost() && g.o_onlcr()
    };

    if !need_opost {
        // Fast path -- no post-processing.
        // SAFETY: `t` is live; `pipe_write` validates `buf` itself.
        return unsafe { Pipe::pipe_write((*t).output_pipe, buf, count, user) };
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

        // Expand NL -> CRNL into a second buffer. N-METH (goal #2): the
        // manual `for j in 0..batch_usize { kbuf[j] }` index walk became a
        // slice iteration over the filled prefix (bounds checks dropped;
        // `olen` still tracks the write cursor into `outbuf`).
        let mut outbuf = [0u8; 128];
        let mut olen = 0usize;
        for &byte in &kbuf[..batch_usize] {
            if byte == b'\n' {
                outbuf[olen] = b'\r';
                olen += 1;
            }
            outbuf[olen] = byte;
            olen += 1;
        }

        let ret = unsafe { Pipe::pipe_write((*t).output_pipe, outbuf.as_ptr() as *const c_char, olen as u64, 0) };
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
    unsafe { Pipe::pipe_read((*t).output_pipe, buf, count, 0) }
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
            // `termios*` for this cmd (fn doc). `termios` is `Copy`, read
            // out of the guarded state under the lock.
            let g = unsafe { (*t).inner.lock() };
            unsafe { *tp = g.termios };
            0
        }
        TCSETS | TCSETSW | TCSETSF => {
            let tp = arg as *mut termios;
            {
                // SAFETY: see TCGETS arm.
                let mut g = unsafe { (*t).inner.lock() };
                unsafe { g.termios = *tp };
            }

            // Notify driver if it cares.
            // SAFETY: `t` is live; `t->ops`, if present, is a
            // `&'static` trait object (tty_alloc's contract); `tp` is
            // a valid `termios*` for this cmd (fn doc).
            unsafe {
                if let Some(ops) = (*t).ops {
                    ops.set_termios(t, tp);
                }
            }

            // TCSETSF: also discard pending input.
            if cmd == TCSETSF {
                {
                    // SAFETY: `t` is live (fn doc).
                    let mut g = unsafe { (*t).inner.lock() };
                    // flush raw ring buffer
                    g.raw_r = g.raw_w;
                }
                // SAFETY: as the `set_termios` dispatch above.
                unsafe {
                    if let Some(ops) = (*t).ops {
                        ops.discard_input(t);
                    }
                }
            }

            0
        }
        TIOCGWINSZ => {
            let wsp = arg as *mut crate::bindings::winsize;
            // SAFETY: `t` is live (fn doc); `wsp` a valid writable
            // `winsize*`. `winsize` is `Copy`, read under the lock.
            let g = unsafe { (*t).inner.lock() };
            unsafe { *wsp = g.winsize };
            0
        }
        TIOCSWINSZ => {
            let wsp = arg as *mut crate::bindings::winsize;
            {
                // SAFETY: see TIOCGWINSZ arm.
                let mut g = unsafe { (*t).inner.lock() };
                unsafe { g.winsize = *wsp };
            }
            // SAFETY: `t` is live; `t->ops`, if present, is `&'static`
            // (tty_alloc's contract); `wsp` is a valid `winsize*` for
            // this cmd (fn doc).
            unsafe {
                if let Some(ops) = (*t).ops {
                    ops.set_winsize(t, wsp);
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
            // N-R6d-2a: `thread.session` is a generational `Sid`; resolve it to
            // a live `*mut session`. `pid_wlock` is held for this path (the
            // `session_set_ctrl_tty` below asserts it), so `session_lookup` is
            // race-free; a `NONE`/stale key resolves to null → `EPERM`, exactly
            // the old "no session leader" outcome.
            let sess = session_lookup(unsafe { (*cur).session }).unwrap_or(core::ptr::null_mut());
            if sess.is_null() {
                return -EPERM;
            }
            unsafe { session_set_ctrl_tty(sess, t) };
            0
        }
        _ => {
            // Let the driver handle unknown ioctls. The trait default
            // is `Err(Errno::NotTy)`, so "no ops" and "driver doesn't
            // override ioctl" both encode to the old `-ENOTTY`.
            // SAFETY: `t` is live; `t->ops`, if present, is `&'static`
            // (tty_alloc's contract); `arg` per this fn's contract.
            unsafe {
                match (*t).ops {
                    Some(ops) => match ops.ioctl(t, cmd, arg) {
                        Ok(v) => v,
                        Err(e) => e.neg(),
                    },
                    None => -ENOTTY,
                }
            }
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
    // SAFETY: `t` is live; `t->ops`, if present, is `&'static`
    // (tty_alloc's contract). The trait default is `Ok(())`, so "no
    // ops" and "driver doesn't override open" both encode to the old
    // fallback 0.
    unsafe {
        match (*t).ops {
            Some(ops) => result_to_neg_errno(ops.open(t)),
            None => 0,
        }
    }
}

/// # Safety
/// `t` must be a live `tty` previously returned by [`tty_alloc`].
pub(crate) unsafe extern "C" fn tty_close(t: *mut tty) {
    // SAFETY: `t` is caller-guaranteed live (fn doc); `t->ops`, if
    // present, is `&'static` (tty_alloc's contract).
    unsafe {
        if let Some(ops) = (*t).ops {
            ops.close(t);
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
    // SAFETY: `t` is caller-guaranteed live (fn doc); `t->ops`, if
    // present, is `&'static` (tty_alloc's contract).
    unsafe {
        if let Some(ops) = (*t).ops {
            ops.hangup(t);
        }
    }
}
