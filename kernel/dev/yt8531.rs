//! Motorcomm YT8531 Gigabit Ethernet PHY + MDIO driver -- Rust port of
//! `kernel/dev/yt8531.c` (Phase 2 Wave 25, see
//! `docs/rustify/phase2_plan.md`; the last C in `kernel/dev/`, ported
//! together with `x1_emac.rs`/`x1_sdhci.rs` in the same wave).
//!
//! MDIO read/write bit-bang the X1-EMAC's `MAC_MDIO_CONTROL`/
//! `MAC_MDIO_DATA` registers (offsets `0x01A0`/`0x01A4` from the EMAC
//! MMIO base) -- this file's `base` parameter is always an EMAC base
//! pointer, never a PHY-private register space (there is no such thing
//! on this bus topology: the YT8531 sits behind the EMAC's own MDIO
//! master). [`x1_emac`](super::x1_emac) is this module's only caller.
//!
//! # QEMU verifiability (Wave 25 charter)
//!
//! This driver probes nothing on `-machine virt`: [`x1_emac::x1_emac_init`]
//! (the only call chain that reaches any function here) early-returns
//! when `platform.has_emac` is false, which it always is on QEMU's
//! generated device tree (verified in Wave 23's fdt.rs port: QEMU virt's
//! DTB has no EMAC-compatible node). **Compile-verify + boot-no-
//! regression only** -- flagged lower confidence per the plan's §3;
//! functional verification needs real Orange-Pi-RV2 hardware with a
//! YT8531 PHY wired to MDIO.
//!
//! # Diff-review attestation
//!
//! Every function below was reviewed line-by-line against
//! `kernel/dev/yt8531.c` (commit-preserved copy) at port time: control
//! flow, register field masks/shifts, timeout loop bounds, and log
//! message text/argument order all match. `yt8531_init`'s C `goto
//! reset_done` is restructured as a `reset_ok` boolean guarding an early
//! return (Rust has no `goto`) -- semantically identical: the C jump
//! only ever skips the "PHY reset timeout" failure path, which the
//! `if !reset_ok { ...; return -1; }` above the (implicit) fallthrough
//! reproduces exactly.
//!
//! # MMIO ordering
//!
//! No `__sync_synchronize()` calls exist in the C original (0, per the
//! plan's Wave-25 count) -- MDIO is a slow, fully register-polled
//! protocol (every transaction ends by re-reading `MAC_MDIO_CONTROL`
//! until `MDIO_START_TRANS` clears), so the poll loop itself is the only
//! ordering primitive needed, exactly as in C. Every register access
//! goes through [`emac_rd`]/[`emac_wr`] (`read_volatile`/
//! `write_volatile`), so the compiler can never reorder or elide any of
//! them, matching the C `volatile uint32 *` semantics 1:1.

#![allow(non_camel_case_types, non_snake_case, non_upper_case_globals)]

use core::ffi::{c_char, c_int};
use core::ptr;

use crate::bindings::phy_state;

// ---------------------------------------------------------------------------
// Externs -- local per-file `unsafe extern "C"` block, matching this
// crate's established convention (see `kernel/dev/bio.rs`,
// `kernel/dev/blkdev.rs`).
// ---------------------------------------------------------------------------
// P3-D3c: `timer/sched_timer.rs`'s `sleep_ms` is a plain safe Rust fn now
// that its `#[no_mangle]` export is gone -- crate-path import.
use crate::timer::sched_timer::sleep_ms;

// P3-D2a: proc/sched.rs entry point, reached as a plain crate-path item
// instead of an `extern "C"` redeclaration.
use crate::proc::scheduler_yield;

// ===========================================================================
// Register/bit constants -- redeclared locally per this crate's
// established convention for macro-only headers (see `fdt.rs`'s
// `FDT_MAGIC` et al., `nullrand.rs`'s `S_IFCHR`).
// ===========================================================================

// -- kernel/inc/dev/yt8531.h: standard MII (IEEE 802.3) register numbers --
const MII_BMCR: c_int = 0x00;
const MII_BMSR: c_int = 0x01;
const MII_PHYSID1: c_int = 0x02;
const MII_PHYSID2: c_int = 0x03;
const MII_ADVERTISE: c_int = 0x04;
const MII_LPA: c_int = 0x05;
const MII_CTRL1000: c_int = 0x09;
const MII_STAT1000: c_int = 0x0A;

// -- BMCR bits --
const BMCR_RESET: u16 = 1 << 15;
const BMCR_ANENABLE: u16 = 1 << 12;
const BMCR_ANRESTART: u16 = 1 << 9;

