//! 16550A UART driver with PXA UART support (SpacemiT K1 / Orange Pi RV2).
//!
//! Rust port of `kernel/uart.c` (Phase 2 Wave 4, see
//! `docs/rustify/phase2_plan.md`). Bottom layer of the console/panic
//! chain: `printf` -> `console.rs` -> this file -> raw MMIO.
//!
//! PXA UART differences from standard 16550A:
//!   - reg-shift=2, reg-io-width=4 (4-byte spacing, 32-bit access)
//!   - 64-byte FIFO (vs 16-byte), requires IER_UUE (0x40) to enable
//!   - MCR_OUT2 (0x08) required for interrupt routing to PLIC
//!
//! # MMIO
//!
//! All hardware register access funnels through [`read_reg`] /
//! [`write_reg`], which compute `UART0 + (reg << reg_shift)` (mirroring
//! the C `Reg8`/`Reg32` macros) and dispatch to `read_volatile` /
//! `write_volatile` on either a `*const/*mut u8` or `*const/*mut u32`
//! depending on `__uart0_reg_io_width`. This is the file's entire
//! volatile surface (2 sites, matching the count in the Wave 4 plan) —
//! every other function in this file goes through these two helpers.
//! No explicit fence is needed: the original C used plain `volatile`
//! pointer dereferences with no barrier, and QEMU's/the SoC's PMA
//! configuration for the UART region is strongly-ordered, so a 1:1
//! port carries the same (already-correct) ordering.
//!
//! # Locking / sleep semantics
//!
//! Two independent spinlock-protected ring buffers, exactly as in the
//! C original:
//!   * `UART_TX` (a `SpinLock<UartTx>`, N-R7 lock-owns-data) owns the TX
//!     software ring; its lock also guards the `UART_IER` TX-enable bit.
//!     [`uartputc`]/[`uartputs`] block (via the guard's `sleep_on`) when
//!     the ring is full — **not** IRQ-safe, only for `write()` paths.
//!     [`uartputc_sync`] is the IRQ-safe alternative used by printf/echo:
//!     it never touches the software ring, only spin-waits on the
//!     hardware `LSR.TX_IDLE` bit while holding `push_off`/`pop_off`
//!     (no sleep, safe from any context including panics).
//!   * `UART_RX_LOCK` guards the RX software ring, drained opportunistically
//!     by [`uartrecv`] (caller must hold the lock) from both
//!     [`uartgetc`]/[`uartgets`] (polling) and [`uartintr`] (interrupt
//!     context — dispatches each byte to `consoleintr`, dropping the RX
//!     lock around that call exactly as the C did, since `consoleintr`
//!     may itself need other locks).

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};

use crate::bindings::spinlock_t;
// N-R7 (lock-owns-data): the TX software ring + its lock are now a
// single data-owning `SpinLock<UartTx>` (was a bare `static mut
// spinlock_t` beside three `static mut` ring fields). See the TX ring
// section below. The RX ring is out of scope this wave and stays raw.
use crate::sync::SpinLock;

// ===========================================================================
// FDT-configured MMIO base + layout (written once by `dev/fdt.c` during
// early single-hart boot, read-only from every hart thereafter — same
// plain-global, no-atomics contract the C original had; `start.rs`'s
// `__jiff_ticks` and `sbi.rs`'s extension probe table document the
// identical "single boot-time writer, published before secondary harts
// start" pattern used crate-wide).
// ===========================================================================

/// Physical MMIO base of UART0. Read by `mm/vm_pgtab.rs`'s `kvmmake` (as
/// `extern`) and written by `dev/fdt.c` (still C) during platform probe.
pub(crate) static mut __uart0_mmio_base: u64 = 0x1000_0000;
/// PLIC hardware IRQ number for UART0.
pub(crate) static mut __uart0_irqno: u64 = 10;
/// UART clock in Hz; 0 = default (assume 1.8432 MHz for QEMU).
pub(crate) static mut __uart0_clock: u32 = 0;
/// Baud rate; 0 = default 115200.
pub(crate) static mut __uart0_baud: u32 = 0;
/// Register spacing: 0 = 1-byte, 2 = 4-byte (common on SoCs).
pub(crate) static mut __uart0_reg_shift: u32 = 0;
/// Register access width: 1 = 8-bit, 4 = 32-bit (PXA UART).
pub(crate) static mut __uart0_reg_io_width: u32 = 1;

