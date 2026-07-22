//! Goldfish RTC driver — Rust port of `kernel/timer/goldfish_rtc.c` (Phase 2
//! Wave 8, see `docs/rustify/phase2_plan.md`).
//!
//! Provides wall-clock time (nanoseconds since Unix epoch) and an
//! alarm/interrupt facility for the Goldfish RTC device QEMU's `virt`
//! machine emulates. `goldfish_rtc_read_ns`/`goldfish_rtc_read_sec` need no
//! initialization — they are plain register reads — and are the only two
//! entry points this port's live callers use today
//! (`proc/sysproc.rs`'s `sys_gettimeofday`/`sys_time` family calls
//! `goldfish_rtc_read_ns` directly via its own `extern` declaration).
//! [`goldfish_rtc_init`] (which registers the alarm IRQ handler and arms a
//! 1-second periodic alarm) has **no live caller**: `start_kernel.c`'s call
//! site is commented out (`// goldfish_rtc_init();`) on the pre-port
//! baseline, and `start_kernel.c` is out of this wave's touch scope, so
//! that stays true after this port -- preserved exactly, not a regression.
//!
//! # MMIO
//!
//! Every register access funnels through [`rtc_read_reg`]/[`rtc_write_reg`]
//! (`read_volatile`/`write_volatile` on `*const/*mut u32` at
//! `__goldfish_rtc_mmio_base + offset`), the same single-funnel pattern as
//! `uart.rs`/`irq/plic.rs`. Unlike those two, `__goldfish_rtc_mmio_base` is
//! a fixed QEMU-`virt` default (`0x101000`) that `dev/fdt.c` never
//! overwrites (confirmed by grep across the C tree) -- so there is no
//! "FDT-configured" boot-time-write hazard here, only the usual
//! "kernel-pinned MMIO window, no barrier needed" contract already
//! documented in `uart.rs`'s module doc (QEMU's PMA for this region is
//! strongly ordered).

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void};
use core::sync::atomic::{AtomicU64, Ordering};

use crate::irq::irq_core::{IrqCore, IrqDesc};

// ===========================================================================
// MMIO base / IRQ number. See the module doc: fixed QEMU-`virt` defaults,
// never written by `dev/fdt.c` (unlike `uart.rs`'s `__uart0_mmio_base` /
// `irq/plic.rs`'s `__plic_mmio_base`). P3-D3c: `#[no_mangle]` dropped --
// the one live external reader (`mm/vm_pgtab.rs`'s `kvmmake`) now reaches
// it by crate path (thin accessor in its `ffi` module, same P3-1D
// `__uart0_mmio_base` pattern) instead of an `extern` redeclaration.
// ===========================================================================
pub(crate) static mut __goldfish_rtc_mmio_base: u64 = 0x101000;

// P3-1B: no other file references this symbol (grep-verified) -- demoted
// from `#[no_mangle]`.
pub(crate) static mut __goldfish_rtc_irqno: u64 = 11;

// Goldfish RTC register offsets. The RTC returns time in nanoseconds since
// the Unix epoch.
const GOLDFISH_RTC_TIME_LOW: u64 = 0x00;
const GOLDFISH_RTC_TIME_HIGH: u64 = 0x04;
const GOLDFISH_RTC_ALARM_LOW: u64 = 0x08;
const GOLDFISH_RTC_ALARM_HIGH: u64 = 0x0C;
const GOLDFISH_RTC_IRQ_ENABLED: u64 = 0x10;
const GOLDFISH_RTC_ALARM_CLEAR: u64 = 0x14;
const GOLDFISH_RTC_IRQ_CLEAR: u64 = 0x1C;

const NS_PER_SEC: u64 = 1_000_000_000;

/// Counter for alarm interrupts (`static volatile uint64 rtc_alarm_count`
/// in the C original, already combined with `__atomic_*` builtins there --
/// the `AtomicU64` here supersedes both the `volatile` qualifier and the
/// builtins with a single, correctly-ordered primitive).
static RTC_ALARM_COUNT: AtomicU64 = AtomicU64::new(0);

/// `static int rtc_initialized;` in the C original -- a plain,
/// non-atomic flag. [`goldfish_rtc_init`] has no live caller (see the
/// module doc), so this is never raced against in practice; kept as a
/// faithful, non-atomic 1:1 port rather than upgraded, since introducing
/// synchronization here would be speculative for an unreachable path.
static mut RTC_INITIALIZED: bool = false;

// ===========================================================================
// Externs.
// ===========================================================================
unsafe extern "C" {
    // printf is variadic, so it cannot be declared `safe`.
}