// -- BMSR bits --
const BMSR_LSTATUS: u16 = 1 << 2;
const BMSR_ANEGCOMPLETE: u16 = 1 << 5;

// -- ADVERTISE bits (register 4) --
const ADVERTISE_10HALF: u16 = 1 << 5;
const ADVERTISE_10FULL: u16 = 1 << 6;
const ADVERTISE_100HALF: u16 = 1 << 7;
const ADVERTISE_100FULL: u16 = 1 << 8;
const ADVERTISE_PAUSE_CAP: u16 = 1 << 10;
const ADVERTISE_CSMA: u16 = 0x01;

// -- LPA bits (register 5) --
const LPA_10HALF: u16 = 1 << 5;
const LPA_10FULL: u16 = 1 << 6;
const LPA_100HALF: u16 = 1 << 7;
const LPA_100FULL: u16 = 1 << 8;

// -- CTRL1000 bits (register 9) --
const ADVERTISE_1000FULL: u16 = 1 << 9;

// -- STAT1000 bits (register 10) --
const LPA_1000FULL: u16 = 1 << 11;
const LPA_1000HALF: u16 = 1 << 10;

// -- YT8531-specific extended register access --
const YT8531_EXT_REG_ADDR: c_int = 0x1E;
const YT8531_EXT_REG_DATA: c_int = 0x1F;
const YT8531_LED0_CFG: u16 = 0xA00D;
const YT8531_LED1_CFG: u16 = 0xA00E;
const YT8531_LED2_CFG: u16 = 0xA00F;
const YT8531_LED0_VAL: u16 = 0x2600;
const YT8531_LED1_VAL: u16 = 0x2070;
const YT8531_LED2_VAL: u16 = 0x000A;

// -- PHY speed constants --
const PHY_SPEED_10: c_int = 10;
const PHY_SPEED_100: c_int = 100;
const PHY_SPEED_1000: c_int = 1000;

// -- kernel/inc/dev/x1_emac.h: MDIO bus access via EMAC registers.
// Duplicated locally (same values as `x1_emac.rs`'s own copy, which
// needs a disjoint subset for the MAC/DMA registers) -- this crate's
// established per-file redeclaration convention. --
const MAC_MDIO_CONTROL: u32 = 0x01A0;
const MAC_MDIO_DATA: u32 = 0x01A4;
const MDIO_PHY_ADDR_MASK: u32 = 0x1F;
const MDIO_REG_ADDR_SHIFT: u32 = 5;
const MDIO_READ_WRITE: u32 = 1 << 10;
const MDIO_START_TRANS: u32 = 1 << 15;
const MDIO_TIMEOUT_US: i32 = 10000;
const MDIO_POLL_INTERVAL_US: i32 = 100;

/// `YT8531_PHY_ID` (`kernel/inc/dev/x1_emac.h`) -- yes, defined in the
/// EMAC header, not `yt8531.h`; matches the C original's own layout
/// (`yt8531.c` `#include`s `dev/x1_emac.h` for exactly this constant
/// plus the MDIO register set above).
const YT8531_PHY_ID: u32 = 0x4f51_e91b;

// ===========================================================================
// Low-level MMIO helpers. Mirror the C `emac_rd`/`emac_wr` static
// inlines -- every field access downstream funnels through these two, so
// this is the file's entire volatile-access surface (11 `volatile` sites
// in the C original's type declarations/casts; every one of those sites'
// *dereferences*, plus the ones the type-level `volatile` implied,
// become an explicit `read_volatile`/`write_volatile` call here or in
// the loops below, since Rust has no volatile-qualified pointer type).
// ===========================================================================

/// Read a 32-bit MMIO register at byte offset `off` from `base`.
///
/// # Safety
/// `base + off .. base + off + 4` must be valid, live MMIO register
/// space for the duration of the access.
#[inline(always)]
unsafe fn emac_rd(base: *mut u32, off: u32) -> u32 {
    // SAFETY: caller contract.
    unsafe { ptr::read_volatile((base as *mut u8).add(off as usize) as *mut u32) }
}

/// Write a 32-bit MMIO register at byte offset `off` from `base`.
///
/// # Safety
/// Same as [`emac_rd`].
#[inline(always)]
unsafe fn emac_wr(base: *mut u32, off: u32, val: u32) {
    // SAFETY: caller contract.
    unsafe { ptr::write_volatile((base as *mut u8).add(off as usize) as *mut u32, val) };
}

// ===========================================================================
// MDIO bus access via EMAC registers.
// ===========================================================================

