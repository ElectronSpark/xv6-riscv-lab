/**
 * @file yt8531.c
 * @brief Motorcomm YT8531 Gigabit Ethernet PHY + MDIO driver for xv6
 *
 * MDIO read/write uses the X1-EMAC MAC_MDIO_CONTROL / MAC_MDIO_DATA
 * registers at offsets 0x01A0 and 0x01A4 from EMAC base.
 *
 * The YT8531 is connected via RGMII to the X1 EMAC.  This driver
 * handles PHY reset, LED fixup, auto-negotiation, and link polling.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "dev/x1_emac.h"
#include "dev/yt8531.h"
#include "timer/timer.h"

/* ======================================================================
 * Low-level helpers
 * ====================================================================== */

/** Read a 32-bit MMIO register at byte offset @off from @base */
static inline uint32 emac_rd(volatile uint32 *base, uint32 off)
{
    return *(volatile uint32 *)((char *)base + off);
}

/** Write a 32-bit MMIO register at byte offset @off from @base */
static inline void emac_wr(volatile uint32 *base, uint32 off, uint32 val)
{
    *(volatile uint32 *)((char *)base + off) = val;
}

/**
 * delay_us - busy-wait for approximately @us microseconds using the
 * RISC-V 'time' CSR (which ticks at TIMEBASE_FREQUENCY Hz).
 */
static void delay_us(uint64 us)
{
    uint64 ticks = (uint64)TIMEBASE_FREQUENCY * us / 1000000ULL;
    uint64 start = r_time();
    while (r_time() - start < ticks)
        ;
}

static void delay_ms(uint64 ms)
{
    delay_us(ms * 1000);
}

/* ======================================================================
 * MDIO bus access via EMAC registers
 * ====================================================================== */

int mdio_read(volatile uint32 *base, int phy_addr, int reg)
{
    uint32 cmd = 0;
    cmd |= (phy_addr & MDIO_PHY_ADDR_MASK);
    cmd |= ((reg & 0x1F) << MDIO_REG_ADDR_SHIFT);
    cmd |= MDIO_START_TRANS | MDIO_READ_WRITE;

    emac_wr(base, MAC_MDIO_DATA, 0);
    emac_wr(base, MAC_MDIO_CONTROL, cmd);

    /* Poll until START_TRANS clears (bit 15) */
    for (int i = 0; i < MDIO_TIMEOUT_US / MDIO_POLL_INTERVAL_US; i++) {
        delay_us(MDIO_POLL_INTERVAL_US);
        uint32 val = emac_rd(base, MAC_MDIO_CONTROL);
        if (!(val & MDIO_START_TRANS)) {
            return (int)(emac_rd(base, MAC_MDIO_DATA) & 0xFFFF);
        }
    }

    printf("mdio_read: timeout phy=%d reg=%d\n", phy_addr, reg);
    return -1;
}

int mdio_write(volatile uint32 *base, int phy_addr, int reg, uint16 val)
{
    uint32 cmd = 0;

    emac_wr(base, MAC_MDIO_DATA, (uint32)val);

    cmd |= (phy_addr & MDIO_PHY_ADDR_MASK);
    cmd |= ((reg & 0x1F) << MDIO_REG_ADDR_SHIFT);
    cmd |= MDIO_START_TRANS; /* READ_WRITE bit = 0 → write */

    emac_wr(base, MAC_MDIO_CONTROL, cmd);

    /* Poll until START_TRANS clears */
    for (int i = 0; i < MDIO_TIMEOUT_US / MDIO_POLL_INTERVAL_US; i++) {
        delay_us(MDIO_POLL_INTERVAL_US);
        uint32 v = emac_rd(base, MAC_MDIO_CONTROL);
        if (!(v & MDIO_START_TRANS))
            return 0;
    }

    printf("mdio_write: timeout phy=%d reg=%d val=0x%x\n", phy_addr, reg, val);
    return -1;
}

/* ======================================================================
 * YT8531 Extended Register Access
 * ====================================================================== */

/**
 * Write to an extended register (address > 0x1F) on the YT8531.
 * Uses the indirect access method: write addr to reg 0x1E,
 * then write data to reg 0x1F.
 */
static int yt8531_ext_write(volatile uint32 *base, int phy_addr,
                            uint16 ext_reg, uint16 val)
{
    if (mdio_write(base, phy_addr, YT8531_EXT_REG_ADDR, ext_reg) < 0)
        return -1;
    return mdio_write(base, phy_addr, YT8531_EXT_REG_DATA, val);
}

/* ======================================================================
 * YT8531 PHY initialisation
 * ====================================================================== */

/**
 * Scan MDIO bus for a PHY and return its address, or -1 if none found.
 * Looks at addresses 0..31.
 */
static int mdio_scan(volatile uint32 *base)
{
    for (int addr = 0; addr < 32; addr++) {
        int id1 = mdio_read(base, addr, MII_PHYSID1);
        if (id1 < 0 || id1 == 0xFFFF || id1 == 0x0000)
            continue;
        int id2 = mdio_read(base, addr, MII_PHYSID2);
        if (id2 < 0 || id2 == 0xFFFF)
            continue;
        uint32 phy_id = ((uint32)id1 << 16) | (uint32)id2;
        printf("mdio: found PHY at addr %d, ID=0x%x\n", addr, phy_id);
        return addr;
    }
    return -1;
}

