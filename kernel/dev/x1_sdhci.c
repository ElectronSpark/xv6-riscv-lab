/**
 * @file x1_sdhci.c
 * @brief SpacemiT X1 SDHCI SD/eMMC controller driver for xv6
 *
 * Supports up to 3 SDHCI instances on the Orange Pi RV2 (SpacemiT X1 SoC):
 *   SDH0 — SD card slot (4-bit bus, card-detect via GPIO)
 *   SDH1 — SDIO (WiFi, skipped for now)
 *   SDH2 — eMMC (8-bit bus, HS400 capable)
 *
 * This driver supports SDMA (Simple DMA) for data transfers when the
 * controller advertises the SDMA capability.  Falls back to PIO
 * (programmed I/O) otherwise.
 *
 * Key data flow:
 *   bio (file system) → blkdev → submit_bio → sdhci_rw_blocks → SDMA/PIO
 */

#include "compiler.h"
#include "types.h"
#include "string.h"
#include "param.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "lock/mutex.h"
#include "defs.h"
#include "printf.h"
#include "dev/net.h"
#include "dev/blkdev.h"
#include "dev/bio.h"
#include "dev/x1_sdhci.h"
#include "dev/fdt.h"
#include "trap.h"
#include "timer/timer.h"
#include "errno.h"
#include <mm/page.h>
#include <vfs/stat.h>
#include "cache.h"
#include "proc/sched.h"
#include "proc/thread.h"

/* ======================================================================
 * Per-instance driver state
 * ====================================================================== */

struct sdhci_softc {
    volatile uint8 *regs;           /* SDHCI MMIO base */

    /* APMU clock/reset register (per-instance) */
    volatile uint32 *apmu_reg;
    /* APMU shared AXI register (always at SDH0 offset 0x54) */
    volatile uint32 *apmu_axi_reg;
    /* APBC AIB clock register (required by SDH0) */
    volatile uint32 *aib_reg;

    /* Card state */
    uint32 rca;                     /* Relative Card Address */
    int card_type;                  /* 0=SD, 1=SDHC/SDXC, 2=eMMC */
    int bus_width;                  /* 1, 4, or 8 */
    uint64 capacity_blocks;         /* total 512-byte sectors */

    /* Lock (mutex — DMA/PIO path needs to sleep/yield) */
    mutex_t lock;

    /* DMA */
    int use_dma;                    /* 1 if SDMA is available */

    /* Netdev-style IRQ */
    int irq;
    int index;                      /* instance index (0, 1, 2) */

    /* Block device */
    blkdev_t bdev;
    int initialized;                /* 1 if card enumerated OK */
};

#define MAX_SDH_INSTANCES 3
static struct sdhci_softc sdhci_sc[MAX_SDH_INSTANCES];
static int sdhci_count = 0;

/* Card type constants */
#define CARD_TYPE_SD    0
#define CARD_TYPE_SDHC  1
#define CARD_TYPE_EMMC  2

/* ======================================================================
 * Low-level MMIO helpers
 * ====================================================================== */

static inline uint8 sdhci_readb(struct sdhci_softc *sc, uint32 off)
{
    return *(volatile uint8 *)(sc->regs + off);
}

static inline uint16 sdhci_readw(struct sdhci_softc *sc, uint32 off)
{
    return *(volatile uint16 *)(sc->regs + off);
}

static inline uint32 sdhci_readl(struct sdhci_softc *sc, uint32 off)
{
    return *(volatile uint32 *)(sc->regs + off);
}

static inline void sdhci_writeb(struct sdhci_softc *sc, uint32 off, uint8 val)
{
    *(volatile uint8 *)(sc->regs + off) = val;
}

static inline void sdhci_writew(struct sdhci_softc *sc, uint32 off, uint16 val)
{
    *(volatile uint16 *)(sc->regs + off) = val;
}

static inline void sdhci_writel(struct sdhci_softc *sc, uint32 off, uint32 val)
{
    *(volatile uint32 *)(sc->regs + off) = val;
}

/* ======================================================================
 * APMU Clock / Reset Configuration
 * ====================================================================== */

/**
 * Wait for Frequency Change (FC) bit to self-clear.
 * Returns 0 on success, -1 on timeout.
 */
static int sdhci_apmu_wait_fc(volatile uint32 *reg, int timeout_us)
{
    uint64 deadline = r_time() +
                      (uint64)TIMEBASE_FREQUENCY * timeout_us / 1000000;
    while (r_time() < deadline) {
        if (!(*reg & SDH_APMU_CLK_FC))
            return 0;
        scheduler_yield();
    }
    return -1;
}

/**
 * Enable the AIB clock via APBC (required by SDH0).
 * APBC base and AIB offset are obtained from the clock-controller's
 * device tree reg property.  Bits [1:0] = 0x3 enable the clock.
 */
static void sdhci_aib_enable(struct sdhci_softc *sc)
{
    if (!sc->aib_reg)
        return;
    uint32 val = *sc->aib_reg;
    val |= APBC_AIB_CLK_EN;
    *sc->aib_reg = val;
    __sync_synchronize();
}

/**
 * Enable clocks and deassert reset for an SDHCI instance.
 * Must be called before any SDHCI register access.
 *
 * IMPORTANT: We preserve the MUX/DIV clock source configuration that
 * U-Boot/firmware already set up (since it booted from SD/eMMC, the
 * clocks must already be configured).  We only ensure the gate and
 * reset bits are asserted.
 *
 * Sequence:
 *  1. Enable AIB clock (APBC, bus fabric)
 *  2. Enable shared AXI bus clock gate (BIT(3) in SDH0 register)
 *  3. Enable per-instance IO clock gate (BIT(4))
 *  4. Deassert shared AXI bus reset (BIT(0) in SDH0 register)
 *  5. Deassert per-instance function reset (BIT(1))
 *
 * If no MUX/DIV is already configured (register reads 0), set a
 * reasonable default and trigger FC.
 */