/// `int mdio_read(volatile uint32 *base, int phy_addr, int reg)`.
///
/// # Safety
/// `base` must be a live EMAC MMIO base pointer (see module doc).
// P3-1D mesh sweep: no caller anywhere outside this file -- demoted.
pub(crate) unsafe extern "C" fn mdio_read(base: *mut u32, phy_addr: c_int, reg: c_int) -> c_int {
    let mut cmd: u32 = 0;
    cmd |= phy_addr as u32 & MDIO_PHY_ADDR_MASK;
    cmd |= ((reg as u32) & 0x1F) << MDIO_REG_ADDR_SHIFT;
    cmd |= MDIO_START_TRANS | MDIO_READ_WRITE;

    // SAFETY: caller contract.
    unsafe {
        emac_wr(base, MAC_MDIO_DATA, 0);
        emac_wr(base, MAC_MDIO_CONTROL, cmd);
    }

    // Poll until START_TRANS clears (bit 15).
    for _ in 0..(MDIO_TIMEOUT_US / MDIO_POLL_INTERVAL_US) {
        scheduler_yield();
        // SAFETY: caller contract.
        let val = unsafe { emac_rd(base, MAC_MDIO_CONTROL) };
        if val & MDIO_START_TRANS == 0 {
            // SAFETY: caller contract.
            return (unsafe { emac_rd(base, MAC_MDIO_DATA) } & 0xFFFF) as c_int;
        }
    }

    crate::kprintln!("mdio_read: timeout phy={} reg={}", phy_addr, reg);
    -1
}

/// `int mdio_write(volatile uint32 *base, int phy_addr, int reg, uint16 val)`.
///
/// # Safety
/// Same as [`mdio_read`].
// P3-1D mesh sweep: no caller anywhere outside this file -- demoted.
pub(crate) unsafe extern "C" fn mdio_write(base: *mut u32, phy_addr: c_int, reg: c_int, val: u16) -> c_int {
    let mut cmd: u32 = 0;

    // SAFETY: caller contract.
    unsafe { emac_wr(base, MAC_MDIO_DATA, val as u32) };

    cmd |= phy_addr as u32 & MDIO_PHY_ADDR_MASK;
    cmd |= ((reg as u32) & 0x1F) << MDIO_REG_ADDR_SHIFT;
    cmd |= MDIO_START_TRANS; // READ_WRITE bit = 0 -> write

    // SAFETY: caller contract.
    unsafe { emac_wr(base, MAC_MDIO_CONTROL, cmd) };

    // Poll until START_TRANS clears.
    for _ in 0..(MDIO_TIMEOUT_US / MDIO_POLL_INTERVAL_US) {
        scheduler_yield();
        // SAFETY: caller contract.
        let v = unsafe { emac_rd(base, MAC_MDIO_CONTROL) };
        if v & MDIO_START_TRANS == 0 {
            return 0;
        }
    }

    crate::kprintln!(
        "mdio_write: timeout phy={} reg={} val=0x{:x}",
        phy_addr,
        reg,
        ((val as c_int) as i32 as i64) as u64,
    );
    -1
}

// ===========================================================================
// YT8531 extended register access.
// ===========================================================================

/// Write to an extended register (address > `0x1F`) on the YT8531. Uses
/// the indirect access method: write `addr` to reg `0x1E`, then write
/// `data` to reg `0x1F`. Mirrors `static int yt8531_ext_write(...)`.
///
/// # Safety
/// Same as [`mdio_read`].
unsafe fn yt8531_ext_write(base: *mut u32, phy_addr: c_int, ext_reg: u16, val: u16) -> c_int {
    // SAFETY: caller contract.
    unsafe {
        if mdio_write(base, phy_addr, YT8531_EXT_REG_ADDR, ext_reg) < 0 {
            return -1;
        }
        mdio_write(base, phy_addr, YT8531_EXT_REG_DATA, val)
    }
}

// ===========================================================================
// YT8531 PHY initialisation.
// ===========================================================================

/// Scan MDIO bus addresses `0..32` for a PHY; returns its address, or
/// `-1` if none found. Mirrors `static int mdio_scan(...)`.
///
/// # Safety
/// Same as [`mdio_read`].
unsafe fn mdio_scan(base: *mut u32) -> c_int {
    for addr in 0..32 {
        // SAFETY: caller contract.
        let id1 = unsafe { mdio_read(base, addr, MII_PHYSID1) };
        if id1 < 0 || id1 == 0xFFFF || id1 == 0x0000 {
            continue;
        }
        // SAFETY: caller contract.
        let id2 = unsafe { mdio_read(base, addr, MII_PHYSID2) };
        if id2 < 0 || id2 == 0xFFFF {
            continue;
        }
        let phy_id = ((id1 as u32) << 16) | (id2 as u32);
        crate::kprintln!("mdio: found PHY at addr {}, ID=0x{:x}", addr, ((phy_id as i32) as i64) as u64);
        return addr;
    }
    -1
}