// ===========================================================================
// 16550A/PXA UART registers.
// ===========================================================================

const RHR: u32 = 0; // receive holding register
const THR: u32 = 0; // transmit holding register
const IER: u32 = 1; // interrupt enable register
const IER_RX_ENABLE: u32 = 1 << 0;
const IER_TX_ENABLE: u32 = 1 << 1;
const IER_RTOIE: u32 = 1 << 4; // PXA: receiver timeout
const IER_UUE: u32 = 1 << 6; // PXA: unit enable
const FCR: u32 = 2; // FIFO control register
const FCR_FIFO_ENABLE: u32 = 1 << 0;
const FCR_FIFO_CLEAR: u32 = 3 << 1;
const FCR_TRIGGER_1: u32 = 0 << 6;
const FCR_TRIGGER_8: u32 = 2 << 6;
const IIR: u32 = 2;
const LCR: u32 = 3;
const LCR_EIGHT_BITS: u32 = 3 << 0;
const MCR: u32 = 4;
const MCR_DTR: u32 = 1 << 0;
const MCR_RTS: u32 = 1 << 1;
const MCR_OUT2: u32 = 1 << 3; // Required for PLIC interrupt routing
const LSR: u32 = 5;
const LSR_RX_READY: u32 = 1 << 0;
const LSR_TX_IDLE: u32 = 1 << 5;
const MSR: u32 = 6;

/// Mirrors the C `UART_FIFO_SIZE` macro.
#[inline(always)]
fn uart_fifo_size() -> u32 {
    // SAFETY: single boot-time writer, read-only from here on (see the
    // module-level doc comment).
    if unsafe { __uart0_reg_io_width } == 4 {
        64
    } else {
        16
    }
}

/// Read one UART register, honouring the runtime-configured spacing
/// (`reg_shift`) and access width (`reg_io_width`). Mirrors the C
/// `ReadReg` static inline. Sole read-volatile site in this file.
#[inline(always)]
fn read_reg(reg: u32) -> u32 {
    // SAFETY: `base`/`shift`/`width` are the single-boot-time-writer
    // globals documented above. The computed address always lands
    // inside the UART0 MMIO window that `kvmmake` maps uncached (see
    // `mm/vm_pgtab.rs`); MMIO reads have no aliasing/lifetime concerns
    // beyond "the mapping exists", which holds for the kernel's entire
    // post-`kvminithart` lifetime.
    unsafe {
        let base = __uart0_mmio_base;
        let shift = __uart0_reg_shift;
        let addr = base + ((reg as u64) << shift);
        if __uart0_reg_io_width == 4 {
            core::ptr::read_volatile(addr as *const u32)
        } else {
            core::ptr::read_volatile(addr as *const u8) as u32
        }
    }
}

/// Write one UART register. Mirrors the C `WriteReg` static inline.
/// Sole write-volatile site in this file.
#[inline(always)]
fn write_reg(reg: u32, v: u32) {
    // SAFETY: see `read_reg`.
    unsafe {
        let base = __uart0_mmio_base;
        let shift = __uart0_reg_shift;
        let addr = base + ((reg as u64) << shift);
        if __uart0_reg_io_width == 4 {
            core::ptr::write_volatile(addr as *mut u32, v);
        } else {
            core::ptr::write_volatile(addr as *mut u8, v as u8);
        }
    }
}

// ===========================================================================
// Externs.
// ===========================================================================

// P3-D3c: `mm/cffi.rs`'s interrupt-nesting helpers are plain crate-path
// imports now that their `#[no_mangle]` exports are gone.
use crate::mm::cffi::{xv6_pop_off, xv6_push_off};