static void sdhci_apmu_enable(struct sdhci_softc *sc)
{
    uint32 val;

    if (!sc->apmu_reg) {
        printf("x1_sdhci%d: no APMU register, skipping clock enable\n",
               sc->index);
        return;
    }

    /* Step 1: Enable AIB clock (needed for SDH bus fabric) */
    sdhci_aib_enable(sc);

    /* Read current APMU register — U-Boot should have configured MUX/DIV */
    val = *sc->apmu_reg;
    printf("x1_sdhci%d: APMU initial value = 0x%x\n", sc->index, val);

    /* Configure clock source if not already set up by firmware.
     * MUX parents: 0=PLL1_D6(409M), 1=PLL1_D4(614M), 2=PLL2_D8(375M),
     *   3=PLL1_D3(819M), 4=PLL1_D11(223M), 5=PLL1_D13(189M), 6=PLL1_D23(107M)
     * Only reconfigure if IO clock is off AND MUX/DIV are at reset defaults. */
    if (!(val & SDH_APMU_IO_CLK_EN) ||
        ((val & SDH_APMU_MUX_MASK) == 0 && (val & SDH_APMU_DIV_MASK) == 0)) {
        printf("x1_sdhci%d: configuring clock (MUX=0 PLL1_D6, DIV=1)\n",
               sc->index);
        val &= ~(SDH_APMU_MUX_MASK | SDH_APMU_DIV_MASK);
        val |= (SDH_MUX_PLL1_D6_409M << SDH_APMU_MUX_SHIFT);
        val |= (1 << SDH_APMU_DIV_SHIFT);
        *sc->apmu_reg = val;
        __sync_synchronize();

        /* Trigger frequency change and wait for self-clear */
        val |= SDH_APMU_CLK_FC;
        *sc->apmu_reg = val;
        __sync_synchronize();
        if (sdhci_apmu_wait_fc(sc->apmu_reg, 20000) < 0) {
            printf("x1_sdhci%d: APMU FC timeout\n", sc->index);
        }
        val = *sc->apmu_reg;
    }

    /* Step 2: Enable shared AXI bus clock (via SDH0 register) */
    if (sc->apmu_axi_reg) {
        uint32 axi = *sc->apmu_axi_reg;
        if (!(axi & SDH_APMU_AXI_CLK_EN)) {
            axi |= SDH_APMU_AXI_CLK_EN;
            *sc->apmu_axi_reg = axi;
            __sync_synchronize();
        }
    }

    /* Step 3: Enable per-instance AXI + IO clock gates */
    val = *sc->apmu_reg;
    if (!(val & SDH_APMU_AXI_CLK_EN)) {
        val |= SDH_APMU_AXI_CLK_EN;
    }
    if (!(val & SDH_APMU_IO_CLK_EN)) {
        val |= SDH_APMU_IO_CLK_EN;
    }
    *sc->apmu_reg = val;
    __sync_synchronize();

    /* Step 4: Deassert shared AXI bus reset (via SDH0 register) */
    if (sc->apmu_axi_reg) {
        uint32 axi = *sc->apmu_axi_reg;
        if (!(axi & SDH_APMU_AXI_RST)) {
            axi |= SDH_APMU_AXI_RST;
            *sc->apmu_axi_reg = axi;
            __sync_synchronize();
        }
    }

    /* Step 5: Deassert per-instance AXI + function resets */
    val = *sc->apmu_reg;
    if (!(val & SDH_APMU_AXI_RST)) {
        val |= SDH_APMU_AXI_RST;
    }
    if (!(val & SDH_APMU_RST)) {
        val |= SDH_APMU_RST;
    }
    *sc->apmu_reg = val;
    __sync_synchronize();

    sleep_ms(1);  /* let clocks and reset stabilize */

    printf("x1_sdhci%d: APMU enabled (reg=0x%x, axi=0x%x)\n",
           sc->index, *sc->apmu_reg,
           sc->apmu_axi_reg ? *sc->apmu_axi_reg : 0);
}

/* ======================================================================
 * SDHCI Controller Reset & Init
 * ====================================================================== */

/**
 * Wait for a register bit to be set or cleared.
 * Returns 0 on success, -1 on timeout.
 */
static int sdhci_wait_bit(struct sdhci_softc *sc, uint32 reg, uint32 mask,
                          int set, int timeout_ms)
{
    uint64 deadline = r_time() + (uint64)TIMEBASE_FREQUENCY * timeout_ms / 1000;
    while (r_time() < deadline) {
        uint32 val = sdhci_readl(sc, reg);
        if (set) {
            if (val & mask) return 0;
        } else {
            if (!(val & mask)) return 0;
        }
        scheduler_yield();
    }
    return -1;
}

/**
 * Software reset. mask is one of SDHCI_RESET_ALL/CMD/DATA.
 *
 * NOTE: SDHCI_SOFTWARE_RESET is an 8-bit register at offset 0x2F
 * (not 4-byte aligned), so we must use readb — not readl via
 * sdhci_wait_bit — to avoid a load access fault on RISC-V MMIO.
 */
static int sdhci_reset(struct sdhci_softc *sc, uint8 mask)
{
    uint64 deadline;

    sdhci_writeb(sc, SDHCI_SOFTWARE_RESET, mask);

    deadline = r_time() + (uint64)TIMEBASE_FREQUENCY * 100 / 1000;
    while (r_time() < deadline) {
        if (!(sdhci_readb(sc, SDHCI_SOFTWARE_RESET) & mask))
            return 0;
        scheduler_yield();
    }
    printf("x1_sdhci%d: reset timeout (mask=0x%x)\n", sc->index, mask);
    return -1;
}

/**
 * Set SD clock to a target frequency.
 * Uses the 10-bit divided clock mode from SDHCI spec v3.0.
 */
static void sdhci_set_clock(struct sdhci_softc *sc, uint32 target_hz)
{
    uint32 caps, base_clk_mhz, div;
    uint16 clk;

    /* Stop clock */
    sdhci_writew(sc, SDHCI_CLOCK_CONTROL, 0);

    if (target_hz == 0)
        return;

    /* Read base clock from capabilities */
    caps = sdhci_readl(sc, SDHCI_CAPABILITIES);
    base_clk_mhz = (caps >> SDHCI_CAP_BASE_CLK_SHIFT) & SDHCI_CAP_BASE_CLK_MASK;
    if (base_clk_mhz == 0)
        base_clk_mhz = 200; /* fallback, typical for X1 */

    uint32 base_clk_hz = base_clk_mhz * 1000000;

    /* Calculate divisor: find smallest divisor that gives <= target_hz */
    if (base_clk_hz <= target_hz) {
        div = 0; /* no division needed */
    } else {
        for (div = 1; div < 2046; div++) {
            if ((base_clk_hz / (2 * div)) <= target_hz)
                break;
        }
    }

    /* Set divisor and enable internal clock */
    clk = ((div & SDHCI_DIV_MASK) << SDHCI_DIVIDER_SHIFT) |
          (((div >> 8) & 0x3) << SDHCI_DIVIDER_HI_SHIFT) |
          SDHCI_CLOCK_INT_EN;
    sdhci_writew(sc, SDHCI_CLOCK_CONTROL, clk);

    /* Wait for internal clock stable */
    if (sdhci_wait_bit(sc, SDHCI_CLOCK_CONTROL, SDHCI_CLOCK_INT_STABLE,
                       1, SDHCI_CLK_TIMEOUT_MS) < 0) {
        printf("x1_sdhci%d: internal clock not stable\n", sc->index);
        return;
    }

    /* Enable clock to card */
    clk |= SDHCI_CLOCK_CARD_EN;
    sdhci_writew(sc, SDHCI_CLOCK_CONTROL, clk);

    scheduler_yield(); /* brief settling time after clock enable */
}

/**
 * Set bus power to 3.3V.
 */
