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
//!   * `UART_TX_LOCK` guards the TX software ring + `UART_IER`'s TX-enable
//!     bit. [`uartputc`]/[`uartputs`] block (via `sleep_on_chan`) when the
//!     ring is full — **not** IRQ-safe, only for `write()` paths.
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
#[no_mangle]
pub static mut __uart0_mmio_base: u64 = 0x1000_0000;
/// PLIC hardware IRQ number for UART0.
#[no_mangle]
pub static mut __uart0_irqno: u64 = 10;
/// UART clock in Hz; 0 = default (assume 1.8432 MHz for QEMU).
#[no_mangle]
pub static mut __uart0_clock: u32 = 0;
/// Baud rate; 0 = default 115200.
#[no_mangle]
pub static mut __uart0_baud: u32 = 0;
/// Register spacing: 0 = 1-byte, 2 = 4-byte (common on SoCs).
#[no_mangle]
pub static mut __uart0_reg_shift: u32 = 0;
/// Register access width: 1 = 8-bit, 4 = 32-bit (PXA UART).
#[no_mangle]
pub static mut __uart0_reg_io_width: u32 = 1;

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

unsafe extern "C" {
    pub safe fn spin_init(lk: *mut spinlock_t, name: *const c_char);
    pub safe fn spin_lock(lk: *mut spinlock_t);
    pub safe fn spin_unlock(lk: *mut spinlock_t);

    pub safe fn sleep_on_chan(chan: *mut c_void, lk: *mut spinlock_t);
    pub safe fn sleep_on_chan_interruptible(chan: *mut c_void, lk: *mut spinlock_t) -> c_int;
    pub safe fn wakeup_on_chan(chan: *mut c_void);

    pub safe fn xv6_push_off();
    pub safe fn xv6_pop_off();

    /// Console line-discipline entry point (`kernel/console.rs`, ported
    /// in this same wave). Forward declaration mirrors the C
    /// `void consoleintr(int)` used by `uart.c`.
    pub safe fn consoleintr(c: c_int);
}

const EINTR: c_int = 4;

// ===========================================================================
// TX ring buffer + lock.
// ===========================================================================

const UART_TX_BUF_SIZE: usize = 128;

/// Mirrors the C `spinlock_t uart_tx_lock = SPINLOCK_INITIALIZED(...)`:
/// valid for locking from the moment `.bss`/`.data` are live, no
/// runtime `spin_init` call required (matches the C compile-time
/// initializer convention).
static mut UART_TX_LOCK: spinlock_t = spinlock_t {
    locked: 0,
    name: c"uart_tx_lock".as_ptr() as *mut c_char,
    cpu: core::ptr::null_mut(),
};

static mut UART_TX_BUF: [u8; UART_TX_BUF_SIZE] = [0; UART_TX_BUF_SIZE];
static mut UART_TX_W: u64 = 0;
static mut UART_TX_R: u64 = 0;
/// Current IER value, for dynamic TX-interrupt enable/disable. Mutated
/// only while `UART_TX_LOCK` is held (mirrors the C `static uint32
/// uart_ier`), except during `uartinit()` which runs before any
/// concurrency is possible.
static mut UART_IER: u32 = 0;

