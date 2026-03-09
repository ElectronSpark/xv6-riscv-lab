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
#include "lock/mutex_types.h"
#include "defs.h"
#include "printf.h"
#include "dev/net.h"
#include "dev/blkdev.h"
#include "dev/bio.h"
#include "dev/x1_sdhci.h"
#include "dev/gendisk.h"
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

    /* UHS mode tracking */
    int uhs_1v8;                    /* 1 if running at 1.8V signaling */

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
        uint32 pst = sdhci_readl(sc, SDHCI_PRESENT_STATE);
        uint16 ist = sdhci_readw(sc, SDHCI_INT_STATUS);
        uint16 est = sdhci_readw(sc, SDHCI_ERR_INT_STATUS);
        printf("x1_sdhci%d: CMD%d inhibit timeout "
               "(pstate=0x%x int=0x%x err=0x%x)\n",
               sc->index, cmd_idx, pst, ist, est);
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
        uint32 pst = sdhci_readl(sc, SDHCI_PRESENT_STATE);
        if (cmd_idx != MMC_GO_IDLE_STATE && cmd_idx != SD_SEND_IF_COND &&
            cmd_idx != MMC_SEND_OP_COND) {
            printf("x1_sdhci%d: CMD%d timeout (err=0x%x pstate=0x%x)\n",
                   sc->index, cmd_idx, err, pst);
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
        uint32 pst = sdhci_readl(sc, SDHCI_PRESENT_STATE);
        uint16 ist = sdhci_readw(sc, SDHCI_INT_STATUS);
        uint16 est = sdhci_readw(sc, SDHCI_ERR_INT_STATUS);
        printf("x1_sdhci%d: transfer complete timeout "
               "(pstate=0x%x int=0x%x err=0x%x)\n",
               sc->index, pst, ist, est);
        /*
         * Reset the data line to clear DATA_INHIBIT in PRESENT_STATE.
         * Without this, every subsequent data command (CMD17/18/24/25)
         * will fail with "inhibit timeout" because DATA_INHIBIT sticks.
         */
        sdhci_writew(sc, SDHCI_ERR_INT_STATUS, SDHCI_ERR_ALL);
        sdhci_reset(sc, SDHCI_RESET_DATA);
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

/* ======================================================================
 * UHS-I SDR104 Support
 * ====================================================================== */

/**
 * Standard SD 4-bit tuning block pattern (64 bytes).
 * The card sends this predefined pattern in response to CMD19.
 * The host compares received data to detect timing errors.
 */
static const uint8 sd_tuning_block_4bit[64] = {
    0xFF, 0x0F, 0xFF, 0x00, 0xFF, 0xCC, 0xC3, 0xCC,
    0xC3, 0x3C, 0xCC, 0xFF, 0xFE, 0xFF, 0xFE, 0xEF,
    0xFF, 0xDF, 0xFF, 0xDD, 0xFF, 0xFB, 0xFF, 0xFB,
    0xBF, 0xFF, 0x7F, 0xFF, 0x77, 0xF7, 0xBD, 0xEF,
    0xFF, 0xF0, 0xFF, 0xF0, 0x0F, 0xFC, 0xCC, 0x3C,
    0xCC, 0x33, 0xCC, 0xCF, 0xFF, 0xEF, 0xFF, 0xEE,
    0xFF, 0xFD, 0xFF, 0xFD, 0xDF, 0xFF, 0xBF, 0xFF,
    0xBB, 0xFF, 0xF7, 0xFF, 0xF7, 0x7F, 0x7B, 0xDE,
};

/**
 * Switch the physical SD I/O voltage via the AIB register.
 *
 * The SpacemiT X1 SoC uses a separate AIB (Always-on I/O Block) register
 * to control the SD slot I/O voltage rail (vqmmc / LDO_1 on the PMIC).
 * The SDHCI Host Control 2 VDD_180 bit only switches signaling level
 * inside the controller — the AIB register gates the actual voltage.
 *
 * Access requires unlocking via the APBC ASFAR/ASSAR security key
 * registers (write-once-read-clear, must be unlocked before each write).
 *
 * @use_1v8: 1 = switch to 1.8V, 0 = switch to 3.3V
 *
 * Register addresses from Orange Pi RV2 DTS:
 *   aib_mmc1_io_reg = 0xD401E81C
 *   apbc_asfar_reg  = 0xD4015050
 *   apbc_assar_reg  = 0xD4015054
 */
static void sdhci_x1_set_io_voltage(int use_1v8)
{
    volatile uint32 *asfar = (volatile uint32 *)APBC_ASFAR_REG;
    volatile uint32 *assar = (volatile uint32 *)APBC_ASSAR_REG;
    volatile uint32 *aib   = (volatile uint32 *)AIB_MMC1_IO_REG;
    uint32 reg;

    /* Unlock APBC security (required before each AIB register write) */
    *asfar = AKEY_ASFAR;
    *assar = AKEY_ASSAR;

    /* Read-modify-write the AIB MMC1 IO register */
    reg = *aib;
    if (use_1v8)
        reg |= MMC1_IO_V18EN;
    else
        reg &= ~MMC1_IO_V18EN;

    /* Unlock again (keys are consumed by the read) */
    *asfar = AKEY_ASFAR;
    *assar = AKEY_ASSAR;
    *aib = reg;
}

/**
 * Perform the SD voltage switch sequence (CMD11) to 1.8V signaling.
 *
 * Per SD Physical Layer Spec v3.01 Section 4.2.4:
 *   1. Send CMD11 (VOLTAGE_SWITCH)
 *   2. Stop SD clock
 *   3. Wait for DAT[3:0] to go LOW  (card drives low)
 *   4. Set 1.8V signaling in Host Control 2
 *   5. Wait >= 5 ms for voltage regulator stabilisation
 *   6. Re-enable SD clock
 *   7. Wait for DAT[3:0] to go HIGH (card releases)
 *
 * Returns 0 on success.  On failure the card may need power-cycling.
 */
static int sdhci_sd_voltage_switch(struct sdhci_softc *sc)
{
    uint32 resp[4];
    int ret;
    uint16 clk;

    /* CMD11: VOLTAGE_SWITCH */
    ret = sdhci_send_cmd(sc, SD_VOLTAGE_SWITCH, 0,
                         SDHCI_CMD_RESP_48, resp);
    if (ret < 0) {
        printf("x1_sdhci%d: CMD11 failed: %d\n", sc->index, ret);
        return ret;
    }

    /* Stop SD clock (keep internal clock running) */
    clk = sdhci_readw(sc, SDHCI_CLOCK_CONTROL);
    clk &= ~SDHCI_CLOCK_CARD_EN;
    sdhci_writew(sc, SDHCI_CLOCK_CONTROL, clk);

    /* Wait for DAT[3:0] to go LOW (card signals readiness, up to 1ms) */
    {
        uint64 deadline = r_time() +
                          (uint64)TIMEBASE_FREQUENCY / 1000;
        int dat_low = 0;
        while (r_time() < deadline) {
            uint32 pstate = sdhci_readl(sc, SDHCI_PRESENT_STATE);
            if (!(pstate & SDHCI_DATA_LVL_MASK)) {
                dat_low = 1;
                break;
            }
            scheduler_yield();
        }
        if (!dat_low) {
            printf("x1_sdhci%d: voltage switch: DAT not low\n", sc->index);
            /* Re-enable clock and abort */
            clk |= SDHCI_CLOCK_CARD_EN;
            sdhci_writew(sc, SDHCI_CLOCK_CONTROL, clk);
            return -EIO;
        }
    }

    /* Switch physical I/O voltage via AIB register.
     * This must happen BEFORE setting CTRL_VDD_180 in the host controller,
     * so that the voltage rail is actually at 1.8V when the controller
     * switches signaling level. */
    sdhci_x1_set_io_voltage(1);

    /* Set 1.8V signaling in Host Control 2 */
    {
        uint16 ctrl2 = sdhci_readw(sc, SDHCI_HOST_CONTROL2);
        ctrl2 |= SDHCI_CTRL_VDD_180;
        sdhci_writew(sc, SDHCI_HOST_CONTROL2, ctrl2);
    }

    /* Wait >= 5 ms for voltage regulator to settle */
    sleep_ms(10);

    /* Re-enable SD clock */
    clk = sdhci_readw(sc, SDHCI_CLOCK_CONTROL);
    clk |= SDHCI_CLOCK_CARD_EN;
    sdhci_writew(sc, SDHCI_CLOCK_CONTROL, clk);

    /* Wait for DAT[3:0] to go HIGH (card verifies 1.8V, up to 1ms) */
    sleep_ms(1);
    {
        uint32 pstate = sdhci_readl(sc, SDHCI_PRESENT_STATE);
        if ((pstate & SDHCI_DATA_LVL_MASK) != SDHCI_DATA_LVL_MASK) {
            uint16 ctrl2 = sdhci_readw(sc, SDHCI_HOST_CONTROL2);
            uint16 clk_st = sdhci_readw(sc, SDHCI_CLOCK_CONTROL);
            uint8 pwr = sdhci_readb(sc, SDHCI_POWER_CONTROL);
            printf("x1_sdhci%d: voltage switch FAILED: pstate=0x%x "
                   "ctrl2=0x%x clk=0x%x pwr=0x%x\n",
                   sc->index, pstate, ctrl2, clk_st, pwr);
            printf("x1_sdhci%d:   DAT[3:0]=%d%d%d%d CMD=%d\n",
                   sc->index,
                   !!(pstate & (1 << 23)), !!(pstate & (1 << 22)),
                   !!(pstate & (1 << 21)), !!(pstate & (1 << 20)),
                   !!(pstate & (1 << 24)));

            /* CRITICAL: Revert I/O voltage to 3.3V and clear VDD_180.
             * Leaving VDD_180 set with actual voltage at 3.3V causes a
             * signaling mismatch that corrupts all subsequent
             * data transfers (CMD6 switch status, CMD17 reads). */
            sdhci_x1_set_io_voltage(0);
            {
                uint16 ctrl2 = sdhci_readw(sc, SDHCI_HOST_CONTROL2);
                ctrl2 &= ~SDHCI_CTRL_VDD_180;
                sdhci_writew(sc, SDHCI_HOST_CONTROL2, ctrl2);
            }

            /* Reset CMD and DAT lines to clear any residual state
             * from the aborted voltage switch sequence. */
            sdhci_reset(sc, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            return -EIO;
        }
    }

    sc->uhs_1v8 = 1;
    printf("x1_sdhci%d: voltage switch to 1.8V OK\n", sc->index);
    return 0;
}

/**
 * Switch the X1 SDHCI from internal-clock mode to PHY clock mode.
 *
 * At SDR104 speeds (>100 MHz) the PHY provides programmable delay lines
 * for TX/RX data timing.  The internal clock path used during identification
 * (400 kHz) cannot operate at these frequencies.
 *
 * Sequence (derived from Linux sdhci-ky driver register state):
 *   1. Clear TX_INT_CLK_SEL (switch from internal clock to PHY)
 *   2. Set TX_MUX_SEL (route through PHY mux)
 *   3. Power up the delay line (DLINE_PU)
 *   4. Enable PHY_FUNC_EN and wait for PHY_PLL_LOCK
 */
static int sdhci_x1_phy_enable(struct sdhci_softc *sc)
{
    uint32 reg;

    /* Step 1-2: Switch TX path from internal clock to PHY */
    reg = sdhci_readl(sc, SDHC_TX_CFG_REG);
    reg &= ~TX_INT_CLK_SEL;   /* clear: use PHY clock, not internal */
    reg |= TX_MUX_SEL;        /* set: route through PHY mux */
    sdhci_writel(sc, SDHC_TX_CFG_REG, reg);

    /* Step 3: Power up the delay line */
    reg = sdhci_readl(sc, SDHC_DLINE_CTRL_REG);
    reg |= DLINE_PU;
    sdhci_writel(sc, SDHC_DLINE_CTRL_REG, reg);

    /* Step 4: Enable PHY and wait for PLL lock */
    reg = sdhci_readl(sc, SDHC_PHY_CTRL_REG);
    reg |= PHY_FUNC_EN;
    /* put into non-legacy mode */
    reg &= ~HOST_LEGACY_MODE;
    sdhci_writel(sc, SDHC_PHY_CTRL_REG, reg);

    /* Wait for PHY PLL lock (up to 50 ms) */
    uint64 deadline = r_time() +
                      (uint64)TIMEBASE_FREQUENCY * 50 / 1000;
    while (r_time() < deadline) {
        reg = sdhci_readl(sc, SDHC_PHY_CTRL_REG);
        if (reg & PHY_PLL_LOCK) {
            printf("x1_sdhci%d: PHY PLL locked\n", sc->index);
            return 0;
        }
        scheduler_yield();
    }

    printf("x1_sdhci%d: PHY PLL lock timeout (reg=0x%x)\n",
           sc->index, sdhci_readl(sc, SDHC_PHY_CTRL_REG));
    /* Continue anyway — some X1 revisions may not report PLL_LOCK cleanly */
    return 0;
}

/**
 * Program a TX delay code into the X1 delay line.
 *
 * The delay code selects the sampling point within one clock period.
 * Values 0-255 sweep through the full phase range.  Tuning sweeps all
 * codes to find the optimal value with the widest passing window.
 */
static void sdhci_x1_set_delay(struct sdhci_softc *sc, uint8 code)
{
    uint32 reg = sdhci_readl(sc, SDHC_DLINE_CFG_REG);
    reg &= ~SDHC_DLINE_CFG_DELAY_MASK;
    reg |= (uint32)code;
    sdhci_writel(sc, SDHC_DLINE_CFG_REG, reg);
}

/**
 * Execute vendor-specific tuning for the X1 SDHCI controller.
 *
 * Sweeps TX delay codes 0-255, sending CMD19 (SEND_TUNING_BLOCK) at each
 * value.  Compares received 64-byte data against the standard SD 4-bit
 * tuning pattern.  Selects the center of the widest passing window.
 *
 * This matches the Linux sdhci-ky driver's tuning algorithm as observed
 * in dmesg ("pass window [...)", "use the firstly delay_code:...").
 *
 * Returns 0 on success, negative on failure.
 */
static int sdhci_x1_execute_tuning(struct sdhci_softc *sc)
{
    uint8 buf[64];
    uint32 *p = (uint32 *)buf;
    uint32 resp[4];

    /* Track passing windows */
    int best_start = -1, best_len = 0;
    int cur_start = -1, cur_len = 0;
    int nwindows = 0;

    printf("x1_sdhci%d: starting tuning sweep (0-255)\n", sc->index);

    for (int delay = 0; delay < 256; delay++) {
        sdhci_x1_set_delay(sc, (uint8)delay);

        /* Set up for CMD19: 64-byte single-block read */
        sdhci_writew(sc, SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(7, 64));
        sdhci_writew(sc, SDHCI_BLOCK_COUNT, 1);
        sdhci_writew(sc, SDHCI_TRANSFER_MODE,
                     SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN);

        int ret = sdhci_send_cmd_to(sc, SD_SEND_TUNING_BLOCK, 0,
                                    SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC |
                                    SDHCI_CMD_INDEX | SDHCI_CMD_DATA,
                                    resp, 50);
        if (ret < 0) {
            sdhci_reset(sc, SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            goto fail;
        }

        /* PIO read: 64 bytes = 16 words */
        if (sdhci_wait_bit(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL,
                           1, 50) < 0) {
            sdhci_reset(sc, SDHCI_RESET_DATA);
            goto fail;
        }
        sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_AVAIL);
        for (int i = 0; i < 16; i++)
            p[i] = sdhci_readl(sc, SDHCI_BUFFER);
        /* Clear transfer complete */
        sdhci_writew(sc, SDHCI_INT_STATUS, SDHCI_INT_DATA_END);

        /* Compare with reference pattern */
        if (memcmp(buf, sd_tuning_block_4bit, 64) == 0) {
            /* Pass */
            if (cur_start < 0)
                cur_start = delay;
            cur_len++;
            continue;
        }

fail:
        /* Fail — close current window if any */
        if (cur_len > 0) {
            printf("x1_sdhci%d: pass window [%d %d)\n",
                   sc->index, cur_start, cur_start + cur_len);
            nwindows++;
            if (cur_len > best_len) {
                best_start = cur_start;
                best_len = cur_len;
            }
        }
        cur_start = -1;
        cur_len = 0;
    }

    /* Close final window */
    if (cur_len > 0) {
        printf("x1_sdhci%d: pass window [%d %d)\n",
               sc->index, cur_start, cur_start + cur_len);
        nwindows++;
        if (cur_len > best_len) {
            best_start = cur_start;
            best_len = cur_len;
        }
    }

    if (best_len == 0) {
        printf("x1_sdhci%d: tuning failed — no passing window\n", sc->index);
        return -EIO;
    }

    /* Select center of widest window */
    int selected = best_start + best_len / 2;
    sdhci_x1_set_delay(sc, (uint8)selected);

    printf("x1_sdhci%d: tuning done, delay=%d (window [%d,%d), %d windows)\n",
           sc->index, selected, best_start, best_start + best_len, nwindows);

    return 0;
}

/**
 * Try to switch an SD card to SDR104 mode (~200 MHz).
 *
 * Requires prior successful voltage switch to 1.8V (sc->uhs_1v8 == 1).
 *
 * Steps:
 *   1. CMD6 to switch card to SDR104 access mode (function 3, group 1)
 *   2. Set UHS-I mode in Host Control 2
 *   3. Set High Speed bit in Host Control
 *   4. Reconfigure SDHCI clock to maximum (divisor 0 → base clock pass-through)
 *   5. Enable X1 PHY (switch from internal clock to PHY clock path)
 *   6. Execute vendor-specific tuning sweep
 *
 * Returns 0 on success.  On failure the caller should fall back to HS (50 MHz).
 */
static int sdhci_sd_try_sdr104(struct sdhci_softc *sc)
{
    uint8 sw_status[128] __attribute__((aligned(64)));
    uint32 resp[4];
    int ret;

    if (!sc->uhs_1v8) {
        printf("x1_sdhci%d: SDR104 needs 1.8V signaling\n", sc->index);
        return -1;
    }

    /* ---- Step 1: CMD6 check — does card support SDR104? ---- */
    dma_cache_inval(sw_status, 64);
    sdhci_writel(sc, SDHCI_DMA_ADDRESS, (uint32)(uint64)sw_status);
    sdhci_writew(sc, SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(7, 64));
    sdhci_writew(sc, SDHCI_BLOCK_COUNT, 1);
    sdhci_writew(sc, SDHCI_TRANSFER_MODE,
                 SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_DMA);

    /* mode=0 (check), group 1 = function 3 (SDR104), others = 0xF (keep) */
    ret = sdhci_send_cmd(sc, SD_SWITCH_FUNC, 0x00FFFFF3,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC |
                         SDHCI_CMD_INDEX | SDHCI_CMD_DATA, resp);
    if (ret < 0)
        return ret;

    ret = sdhci_sdma_wait(sc, (uint64)sw_status, 64, 0);
    if (ret < 0)
        return ret;
    dma_cache_inval(sw_status, 64);

    /* Byte 13 bit 3 = SDR104 supported */
    if (!(sw_status[13] & 0x08)) {
        printf("x1_sdhci%d: card does not support SDR104\n", sc->index);
        return -1;
    }

    /* ---- Step 1b: CMD6 switch — activate SDR104 ---- */
    dma_cache_inval(sw_status, 64);
    sdhci_writel(sc, SDHCI_DMA_ADDRESS, (uint32)(uint64)sw_status);
    sdhci_writew(sc, SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(7, 64));
    sdhci_writew(sc, SDHCI_BLOCK_COUNT, 1);
    sdhci_writew(sc, SDHCI_TRANSFER_MODE,
                 SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_DMA);

    /* mode=1 (switch), group 1 = function 3 (SDR104) */
    ret = sdhci_send_cmd(sc, SD_SWITCH_FUNC, 0x80FFFFF3,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC |
                         SDHCI_CMD_INDEX | SDHCI_CMD_DATA, resp);
    if (ret < 0)
        return ret;

    ret = sdhci_sdma_wait(sc, (uint64)sw_status, 64, 0);
    if (ret < 0)
        return ret;
    dma_cache_inval(sw_status, 64);

    /* Byte 16 low nibble: selected function (expect 3 = SDR104) */
    if ((sw_status[16] & 0x0F) != 3) {
        printf("x1_sdhci%d: SDR104 switch rejected (result=0x%x)\n",
               sc->index, sw_status[16] & 0x0F);
        return -EIO;
    }

    /* ---- Step 2: Set UHS-I SDR104 mode in Host Control 2 ---- */
    {
        uint16 ctrl2 = sdhci_readw(sc, SDHCI_HOST_CONTROL2);
        ctrl2 &= ~SDHCI_CTRL_UHS_MASK;
        ctrl2 |= SDHCI_CTRL_UHS_SDR104;
        sdhci_writew(sc, SDHCI_HOST_CONTROL2, ctrl2);
    }

    /* ---- Step 3: Set High Speed bit ---- */
    {
        uint8 ctrl = sdhci_readb(sc, SDHCI_HOST_CONTROL);
        ctrl |= SDHCI_CTRL_HISPD;
        sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl);
    }

    /* ---- Step 4: Set maximum clock (div=0 → base clock ~200 MHz) ---- */
    sdhci_set_clock(sc, 400000000); /* request > base → div=0 (pass-through) */

    /* ---- Step 5: Enable X1 PHY for high-speed operation ---- */
    sdhci_x1_phy_enable(sc);

    /* ---- Step 6: Execute vendor-specific tuning ---- */
    ret = sdhci_x1_execute_tuning(sc);
    if (ret < 0) {
        printf("x1_sdhci%d: SDR104 tuning failed, reverting\n", sc->index);
        /* Revert: switch PHY back to internal clock */
        uint32 tx = sdhci_readl(sc, SDHC_TX_CFG_REG);
        tx |= TX_INT_CLK_SEL;
        tx &= ~TX_MUX_SEL;
        sdhci_writel(sc, SDHC_TX_CFG_REG, tx);
        return ret;
    }

    return 0;
}

/**
 * Try to switch an SD card to High Speed (50 MHz) via CMD6 (SWITCH_FUNC).
 *
 * CMD6 mode=0 queries supported functions; mode=1 activates the switch.
 * The 512-bit (64-byte) status structure reports supported and selected
 * functions per group.  Group 1 = Access Mode, function 1 = High Speed.
 *
 * On success the HISPD bit in Host Control is set; the caller must then
 * reconfigure the SD clock to 50 MHz.
 *
 * Returns 0 on success, negative on failure (card stays at default speed).
 */
static int sdhci_sd_try_highspeed(struct sdhci_softc *sc)
{
    /*
     * CMD6 returns a 64-byte (512-bit) switch function status block.
     * Use a cache-line-aligned DMA buffer instead of PIO — this
     * controller's PIO path appears unreliable for data reads (Transfer
     * Complete never fires even though buffer data arrives).
     */
    uint8 sw_status[128] __attribute__((aligned(64)));
    uint32 resp[4];
    int ret;

    /*
     * CMD6 check: query High Speed support.
     *   mode = 0 (check), group 1 = function 1 (HS), groups 2-6 = 0xF (keep)
     *   arg = 0x00FFFFF1
     */
    dma_cache_inval(sw_status, 64);
    sdhci_writel(sc, SDHCI_DMA_ADDRESS, (uint32)(uint64)sw_status);
    sdhci_writew(sc, SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(7, 64));
    sdhci_writew(sc, SDHCI_BLOCK_COUNT, 1);
    sdhci_writew(sc, SDHCI_TRANSFER_MODE,
                 SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_DMA);

    ret = sdhci_send_cmd(sc, SD_SWITCH_FUNC, 0x00FFFFF1,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC |
                         SDHCI_CMD_INDEX | SDHCI_CMD_DATA, resp);
    if (ret < 0)
        return ret;

    ret = sdhci_sdma_wait(sc, (uint64)sw_status, 64, 0);
    if (ret < 0)
        return ret;
    dma_cache_inval(sw_status, 64);

    /* Function group 1 support bits at byte 13; bit 1 = High Speed */
    if (!(sw_status[13] & 0x02)) {
        printf("x1_sdhci%d: card does not support High Speed\n", sc->index);
        return -1;
    }

    /*
     * CMD6 switch: activate High Speed.
     *   mode = 1 (switch), group 1 = function 1 (HS)
     *   arg = 0x80FFFFF1
     */
    dma_cache_inval(sw_status, 64);
    sdhci_writel(sc, SDHCI_DMA_ADDRESS, (uint32)(uint64)sw_status);
    sdhci_writew(sc, SDHCI_BLOCK_SIZE, SDHCI_MAKE_BLKSZ(7, 64));
    sdhci_writew(sc, SDHCI_BLOCK_COUNT, 1);
    sdhci_writew(sc, SDHCI_TRANSFER_MODE,
                 SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_DMA);

    ret = sdhci_send_cmd(sc, SD_SWITCH_FUNC, 0x80FFFFF1,
                         SDHCI_CMD_RESP_48 | SDHCI_CMD_CRC |
                         SDHCI_CMD_INDEX | SDHCI_CMD_DATA, resp);
    if (ret < 0)
        return ret;

    ret = sdhci_sdma_wait(sc, (uint64)sw_status, 64, 0);
    if (ret < 0)
        return ret;
    dma_cache_inval(sw_status, 64);

    /* Byte 16 low nibble: selected function in group 1 (expect 1 = HS) */
    if ((sw_status[16] & 0x0F) != 1) {
        printf("x1_sdhci%d: HS switch rejected (result=0x%x)\n",
               sc->index, sw_status[16] & 0x0F);
        return -EIO;
    }

    /* Enable High Speed bit in host controller */
    uint8 ctrl = sdhci_readb(sc, SDHCI_HOST_CONTROL);
    ctrl |= SDHCI_CTRL_HISPD;
    sdhci_writeb(sc, SDHCI_HOST_CONTROL, ctrl);
    sleep_ms(1); /* let card settle after mode switch */

    return 0;
}

/**
 * Enumerate an SD card: CMD0 → CMD8 → ACMD41 → CMD2 → CMD3 → CMD7 → CMD16
 * Returns 0 on success.
 */
static int sdhci_enumerate_sd(struct sdhci_softc *sc)
{
    uint32 resp[4];
    int ret;
    /*
     * UHS-I 1.8V voltage switch: DISABLED by default.
     *
     * The SpacemiT X1 SoC requires writing to the AIB register
     * (0xD401E81C) to physically switch the SD I/O voltage from 3.3V
     * to 1.8V.  The SDHCI CTRL_VDD_180 bit only controls signaling
     * level inside the controller — the AIB register gates the actual
     * voltage rail (vqmmc / LDO_1 on the PMIC).
     *
     * Additionally, if the voltage switch (CMD11) fails, the SD card
     * enters an undefined state (SD spec §4.2.4) that requires a real
     * Vdd power cycle via the PMIC to recover — writing 0 to
     * SDHCI_POWER_CONTROL only affects the controller, not the PMIC's
     * vmmc-supply (DCDC_4).  This makes recovery impossible.
     *
     * Set to 1 only after verifying AIB voltage control works on the
     * target board.  At 3.3V the card runs at High Speed 50 MHz,
     * which is ~23 MB/s and sufficient for most workloads.
     */
    int request_1v8 = 0;
    int retry = 0; /* set to 1 when retrying after voltage switch failure */

retry_without_1v8:
    sc->rca = 0;
    sc->card_type = CARD_TYPE_SD;

    /*
     * If retrying after a failed voltage switch, power-cycle the bus.
     * After CMD11 the card switches to 1.8V signaling internally.
     * The SDHCI_POWER_CONTROL register does NOT control the PMIC's
     * vmmc-supply (DCDC_4), so the card Vdd is never actually cut.
     * Nevertheless, we reset the controller to get a clean state.
     * The card is likely unrecoverable without a real PMIC power cycle,
     * but we try anyway for robustness.
     */
    if (retry) {
        printf("x1_sdhci%d: voltage switch failed, "
               "resetting controller for 3.3V retry\n", sc->index);

        /* Full controller re-init (reset + power on + clock + quirks) */
        ret = sdhci_hw_init(sc);
        if (ret < 0)
            return ret;

        sleep_ms(10);
    }

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
     * We request 3.3V range, HCS (high capacity support), and optionally
     * S18R (switch to 1.8V request) for UHS-I support. */
    sc->uhs_1v8 = 0;
    {
        uint32 ocr_arg = MMC_OCR_HCS | MMC_OCR_3V3;
        if (request_1v8)
            ocr_arg |= MMC_OCR_S18R;

        int tries = 0;
        do {
            ret = sdhci_send_acmd(sc, SD_APP_OP_COND, ocr_arg,
                                  SDHCI_CMD_RESP_48, /* R3: no CRC/index */
                                  resp);
            if (ret < 0) {
                printf("x1_sdhci%d: ACMD41 failed\n", sc->index);
                return ret;
            }
            if (++tries > 100) {
                printf("x1_sdhci%d: ACMD41 timeout — card not ready\n",
                       sc->index);
                return -ETIMEDOUT;
            }
            sleep_ms(10);
        } while (!(resp[0] & MMC_OCR_BUSY));
    }

    /* Check if card is SDHC/SDXC */
    if (resp[0] & MMC_OCR_HCS)
        sc->card_type = CARD_TYPE_SDHC;

    printf("x1_sdhci%d: card type: %s\n", sc->index,
           sc->card_type == CARD_TYPE_SDHC ? "SDHC/SDXC" : "SD");

    /* S18A: card accepted 1.8V switch request — perform voltage switch.
     * This must happen before CMD2 (while card is in "ready" state).
     *
     * If the voltage switch fails, the card is in an undefined state
     * (SD spec 4.2.4): it received CMD11 and started switching but
     * the host couldn't complete the sequence.  We must re-initialize
     * from CMD0 without S18R to bring the card back to a known state. */
    if (request_1v8 && (resp[0] & MMC_OCR_S18R)) {
        printf("x1_sdhci%d: card supports 1.8V signaling (S18A)\n", sc->index);
        if (sdhci_sd_voltage_switch(sc) != 0) {
            printf("x1_sdhci%d: voltage switch failed, "
                   "re-initializing at 3.3V\n", sc->index);
            request_1v8 = 0;
            retry = 1;
            goto retry_without_1v8;
        }
    }

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

    /*
     * Raise clock from 400 kHz (identification) to 25 MHz (data transfer).
     * The SD spec requires raising the clock after CMD7 selects the card.
     * CMD6 data reads fail at 400 kHz on this controller.
     */
    sdhci_set_clock(sc, 25000000);

    /* Speed mode cascade: SDR104 (200 MHz) → HS (50 MHz) → Default (25 MHz)
     * SDR104 requires prior successful 1.8V voltage switch. */
    if (sc->uhs_1v8 && sdhci_sd_try_sdr104(sc) == 0) {
        printf("x1_sdhci%d: running at SDR104 ~200 MHz\n", sc->index);
    } else if (sdhci_sd_try_highspeed(sc) == 0) {
        sdhci_set_clock(sc, 50000000);
        printf("x1_sdhci%d: running at High Speed 50 MHz\n", sc->index);
    } else {
        sdhci_set_clock(sc, 25000000);
        printf("x1_sdhci%d: running at Default Speed 25 MHz\n", sc->index);
    }

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
            if (ret == 0 && sdhci_wait_xfer_done(sc) == 0) {
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
    }

    mutex_unlock(&sc->lock);

    bio->error = ret;
    bio_complete(bio);
    return ret;
}

/**
 * flush — ensure all written data reaches stable storage.
 *
 * For SD/SDHC cards, writes are synchronous (the data transfer completes
 * after the card exits busy state), so there is nothing to flush.
 *
 * For eMMC, the device may have a volatile write cache that must be flushed
 * via CMD6 (SWITCH) writing EXT_CSD byte 32 (FLUSH_CACHE = 1).  Since our
 * eMMC init does not currently enable the write cache (EXT_CSD[33] CACHE_CTRL),
 * the flush is effectively a no-op but we issue it anyway for correctness
 * if the device is eMMC.
 */
static int sdhci_flush(blkdev_t *blkdev)
{
    struct sdhci_softc *sc = container_of(blkdev, struct sdhci_softc, bdev);

    if (sc->card_type != CARD_TYPE_EMMC)
        return 0; /* SD/SDHC: no volatile cache */

    /* CMD6 (SWITCH): Access=Write Byte (0x03), Index=32 (FLUSH_CACHE),
     * Value=1 (trigger flush).  Response is R1b (busy). */
    mutex_lock(&sc->lock);
    uint32 resp[4];
    int ret = sdhci_send_cmd(sc, SD_SWITCH_FUNC,
                             (3u << 24) | (32u << 16) | (1u << 8),
                             SDHCI_CMD_RESP_48_BUSY | SDHCI_CMD_CRC |
                             SDHCI_CMD_INDEX,
                             resp);
    mutex_unlock(&sc->lock);
    return ret;
}

static blkdev_ops_t sdhci_blk_ops = {
    .open = sdhci_blk_open,
    .release = sdhci_blk_release,
    .submit_bio = sdhci_submit_bio,
    .flush = sdhci_flush,
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

    /* Probe for partition tables (GPT/MBR) on the raw device */
    gendisk_probe(&sc->bdev);

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