static void sdhci_set_power(struct sdhci_softc *sc)
{
    sdhci_writeb(sc, SDHCI_POWER_CONTROL, SDHCI_POWER_330 | SDHCI_POWER_ON);
    sleep_ms(10);
}

/**
 * Apply vendor-specific (KY X1) quirks after reset.
 *
 * All instances start in internal-clock (bypass PHY) mode for the
 * 400 kHz identification clock.  The PHY PLL cannot lock at such a
 * low frequency, so we must NOT enable PHY_FUNC_EN during init.
 *
 * For eMMC we additionally set MMC_CARD_MODE.
 */
static void sdhci_x1_post_reset(struct sdhci_softc *sc)
{
    uint32 reg;

    /* Use internal clock (bypass PHY) for all instances at init.
     * PHY mode should only be enabled later for HS200/HS400. */
    reg = sdhci_readl(sc, SDHC_TX_CFG_REG);
    reg |= TX_INT_CLK_SEL;
    sdhci_writel(sc, SDHC_TX_CFG_REG, reg);

    if (sc->index >= 2) {
        /* eMMC: set MMC card mode */
        reg = sdhci_readl(sc, SDHC_MMC_CTRL_REG);
        reg |= MMC_CARD_MODE;
        sdhci_writel(sc, SDHC_MMC_CTRL_REG, reg);
    }
}

/**
 * Full controller initialization: reset, power, clock, vendor quirks.
 */
static int sdhci_hw_init(struct sdhci_softc *sc)
{
    /* Full reset */
    if (sdhci_reset(sc, SDHCI_RESET_ALL) < 0)
        return -1;

    /* Apply X1 vendor quirks */
    sdhci_x1_post_reset(sc);

    /* Set power to 3.3V */
    sdhci_set_power(sc);

    /* Set initial clock to 400 KHz (identification mode) */
    sdhci_set_clock(sc, 400000);

    /* Set timeout */
    sdhci_writeb(sc, SDHCI_TIMEOUT_CONTROL, SDHCI_TIMEOUT_DEFAULT);

    /* Enable all normal + error interrupt status (for polling) */
    sdhci_writew(sc, SDHCI_INT_ENABLE, SDHCI_INT_ALL_MASK);
    sdhci_writew(sc, SDHCI_ERR_INT_ENABLE, SDHCI_ERR_ALL);

    /* Disable interrupt signals (we use polling, not IRQ) */
    sdhci_writew(sc, SDHCI_SIGNAL_ENABLE, 0);
    sdhci_writew(sc, SDHCI_ERR_SIGNAL_ENABLE, 0);

    /* Check SDMA capability and configure host control */
    {
        uint32 caps = sdhci_readl(sc, SDHCI_CAPABILITIES);
        if (caps & SDHCI_CAP_SDMA) {
            sc->use_dma = 1;
            /* Select SDMA mode in Host Control register (bits [4:3] = 00) */
            uint8 ctrl = sdhci_readb(sc, SDHCI_HOST_CONTROL);
            ctrl &= ~SDHCI_CTRL_DMA_MASK;
            ctrl |= SDHCI_CTRL_SDMA;
            sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl);
            printf("x1_sdhci%d: SDMA enabled\n", sc->index);
        } else {
            sc->use_dma = 0;
            printf("x1_sdhci%d: no SDMA capability, using PIO\n", sc->index);
        }
    }

    return 0;
}

/* ======================================================================
 * Command Engine
 * ====================================================================== */

/**
 * Send a command and wait for completion (polled), with explicit timeout.
 *
 * @sc:         softc
 * @cmd_idx:    SD/MMC command index (0-63)
 * @arg:        32-bit command argument
 * @flags:      SDHCI_CMD_* flags (response type, CRC, index check, data)
 * @resp:       output buffer for response (up to 4 words), NULL if not needed
 * @timeout_ms: per-wait timeout in milliseconds
 *
 * Returns 0 on success, negative errno on error.
 */
static int sdhci_send_cmd_to(struct sdhci_softc *sc, uint32 cmd_idx, uint32 arg,
                             uint16 flags, uint32 *resp, int timeout_ms)
{
    uint32 mask = SDHCI_CMD_INHIBIT;
    uint16 cmd_reg;

    if (flags & SDHCI_CMD_DATA)
        mask |= SDHCI_DATA_INHIBIT;

    /* Wait for command line to be free */
    if (sdhci_wait_bit(sc, SDHCI_PRESENT_STATE, mask, 0, timeout_ms) < 0) {
        printf("x1_sdhci%d: CMD%d inhibit timeout\n", sc->index, cmd_idx);
        return -ETIMEDOUT;
    }

    /* Clear pending interrupts */
    sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_ALL_MASK);
    sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);

    /* Write argument */
    sdhci_writel(sc, SDHCI_ARGUMENT, arg);

    /* Write command register (triggers command issue) */
    cmd_reg = SDHCI_MAKE_CMD(cmd_idx, flags);
    sdhci_writew(sc, SDHCI_COMMAND, cmd_reg);

    /* Wait for command complete */
    if (sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_RESPONSE, 1,
                       timeout_ms) < 0) {
        uint16 err = sdhci_readw(sc, SDHCI_ERR_INT_STATUS);
        if (cmd_idx != MMC_GO_IDLE_STATE && cmd_idx != SD_SEND_IF_COND &&
            cmd_idx != MMC_SEND_OP_COND) {
            printf("x1_sdhci%d: CMD%d timeout (err=0x%x)\n",
                   sc->index, cmd_idx, err);
        }
        sdhci_reset(sc, SDHCI_RESET_CMD);
        return -ETIMEDOUT;
    }

    /* Check for errors */
    uint16 int_status = sdhci_readw(sc, SDHCI_INT_STATUS);
    if (int_status & SDHCI_INT_ERROR) {
        uint16 err = sdhci_readw(sc, SDHCI_ERR_INT_STATUS);
        printf("x1_sdhci%d: CMD%d error (int=0x%x, err=0x%x)\n",
               sc->index, cmd_idx, int_status, err);
        sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);
        sdhci_reset(sc, SDHCI_RESET_CMD);
        return -EIO;
    }

    /* Clear command complete */
    sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_RESPONSE);

    /* Read response */
    if (resp) {
        if ((flags & 0x03) == SDHCI_CMD_RESP_136) {
            /* R2: 128-bit response across four registers */
            resp[0] = sdhci_readl(sc, SDHCI_RESPONSE + 0);
            resp[1] = sdhci_readl(sc, SDHCI_RESPONSE + 4);
            resp[2] = sdhci_readl(sc, SDHCI_RESPONSE + 8);
            resp[3] = sdhci_readl(sc, SDHCI_RESPONSE + 12);
        } else {
            resp[0] = sdhci_readl(sc, SDHCI_RESPONSE);
        }
    }

    return 0;
}

/**
 * Send a command with the default timeout (SDHCI_TIMEOUT_MS).
 */