// P3-D3c: the spinlock primitives are genuinely `unsafe fn`s in
// `crate::lock::spinlock` now that their `#[no_mangle]` exports are gone;
// this file's original extern declarations asserted `safe fn` (usual
// FFI-facade convention). Thin wrappers preserve that safe facade for the
// unchanged call sites.
// ABI-truth note: the old redeclaration typed `name` as `*const c_char`;
// the real fn takes `*mut c_char` (the callee only reads it) -- the
// wrapper carries the cast.
/// SAFETY: see [`crate::lock::spinlock::RawSpinlock::init`]'s contract.
fn spin_init(lk: *mut spinlock_t, name: *const c_char) {
    unsafe { crate::lock::spinlock::RawSpinlock::init(lk, name as *mut c_char) }
}
/// SAFETY: see [`crate::lock::spinlock::RawSpinlock::lock`]'s contract.
fn spin_lock(lk: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::RawSpinlock::lock(lk) }
}
/// SAFETY: see [`crate::lock::spinlock::RawSpinlock::unlock`]'s contract.
fn spin_unlock(lk: *mut spinlock_t) {
    unsafe { crate::lock::spinlock::RawSpinlock::unlock(lk) }
}

// P3-1C mesh sweep: console.rs is in scope for this wave, so
// `consoleintr` becomes a plain crate-path import instead of an
// `extern "C"` redeclaration (identical signature).
use crate::console::consoleintr;
// P3-D2a: proc/sched.rs chan sleep/wake entry points, reached as plain
// crate-path items instead of `extern "C"` redeclarations. N-R7c: both
// the uninterruptible (`sleep_on`) and the interruptible
// (`sleep_on_interruptible`) TX waits are now reached through the guard
// (lock-owns-data), so only the waker is imported directly here.
use crate::proc::Scheduler;

const EINTR: c_int = 4;

// ===========================================================================
// TX ring buffer + lock.
// ===========================================================================

const UART_TX_BUF_SIZE: usize = 128;

/// The TX software ring buffer and its two indices (the C `uart_tx_buf`
/// / `uart_tx_w` / `uart_tx_r`). All three were guarded by the same
/// single `uart_tx_lock`, so they now live together inside one
/// [`SpinLock`] that *owns* them (N-R7 lock-owns-data): every access
/// goes through a guard that `Deref`s to `&UartTx`/`&mut UartTx`, so the
/// borrow checker enforces "only while the lock is held" and no raw
/// `static mut` access (nor its `unsafe`) remains on the TX ring.
struct UartTx {
    /// TX output ring buffer.
    buf: [u8; UART_TX_BUF_SIZE],
    /// Write index.
    w: u64,
    /// Read index (drained by `uartstart`).
    r: u64,
}

/// The UART TX ring + its lock, as one data-owning primitive.
/// `SpinLock::new` const-initialises the embedded lock to the exact
/// post-`spin_init` state (`uart_tx_lock` name, `locked = 0`), matching
/// the C `SPINLOCK_INITIALIZED("uart_tx_lock")` compile-time
/// initializer — valid for locking from the moment `.bss`/`.data` are
/// live, no runtime `spin_init` call required.
static UART_TX: SpinLock<UartTx> =
    SpinLock::new(c"uart_tx_lock", UartTx { buf: [0; UART_TX_BUF_SIZE], w: 0, r: 0 });

/// Current IER value, for dynamic TX-interrupt enable/disable. Mutated
/// only while `UART_TX`'s lock is held (mirrors the C `static uint32
/// uart_ier`), except during `uartinit()` which runs before any
/// concurrency is possible. Kept a bare `static mut` (this wave moves
/// only the tx-ring *buffer* state into the lock, per scope): its
/// accesses stay `unsafe`, but every one of them happens on a path that
/// provably holds `UART_TX`'s guard (or is single-threaded init).
static mut UART_IER: u32 = 0;

/// Address used as the `sleep_on_chan`/`wakeup_on_chan` wait channel
/// for TX buffer space. The C original used `&uart_tx_r` as this token;
/// here the single fixed address of the `UART_TX` static serves the
/// same role — it is only ever paired between the TX waiters
/// ([`uartputc`]/[`uartputs`] via `sleep_on`, [`uart_tx_wait`] via
/// `sleep_on_interruptible`) and [`uartstart`]'s `wakeup_on_chan`,
/// and is never dereferenced, so any consistent unique address is a
/// faithful replacement.
#[inline(always)]
fn tx_chan() -> *mut c_void {
    core::ptr::addr_of!(UART_TX) as *mut c_void
}

// ===========================================================================
// RX ring buffer + lock.
// ===========================================================================