/// `int yt8531_init(volatile uint32 *base, int phy_addr)`. Performs soft
/// reset, verifies PHY ID, configures LEDs, advertises all speeds, and
/// restarts auto-negotiation. Returns the PHY address (`>= 0`) on
/// success, `-1` on failure.
///
/// # Safety
/// Same as [`mdio_read`].
// P3-1D mesh sweep: caller (`dev/x1_emac.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn yt8531_init(base: *mut u32, mut phy_addr: c_int) -> c_int {
    // If phy_addr < 0, auto-detect.
    if phy_addr < 0 {
        // SAFETY: caller contract.
        phy_addr = unsafe { mdio_scan(base) };
        if phy_addr < 0 {
            crate::kprintln!("yt8531: no PHY found on MDIO bus");
            return -1;
        }
    }

    // Verify PHY ID.
    // SAFETY: caller contract.
    let id1 = unsafe { mdio_read(base, phy_addr, MII_PHYSID1) };
    // SAFETY: caller contract.
    let id2 = unsafe { mdio_read(base, phy_addr, MII_PHYSID2) };
    if id1 < 0 || id2 < 0 {
        return -1;
    }
    let phy_id = ((id1 as u32) << 16) | (id2 as u32);
    crate::kprintln!("yt8531: PHY ID = 0x{:x} at addr {}", ((phy_id as i32) as i64) as u64, phy_addr);

    // Soft reset the PHY.
    // SAFETY: caller contract.
    if unsafe { mdio_write(base, phy_addr, MII_BMCR, BMCR_RESET) } < 0 {
        return -1;
    }

    // Wait for reset to complete (BMCR_RESET self-clears). Mirrors the C
    // `goto reset_done` -- see module doc's diff-review note.
    let mut reset_ok = false;
    for _ in 0..50 {
        sleep_ms(10);
        // SAFETY: caller contract.
        let bmcr = unsafe { mdio_read(base, phy_addr, MII_BMCR) };
        if bmcr >= 0 && (bmcr as u32 & BMCR_RESET as u32) == 0 {
            reset_ok = true;
            break;
        }
    }
    if !reset_ok {
        crate::kprintln!("yt8531: PHY reset timeout");
        return -1;
    }

    // reset_done:
    // YT8531 LED fixup (from Linux vendor driver).
    if (phy_id & 0xFFFF_FFF0) == (YT8531_PHY_ID & 0xFFFF_FFF0) {
        crate::kprintln!("yt8531: applying LED fixup");
        // SAFETY: caller contract.
        unsafe {
            yt8531_ext_write(base, phy_addr, YT8531_LED0_CFG, YT8531_LED0_VAL);
            yt8531_ext_write(base, phy_addr, YT8531_LED1_CFG, YT8531_LED1_VAL);
            yt8531_ext_write(base, phy_addr, YT8531_LED2_CFG, YT8531_LED2_VAL);
        }
    }

    // Advertise all capabilities: 10/100/1000 full/half duplex.
    let adv = ADVERTISE_CSMA
        | ADVERTISE_10HALF
        | ADVERTISE_10FULL
        | ADVERTISE_100HALF
        | ADVERTISE_100FULL
        | ADVERTISE_PAUSE_CAP;
    // SAFETY: caller contract.
    if unsafe { mdio_write(base, phy_addr, MII_ADVERTISE, adv) } < 0 {
        return -1;
    }

    // Advertise 1000BASE-T full duplex.
    let ctrl1000 = ADVERTISE_1000FULL;
    // SAFETY: caller contract.
    if unsafe { mdio_write(base, phy_addr, MII_CTRL1000, ctrl1000) } < 0 {
        return -1;
    }

    // Enable and restart auto-negotiation.
    let bmcr = BMCR_ANENABLE | BMCR_ANRESTART;
    // SAFETY: caller contract.
    if unsafe { mdio_write(base, phy_addr, MII_BMCR, bmcr) } < 0 {
        return -1;
    }

    crate::kprintln!("yt8531: auto-negotiation started");
    phy_addr
}

// ===========================================================================
// Link state polling.
// ===========================================================================

