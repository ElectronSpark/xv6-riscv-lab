/**
 * @file x1_sdhci.h
 * @brief SpacemiT X1 SDHCI register definitions
 *
 * Standard SD Host Controller Interface (SDHCI) registers plus
 * vendor-specific (KY X1) extensions.
 *
 * Three instances on the SpacemiT X1 SoC:
 *   SDH0 — SD card slot (4-bit, card detect GPIO 80)
 *   SDH1 — SDIO (WiFi)
 *   SDH2 — eMMC (8-bit, HS400)
 *
 * MMIO base addresses, IRQs, and clock-controller (APMU/APBC) base
 * addresses are obtained from the device tree at runtime via FDT parsing.
 *
 * APMU per-instance offsets (hardware-fixed within the APMU register space):
 *   SDH0: APMU base + 0x54
 *   SDH1: APMU base + 0x58
 *   SDH2: APMU base + 0xE0
 *
 * Clock/reset bits in APMU_SDHx_CLK_RES_CTRL:
 *   BIT(0)  — AXI bus reset deassert
 *   BIT(1)  — per-instance reset deassert
 *   BIT(3)  — AXI bus clock enable
 *   BIT(4)  — IO (functional) clock enable
 */
#ifndef __KERNEL_DEV_X1_SDHCI_H
#define __KERNEL_DEV_X1_SDHCI_H

#include "types.h"

/* ======================================================================
 * APMU Clock/Reset Control bits (for SDH ctrl registers)
 * ====================================================================== */

#define SDH_APMU_AXI_RST        (1 << 0)   /* AXI bus reset deassert */
#define SDH_APMU_RST            (1 << 1)   /* per-instance reset deassert */
#define SDH_APMU_AXI_CLK_EN    (1 << 3)   /* AXI bus clock enable (shared, SDH0 reg only) */
#define SDH_APMU_IO_CLK_EN     (1 << 4)   /* IO (functional) clock enable */
#define SDH_APMU_MUX_SHIFT      5          /* clock source mux [7:5] */
#define SDH_APMU_MUX_MASK       (0x7 << 5) /* 3-bit mux field */
#define SDH_APMU_DIV_SHIFT      8          /* clock divider [10:8] */
#define SDH_APMU_DIV_MASK       (0x7 << 8) /* 3-bit divider field */
#define SDH_APMU_CLK_FC        (1 << 11)  /* frequency change trigger (self-clearing) */

/* Clock source mux values (bits [7:5], index into parent PLL array) */
#define SDH_MUX_PLL1_D6_409M    0  /* pll1_d6_409p6: 409.6 MHz */
#define SDH_MUX_PLL1_D4_614M    1  /* pll1_d4_614p4: 614.4 MHz */
#define SDH_MUX_PLL1_D11_223M   4  /* pll1_d11_223p4: 223.4 MHz */
#define SDH_MUX_PLL1_D13_189M   5  /* pll1_d13_189: 189 MHz */
#define SDH_MUX_PLL1_D23_106M   6  /* pll1_d23_106p8: 106.8 MHz */

/* AIB clock register offset within APBC region (required by SDH0).
 * APBC base address is obtained from the clock-controller DT node. */
#define APBC_AIB_CLK_RST        0x3C
#define APBC_AIB_CLK_EN         0x3   /* bits [1:0] = 0x3 to enable */

/* ======================================================================
 * Standard SDHCI Register Offsets (SD Host Controller Spec v3.0)
 *
 * All registers are at the base address of each SDH instance.
 * ====================================================================== */

/* 32-bit System Address / SDMA Address */
#define SDHCI_DMA_ADDRESS       0x00

/* 16-bit Block Size */
#define SDHCI_BLOCK_SIZE        0x04
#define  SDHCI_MAKE_BLKSZ(dma_boundary, blksz) \
    (((dma_boundary) << 12) | ((blksz) & 0x0FFF))

/* 16-bit Block Count */
#define SDHCI_BLOCK_COUNT       0x06

/* 32-bit Argument */
#define SDHCI_ARGUMENT          0x08

/* 16-bit Transfer Mode */
#define SDHCI_TRANSFER_MODE     0x0C
#define  SDHCI_TRNS_DMA         (1 << 0)
#define  SDHCI_TRNS_BLK_CNT_EN  (1 << 1)
#define  SDHCI_TRNS_AUTO_CMD12  (1 << 2)
#define  SDHCI_TRNS_AUTO_CMD23  (1 << 3)
#define  SDHCI_TRNS_READ        (1 << 4)
#define  SDHCI_TRNS_MULTI       (1 << 5)