const UART_RX_BUF_SIZE: usize = 128;

static mut UART_RX_LOCK: spinlock_t = spinlock_t {
    locked: 0,
    name: c"uart_rx_lock".as_ptr() as *mut c_char,
    cpu: core::ptr::null_mut(),
};

static mut UART_RX_BUF: [u8; UART_RX_BUF_SIZE] = [0; UART_RX_BUF_SIZE];
static mut UART_RX_W: u64 = 0;
static mut UART_RX_R: u64 = 0;

// ===========================================================================
// uartinit — bring-up sequence.
// ===========================================================================
//
// UART bring-up rationale (PXA + 16550) and prior failure modes:
// 1) IER=0: stop IRQs while changing FIFO/LCR/MCR to avoid spurious
//    interrupts.
// 2) FIFO reset (enable->clear->disable): flush stale RX/TX state;
//    mirrors Linux PXA flow.
// 3) Read LSR/RHR/IIR/MSR: drains latched status so later enables do
//    not fire immediately.
// 4) LCR=8N1: console framing expected by boot ROM/host; keeps
//    parity/stop bits default.
// 5) MCR sets DTR/RTS and OUT2: OUT2 is required on PXA to wire the
//    IRQ line into the PLIC; without OUT2 we previously saw no UART
//    interrupts and a stuck TX path.
// 6) FCR trigger: PXA (64-byte FIFO) uses 8-byte to cut interrupt
//    rate; 16550 (16-byte) uses 1-byte for latency. Earlier, forcing
//    8-byte on 16550 caused sluggish echo.
// 7) Read status again after FIFO re-enable: ensure no pending
//    conditions remain.
// 8) IER: enable RX; on PXA also RTOIE (RX timeout) plus UUE to power
//    the block; TX is toggled dynamically when data exists.
// Historical missteps: using UART5 (unaligned base) broke MMIO
// mapping; missing reg-shift/reg-io-width led to bad register
// offsets; omitting UUE left the PXA UART inert; omitting \r before
// \n caused right-shifted terminal lines.

/// Bring up the UART hardware. Returns 1 on success (matches the C
/// `int uartinit(void)` — always succeeds today, the return value
/// exists for the real-hardware/SBI-fallback path in `console.rs`).
pub(crate) extern "C" fn uartinit() -> c_int {
    // Disable interrupts to avoid spurious IRQs while reprogramming.
    write_reg(IER, 0x00);

    // Reset FIFOs: enable, flush both, then disable (Linux PXA sequence).
    write_reg(FCR, FCR_FIFO_ENABLE);
    write_reg(FCR, FCR_FIFO_ENABLE | FCR_FIFO_CLEAR);
    write_reg(FCR, 0);

    // Drain latched status so new config starts clean.
    let _ = read_reg(LSR);
    let _ = read_reg(RHR);
    let _ = read_reg(IIR);
    let _ = read_reg(MSR);

    // 8N1 framing; OUT2 required on PXA for IRQ line to reach PLIC.
    write_reg(LCR, LCR_EIGHT_BITS);
    write_reg(MCR, MCR_DTR | MCR_RTS | MCR_OUT2);

    // RX trigger: 8-byte on PXA (64-byte FIFO to cut IRQ rate), 1-byte
    // on 16550 for responsiveness.
    // SAFETY: single boot-time-writer global, see module doc.
    if unsafe { __uart0_reg_io_width } == 4 {
        write_reg(FCR, FCR_FIFO_ENABLE | FCR_TRIGGER_8);
    } else {
        write_reg(FCR, FCR_FIFO_ENABLE | FCR_TRIGGER_1);
    }

    // Clear status again after re-enabling FIFO.
    let _ = read_reg(LSR);
    let _ = read_reg(RHR);
    let _ = read_reg(IIR);
    let _ = read_reg(MSR);

    // Enable RX interrupts; PXA also needs RTOIE for RX timeout and
    // UUE to power the block.
    // SAFETY: runs during single-threaded `consoleinit()`, before any
    // other hart or interrupt handler can touch `UART_IER`.
    unsafe {
        UART_IER = if __uart0_reg_io_width == 4 {
            IER_RX_ENABLE | IER_RTOIE | IER_UUE
        } else {
            IER_RX_ENABLE
        };
        write_reg(IER, UART_IER);
    }

    1 // Success - UART is now initialized
}