static int sdhci_send_cmd(struct sdhci_softc *sc, uint32 cmd_idx, uint32 arg,
                          uint16 flags, uint32 *resp)
{
    return sdhci_send_cmd_to(sc, cmd_idx, arg, flags, resp, SDHCI_TIMEOUT_MS);
}

/**
 * Send an application-specific command (ACMD).
 * Sends CMD55 first, then the actual command.
 */

static int sdhci_send_acmd(struct sdhci_softc *sc, uint32 cmd_idx, uint32 arg,
                           uint16 flags, uint32 *resp)
{
    int ret;

    /* Send CMD55 (APP_CMD) */
    ret = sdhci_send_cmd(sc, MMC_APP_CMD, (uint32)sc->rca << 16,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         NULL);
    if (ret < 0)
        return ret;

    /* Send the actual ACMD */
    return sdhci_send_cmd(sc, cmd_idx, arg, flags, resp);
}

/* ======================================================================
 * SDMA Data Transfer
 * ====================================================================== */

/* SDMA boundary size: encoding 7 in SDHCI_MAKE_BLKSZ = 512 KB */
#define SDHCI_SDMA_BOUNDARY     (512 * 1024)

/**
 * Wait for an SDMA transfer to complete, handling boundary interrupts.
 *
 * SDMA transfers in fixed-size chunks (512 KB boundary).  When the
 * controller reaches a boundary, it pauses and sets SDHCI_INT_DMA_END.
 * We must re-program SDHCI_DMA_ADDRESS with the next boundary-aligned
 * address to resume, until SDHCI_INT_DATA_END signals completion.
 *
 * @sc:        softc
 * @dma_addr:  starting physical address of the DMA buffer
 * @total:     total bytes being transferred
 * @is_write:  1 for write, 0 for read
 *
 * Returns 0 on success, negative errno on error.
 */
static int sdhci_sdma_wait(struct sdhci_softc *sc, uint64 dma_addr,
                           uint32 total, int is_write)
{
    uint64 deadline = r_time() +
                      (uint64)TIMEBASE_FREQUENCY * SDHCI_TIMEOUT_MS / 1000;

    while (r_time() < deadline) {
        uint16 int_st = sdhci_readw(sc, SDHCI_INT_STATUS);

        /* Check for errors */
        if (int_st & SDHCI_INT_ERROR) {
            uint16 err = sdhci_readw(sc, SDHCI_ERR_INT_STATUS);
            printf("x1_sdhci%d: DMA %s error (int=0x%x, err=0x%x)\n",
                   sc->index, is_write ? "write" : "read", int_st, err);
            sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);
            sdhci_reset(sc, SDHCI_RESET_DATA);
            return -EIO;
        }

        /* Transfer complete — all data moved */
        if (int_st & SDHCI_INT_DATA_END) {
            sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_END);
            return 0;
        }

        /* SDMA boundary reached — reprogram address and continue */
        if (int_st & SDHCI_INT_DMA_END) {
            sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DMA_END);
            /* Advance to next boundary */
            dma_addr &= ~((uint64)SDHCI_SDMA_BOUNDARY - 1);
            dma_addr += SDHCI_SDMA_BOUNDARY;
            sdhci_writel(sc, SDHCI_DMA_ADDRESS, (uint32)dma_addr);
        }

        scheduler_yield();
    }

    printf("x1_sdhci%d: DMA %s timeout\n", sc->index,
           is_write ? "write" : "read");
    sdhci_reset(sc, SDHCI_RESET_DATA);
    return -ETIMEDOUT;
}

/* ======================================================================
 * PIO Data Transfer
 * ====================================================================== */

/**
 * Read a single 512-byte block from the card using PIO.
 */
static int sdhci_pio_read_block(struct sdhci_softc *sc, void *buf)
{
    uint32 *p = (uint32 *)buf;

    /* Wait for data available */
    if (sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL, 1,
                       SDHCI_TIMEOUT_MS) < 0) {
        printf("x1_sdhci%d: PIO read data timeout\n", sc->index);
        return -ETIMEDOUT;
    }

    /* Clear the data available interrupt */
    sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);

    /* Read 512 / 4 = 128 words from data buffer port */
    for (int i = 0; i < SDHCI_BLOCK_SIZE_VAL / 4; i++) {
        p[i] = sdhci_readl(sc, SDHCI_BUFFER);
    }

    return 0;
}

/**
 * Write a single 512-byte block to the card using PIO.
 */
static int sdhci_pio_write_block(struct sdhci_softc *sc, const void *buf)
{
    const uint32 *p = (const uint32 *)buf;

    /* Wait for space available */
    if (sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_SPACE_AVAIL, 1,
                       SDHCI_TIMEOUT_MS) < 0) {
        printf("x1_sdhci%d: PIO write space timeout\n", sc->index);
        return -ETIMEDOUT;
    }

    /* Clear the space available interrupt */
    sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_SPACE_AVAIL);

    /* Write 128 words to data buffer port */
    for (int i = 0; i < SDHCI_BLOCK_SIZE_VAL / 4; i++) {
        sdhci_writel(sc, SDHCI_BUFFER, p[i]);
    }

    return 0;
}

/**
 * Wait for transfer complete after all blocks transferred.
 */
static int sdhci_wait_xfer_done(struct sdhci_softc *sc)
{
    if (sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_END, 1,
                       SDHCI_TIMEOUT_MS) < 0) {
        printf("x1_sdhci%d: transfer complete timeout\n", sc->index);
        return -ETIMEDOUT;
    }

    /* Check for errors */
    uint16 err = sdhci_readw(sc, SDHCI_ERR_INT_STATUS);
    if (err) {
        printf("x1_sdhci%d: transfer error 0x%x\n", sc->index, err);
        sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);
        sdhci_reset(sc, SDHCI_RESET_DATA);
        return -EIO;
    }

    /* Clear transfer complete */
    sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_END);
    return 0;
}

/* ======================================================================
 * Block Read/Write (single + multi-block)
 * ====================================================================== */

/**
 * Read @nblocks 512-byte blocks starting at @lba into @buf.
 * Uses SDMA if available, otherwise falls back to PIO.
 *
 * @buf must point to a physically-contiguous buffer (physical address
 *      usable as both VA and PA due to identity mapping).
 */