/* 16-bit Command */
#define SDHCI_COMMAND           0x0E
#define  SDHCI_CMD_RESP_NONE    0x00
#define  SDHCI_CMD_RESP_136     0x01  /* R2 */
#define  SDHCI_CMD_RESP_48      0x02  /* R1, R3, R5, R6, R7 */
#define  SDHCI_CMD_RESP_48_BUSY 0x03  /* R1b, R5b */
#define  SDHCI_CMD_CRC          (1 << 3)
#define  SDHCI_CMD_INDEX        (1 << 4)
#define  SDHCI_CMD_DATA         (1 << 5)
#define  SDHCI_CMD_TYPE_NORMAL  (0 << 6)
#define  SDHCI_CMD_TYPE_SUSPEND (1 << 6)
#define  SDHCI_CMD_TYPE_RESUME  (2 << 6)
#define  SDHCI_CMD_TYPE_ABORT   (3 << 6)
#define  SDHCI_MAKE_CMD(idx, flags) (((idx) << 8) | (flags))
#define  SDHCI_GET_CMD(cmd)     (((cmd) >> 8) & 0x3F)

/* 128-bit (4x32) Response Registers */
#define SDHCI_RESPONSE          0x10  /* 0x10..0x1F */

/* 32-bit Buffer Data Port (PIO) */
#define SDHCI_BUFFER            0x20

/* 32-bit Present State */
#define SDHCI_PRESENT_STATE     0x24
#define  SDHCI_CMD_INHIBIT      (1 << 0)
#define  SDHCI_DATA_INHIBIT     (1 << 1)
#define  SDHCI_DAT_ACTIVE       (1 << 2)
#define  SDHCI_DOING_WRITE      (1 << 8)
#define  SDHCI_DOING_READ       (1 << 9)
#define  SDHCI_SPACE_AVAILABLE  (1 << 10) /* buffer write ready */
#define  SDHCI_DATA_AVAILABLE   (1 << 11) /* buffer read ready  */
#define  SDHCI_CARD_INSERTED    (1 << 16)
#define  SDHCI_CARD_STABLE      (1 << 17)
#define  SDHCI_CARD_DETECT_PIN  (1 << 18)
#define  SDHCI_WRITE_PROTECT    (1 << 19)
#define  SDHCI_DATA_0_LVL_MASK  (1 << 20)
#define  SDHCI_DATA_1_LVL_MASK  (1 << 21)
#define  SDHCI_DATA_2_LVL_MASK  (1 << 22)
#define  SDHCI_DATA_3_LVL_MASK  (1 << 23)

/* 8-bit Host Control 1 */
#define SDHCI_HOST_CONTROL      0x28
#define  SDHCI_CTRL_LED         (1 << 0)
#define  SDHCI_CTRL_4BITBUS     (1 << 1)
#define  SDHCI_CTRL_HISPD       (1 << 2)
#define  SDHCI_CTRL_DMA_MASK    0x18
#define  SDHCI_CTRL_SDMA        0x00
#define  SDHCI_CTRL_ADMA1       0x08
#define  SDHCI_CTRL_ADMA32      0x10
#define  SDHCI_CTRL_ADMA64      0x18
#define  SDHCI_CTRL_8BITBUS     (1 << 5)
#define  SDHCI_CTRL_CD_TEST_INS (1 << 6)
#define  SDHCI_CTRL_CD_TEST     (1 << 7)

/* 8-bit Power Control */
#define SDHCI_POWER_CONTROL     0x29
#define  SDHCI_POWER_ON         (1 << 0)
#define  SDHCI_POWER_180        0x0A
#define  SDHCI_POWER_300        0x0C
#define  SDHCI_POWER_330        0x0E

/* 8-bit Block Gap Control */
#define SDHCI_BLOCK_GAP_CONTROL 0x2A

/* 8-bit Wakeup Control */
#define SDHCI_WAKE_UP_CONTROL   0x2B