// ===========================================================================
// TX path.
// ===========================================================================

/// Add a character to the output buffer and tell the UART to start
/// sending if it isn't already. Blocks if the output buffer is full.
/// Because it may block, it can't be called from interrupts; it's
/// only suitable for use by `write()`.
pub(crate) extern "C" fn uartputc(c: c_int) {
    // N-R7 lock-owns-data: the TX ring + its lock are `UART_TX`
    // (`SpinLock<UartTx>`); `tx` `DerefMut`s to `&mut UartTx`, so
    // `tx.buf`/`tx.w`/`tx.r` are plain safe field accesses. The guard
    // drops (unlocks) at scope end. TX path can sleep, so (as in C) it
    // must not run from IRQ context.
    let mut tx = UART_TX.lock();

    while tx.w == tx.r + UART_TX_BUF_SIZE as u64 {
        // Buffer full: atomically release `UART_TX`, block on
        // `tx_chan()`, and reacquire before returning — the guard is
        // still valid (lock re-held) afterward. Mirrors C `sleep(&uart_tx_r,
        // &uart_tx_lock)`; `uartstart` wakes this exact channel. `sleep_on`
        // takes `&mut self`, ending any `Deref` borrow before the sleep
        // and barring the compiler from hoisting the `tx.w`/`tx.r` retest
        // out of the wait loop (freeze-noalias defense).
        tx.sleep_on(tx_chan());
    }
    let idx = (tx.w % UART_TX_BUF_SIZE as u64) as usize;
    tx.buf[idx] = c as u8;
    tx.w += 1;
    uartstart(&mut tx);
}

/// Batch version of [`uartputc`] — write multiple characters at once.
/// Blocks if the output buffer is full. Because it may block, it
/// can't be called from interrupts.
///
/// # Safety
/// `s` must point to at least `n` readable bytes.
pub(crate) unsafe extern "C" fn uartputs(s: *const c_char, n: c_int) {
    // N-R7 lock-owns-data: `UART_TX` guard, as in `uartputc`. Only the
    // `*s.offset(i)` user-pointer read remains `unsafe` (this fn's
    // documented precondition); the ring state is safe field access.
    let mut tx = UART_TX.lock();

    for i in 0..n as isize {
        while tx.w == tx.r + UART_TX_BUF_SIZE as u64 {
            tx.sleep_on(tx_chan());
        }
        let idx = (tx.w % UART_TX_BUF_SIZE as u64) as usize;
        // SAFETY: `s`/`n` validity is this function's documented precondition.
        tx.buf[idx] = unsafe { *s.offset(i) } as u8;
        tx.w += 1;
    }

    uartstart(&mut tx);
}

/// Non-blocking batch enqueue: copy up to `n` bytes from `s` into the
/// UART TX software buffer. Returns the number of characters accepted
/// (may be 0 if the buffer is full). Kicks `uartstart()` so the
/// interrupt handler will drain the buffer.
///
/// # Safety
/// `s` must point to at least `n` readable bytes.
pub(crate) unsafe extern "C" fn uartputs_nb(s: *const c_char, n: c_int) -> c_int {
    let mut enqueued: c_int = 0;
    // N-R7 lock-owns-data: `UART_TX` guard (non-blocking, never sleeps).
    let mut tx = UART_TX.lock();
    for i in 0..n as isize {
        if tx.w == tx.r + UART_TX_BUF_SIZE as u64 {
            break; // buffer full
        }
        let idx = (tx.w % UART_TX_BUF_SIZE as u64) as usize;
        // SAFETY: `s`/`n` validity is this function's documented precondition.
        tx.buf[idx] = unsafe { *s.offset(i) } as u8;
        tx.w += 1;
        enqueued += 1;
    }
    if enqueued > 0 {
        uartstart(&mut tx);
    }
    enqueued
}