static int sdhci_read_blocks(struct sdhci_softc *sc, uint32 lba, uint32 nblocks,
                             void *buf)
{
    int ret;
    uint16 mode;
    uint32 cmd_idx;
    uint16 cmd_flags;
    uint32 card_addr;
    uint32 total = nblocks * SDHCI_BLOCK_SIZE_VAL;

    /* SDHC/SDXC/eMMC uses block addressing; SD uses byte addressing */
    card_addr = (sc->card_type == CARD_TYPE_SD) ? (lba * 512) : lba;

    /* Set block size and count */
    sdhci_writew(sc, SDHCI_BLOCK_SIZE,
                 SDHCI_MAKE_BLKSZ(7, SDHCI_BLOCK_SIZE_VAL));
    sdhci_writew(sc, SDHCI_BLOCK_COUNT, nblocks);

    /* Transfer mode: read direction, block count enable */
    mode = SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN;
    if (nblocks > 1)
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;

    if (sc->use_dma) {
        uint64 dma_addr = (uint64)buf;

        /* Invalidate cache before device writes to memory */
        dma_cache_inval(buf, total);

        /* Program SDMA system address */
        sdhci_writel(sc, SDHCI_DMA_ADDRESS, (uint32)dma_addr);
        mode |= SDHCI_TRNS_DMA;
    }

    sdhci_writew(sc, SDHCI_TRANSFER_MODE, mode);

    /* Command */
    cmd_idx = (nblocks == 1) ? MMC_READ_SINGLE_BLOCK
                             : MMC_READ_MULTIPLE_BLOCK;
    cmd_flags = SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX |
                SDHCI_CMD_DATA;

    ret = sdhci_send_cmd(sc, cmd_idx, card_addr, cmd_flags, NULL);
    if (ret < 0)
        return ret;

    if (sc->use_dma) {
        ret = sdhci_sdma_wait(sc, (uint64)buf, total, 0);
        if (ret < 0)
            return ret;
        /* Invalidate again to ensure CPU sees DMA-written data */
        dma_cache_inval(buf, total);
    } else {
        printf("sdhci_read_blocks: fallback to bio.\n");
        /* Read blocks via PIO */
        for (uint32 i = 0; i < nblocks; i++) {
            ret = sdhci_pio_read_block(sc,
                      (uint8 *)buf + i * SDHCI_BLOCK_SIZE_VAL);
            if (ret < 0)
                return ret;
        }
        ret = sdhci_wait_xfer_done(sc);
    }

    return ret;
}

/**
 * Write @nblocks 512-byte blocks starting at @lba from @buf.
 * Uses SDMA if available, otherwise falls back to PIO.
 */
static int sdhci_write_blocks(struct sdhci_softc *sc, uint32 lba,
                              uint32 nblocks, const void *buf)
{
    int ret;
    uint16 mode;
    uint32 cmd_idx;
    uint16 cmd_flags;
    uint32 card_addr;
    uint32 total = nblocks * SDHCI_BLOCK_SIZE_VAL;

    card_addr = (sc->card_type == CARD_TYPE_SD) ? (lba * 512) : lba;

    sdhci_writew(sc, SDHCI_BLOCK_SIZE,
                 SDHCI_MAKE_BLKSZ(7, SDHCI_BLOCK_SIZE_VAL));
    sdhci_writew(sc, SDHCI_BLOCK_COUNT, nblocks);

    /* Transfer mode: write direction, block count enable */
    mode = SDHCI_TRNS_BLK_CNT_EN; /* no SDHCI_TRNS_READ = write */
    if (nblocks > 1)
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;

    if (sc->use_dma) {
        uint64 dma_addr = (uint64)buf;

        /* Clean cache so device reads CPU's latest data */
        dma_cache_clean((void *)buf, total);

        /* Program SDMA system address */
        sdhci_writel(sc, SDHCI_DMA_ADDRESS, (uint32)dma_addr);
        mode |= SDHCI_TRNS_DMA;
    }

    sdhci_writew(sc, SDHCI_TRANSFER_MODE, mode);

    cmd_idx = (nblocks == 1) ? MMC_WRITE_BLOCK
                             : MMC_WRITE_MULTIPLE_BLOCK;
    cmd_flags = SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX |
                SDHCI_CMD_DATA;

    ret = sdhci_send_cmd(sc, cmd_idx, card_addr, cmd_flags, NULL);
    if (ret < 0)
        return ret;

    if (sc->use_dma) {
        ret = sdhci_sdma_wait(sc, (uint64)buf, total, 1);
    } else {
        printf("sdhci_write_blocks: fallback to bio.\n");
        /* Write blocks via PIO */
        for (uint32 i = 0; i < nblocks; i++) {
            ret = sdhci_pio_write_block(sc,
                      (const uint8 *)buf + i * SDHCI_BLOCK_SIZE_VAL);
            if (ret < 0)
                return ret;
        }
        ret = sdhci_wait_xfer_done(sc);
    }

    return ret;
}

/* ======================================================================
 * SD Card Enumeration
 * ====================================================================== */

/**
 * Check for card presence using the Present State register.
 */
static int sdhci_card_present(struct sdhci_softc *sc)
{
    uint32 state = sdhci_readl(sc, SDHCI_PRESENT_STATE);
    return !!(state & SDHCI_CARD_INSERTED);
}

/**
 * Enumerate an SD card: CMD0 → CMD8 → ACMD41 → CMD2 → CMD3 → CMD7 → CMD16
 * Returns 0 on success.
 */
