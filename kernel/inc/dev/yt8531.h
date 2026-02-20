/**
 * @file yt8531.h
 * @brief Motorcomm YT8531 Gigabit Ethernet PHY driver for xv6
 *
 * Provides MDIO read/write over the X1-EMAC MDIO registers,
 * YT8531 PHY initialisation (LED fixup, soft-reset),
 * and auto-negotiation for link speed/duplex.
 */
#ifndef __KERNEL_DEV_YT8531_H
#define __KERNEL_DEV_YT8531_H

#include "types.h"

/* ======================================================================
 * Standard MII (IEEE 802.3) Register Definitions
 * ====================================================================== */

#define MII_BMCR                0x00    /* Basic Mode Control */
#define MII_BMSR                0x01    /* Basic Mode Status */
#define MII_PHYSID1             0x02    /* PHY Identifier 1 */
#define MII_PHYSID2             0x03    /* PHY Identifier 2 */
#define MII_ADVERTISE           0x04    /* Auto-Neg Advertisement */
#define MII_LPA                 0x05    /* Link Partner Ability */
#define MII_EXPANSION           0x06    /* Auto-Neg Expansion */
#define MII_CTRL1000            0x09    /* 1000BASE-T Control */
#define MII_STAT1000            0x0A    /* 1000BASE-T Status */

/* BMCR bits */
#define BMCR_RESET              (1 << 15)
#define BMCR_LOOPBACK           (1 << 14)
#define BMCR_SPEED100           (1 << 13)
#define BMCR_ANENABLE           (1 << 12)
#define BMCR_PDOWN              (1 << 11)
#define BMCR_ISOLATE            (1 << 10)
#define BMCR_ANRESTART          (1 << 9)
#define BMCR_FULLDPLX           (1 << 8)
#define BMCR_SPEED1000          (1 << 6)

/* BMSR bits */
#define BMSR_LSTATUS            (1 << 2)   /* Link status */
#define BMSR_ANEGCOMPLETE       (1 << 5)   /* Auto-neg complete */
#define BMSR_100FULL            (1 << 14)
#define BMSR_100HALF            (1 << 13)
#define BMSR_10FULL             (1 << 12)
#define BMSR_10HALF             (1 << 11)

/* ADVERTISE bits (register 4) */
#define ADVERTISE_10HALF        (1 << 5)
#define ADVERTISE_10FULL        (1 << 6)
#define ADVERTISE_100HALF       (1 << 7)
#define ADVERTISE_100FULL       (1 << 8)
#define ADVERTISE_PAUSE_CAP     (1 << 10)
#define ADVERTISE_PAUSE_ASYM    (1 << 11)
#define ADVERTISE_CSMA          0x01       /* 802.3 CSMA selector */

/* LPA bits (register 5) */
#define LPA_10HALF              (1 << 5)
#define LPA_10FULL              (1 << 6)
#define LPA_100HALF             (1 << 7)
#define LPA_100FULL             (1 << 8)

/* CTRL1000 bits (register 9) */
#define ADVERTISE_1000FULL      (1 << 9)
#define ADVERTISE_1000HALF      (1 << 8)

/* STAT1000 bits (register 10) */
#define LPA_1000FULL            (1 << 11)
#define LPA_1000HALF            (1 << 10)

/* ======================================================================
 * YT8531-Specific Extended Register Access
 * ====================================================================== */

#define YT8531_EXT_REG_ADDR     0x1E    /* Extended register address port */
#define YT8531_EXT_REG_DATA     0x1F    /* Extended register data port */

/* YT8531 LED configuration registers (extended) */
#define YT8531_LED0_CFG         0xA00D
#define YT8531_LED1_CFG         0xA00E
#define YT8531_LED2_CFG         0xA00F

/* Default LED values from Linux driver */
#define YT8531_LED0_VAL         0x2600
#define YT8531_LED1_VAL         0x2070
#define YT8531_LED2_VAL         0x000A

/* PHY speed constants */
#define PHY_SPEED_10            10
#define PHY_SPEED_100           100
#define PHY_SPEED_1000          1000

/* Link state */
struct phy_state {
    int     link_up;
    int     speed;      /* 10, 100, 1000 */
    int     full_duplex;
};

/* ======================================================================
 * MDIO access functions (implemented in x1_emac.c, using EMAC MDIO regs)
 * ====================================================================== */

/**
 * mdio_read - Read a PHY register via EMAC MDIO
 * @base:     EMAC base address (volatile uint32 *)
 * @phy_addr: PHY address on the MDIO bus (0-31)
 * @reg:      Register number (0-31)
 * Returns:   Register value (16-bit) or -1 on timeout
 */
int mdio_read(volatile uint32 *base, int phy_addr, int reg);

/**
 * mdio_write - Write a PHY register via EMAC MDIO
 * @base:     EMAC base address (volatile uint32 *)
 * @phy_addr: PHY address on the MDIO bus (0-31)
 * @reg:      Register number (0-31)
 * @val:      Value to write (16-bit)
 * Returns:   0 on success, -1 on timeout
 */
int mdio_write(volatile uint32 *base, int phy_addr, int reg, uint16 val);

/**
 * yt8531_init - Initialise the YT8531 PHY
 * @base:     EMAC base address
 * @phy_addr: PHY address on MDIO bus
 *
 * Performs soft reset, verifies PHY ID, configures LEDs,
 * advertises all speeds, and restarts auto-negotiation.
 * Returns 0 on success, -1 on failure.
 */
int yt8531_init(volatile uint32 *base, int phy_addr);

/**
 * yt8531_poll_link - Read current link state from PHY
 * @base:     EMAC base address
 * @phy_addr: PHY address on MDIO bus
 * @state:    Output: filled with current link/speed/duplex
 * Returns 0 on success.
 */
int yt8531_poll_link(volatile uint32 *base, int phy_addr, struct phy_state *state);

/**
 * yt8531_wait_autoneg - Block until auto-negotiation completes or times out
 * @base:     EMAC base address
 * @phy_addr: PHY address on MDIO bus
 * @state:    Output: filled with final link state
 * @timeout_ms: Max time to wait in milliseconds
 * Returns 0 on link up, -1 on timeout
 */
int yt8531_wait_autoneg(volatile uint32 *base, int phy_addr,
                        struct phy_state *state, int timeout_ms);

#endif /* __KERNEL_DEV_YT8531_H */