/// Wait interruptibly for space in the UART TX buffer. Returns 0 when
/// space is available, `-EINTR` on pending signal.
pub(crate) extern "C" fn uart_tx_wait() -> c_int {
    // N-R7 lock-owns-data: `UART_TX` guard. The wait is interruptible, so
    // it goes through the guard's own `sleep_on_interruptible(&mut self)`
    // — which releases and reacquires *this* lock on both the woken and
    // the signalled path, and which (taking `&mut tx`) gives the same
    // borrow-ending / anti-hoist guarantees the guard's `sleep_on` gives
    // the blocking TX paths (`w`/`r` are `Freeze` `u64`s another hart
    // mutates via `uartstart` under the same lock).
    let mut tx = UART_TX.lock();
    while tx.w == tx.r + UART_TX_BUF_SIZE as u64 {
        if tx.sleep_on_interruptible(tx_chan()) != 0 {
            // Interrupted by signal; lock is re-held, drop it and bail.
            drop(tx);
            return -EINTR;
        }
    }
    0
}

/// Alternate version of [`uartputc`] that doesn't use interrupts, for
/// use by kernel `printf()` and to echo characters. It spins waiting
/// for the uart's output register to be empty. IRQ-safe: never sleeps,
/// only spin-waits on hardware state.
pub(crate) extern "C" fn uartputc_sync(c: c_int) {
    xv6_push_off();
    // Wait for Transmit Holding Empty to be set in LSR.
    while (read_reg(LSR) & LSR_TX_IDLE) == 0 {}
    write_reg(THR, c as u32);
    xv6_pop_off();
}

/// If the UART is idle, and a character is waiting in the transmit
/// buffer, send it. Called from both the top- and bottom-half.
///
/// N-R7 lock-owns-data: takes `&mut UartTx` — the caller passes its live
/// `UART_TX` guard (`&mut tx`), which *is* the proof the lock is held
/// (was the C `// caller must hold uart_tx_lock` comment). The ring
/// state is now safe field access; only the `UART_IER` shadow (still a
/// bare `static mut`, out of this wave's scope) keeps an `unsafe` block,
/// itself sound because every caller holds the TX guard.
fn uartstart(tx: &mut UartTx) {
    // Check if UART TX FIFO is ready (THR empty).
    if (read_reg(LSR) & LSR_TX_IDLE) == 0 {
        // The UART transmit holding register is full; enable TX
        // interrupt so we get notified when ready.
        // SAFETY: `UART_IER` is guarded by the TX lock, which the caller
        // holds (it passed us its live `UART_TX` guard as `tx`).
        unsafe {
            if tx.w != tx.r && (UART_IER & IER_TX_ENABLE) == 0 {
                UART_IER |= IER_TX_ENABLE;
                write_reg(IER, UART_IER);
            }
        }
        return;
    }

    // Fill TX FIFO with up to half the FIFO size.
    let max_batch = uart_fifo_size() / 2;
    let mut sent: u32 = 0;

    while sent < max_batch {
        if tx.w == tx.r {
            // transmit buffer is empty.
            break;
        }
        let c = tx.buf[(tx.r % UART_TX_BUF_SIZE as u64) as usize];
        tx.r += 1;
        sent += 1;
        write_reg(THR, c as u32);
    }

    // Enable or disable TX interrupt based on whether more data is
    // pending.
    // SAFETY: `UART_IER` guarded by the TX lock the caller holds (see above).
    unsafe {
        if tx.w != tx.r {
            // More data to send - enable TX interrupt.
            if (UART_IER & IER_TX_ENABLE) == 0 {
                UART_IER |= IER_TX_ENABLE;
                write_reg(IER, UART_IER);
            }
        } else {
            // Buffer empty - disable TX interrupt to avoid spurious
            // interrupts.
            if (UART_IER & IER_TX_ENABLE) != 0 {
                UART_IER &= !IER_TX_ENABLE;
                write_reg(IER, UART_IER);
            }
        }
    }

    // Maybe uartputc() is waiting for space in the buffer.
    if sent > 0 {
        Scheduler::wakeup_on_chan(tx_chan());
    }
}

// ===========================================================================
// RX path.
// ===========================================================================