/// `int yt8531_poll_link(volatile uint32 *base, int phy_addr, struct
/// phy_state *state)`.
///
/// # Safety
/// `base` per [`mdio_read`]; `state` must point to live, writable
/// `phy_state` storage for the duration of this call.
// P3-1D mesh sweep: caller (`dev/x1_emac.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn yt8531_poll_link(base: *mut u32, phy_addr: c_int, state: *mut phy_state) -> c_int {
    // SAFETY: caller contract.
    unsafe {
        (*state).link_up = 0;
        (*state).speed = 0;
        (*state).full_duplex = 0;
    }

    // SAFETY: caller contract.
    let bmsr = unsafe { mdio_read(base, phy_addr, MII_BMSR) };
    if bmsr < 0 {
        return -1;
    }

    // Read BMSR twice -- link status is latching-low.
    // SAFETY: caller contract.
    let bmsr = unsafe { mdio_read(base, phy_addr, MII_BMSR) };
    if bmsr < 0 {
        return -1;
    }

    if bmsr as u32 & BMSR_LSTATUS as u32 == 0 {
        return 0; // No link.
    }

    // SAFETY: caller contract.
    unsafe { (*state).link_up = 1 };

    // Check 1000BASE-T first.
    // SAFETY: caller contract.
    let stat1000 = unsafe { mdio_read(base, phy_addr, MII_STAT1000) };
    if stat1000 >= 0 && (stat1000 as u32 & LPA_1000FULL as u32) != 0 {
        // SAFETY: caller contract.
        unsafe {
            (*state).speed = PHY_SPEED_1000;
            (*state).full_duplex = 1;
        }
        return 0;
    }
    if stat1000 >= 0 && (stat1000 as u32 & LPA_1000HALF as u32) != 0 {
        // SAFETY: caller contract.
        unsafe {
            (*state).speed = PHY_SPEED_1000;
            (*state).full_duplex = 0;
        }
        return 0;
    }

    // Check 100/10 via LPA.
    // SAFETY: caller contract.
    let lpa = unsafe { mdio_read(base, phy_addr, MII_LPA) };
    if lpa < 0 {
        return -1;
    }

    // SAFETY: caller contract.
    unsafe {
        if lpa as u32 & LPA_100FULL as u32 != 0 {
            (*state).speed = PHY_SPEED_100;
            (*state).full_duplex = 1;
        } else if lpa as u32 & LPA_100HALF as u32 != 0 {
            (*state).speed = PHY_SPEED_100;
            (*state).full_duplex = 0;
        } else if lpa as u32 & LPA_10FULL as u32 != 0 {
            (*state).speed = PHY_SPEED_10;
            (*state).full_duplex = 1;
        } else if lpa as u32 & LPA_10HALF as u32 != 0 {
            (*state).speed = PHY_SPEED_10;
            (*state).full_duplex = 0;
        } else {
            // Fallback: assume 100 full.
            (*state).speed = PHY_SPEED_100;
            (*state).full_duplex = 1;
        }
    }

    0
}

/// `int yt8531_wait_autoneg(volatile uint32 *base, int phy_addr, struct
/// phy_state *state, int timeout_ms)`.
///
/// # Safety
/// Same as [`yt8531_poll_link`].
// P3-1D mesh sweep: caller (`dev/x1_emac.rs`) now imports this via
// crate-path `use` instead of an `extern` redeclaration -- demoted.
pub(crate) unsafe extern "C" fn yt8531_wait_autoneg(
    base: *mut u32,
    phy_addr: c_int,
    state: *mut phy_state,
    timeout_ms: c_int,
) -> c_int {
    let mut elapsed: c_int = 0;
    let interval: c_int = 100; // poll every 100ms

    while elapsed < timeout_ms {
        // SAFETY: caller contract.
        let bmsr = unsafe { mdio_read(base, phy_addr, MII_BMSR) };
        if bmsr >= 0 && (bmsr as u32 & BMSR_ANEGCOMPLETE as u32) != 0 {
            crate::kprintln!("yt8531: auto-negotiation complete ({}ms)", elapsed);
            // SAFETY: caller contract.
            return unsafe { yt8531_poll_link(base, phy_addr, state) };
        }
        sleep_ms(interval as u64);
        elapsed += interval;
    }

    // Timeout -- check link anyway.
    crate::kprintln!("yt8531: auto-negotiation timeout after {}ms, checking link...", timeout_ms);
    // SAFETY: caller contract.
    unsafe { yt8531_poll_link(base, phy_addr, state) };
    // SAFETY: caller contract.
    if unsafe { (*state).link_up } != 0 {
        // SAFETY: caller contract.
        unsafe {
            crate::kprintln!(
                "yt8531: link up despite AN timeout: {}Mbps {}",
                (*state).speed,
                if (*state).full_duplex != 0 { "full" } else { "half" },
            );
        }
        return 0;
    }
    -1
}