int yt8531_init(volatile uint32 *base, int phy_addr)
{
    /* If phy_addr < 0, auto-detect */
    if (phy_addr < 0) {
        phy_addr = mdio_scan(base);
        if (phy_addr < 0) {
            printf("yt8531: no PHY found on MDIO bus\n");
            return -1;
        }
    }

    /* Verify PHY ID */
    int id1 = mdio_read(base, phy_addr, MII_PHYSID1);
    int id2 = mdio_read(base, phy_addr, MII_PHYSID2);
    if (id1 < 0 || id2 < 0)
        return -1;
    uint32 phy_id = ((uint32)id1 << 16) | (uint32)id2;
    printf("yt8531: PHY ID = 0x%x at addr %d\n", phy_id, phy_addr);

    /* Soft reset the PHY */
    if (mdio_write(base, phy_addr, MII_BMCR, BMCR_RESET) < 0)
        return -1;

    /* Wait for reset to complete (BMCR_RESET self-clears) */
    for (int i = 0; i < 50; i++) {
        delay_ms(10);
        int bmcr = mdio_read(base, phy_addr, MII_BMCR);
        if (bmcr >= 0 && !(bmcr & BMCR_RESET))
            goto reset_done;
    }
    printf("yt8531: PHY reset timeout\n");
    return -1;

reset_done:
    /* YT8531 LED fixup (from Linux vendor driver) */
    if ((phy_id & 0xFFFFFFF0) == (YT8531_PHY_ID & 0xFFFFFFF0)) {
        printf("yt8531: applying LED fixup\n");
        yt8531_ext_write(base, phy_addr, YT8531_LED0_CFG, YT8531_LED0_VAL);
        yt8531_ext_write(base, phy_addr, YT8531_LED1_CFG, YT8531_LED1_VAL);
        yt8531_ext_write(base, phy_addr, YT8531_LED2_CFG, YT8531_LED2_VAL);
    }

    /* Advertise all capabilities: 10/100/1000 full/half duplex */
    uint16 adv = ADVERTISE_CSMA | ADVERTISE_10HALF | ADVERTISE_10FULL |
                 ADVERTISE_100HALF | ADVERTISE_100FULL |
                 ADVERTISE_PAUSE_CAP;
    if (mdio_write(base, phy_addr, MII_ADVERTISE, adv) < 0)
        return -1;

    /* Advertise 1000BASE-T full duplex */
    uint16 ctrl1000 = ADVERTISE_1000FULL;
    if (mdio_write(base, phy_addr, MII_CTRL1000, ctrl1000) < 0)
        return -1;

    /* Enable and restart auto-negotiation */
    uint16 bmcr = BMCR_ANENABLE | BMCR_ANRESTART;
    if (mdio_write(base, phy_addr, MII_BMCR, bmcr) < 0)
        return -1;

    printf("yt8531: auto-negotiation started\n");
    return phy_addr;
}

/* ======================================================================
 * Link state polling
 * ====================================================================== */

int yt8531_poll_link(volatile uint32 *base, int phy_addr, struct phy_state *state)
{
    state->link_up = 0;
    state->speed = 0;
    state->full_duplex = 0;

    int bmsr = mdio_read(base, phy_addr, MII_BMSR);
    if (bmsr < 0)
        return -1;

    /* Read BMSR twice — link status is latching-low */
    bmsr = mdio_read(base, phy_addr, MII_BMSR);
    if (bmsr < 0)
        return -1;

    if (!(bmsr & BMSR_LSTATUS))
        return 0; /* No link */

    state->link_up = 1;

    /* Check 1000BASE-T first */
    int stat1000 = mdio_read(base, phy_addr, MII_STAT1000);
    if (stat1000 >= 0 && (stat1000 & LPA_1000FULL)) {
        state->speed = PHY_SPEED_1000;
        state->full_duplex = 1;
        return 0;
    }
    if (stat1000 >= 0 && (stat1000 & LPA_1000HALF)) {
        state->speed = PHY_SPEED_1000;
        state->full_duplex = 0;
        return 0;
    }

    /* Check 100/10 via LPA */
    int lpa = mdio_read(base, phy_addr, MII_LPA);
    if (lpa < 0)
        return -1;

    if (lpa & LPA_100FULL) {
        state->speed = PHY_SPEED_100;
        state->full_duplex = 1;
    } else if (lpa & LPA_100HALF) {
        state->speed = PHY_SPEED_100;
        state->full_duplex = 0;
    } else if (lpa & LPA_10FULL) {
        state->speed = PHY_SPEED_10;
        state->full_duplex = 1;
    } else if (lpa & LPA_10HALF) {
        state->speed = PHY_SPEED_10;
        state->full_duplex = 0;
    } else {
        /* Fallback: assume 100 full */
        state->speed = PHY_SPEED_100;
        state->full_duplex = 1;
    }

    return 0;
}

int yt8531_wait_autoneg(volatile uint32 *base, int phy_addr,
                        struct phy_state *state, int timeout_ms)
{
    int elapsed = 0;
    int interval = 100; /* poll every 100ms */

    while (elapsed < timeout_ms) {
        int bmsr = mdio_read(base, phy_addr, MII_BMSR);
        if (bmsr >= 0 && (bmsr & BMSR_ANEGCOMPLETE)) {
            printf("yt8531: auto-negotiation complete (%dms)\n", elapsed);
            return yt8531_poll_link(base, phy_addr, state);
        }
        delay_ms(interval);
        elapsed += interval;
    }

    /* Timeout — check link anyway */
    printf("yt8531: auto-negotiation timeout after %dms, checking link...\n",
           timeout_ms);
    yt8531_poll_link(base, phy_addr, state);
    if (state->link_up) {
        printf("yt8531: link up despite AN timeout: %dMbps %s\n",
               state->speed, state->full_duplex ? "full" : "half");
        return 0;
    }
    return -1;
}