/// Drain the hardware RX FIFO into the software buffer. Caller must
/// hold `UART_RX_LOCK`.
///
/// # Safety
/// Caller must hold `UART_RX_LOCK`.
unsafe fn uartrecv() {
    // Read all available bytes from the hardware RX FIFO.
    while (read_reg(LSR) & LSR_RX_READY) != 0 {
        // SAFETY: caller holds `UART_RX_LOCK`.
        unsafe {
            if UART_RX_W == UART_RX_R + UART_RX_BUF_SIZE as u64 {
                // SW buffer full: drop byte (console path is
                // best-effort).
                let _ = read_reg(RHR); // discard to advance HW FIFO
                continue;
            }
            UART_RX_BUF[(UART_RX_W % UART_RX_BUF_SIZE as u64) as usize] = read_reg(RHR) as u8;
            UART_RX_W += 1;
        }
    }
}

/// Read one input character from the UART. Return -1 if none is
/// waiting.
pub(crate) extern "C" fn uartgetc() -> c_int {
    let mut c: c_int = -1;
    // SAFETY: `UART_RX_LOCK` is a valid, compile-time-initialised
    // spinlock; `uartrecv` is called with it held, as required.
    unsafe {
        spin_lock(&raw mut UART_RX_LOCK);

        uartrecv();

        if UART_RX_R != UART_RX_W {
            c = UART_RX_BUF[(UART_RX_R % UART_RX_BUF_SIZE as u64) as usize] as c_int;
            UART_RX_R += 1;
        }

        spin_unlock(&raw mut UART_RX_LOCK);
    }
    c
}

/// Batch read from the UART. Reads up to `n` characters into `buf`.
/// Returns the number of characters read.
///
/// # Safety
/// `buf` must point to at least `n` writable bytes.
pub(crate) unsafe extern "C" fn uartgets(buf: *mut c_char, n: c_int) -> c_int {
    let mut i: isize = 0;
    // SAFETY: `UART_RX_LOCK` valid as above; `buf`/`n` validity is
    // this function's documented precondition.
    unsafe {
        spin_lock(&raw mut UART_RX_LOCK);

        uartrecv();

        while (i as c_int) < n && UART_RX_R != UART_RX_W {
            *buf.offset(i) = UART_RX_BUF[(UART_RX_R % UART_RX_BUF_SIZE as u64) as usize] as c_char;
            i += 1;
            UART_RX_R += 1;
        }

        spin_unlock(&raw mut UART_RX_LOCK);
    }
    i as c_int
}

// ===========================================================================
// Interrupt handler.
// ===========================================================================

/// Handle a uart interrupt, raised because input has arrived, or the
/// uart is ready for more output, or both. Called from `do_irq()`.
///
/// # Safety
/// Must be called from interrupt/IRQ-dispatch context with a valid
/// `irq_handler_t` calling convention (matches C `void uartintr(int,
/// void*, device_t*)`); `data`/`dev` are unused, forwarded only for
/// ABI compatibility with the registered `irq_desc`.
pub(crate) extern "C" fn uartintr(_irq: c_int, _data: *mut c_void, _dev: *mut c_void) {
    // SAFETY: `UART_RX_LOCK` is a valid, compile-time-initialised
    // spinlock; the RX ring is out of this wave's scope and stays raw.
    // (The TX ring moved into `UART_TX`; its acquire is below, outside
    // this block.)
    unsafe {
        spin_lock(&raw mut UART_RX_LOCK);
        uartrecv();

        // Process all buffered input. Drop the RX lock around each
        // `consoleintr` call, exactly as the C did, since the console
        // line discipline may need other locks.
        while UART_RX_R != UART_RX_W {
            let c = UART_RX_BUF[(UART_RX_R % UART_RX_BUF_SIZE as u64) as usize] as c_int;
            UART_RX_R += 1;
            spin_unlock(&raw mut UART_RX_LOCK);
            consoleintr(c);
            spin_lock(&raw mut UART_RX_LOCK);
        }
        spin_unlock(&raw mut UART_RX_LOCK);
    }

    // Send buffered characters. N-R7 lock-owns-data: the TX ring is now
    // `UART_TX` (`SpinLock<UartTx>`); acquire its guard and hand it to
    // `uartstart`. Guard drops (unlocks) at scope end — byte-identical
    // to the old `spin_lock(uart_tx_lock); uartstart(); spin_unlock`.
    let mut tx = UART_TX.lock();
    uartstart(&mut tx);
}