/* 16-bit Clock Control */
#define SDHCI_CLOCK_CONTROL     0x2C
#define  SDHCI_DIVIDER_SHIFT    8
#define  SDHCI_DIVIDER_HI_SHIFT 6
#define  SDHCI_DIV_MASK         0xFF
#define  SDHCI_DIV_HI_MASK      0x300
#define  SDHCI_CLOCK_INT_EN     (1 << 0)
#define  SDHCI_CLOCK_INT_STABLE (1 << 1)
#define  SDHCI_CLOCK_CARD_EN    (1 << 2)
#define  SDHCI_PROG_CLOCK_MODE  (1 << 5)

/* 8-bit Timeout Control */
#define SDHCI_TIMEOUT_CONTROL   0x2E
#define  SDHCI_TIMEOUT_DEFAULT  0x0E  /* TMCLK * 2^27 */

/* 8-bit Software Reset */
#define SDHCI_SOFTWARE_RESET    0x2F
#define  SDHCI_RESET_ALL        0x01
#define  SDHCI_RESET_CMD        0x02
#define  SDHCI_RESET_DATA       0x04

/* 16-bit Normal Interrupt Status */
#define SDHCI_INT_STATUS        0x30
/* 16-bit Error Interrupt Status */
#define SDHCI_ERR_INT_STATUS    0x32

/* 16-bit Normal Interrupt Status Enable */
#define SDHCI_INT_ENABLE        0x34
/* 16-bit Error Interrupt Status Enable */
#define SDHCI_ERR_INT_ENABLE    0x36

/* 16-bit Normal Interrupt Signal Enable */
#define SDHCI_SIGNAL_ENABLE     0x38
/* 16-bit Error Interrupt Signal Enable */
#define SDHCI_ERR_SIGNAL_ENABLE 0x3A

/* Interrupt bits (shared between status, enable, and signal registers) */
#define  SDHCI_INT_RESPONSE     (1 << 0)   /* Command Complete */
#define  SDHCI_INT_DATA_END     (1 << 1)   /* Transfer Complete */
#define  SDHCI_INT_BLK_GAP      (1 << 2)
#define  SDHCI_INT_DMA_END      (1 << 3)
#define  SDHCI_INT_SPACE_AVAIL  (1 << 4)   /* Buffer Write Ready */
#define  SDHCI_INT_DATA_AVAIL   (1 << 5)   /* Buffer Read Ready */
#define  SDHCI_INT_CARD_INSERT  (1 << 6)
#define  SDHCI_INT_CARD_REMOVE  (1 << 7)
#define  SDHCI_INT_CARD_INT     (1 << 8)
#define  SDHCI_INT_RETUNE       (1 << 12)
#define  SDHCI_INT_CRC          (1 << 15)  /* part of error summary */
#define  SDHCI_INT_ERROR        (1 << 15)  /* Error interrupt */

/* Error interrupt bits (in ERR_INT_STATUS / ERR_INT_ENABLE) */
#define  SDHCI_ERR_TIMEOUT      (1 << 0)
#define  SDHCI_ERR_CRC          (1 << 1)
#define  SDHCI_ERR_END_BIT      (1 << 2)
#define  SDHCI_ERR_INDEX        (1 << 3)
#define  SDHCI_ERR_DATA_TIMEOUT (1 << 4)
#define  SDHCI_ERR_DATA_CRC     (1 << 5)
#define  SDHCI_ERR_DATA_END_BIT (1 << 6)
#define  SDHCI_ERR_BUS_POWER    (1 << 7)
#define  SDHCI_ERR_AUTO_CMD     (1 << 8)
#define  SDHCI_ERR_ADMA         (1 << 9)

/* All error bits for convenience */
#define  SDHCI_ERR_ALL          0x03FF

/* All normal interrupt bits we care about (PIO mode) */
#define  SDHCI_INT_ALL_MASK     0xFFFF

/* 16-bit Auto CMD Error Status */
#define SDHCI_AUTO_CMD_STATUS   0x3C

/* 16-bit Host Control 2 */
#define SDHCI_HOST_CONTROL2     0x3E
#define  SDHCI_CTRL_UHS_MASK    0x0007
#define  SDHCI_CTRL_UHS_SDR12   0x0000
#define  SDHCI_CTRL_UHS_SDR25   0x0001
#define  SDHCI_CTRL_UHS_SDR50   0x0002
#define  SDHCI_CTRL_UHS_SDR104  0x0003
#define  SDHCI_CTRL_UHS_DDR50   0x0004
#define  SDHCI_CTRL_HS400       0x0005
#define  SDHCI_CTRL_VDD_180     (1 << 3)
#define  SDHCI_CTRL_DRV_TYPE_B  (0 << 4)
#define  SDHCI_CTRL_DRV_TYPE_A  (1 << 4)
#define  SDHCI_CTRL_DRV_TYPE_C  (2 << 4)
#define  SDHCI_CTRL_DRV_TYPE_D  (3 << 4)
#define  SDHCI_CTRL_EXEC_TUNING (1 << 6)
#define  SDHCI_CTRL_TUNED_CLK   (1 << 7)
#define  SDHCI_CTRL_PRESET_VAL  (1 << 15)