/// Address used as the `sleep_on_chan`/`wakeup_on_chan` wait channel
/// for TX buffer space, mirroring the C `&uart_tx_r` (the channel
/// value only needs to be a stable, unique address — never
/// dereferenced as the `u64` it points to).
#[inline(always)]
fn tx_chan() -> *mut c_void {
    &raw mut UART_TX_R as *mut c_void
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
#[no_mangle]
pub extern "C" fn uartinit() -> c_int {
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
#[no_mangle]
pub extern "C" fn uartputc(c: c_int) {
    // SAFETY: `UART_TX_LOCK` is a valid, compile-time-initialised
    // spinlock (see its definition above); TX path is lock-protected
    // and can sleep, so it must not run from IRQ context (matches the
    // C comment).
    unsafe {
        spin_lock(&raw mut UART_TX_LOCK);

        while UART_TX_W == UART_TX_R + UART_TX_BUF_SIZE as u64 {
            // Buffer full: sleep until uartstart() frees space.
            sleep_on_chan(tx_chan(), &raw mut UART_TX_LOCK);
        }
        UART_TX_BUF[(UART_TX_W % UART_TX_BUF_SIZE as u64) as usize] = c as u8;
        UART_TX_W += 1;
        uartstart();

        spin_unlock(&raw mut UART_TX_LOCK);
    }
}

/// Batch version of [`uartputc`] — write multiple characters at once.
/// Blocks if the output buffer is full. Because it may block, it
/// can't be called from interrupts.
///
/// # Safety
/// `s` must point to at least `n` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn uartputs(s: *const c_char, n: c_int) {
    // SAFETY: same lock contract as `uartputc`; `s`/`n` validity is
    // this function's documented precondition.
    unsafe {
        spin_lock(&raw mut UART_TX_LOCK);

        for i in 0..n as isize {
            while UART_TX_W == UART_TX_R + UART_TX_BUF_SIZE as u64 {
                sleep_on_chan(tx_chan(), &raw mut UART_TX_LOCK);
            }
            UART_TX_BUF[(UART_TX_W % UART_TX_BUF_SIZE as u64) as usize] = *s.offset(i) as u8;
            UART_TX_W += 1;
        }

        uartstart();
        spin_unlock(&raw mut UART_TX_LOCK);
    }
}

/// Non-blocking batch enqueue: copy up to `n` bytes from `s` into the
/// UART TX software buffer. Returns the number of characters accepted
/// (may be 0 if the buffer is full). Kicks `uartstart()` so the
/// interrupt handler will drain the buffer.
///
/// # Safety
/// `s` must point to at least `n` readable bytes.
#[no_mangle]
pub unsafe extern "C" fn uartputs_nb(s: *const c_char, n: c_int) -> c_int {
    let mut enqueued: c_int = 0;
    // SAFETY: same lock contract as `uartputc`; `s`/`n` validity is
    // this function's documented precondition.
    unsafe {
        spin_lock(&raw mut UART_TX_LOCK);
        for i in 0..n as isize {
            if UART_TX_W == UART_TX_R + UART_TX_BUF_SIZE as u64 {
                break; // buffer full
            }
            UART_TX_BUF[(UART_TX_W % UART_TX_BUF_SIZE as u64) as usize] = *s.offset(i) as u8;
            UART_TX_W += 1;
            enqueued += 1;
        }
        if enqueued > 0 {
            uartstart();
        }
        spin_unlock(&raw mut UART_TX_LOCK);
    }
    enqueued
}

/// Wait interruptibly for space in the UART TX buffer. Returns 0 when
/// space is available, `-EINTR` on pending signal.
#[no_mangle]
pub extern "C" fn uart_tx_wait() -> c_int {
    // SAFETY: same lock contract as `uartputc`.
    unsafe {
        spin_lock(&raw mut UART_TX_LOCK);
        while UART_TX_W == UART_TX_R + UART_TX_BUF_SIZE as u64 {
            let ret = sleep_on_chan_interruptible(tx_chan(), &raw mut UART_TX_LOCK);
            if ret != 0 {
                spin_unlock(&raw mut UART_TX_LOCK);
                return -EINTR;
            }
        }
        spin_unlock(&raw mut UART_TX_LOCK);
    }
    0
}

/// Alternate version of [`uartputc`] that doesn't use interrupts, for
/// use by kernel `printf()` and to echo characters. It spins waiting
/// for the uart's output register to be empty. IRQ-safe: never sleeps,
/// only spin-waits on hardware state.
#[no_mangle]
pub extern "C" fn uartputc_sync(c: c_int) {
    xv6_push_off();
    // Wait for Transmit Holding Empty to be set in LSR.
    while (read_reg(LSR) & LSR_TX_IDLE) == 0 {}
    write_reg(THR, c as u32);
    xv6_pop_off();
}

/// If the UART is idle, and a character is waiting in the transmit
/// buffer, send it. Caller must hold `UART_TX_LOCK`. Called from both
/// the top- and bottom-half.
///
/// # Safety
/// Caller must hold `UART_TX_LOCK`.
#[no_mangle]
pub unsafe extern "C" fn uartstart() {
    // Check if UART TX FIFO is ready (THR empty).
    if (read_reg(LSR) & LSR_TX_IDLE) == 0 {
        // The UART transmit holding register is full; enable TX
        // interrupt so we get notified when ready.
        // SAFETY: caller holds `UART_TX_LOCK`, which is this static's
        // documented invariant.
        unsafe {
            if UART_TX_W != UART_TX_R && (UART_IER & IER_TX_ENABLE) == 0 {
                UART_IER |= IER_TX_ENABLE;
                write_reg(IER, UART_IER);
            }
        }
        return;
    }

    // Fill TX FIFO with up to half the FIFO size.
    let max_batch = uart_fifo_size() / 2;
    let mut sent: u32 = 0;

    // SAFETY: caller holds `UART_TX_LOCK`.
    unsafe {
        while sent < max_batch {
            if UART_TX_W == UART_TX_R {
                // transmit buffer is empty.
                break;
            }
            let c = UART_TX_BUF[(UART_TX_R % UART_TX_BUF_SIZE as u64) as usize];
            UART_TX_R += 1;
            sent += 1;
            write_reg(THR, c as u32);
        }

        // Enable or disable TX interrupt based on whether more data is
        // pending.
        if UART_TX_W != UART_TX_R {
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
        wakeup_on_chan(tx_chan());
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
#[no_mangle]
pub extern "C" fn uartgetc() -> c_int {
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
#[no_mangle]
pub unsafe extern "C" fn uartgets(buf: *mut c_char, n: c_int) -> c_int {
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
#[no_mangle]
pub extern "C" fn uartintr(_irq: c_int, _data: *mut c_void, _dev: *mut c_void) {
    // SAFETY: `UART_RX_LOCK`/`UART_TX_LOCK` are valid,
    // compile-time-initialised spinlocks.
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

        // Send buffered characters.
        spin_lock(&raw mut UART_TX_LOCK);
        uartstart();
        spin_unlock(&raw mut UART_TX_LOCK);
    }
}