static int sdhci_enumerate_sd(struct sdhci_softc *sc)
{
    uint32 resp[4];
    int ret;

    sc->rca = 0;
    sc->card_type = CARD_TYPE_SD;

    /* CMD0: Go idle */
    sdhci_send_cmd(sc, MMC_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE, NULL);
    sleep_ms(10);

    /* CMD8: Send IF Condition (voltage check for SD 2.0+) */
    ret = sdhci_send_cmd(sc, SD_SEND_IF_COND, 0x1AA,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         resp);
    if (ret < 0) {
        printf("x1_sdhci%d: CMD8 failed — legacy SD v1.x card?\n", sc->index);
        /* Might be SD v1.x — try ACMD41 anyway */
    } else if ((resp[0] & 0xFF) != 0xAA) {
        printf("x1_sdhci%d: CMD8 bad echo: 0x%x\n", sc->index, resp[0]);
    }

    /* ACMD41: SD_APP_OP_COND — negotiate operating conditions
     * We request 3.3V range and HCS (high capacity support). */
    int tries = 0;
    do {
        ret = sdhci_send_acmd(sc, SD_APP_OP_COND,
                              MMC_OCR_HCS | MMC_OCR_3V3,
                              SDHCI_CMD_RESP_48, /* R3: no CRC/index */
                              resp);
        if (ret < 0) {
            printf("x1_sdhci%d: ACMD41 failed\n", sc->index);
            return ret;
        }
        if (++tries > 100) {
            printf("x1_sdhci%d: ACMD41 timeout — card not ready\n", sc->index);
            return -ETIMEDOUT;
        }
        sleep_ms(10);
    } while (!(resp[0] & MMC_OCR_BUSY));

    /* Check if card is SDHC/SDXC */
    if (resp[0] & MMC_OCR_HCS)
        sc->card_type = CARD_TYPE_SDHC;

    printf("x1_sdhci%d: card type: %s\n", sc->index,
           sc->card_type == CARD_TYPE_SDHC ? "SDHC/SDXC" : "SD");

    /* CMD2: ALL_SEND_CID — get card identification */
    ret = sdhci_send_cmd(sc, MMC_ALL_SEND_CID, 0,
                         SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp);
    if (ret < 0) {
        printf("x1_sdhci%d: CMD2 failed\n", sc->index);
        return ret;
    }

    /* CMD3: SEND_RELATIVE_ADDR — get RCA */
    ret = sdhci_send_cmd(sc, MMC_SET_RELATIVE_ADDR, 0,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         resp);
    if (ret < 0) {
        printf("x1_sdhci%d: CMD3 failed\n", sc->index);
        return ret;
    }
    sc->rca = (resp[0] >> 16) & 0xFFFF;
    printf("x1_sdhci%d: RCA = 0x%x\n", sc->index, sc->rca);

    /* CMD9: SEND_CSD — get card-specific data (read capacity etc.) */
    ret = sdhci_send_cmd(sc, MMC_SEND_CSD, (uint32)sc->rca << 16,
                         SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp);
    if (ret < 0) {
        printf("x1_sdhci%d: CMD9 failed\n", sc->index);
        return ret;
    }

    /* Parse CSD to get capacity.
     *
     * SDHCI R2 response mapping: the controller strips CRC7+end bit,
     * so card CSD bit X maps to SDHCI response bit (X - 8).
     *   resp[0] (offset 0x10) = CSD bits [39:8]
     *   resp[1] (offset 0x14) = CSD bits [71:40]
     *   resp[2] (offset 0x18) = CSD bits [103:72]
     *   resp[3] (offset 0x1C) = CSD bits [127:104]
     */
    if (sc->card_type == CARD_TYPE_SDHC) {
        /* CSD v2 (SDHC/SDXC): C_SIZE is CSD bits [69:48] (22 bits)
         * → SDHCI bits [61:40] → resp[1] bits [29:8] */
        uint32 c_size = (resp[1] >> 8) & 0x3FFFFF;
        sc->capacity_blocks = ((uint64)c_size + 1) * 1024;
    } else {
        /* CSD v1: READ_BL_LEN = CSD[83:80] → resp[2] bits [11:8]
         *         C_SIZE      = CSD[73:62] → resp[2][1:0]:resp[1][31:22]
         *         C_SIZE_MULT = CSD[49:47] → resp[1] bits [9:7] */
        uint32 read_bl_len = (resp[2] >> 8) & 0xF;
        uint32 c_size = ((resp[2] & 0x3) << 10) | ((resp[1] >> 22) & 0x3FF);
        uint32 c_size_mult = (resp[1] >> 7) & 0x7;
        uint32 mult = 1 << (c_size_mult + 2);
        uint32 blocknr = (c_size + 1) * mult;
        uint32 block_len = 1 << read_bl_len;
        sc->capacity_blocks = (uint64)blocknr * block_len / 512;
    }
    printf("x1_sdhci%d: capacity = %ld sectors (%ld MB)\n", sc->index,
           sc->capacity_blocks, sc->capacity_blocks / 2048);

    /* CMD7: SELECT_CARD — move card to transfer state */
    ret = sdhci_send_cmd(sc, MMC_SELECT_CARD, (uint32)sc->rca << 16,
                         SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         resp);
    if (ret < 0) {
        printf("x1_sdhci%d: CMD7 failed\n", sc->index);
        return ret;
    }

    /* CMD16: SET_BLOCKLEN to 512 (for non-SDHC cards) */
    if (sc->card_type != CARD_TYPE_SDHC) {
        ret = sdhci_send_cmd(sc, MMC_SET_BLOCKLEN, SDHCI_BLOCK_SIZE_VAL,
                             SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                             resp);
        if (ret < 0) {
            printf("x1_sdhci%d: CMD16 failed\n", sc->index);
            return ret;
        }
    }

    /* Switch to 4-bit bus width */
    sc->bus_width = 4;
    ret = sdhci_send_acmd(sc, SD_APP_SET_BUS_WIDTH, 2,  /* 2 = 4-bit */
                          SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                          NULL);
    if (ret < 0) {
        printf("x1_sdhci%d: ACMD6 (set 4-bit) failed, using 1-bit\n", sc->index);
        sc->bus_width = 1;
    } else {
        /* Tell the host controller to use 4-bit mode */
        uint8 ctrl = sdhci_readb(sc, SDHCI_HOST_CONTROL);
        ctrl |= SDHCI_CTRL_4BITBUS;
        sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl);
    }

    /* Increase clock to 25 MHz (SD default speed) */
    sdhci_set_clock(sc, 25000000);

    return 0;
}

/**
 * Enumerate an eMMC device: CMD0 → CMD1 → CMD2 → CMD3 → CMD7 → CMD16
 * Returns 0 on success.
 */