/// Goldfish RTC driver -- ZST housing every free fn this file used to
/// export at crate scope (P3-OO mesh sweep, see `git show 91ef670` for the
/// established convention). No receiver: every fn below keeps its exact
/// C-derived parameter list.
pub(crate) struct GoldfishRtc;

impl GoldfishRtc {
    #[inline(always)]
    fn rtc_base() -> u64 {
        // SAFETY: single boot-time-constant value (see the module doc: never
        // written by `dev/fdt.c`, unlike the uart/plic MMIO bases), read-only
        // for the kernel's entire life.
        unsafe { __goldfish_rtc_mmio_base }
    }

    /// Read a 32-bit register from the RTC. The file's only volatile read
    /// site.
    #[inline(always)]
    fn rtc_read_reg(offset: u64) -> u32 {
        // SAFETY: `rtc_base() + offset` is always one of this file's fixed
        // register offsets, within the Goldfish RTC's MMIO window (reserved in
        // the kernel page table by `mm/vm_pgtab.rs::kvmmake`, unconditionally,
        // regardless of whether `goldfish_rtc_init` ever runs).
        unsafe { core::ptr::read_volatile((Self::rtc_base() + offset) as *const u32) }
    }

    /// Write a 32-bit value to an RTC register. The file's only volatile write
    /// site.
    #[inline(always)]
    fn rtc_write_reg(offset: u64, value: u32) {
        // SAFETY: see `rtc_read_reg`.
        unsafe { core::ptr::write_volatile((Self::rtc_base() + offset) as *mut u32, value) };
    }

    /// Rust port of `goldfish_rtc_read_ns()`. Reads current time in
    /// nanoseconds since the Unix epoch using a high-low-high read pattern to
    /// handle the low word wrapping mid-read.
    pub(crate) extern "C" fn goldfish_rtc_read_ns() -> u64 {
        loop {
            let high = Self::rtc_read_reg(GOLDFISH_RTC_TIME_HIGH);
            let low = Self::rtc_read_reg(GOLDFISH_RTC_TIME_LOW);
            let high2 = Self::rtc_read_reg(GOLDFISH_RTC_TIME_HIGH);
            if high == high2 {
                return ((high as u64) << 32) | low as u64;
            }
        }
    }

    /// Rust port of `goldfish_rtc_read_sec()`.
    pub(crate) extern "C" fn goldfish_rtc_read_sec() -> u64 {
        Self::goldfish_rtc_read_ns() / NS_PER_SEC
    }

    /// Mirrors `rtc_set_alarm_absolute()`: write high first, then low
    /// (hardware latches the alarm on the low-word write).
    fn rtc_set_alarm_absolute(alarm_ns: u64) {
        Self::rtc_write_reg(GOLDFISH_RTC_ALARM_HIGH, (alarm_ns >> 32) as u32);
        Self::rtc_write_reg(GOLDFISH_RTC_ALARM_LOW, (alarm_ns & 0xFFFF_FFFF) as u32);
    }

    /// Rust port of `goldfish_rtc_set_alarm_ns()`: fire an alarm `ns`
    /// nanoseconds from now. `wrapping_add` mirrors the C original's plain
    /// unsigned `+` (defined wraparound, not UB, for `uint64`).
    pub(crate) extern "C" fn goldfish_rtc_set_alarm_ns(ns: u64) {
        let now = Self::goldfish_rtc_read_ns();
        Self::rtc_set_alarm_absolute(now.wrapping_add(ns));
    }

    /// Rust port of `goldfish_rtc_set_alarm_sec()`. `wrapping_mul` mirrors the
    /// C original's plain unsigned `*`.
    pub(crate) extern "C" fn goldfish_rtc_set_alarm_sec(sec: u64) {
        Self::goldfish_rtc_set_alarm_ns(sec.wrapping_mul(NS_PER_SEC));
    }

    /// Rust port of `goldfish_rtc_clear_alarm()`.
    pub(crate) extern "C" fn goldfish_rtc_clear_alarm() {
        Self::rtc_write_reg(GOLDFISH_RTC_ALARM_CLEAR, 1);
    }

    /// Rust port of `goldfish_rtc_irq_enable()`.
    pub(crate) extern "C" fn goldfish_rtc_irq_enable(enable: c_int) {
        Self::rtc_write_reg(GOLDFISH_RTC_IRQ_ENABLED, if enable != 0 { 1 } else { 0 });
    }

    /// Mirrors `rtc_clear_interrupt()`.
    fn rtc_clear_interrupt() {
        Self::rtc_write_reg(GOLDFISH_RTC_IRQ_CLEAR, 1);
    }

    /// Rust port of `goldfish_rtc_get_alarm_count()`.
    pub(crate) extern "C" fn goldfish_rtc_get_alarm_count() -> u64 {
        RTC_ALARM_COUNT.load(Ordering::SeqCst)
    }
}