/* 32-bit Capabilities 0 */
#define SDHCI_CAPABILITIES      0x40
#define  SDHCI_CAP_TIMEOUT_CLK_MASK  0x3F
#define  SDHCI_CAP_TIMEOUT_CLK_MHZ  (1 << 7)
#define  SDHCI_CAP_BASE_CLK_MASK    0xFF
#define  SDHCI_CAP_BASE_CLK_SHIFT   8
#define  SDHCI_CAP_MAX_BLK_MASK     0x30000
#define  SDHCI_CAP_MAX_BLK_SHIFT    16
#define  SDHCI_CAP_8BIT             (1 << 18)
#define  SDHCI_CAP_ADMA2            (1 << 19)
#define  SDHCI_CAP_ADMA1            (1 << 20)
#define  SDHCI_CAP_HISPD            (1 << 21)
#define  SDHCI_CAP_SDMA             (1 << 22)
#define  SDHCI_CAP_SUSPEND_RESUME   (1 << 23)
#define  SDHCI_CAP_VOLTAGE_33       (1 << 24)
#define  SDHCI_CAP_VOLTAGE_30       (1 << 25)
#define  SDHCI_CAP_VOLTAGE_18       (1 << 26)
#define  SDHCI_CAP_64BIT_SYSTEM_BUS (1 << 28)
#define  SDHCI_CAP_ASYNC_INT        (1 << 29)

/* 32-bit Capabilities 1 */
#define SDHCI_CAPABILITIES_1    0x44
/* Host Version */
#define SDHCI_HOST_VERSION      0xFE

/* 32-bit Max Current */
#define SDHCI_MAX_CURRENT       0x48

/* ======================================================================
 * KY X1 Vendor-Specific Register Extensions (offset 0x100+)
 * ====================================================================== */

#define SDHC_OP_EXT_REG         0x108
#define  OVRRD_CLK_OEN          0x0800
#define  FORCE_CLK_ON           0x1000

#define SDHC_LEGACY_CTRL_REG    0x10C
#define  GEN_PAD_CLK_ON         0x0040

#define SDHC_MMC_CTRL_REG       0x114
#define  MISC_INT_EN            0x0002
#define  MISC_INT               0x0004
#define  ENHANCE_STROBE_EN      0x0100
#define  MMC_HS400_MODE         0x0200
#define  MMC_HS200_MODE         0x0400
#define  MMC_CARD_MODE          0x1000

#define SDHC_RX_CFG_REG         0x118

#define SDHC_TX_CFG_REG         0x11C
#define  TX_INT_CLK_SEL         0x40000000
#define  TX_MUX_SEL             0x80000000

#define SDHC_DLINE_CTRL_REG     0x130
#define  DLINE_PU               0x01

#define SDHC_DLINE_CFG_REG      0x134

#define SDHC_PHY_CTRL_REG       0x160
#define  PHY_FUNC_EN            0x0001
#define  PHY_PLL_LOCK           0x0002
#define  HOST_LEGACY_MODE       0x80000000

#define SDHC_PHY_FUNC_REG       0x164
#define  PHY_TEST_EN            0x0080

#define SDHC_PHY_DLLCFG         0x168
#define  DLL_ENABLE             0x80000000

#define SDHC_PHY_DLLCFG1        0x16C

#define SDHC_PHY_DLLSTS         0x170
#define  DLL_LOCK_STATE         0x01

#define SDHC_PHY_DLLSTS1        0x174

#define SDHC_PHY_PADCFG_REG     0x178
#define  RX_BIAS_CTRL_SHIFT     5
#define  PHY_DRIVE_SEL_SHIFT    0
#define  PHY_DRIVE_SEL_MASK     0x7
#define  PHY_DRIVE_SEL_DEFAULT  0x4

/* ======================================================================
 * SD/MMC Command Indices
 * ====================================================================== */