static int sdhci_enumerate_emmc(struct sdhci_softc *sc)
{
    uint32 resp[4];
    int ret;

    sc->rca = MMC_RCA_DEFAULT;
    sc->card_type = CARD_TYPE_EMMC;

    /* CMD0: Go idle */
    sdhci_send_cmd(sc, MMC_GO_IDLE_STATE, 0, SDHCI_CMD_RESP_NONE, NULL);
    sleep_ms(10);

    /* CMD1: SEND_OP_COND — negotiate operating conditions.
     * Request sector addressing mode + 3.3V.
     * eMMC may take time to power up, so retry on timeout.
     * Use 100ms command timeout to avoid long waits. */
    int tries = 0;
    int cmd1_ok = 0;
    do {
        ret = sdhci_send_cmd_to(sc, MMC_SEND_OP_COND,
                                0x40FF8080,  /* sector mode + voltage */
                                SDHCI_CMD_RESP_48, /* R3 */
                                resp, 100);
        if (ret == 0) {
            cmd1_ok = 1;
            if (resp[0] & MMC_OCR_BUSY)
                break;  /* card is ready */
        }
        /* Retry on timeout — eMMC may still be powering up */
        if (++tries > 30) {
            printf("x1_sdhci%d: CMD1 no response after %d attempts — "
                   "eMMC not present or not powered\n", sc->index, tries);
            return -ETIMEDOUT;
        }
        sleep_ms(10);
    } while (1);

    if (!cmd1_ok) {
        printf("x1_sdhci%d: CMD1 never succeeded\n", sc->index);
        return -EIO;
    }

    printf("x1_sdhci%d: eMMC OCR = 0x%x\n", sc->index, resp[0]);

    /* CMD2: ALL_SEND_CID */
    ret = sdhci_send_cmd(sc, MMC_ALL_SEND_CID, 0,
                         SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp);
    if (ret < 0)
        return ret;

    /* CMD3: SET_RELATIVE_ADDR — eMMC: we assign the RCA */
    ret = sdhci_send_cmd(sc, MMC_SET_RELATIVE_ADDR,
                         (uint32)sc->rca << 16,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         resp);
    if (ret < 0)
        return ret;

    /* CMD9: SEND_CSD — get card specific data */
    ret = sdhci_send_cmd(sc, MMC_SEND_CSD, (uint32)sc->rca << 16,
                         SDHCI_CMD_RESP_136 | SDHCI_CMD_CRC, resp);
    if (ret < 0)
        return ret;

    /* eMMC CSD: fields use same SDHCI R2 bit mapping as SD.
     * For >2GB eMMC, C_SIZE=0xFFF → read SEC_COUNT from EXT_CSD. */
    {
        /* READ_BL_LEN = CSD[83:80] → resp[2] bits [11:8] */
        uint32 read_bl_len = (resp[2] >> 8) & 0xF;
        /* C_SIZE = CSD[73:62] → resp[2][1:0]:resp[1][31:22] */
        uint32 c_size = ((resp[2] & 0x3) << 10) | ((resp[1] >> 22) & 0x3FF);
        /* C_SIZE_MULT = CSD[49:47] → resp[1] bits [9:7] */
        uint32 c_size_mult = (resp[1] >> 7) & 0x7;
        if (c_size == 0xFFF) {
            /* Capacity is in EXT_CSD — we'll read it after SELECT */
            sc->capacity_blocks = 0; /* will be read from EXT_CSD */
        } else {
            uint32 mult = 1 << (c_size_mult + 2);
            uint32 blocknr = (c_size + 1) * mult;
            uint32 block_len = 1 << read_bl_len;
            sc->capacity_blocks = (uint64)blocknr * block_len / 512;
        }
    }

    /* CMD7: SELECT_CARD */
    ret = sdhci_send_cmd(sc, MMC_SELECT_CARD, (uint32)sc->rca << 16,
                         SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         resp);
    if (ret < 0)
        return ret;

    /* CMD16: SET_BLOCKLEN to 512 */
    ret = sdhci_send_cmd(sc, MMC_SET_BLOCKLEN, SDHCI_BLOCK_SIZE_VAL,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         resp);
    if (ret < 0)
        return ret;

    /* Read EXT_CSD to get true capacity (CMD8 for eMMC) */
    if (sc->capacity_blocks == 0) {
        uint8 ext_csd[512];
        sdhci_writew(sc, SDHCI_BLOCK_SIZE,
                     SDHCI_MAKE_BLKSZ(7, SDHCI_BLOCK_SIZE_VAL));
        sdhci_writew(sc, SDHCI_BLOCK_COUNT, 1);
        sdhci_writew(sc, SDHCI_TRANSFER_MODE,
                     SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN);

        ret = sdhci_send_cmd(sc, MMC_SEND_EXT_CSD, 0,
                             SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC |
                             SDHCI_CMD_INDEX | SDHCI_CMD_DATA,
                             resp);
        if (ret == 0) {
            ret = sdhci_pio_read_block(sc, ext_csd);
            if (ret == 0) {
                sdhci_wait_xfer_done(sc);
                /* SEC_COUNT is at EXT_CSD bytes 212-215 (little-endian) */
                sc->capacity_blocks =
                    (uint64)ext_csd[212] |
                    ((uint64)ext_csd[213] << 8) |
                    ((uint64)ext_csd[214] << 16) |
                    ((uint64)ext_csd[215] << 24);
            }
        }
    }

    printf("x1_sdhci%d: eMMC capacity = %ld sectors (%ld MB)\n", sc->index,
           sc->capacity_blocks, sc->capacity_blocks / 2048);

    /* Switch to 8-bit bus width via CMD6 (SWITCH):
     * Access=Write Byte (0x03), Index=183 (BUS_WIDTH), Value=2 (8-bit) */
    ret = sdhci_send_cmd(sc, SD_SWITCH_FUNC,
                         (3 << 24) | (183 << 16) | (2 << 8),
                         SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC | SDHCI_CMD_INDEX,
                         resp);
    if (ret == 0) {
        uint8 ctrl = sdhci_readb(sc, SDHCI_HOST_CONTROL);
        ctrl |= SDHCI_CTRL_8BITBUS;
        sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl);
        sc->bus_width = 8;
        sleep_ms(1);
    } else {
        sc->bus_width = 1;
        printf("x1_sdhci%d: 8-bit bus switch failed, using 1-bit\n", sc->index);
    }

    /* Increase clock to 26 MHz (eMMC backward-compatible speed) */
    sdhci_set_clock(sc, 26000000);

    return 0;
}

/* ======================================================================
 * Block Device Interface
 * ====================================================================== */

static int sdhci_blk_open(blkdev_t *blkdev) { return 0; }
static int sdhci_blk_release(blkdev_t *blkdev) { return 0; }

/**
 * submit_bio — process a block I/O request.
 * Iterates over bio segments and reads/writes blocks via PIO.
 */
static int sdhci_submit_bio(blkdev_t *blkdev, struct bio *bio)
{
    struct sdhci_softc *sc = container_of(blkdev, struct sdhci_softc, bdev);
    struct bio_vec bvec;
    struct bio_iter iter;
    int ret = 0;

    mutex_lock(&sc->lock);
    bio_start_io_acct(bio);

    bio_for_each_segment(&bvec, bio, &iter) {
        uint64 sector = iter.blkno;
        page_t *page = bvec.bv_page;

        if (page == NULL) {
            ret = -EINVAL;
            break;
        }

        void *pa = (void *)__page_to_pa(page);
        if (pa == NULL) {
            ret = -EINVAL;
            break;
        }

        void *data = (void *)((uint64)pa + bvec.offset);
        uint32 nblocks = bvec.len / SDHCI_BLOCK_SIZE_VAL;

        if (nblocks == 0) {
            ret = -EINVAL;
            break;
        }

        if (bio_dir_write(bio)) {
            ret = sdhci_write_blocks(sc, (uint32)sector, nblocks, data);
        } else {
            ret = sdhci_read_blocks(sc, (uint32)sector, nblocks, data);
        }

        if (ret < 0)
            break;

        iter.size_done += bvec.len;
    }

    mutex_unlock(&sc->lock);

    bio->error = ret;
    bio_complete(bio);
    return ret;
}

static blkdev_ops_t sdhci_blk_ops = {
    .open = sdhci_blk_open,
    .release = sdhci_blk_release,
    .submit_bio = sdhci_submit_bio,
};

/* ======================================================================
 * Instance Initialization
 * ====================================================================== */

/**
 * Initialize a single SDHCI instance.
 *
 * @idx:  Index into platform.sdhci[] (0, 1, or 2)
 * @is_emmc: non-zero if this is an eMMC instance
 * Returns 0 on success.
 */