/// [`IrqHandler`] implementor for the goldfish RTC alarm interrupt
/// (fn-pointer-callback-slot -> trait-dispatch campaign; was the free
/// fn `goldfish_rtc_intr` + `Some(goldfish_rtc_intr)` in
/// `goldfish_rtc_init`'s `IrqDesc.handler`, now folded into this ZST +
/// trait impl). Called when the alarm fires; sets up the next
/// 1-second alarm. Unreachable today (see the module doc), but ported
/// faithfully since `goldfish_rtc_init` below installs it.
struct GoldfishRtcIrqHandler;

impl crate::irq::irq_core::IrqHandler for GoldfishRtcIrqHandler {
    unsafe fn handle(&self, _irq: c_int, _data: *mut c_void, _dev: *mut c_void) {
        RTC_ALARM_COUNT.fetch_add(1, Ordering::SeqCst);
        GoldfishRtc::rtc_clear_interrupt();
        GoldfishRtc::goldfish_rtc_set_alarm_sec(1);

        // Print a message every 10 seconds for debugging.
        let count = GoldfishRtc::goldfish_rtc_get_alarm_count();
        if count % 10 == 0 {
            let now_sec = GoldfishRtc::goldfish_rtc_read_sec();
            crate::kprintln!(
                "goldfish_rtc: alarm #{}, unix time: {}",
                count as u64,
                now_sec as u64
            );
        }
    }
}

static GOLDFISH_RTC_IRQ_HANDLER: GoldfishRtcIrqHandler = GoldfishRtcIrqHandler;

impl GoldfishRtc {
    /// Rust port of `goldfish_rtc_init()`. Registers the alarm IRQ handler and
    /// arms a periodic 1-second alarm. See the module doc: not called by
    /// anything on the pre-port baseline, and stays that way after this port
    /// (`start_kernel.c` is out of Wave 8's touch scope).
    // P3-1B: no live caller anywhere (see the module doc: the only call site,
    // `start_kernel.c`'s, is commented out and stays that way -- confirmed by
    // this wave's own grep survey). Demoted from `#[no_mangle]`;
    // `#[allow(dead_code)]` documents the gap instead of silently deleting
    // still-plausible public API (matches `timer/sched_timer.rs`'s
    // `sched_timer_add`/`sched_timer_add_deadline` precedent from this same
    // wave).
    #[allow(dead_code)]
    pub(crate) extern "C" fn goldfish_rtc_init() {
        // SAFETY: `goldfish_rtc_init` has no live caller (see the module doc),
        // so there is no concurrent access to guard against in practice; this
        // is a faithful 1:1 port of the C original's non-atomic
        // `rtc_initialized` flag.
        if unsafe { RTC_INITIALIZED } {
            return;
        }

        let now_ns = Self::goldfish_rtc_read_ns();
        let now_sec = now_ns / NS_PER_SEC;
        crate::kprintln!(
            "goldfish_rtc: initializing, current unix time: {}",
            now_sec as u64
        );

        let mut rtc_irq_desc = IrqDesc {
            handler: Some(&GOLDFISH_RTC_IRQ_HANDLER),
            data: core::ptr::null_mut(),
            dev: core::ptr::null_mut(),
            irq: 0,
            count: 0,
            // SAFETY: `rcu_head` is plain-old-data; `register_irq_handler`
            // overwrites it unconditionally (see `irq/irq_core.rs`).
            rcu_head: unsafe { core::mem::zeroed() },
        };
        // SAFETY: `rtc_irq_desc` is a live, fully-initialized `IrqDesc` for the
        // duration of this call; `__goldfish_rtc_irqno` is a boot-time
        // constant (see the module doc).
        let ret = unsafe {
            let irqno = __goldfish_rtc_irqno as c_int;
            IrqCore::register_irq_handler(IrqCore::plic_irq(irqno), &raw mut rtc_irq_desc)
        };
        if ret != 0 {
            crate::kprintln!("goldfish_rtc: failed to register IRQ handler: {}", ret);
            return;
        }

        // Clear any pending interrupts.
        Self::rtc_clear_interrupt();
        Self::goldfish_rtc_clear_alarm();

        // Enable RTC interrupts.
        Self::goldfish_rtc_irq_enable(1);

        // Set the first alarm for 1 second from now.
        Self::goldfish_rtc_set_alarm_sec(1);

        // SAFETY: see the read above -- same "no live caller" justification.
        unsafe { RTC_INITIALIZED = true };
        crate::kprintln!("goldfish_rtc: initialized, alarm set for 1 second intervals");
    }
}