#define MMC_GO_IDLE_STATE       0
#define MMC_SEND_OP_COND        1
#define MMC_ALL_SEND_CID        2
#define MMC_SET_RELATIVE_ADDR   3
#define MMC_SET_DSR             4
#define SD_SWITCH_FUNC          6
#define MMC_SELECT_CARD         7
#define MMC_SEND_EXT_CSD        8
#define SD_SEND_IF_COND         8
#define MMC_SEND_CSD            9
#define MMC_SEND_CID            10
#define MMC_STOP_TRANSMISSION   12
#define MMC_SEND_STATUS         13
#define MMC_SET_BLOCKLEN        16
#define MMC_READ_SINGLE_BLOCK   17
#define MMC_READ_MULTIPLE_BLOCK 18
#define MMC_WRITE_BLOCK         24
#define MMC_WRITE_MULTIPLE_BLOCK 25
#define SD_APP_OP_COND          41  /* ACMD41 */
#define SD_APP_SET_BUS_WIDTH    6   /* ACMD6 */
#define SD_APP_SEND_SCR         51  /* ACMD51 */
#define MMC_APP_CMD             55

/* SD/MMC OCR (Operating Conditions Register) bits */
#define MMC_OCR_HCS             (1 << 30)  /* Host Capacity Support (SDHC/SDXC) */
#define MMC_OCR_BUSY            (1 << 31)  /* Card power-up done */
#define MMC_OCR_3V3             (0xFF8000) /* 2.7-3.6V range */
#define MMC_OCR_S18R            (1 << 24)  /* Switching to 1.8V request */

/* Card Status bits (R1 response) */
#define MMC_STATUS_READY        (1 << 8)
#define MMC_STATUS_CUR_STATE    (0xF << 9)

/* RCA (Relative Card Address) */
#define MMC_RCA_DEFAULT         0x0001  /* default RCA for single-card setup */

/* Card states */
#define MMC_STATE_IDLE          0
#define MMC_STATE_READY         1
#define MMC_STATE_IDENT         2
#define MMC_STATE_STBY          3
#define MMC_STATE_TRAN          4
#define MMC_STATE_DATA          5

/* ======================================================================
 * Driver constants
 * ====================================================================== */

#define SDHCI_MAX_INSTANCES     3
#define SDHCI_TIMEOUT_MS        1000  /* generic command timeout in ms */
#define SDHCI_CLK_TIMEOUT_MS    200   /* clock stabilization timeout */

/* Maximum transfer size per PIO operation */
#define SDHCI_BLOCK_SIZE_VAL    512

/* SD command indices for UHS-I */
#define SD_VOLTAGE_SWITCH       11  /* CMD11: voltage switch to 1.8V */
#define SD_SEND_TUNING_BLOCK    19  /* CMD19: send tuning block */

/* Present State DAT[3:0] level mask (bits 23:20) */
#define SDHCI_DATA_LVL_MASK     (0xF << 20)

/* Delay line register fields (X1-specific) */
#define SDHC_DLINE_CFG_DELAY_MASK   0xFF   /* bits [7:0]: delay code */

/* ======================================================================
 * AIB (Always-on I/O Block) SD I/O Voltage Control
 *
 * The SpacemiT X1 SoC uses an external AIB register to physically
 * switch the SD slot I/O voltage between 3.3V and 1.8V.  The SDHCI
 * CTRL_VDD_180 bit only controls signaling level inside the controller;
 * the AIB register gates the actual voltage rail (vqmmc / LDO_1).
 *
 * Access to the AIB register requires unlocking via the APBC security
 * key registers (ASFAR/ASSAR).  Values from Orange Pi RV2 DTS and
 * the Linux sdhci-of-k1x.c driver.
 * ====================================================================== */

#define AIB_MMC1_IO_REG         0xD401E81C  /* AIB SD I/O voltage control */
#define APBC_ASFAR_REG          0xD4015050  /* APBC security key 1 */
#define APBC_ASSAR_REG          0xD4015054  /* APBC security key 2 */

#define AKEY_ASFAR              0xBABA      /* unlock key for ASFAR */
#define AKEY_ASSAR              0xEB10      /* unlock key for ASSAR */
#define MMC1_IO_V18EN           0x04        /* bit 2: enable 1.8V I/O */

#endif /* __KERNEL_DEV_X1_SDHCI_H */