static int sdhci_init_one(int idx, int is_emmc)
{
    struct sdhci_softc *sc = &sdhci_sc[idx];
    int ret;

    memset(sc, 0, sizeof(*sc));
    mutex_init(&sc->lock, "x1_sdhci");
    sc->index = idx;
    sc->irq = platform.sdhci[idx].irq;

    /* Map SDHCI registers (already identity-mapped by vm.c) */
    sc->regs = (volatile uint8 *)(uint64)platform.sdhci[idx].base;
    printf("x1_sdhci%d: MMIO base 0x%lx, IRQ %d\n", idx,
           platform.sdhci[idx].base, platform.sdhci[idx].irq);

    /* Map APMU register (per-instance) */
    if (platform.sdhci[idx].apmu_base && platform.sdhci[idx].apmu_offset) {
        sc->apmu_reg = (volatile uint32 *)(uint64)(
            platform.sdhci[idx].apmu_base + platform.sdhci[idx].apmu_offset);
        printf("x1_sdhci%d: APMU ctrl @ 0x%lx\n", idx,
               (uint64)platform.sdhci[idx].apmu_base +
               platform.sdhci[idx].apmu_offset);
    }

    /* Map shared AXI register (always at SDH0 offset in APMU).
     * The shared AXI offset is parsed from the FDT (apmu_axi_offset). */
    if (platform.sdhci[idx].apmu_base && platform.sdhci[idx].apmu_axi_offset) {
        sc->apmu_axi_reg = (volatile uint32 *)(uint64)(
            platform.sdhci[idx].apmu_base + platform.sdhci[idx].apmu_axi_offset);
    }

    /* Map AIB clock register (APBC base + AIB offset, from FDT) */
    if (platform.sdhci[idx].apbc_base) {
        sc->aib_reg = (volatile uint32 *)(uint64)(
            platform.sdhci[idx].apbc_base + APBC_AIB_CLK_RST);
    }

    /* Enable clocks and deassert reset via APMU */
    sdhci_apmu_enable(sc);

    /* Probe read: verify controller is alive after clock enable.
     * SDHCI_HOST_VERSION is a 16-bit register at offset 0xFE —
     * must use readw (not readl) to avoid unaligned MMIO access. */
    {
        uint16 ver = sdhci_readw(sc, SDHCI_HOST_VERSION);
        if (ver == 0xFFFF || ver == 0x0000) {
            printf("x1_sdhci%d: controller not responding "
                   "(version=0x%x), aborting\n", idx, ver);
            return -1;
        }
        printf("x1_sdhci%d: controller alive (version=0x%x)\n", idx, ver);
    }

    /* Initialize SDHCI controller hardware */
    ret = sdhci_hw_init(sc);
    if (ret < 0) {
        printf("x1_sdhci%d: hardware init failed\n", idx);
        return -1;
    }

    /* Check card presence (for SD card slots) */
    if (!is_emmc && !sdhci_card_present(sc)) {
        printf("x1_sdhci%d: no card detected\n", idx);
        /* Not a fatal error — slot is empty */
        return 0;
    }

    /* Enumerate the card/device */
    if (is_emmc) {
        ret = sdhci_enumerate_emmc(sc);
    } else {
        ret = sdhci_enumerate_sd(sc);
    }

    if (ret < 0) {
        printf("x1_sdhci%d: card enumeration failed\n", idx);
        return -1;
    }

    sc->initialized = 1;

    /* Register as a block device */
    sc->bdev.dev.major = 4;  /* new major for SD/eMMC */
    sc->bdev.dev.minor = idx + 1;
    sc->bdev.dev.devmode = S_IFBLK | 0600;
    sc->bdev.readable = 1;
    sc->bdev.writable = 1;
    sc->bdev.block_shift = 0; /* 512 bytes per sector */
    sc->bdev.ops = sdhci_blk_ops;

    if (is_emmc) {
        static const char *emmc_names[] = {"mmc0", "mmc1", "mmc2"};
        sc->bdev.dev.devname = emmc_names[idx];
    } else {
        static const char *sd_names[] = {"sd0", "sd1", "sd2"};
        sc->bdev.dev.devname = sd_names[idx];
    }

    ret = blkdev_register(&sc->bdev);
    if (ret != 0) {
        printf("x1_sdhci%d: blkdev_register failed: %d\n", idx, ret);
        return -1;
    }

    printf("x1_sdhci%d: registered as %s (%ld MB, %d-bit bus)\n",
           idx, sc->bdev.dev.devname,
           sc->capacity_blocks / 2048, sc->bus_width);

    __atomic_fetch_add(&sdhci_count, 1, __ATOMIC_SEQ_CST);
    return 0;
}

/* ======================================================================
 * Top-level init — spawns a kthread for post-init probing
 * ====================================================================== */

/**
 * Per-instance init kthread: initialises one SDHCI slot.
 */
static void x1_sdhci_init_one_kthread(uint64 idx,
                                      uint64 arg2 __attribute__((unused)))
{
    int i = (int)idx;

    if (platform.sdhci[i].base == 0)
        return;

    /* Skip SDIO (sdhci1) — WiFi, not a block device */
    if (platform.sdhci[i].is_sdio) {
        printf("x1_sdhci%d: SDIO instance, skipping\n", i);
        return;
    }

    int is_emmc = platform.sdhci[i].is_emmc;
    if (sdhci_init_one(i, is_emmc) < 0)
        printf("x1_sdhci%d: init failed, skipping\n", i);
}

/**
 * Kernel thread entry point: spawns per-instance init kthreads so that
 * all SDHCI slots probe in parallel.
 *
 * Running in kthread context allows us to use sleep_ms() (scheduler-
 * based timer sleep) instead of busy-waiting, which is friendlier to
 * the system during the potentially slow SD/eMMC enumeration.
 */
static void x1_sdhci_kthread(uint64 arg1 __attribute__((unused)),
                             uint64 arg2 __attribute__((unused)))
{
    int n = platform.sdhci_count;
    if (n > MAX_SDH_INSTANCES)
        n = MAX_SDH_INSTANCES;

    printf("x1_sdhci: found %d SDHCI instance(s)\n", n);

    for (int i = 0; i < n; i++) {
        char name[16];
        name[0] = 's'; name[1] = 'd'; name[2] = 'h';
        name[3] = '0' + i; name[4] = '\0';
        struct thread *t = kthread_create(name, x1_sdhci_init_one_kthread,
                                          (uint64)i, 0, KERNEL_STACK_ORDER);
        if (IS_ERR_OR_NULL(t)) {
            printf("x1_sdhci%d: failed to create init kthread\n", i);
        } else {
            wakeup(t);
        }
    }

    /* kthread exits — per-instance threads run independently */
}

/**
 * x1_sdhci_init — schedule SDHCI probing as a post-init kthread.
 *
 * Called from start_kernel.  The actual hardware probing and card
 * enumeration run in a dedicated kernel thread so that:
 *  1. The scheduler is already running → sleep_ms() works.
 *  2. Boot proceeds without blocking on slow card init.
 */
void x1_sdhci_init(void)
{
    if (!platform.has_sdhci)
        return;

    struct thread *t = kthread_create("x1_sdhci", x1_sdhci_kthread,
                                      0, 0, KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("x1_sdhci: failed to create init kthread\n");
        return;
    }
    wakeup(t);
}
