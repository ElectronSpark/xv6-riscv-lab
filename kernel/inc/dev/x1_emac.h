/**
 * @file x1_emac.h
 * @brief SpacemiT X1 EMAC (Ethernet MAC) register definitions
 *
 * Register map and descriptor structures for the KY X1 EMAC IP.
 * Derived from the vendor Linux driver (drivers/net/ethernet/ky/x1-emac.h).
 *
 * The EMAC has two register banks:
 *   - DMA engine registers at offset 0x0000
 *   - MAC core registers at offset 0x0100
 *
 * Two instances exist on the Orange Pi RV2:
 *   eth0 @ 0xCAC80000, IRQ 131
 *   eth1 @ 0xCAC81000, IRQ 133
 */
#ifndef __KERNEL_DEV_X1_EMAC_H
#define __KERNEL_DEV_X1_EMAC_H

#include "types.h"

/* ======================================================================
 * APMU Control Register bits (ctrl_reg, relative to apmu_base)
 * ====================================================================== */

#define EMAC_BUS_CLK_EN             (1 << 0)  /* AXI bus clock gate enable */
#define EMAC_RESET_DEASSERT         (1 << 1)  /* 1=out of reset, 0=in reset */
#define PHY_INTF_RGMII              (1 << 2)
#define REF_CLK_SEL                 (1 << 3)  /* RMII: 0=ext PHY, 1=SoC */
#define FUNC_CLK_SEL                (1 << 4)  /* 0=208M, 1=312M */
#define RGMII_TX_CLK_SEL            (1 << 8)  /* RGMII: 0=from rx, 1=from SoC */
#define AXI_SINGLE_ID               (1 << 13)

/* Delay-line register bits (dline_reg, relative to apmu_base) */
#define EMAC_RX_DLINE_EN            (1 << 0)
#define EMAC_RX_DLINE_CODE_OFFSET   8
#define EMAC_RX_DLINE_CODE_MASK     0x0000FF00
#define EMAC_TX_DLINE_EN            (1 << 16)
#define EMAC_TX_DLINE_CODE_OFFSET   24
#define EMAC_TX_DLINE_CODE_MASK     0xFF000000

/* ======================================================================
 * DMA Register Set (offset from EMAC base)
 * ====================================================================== */

#define DMA_CONFIGURATION                   0x0000
#define DMA_CONTROL                         0x0004
#define DMA_STATUS_IRQ                      0x0008
#define DMA_INTERRUPT_ENABLE                0x000C
#define DMA_TRANSMIT_AUTO_POLL_COUNTER      0x0010
#define DMA_TRANSMIT_POLL_DEMAND            0x0014
#define DMA_RECEIVE_POLL_DEMAND             0x0018
#define DMA_TRANSMIT_BASE_ADDRESS           0x001C
#define DMA_RECEIVE_BASE_ADDRESS            0x0020
#define DMA_MISSED_FRAME_COUNTER            0x0024
#define DMA_STOP_FLUSH_COUNTER              0x0028
#define DMA_RECEIVE_IRQ_MITIGATION_CTRL     0x002C
#define DMA_CURRENT_TX_DESC_PTR             0x0030
#define DMA_CURRENT_TX_BUF_PTR              0x0034
#define DMA_CURRENT_RX_DESC_PTR             0x0038
#define DMA_CURRENT_RX_BUF_PTR              0x003C

/* DMA_CONFIGURATION bits */
#define DMA_CFG_SW_RESET            (1 << 0)
#define DMA_CFG_BURST_1WORD         (1 << 1)
#define DMA_CFG_BURST_2WORD         (1 << 2)
#define DMA_CFG_BURST_4WORD         (1 << 3)
#define DMA_CFG_BURST_8WORD         (1 << 4)
#define DMA_CFG_BURST_16WORD        (1 << 5)
#define DMA_CFG_BURST_32WORD        (1 << 6)
#define DMA_CFG_BURST_64WORD        (1 << 7)
#define DMA_CFG_WAIT_FOR_DONE       (1 << 16)
#define DMA_CFG_STRICT_BURST        (1 << 17)
#define DMA_CFG_64BIT_MODE          (1 << 18)

/* DMA_CONTROL bits */
#define DMA_CTRL_START_TX           (1 << 0)
#define DMA_CTRL_START_RX           (1 << 1)

/* DMA_STATUS_IRQ bits */
#define DMA_IRQ_TX_DONE             (1 << 0)
#define DMA_IRQ_TX_DESC_UNAVAIL     (1 << 1)
#define DMA_IRQ_TX_STOPPED          (1 << 2)
#define DMA_IRQ_RX_DONE             (1 << 4)
#define DMA_IRQ_RX_DESC_UNAVAIL     (1 << 5)
#define DMA_IRQ_RX_STOPPED          (1 << 6)
#define DMA_IRQ_RX_MISSED           (1 << 7)
#define DMA_IRQ_MAC                 (1 << 8)

/* DMA_INTERRUPT_ENABLE bits */
#define DMA_IE_TX_DONE              (1 << 0)
#define DMA_IE_TX_DESC_UNAVAIL      (1 << 1)
#define DMA_IE_TX_STOPPED           (1 << 2)
#define DMA_IE_RX_DONE              (1 << 4)
#define DMA_IE_RX_DESC_UNAVAIL      (1 << 5)
#define DMA_IE_RX_STOPPED           (1 << 6)
#define DMA_IE_RX_MISSED            (1 << 7)

/* ======================================================================
 * MAC Register Set (offset from EMAC base)
 * ====================================================================== */

#define MAC_GLOBAL_CONTROL                  0x0100
#define MAC_TRANSMIT_CONTROL                0x0104
#define MAC_RECEIVE_CONTROL                 0x0108
#define MAC_MAXIMUM_FRAME_SIZE              0x010C
#define MAC_TRANSMIT_JABBER_SIZE            0x0110
#define MAC_RECEIVE_JABBER_SIZE             0x0114
#define MAC_ADDRESS_CONTROL                 0x0118
#define MAC_MDIO_CLK_DIV                    0x011C
#define MAC_ADDRESS1_HIGH                   0x0120
#define MAC_ADDRESS1_MED                    0x0124
#define MAC_ADDRESS1_LOW                    0x0128
#define MAC_MULTICAST_HASH_TABLE1           0x0150
#define MAC_MULTICAST_HASH_TABLE2           0x0154
#define MAC_MULTICAST_HASH_TABLE3           0x0158
#define MAC_MULTICAST_HASH_TABLE4           0x015C
#define MAC_FC_CONTROL                      0x0160
#define MAC_MDIO_CONTROL                    0x01A0
#define MAC_MDIO_DATA                       0x01A4
#define MAC_TX_FIFO_ALMOST_FULL             0x01C0
#define MAC_TX_PKT_START_THRESHOLD          0x01C4
#define MAC_RX_PKT_START_THRESHOLD          0x01C8
#define MAC_STATUS_IRQ                      0x01E0
#define MAC_INTERRUPT_ENABLE                0x01E4

/* MAC_GLOBAL_CONTROL bits */
#define MAC_GC_SPEED_MASK           0x03
#define MAC_GC_SPEED_10M            0x00
#define MAC_GC_SPEED_100M           0x01
#define MAC_GC_SPEED_1000M          0x02
#define MAC_GC_FULL_DUPLEX          (1 << 2)
#define MAC_GC_RESET_RX_STATS       (1 << 3)
#define MAC_GC_RESET_TX_STATS       (1 << 4)

/* MAC_TRANSMIT_CONTROL bits */
#define MAC_TC_TX_ENABLE            (1 << 0)
#define MAC_TC_TX_AUTO_RETRY        (1 << 3)

/* MAC_RECEIVE_CONTROL bits */
#define MAC_RC_RX_ENABLE            (1 << 0)
#define MAC_RC_STRIP_FCS            (1 << 2)
#define MAC_RC_STORE_FORWARD        (1 << 3)

/* MAC_ADDRESS_CONTROL bits */
#define MAC_AC_ADDR1_ENABLE         (1 << 0)
#define MAC_AC_PROMISCUOUS          (1 << 8)

/* MAC_FC_CONTROL bits */
#define MAC_FC_DECODE_ENABLE        (1 << 0)

/* MAC_MDIO_CONTROL bits */
#define MDIO_PHY_ADDR_MASK          0x1F
#define MDIO_REG_ADDR_SHIFT         5
#define MDIO_REG_ADDR_MASK          (0x1F << 5)
#define MDIO_READ_WRITE             (1 << 10)  /* 1=read, 0=write */
#define MDIO_START_TRANS            (1 << 15)

/* ======================================================================
 * RX Frame Status bits (in descriptor word 0, bits [28:14])
 * ====================================================================== */

#define RX_FRAME_ALIGN_ERR          (1 << 0)
#define RX_FRAME_RUNT               (1 << 1)
#define RX_FRAME_CRC_ERR            (1 << 6)
#define RX_FRAME_MAX_LEN_ERR        (1 << 7)
#define RX_FRAME_JABBER_ERR         (1 << 8)
#define RX_FRAME_LENGTH_ERR         (1 << 9)
#define RX_FRAME_ERROR_MASK         (RX_FRAME_ALIGN_ERR | RX_FRAME_RUNT | \
                                     RX_FRAME_CRC_ERR | RX_FRAME_MAX_LEN_ERR | \
                                     RX_FRAME_JABBER_ERR | RX_FRAME_LENGTH_ERR)

/* ======================================================================
 * DMA Descriptor Structures
 *
 * Each hardware descriptor is 4 x uint32 = 16 bytes.  However, on
 * non-coherent DMA platforms the Zicbom cache management instructions
 * (cbo.clean, cbo.inval) operate on full 64-byte cache lines.  When
 * multiple 16-byte descriptors share a cache line, writing back one
 * descriptor clobbers DMA's updates to its neighbours, causing lost
 * packets.
 *
 * Fix: pad each descriptor to 64 bytes (== CBOM_BLOCK_SIZE) so every
 * descriptor occupies its own cache line.  We use chained descriptor
 * mode (SecondAddressChained bit) so that buf_addr2 points to the next
 * descriptor, allowing the DMA engine to stride 64 bytes between them.
 * ====================================================================== */

#define DESC_CHAINED            (1U << 25)  /* SecondAddressChained in word1 */

/**
 * Receive Descriptor — padded to 64 bytes (one cache line).
 *
 * Word 0: FramePacketLength[13:0] | ApplicationStatus[28:14] |
 *         LastDescriptor[29] | FirstDescriptor[30] | OWN[31]
 * Word 1: BufferSize1[11:0] | BufferSize2[23:12] | rsvd[24] |
 *         SecondAddressChained[25] | EndRing[26] | rsvd[29:27] |
 *         rx_timestamp[30] | ptp_pkt[31]
 * Word 2: BufferAddr1[31:0]
 * Word 3: BufferAddr2[31:0]  (next descriptor address in chained mode)
 * Words 4-15: padding to fill 64-byte cache line
 */
struct x1_rx_desc {
    uint32 word0;     /* frame length + status + OWN */
    uint32 word1;     /* buffer size + flags (incl. DESC_CHAINED) */
    uint32 buf_addr;  /* buffer physical address (32-bit) */
    uint32 buf_addr2; /* next descriptor address (chained mode) */
    uint32 __pad[12]; /* pad to 64 bytes */
};

/* RX descriptor word0 field accessors */
#define RX_DESC_FRAME_LEN(w0)       ((w0) & 0x3FFF)
#define RX_DESC_APP_STATUS(w0)      (((w0) >> 14) & 0x7FFF)
#define RX_DESC_LAST_DESC           (1U << 29)
#define RX_DESC_FIRST_DESC          (1U << 30)
#define RX_DESC_OWN                 (1U << 31)

/* RX descriptor word1 field setters */
#define RX_DESC_BUF1_SIZE(sz)       ((sz) & 0xFFF)
#define RX_DESC_END_RING            (1U << 26)

/**
 * Transmit Descriptor — padded to 64 bytes (one cache line).
 *
 * Word 0: FramePacketStatus[29:0] | tx_timestamp[30] | OWN[31]
 * Word 1: BufferSize1[11:0] | BufferSize2[23:12] |
 *         ForceEOPError[24] | SecondAddressChained[25] | EndRing[26] |
 *         DisablePadding[27] | AddCRCDisable[28] |
 *         FirstSegment[29] | LastSegment[30] | InterruptOnCompletion[31]
 * Word 2: BufferAddr1[31:0]
 * Word 3: BufferAddr2[31:0]  (next descriptor address in chained mode)
 * Words 4-15: padding to fill 64-byte cache line
 */
struct x1_tx_desc {
    uint32 word0;     /* status + OWN */
    uint32 word1;     /* size + flags (incl. DESC_CHAINED) */
    uint32 buf_addr;  /* buffer physical address */
    uint32 buf_addr2; /* next descriptor address (chained mode) */
    uint32 __pad[12]; /* pad to 64 bytes */
};

/* TX descriptor word0 */
#define TX_DESC_OWN                 (1U << 31)

/* TX descriptor word1 field setters */
#define TX_DESC_BUF1_SIZE(sz)       ((sz) & 0xFFF)
#define TX_DESC_END_RING            (1U << 26)
#define TX_DESC_FIRST_SEG           (1U << 29)
#define TX_DESC_LAST_SEG            (1U << 30)
#define TX_DESC_IOC                 (1U << 31) /* interrupt on completion */

/* ======================================================================
 * Driver constants
 * ====================================================================== */

#define X1_EMAC_DEFAULT_BUF_SIZE    2048
/*
 * Ring sizes: 64 entries × 64 bytes/descriptor = 4096 bytes = 1 page.
 * (Previously 256 × 16 = 4096.)  64 entries is still plenty for GbE.
 */
#define X1_EMAC_TX_RING_SIZE        64
#define X1_EMAC_RX_RING_SIZE        64
#define X1_EMAC_DEFAULT_TX_THRESH   0x5EE  /* store-forward mode (matches U-Boot) */
#define X1_EMAC_DEFAULT_RX_THRESH   12
#define X1_EMAC_MAX_FRAME_SIZE      1518
#define X1_EMAC_FCS_SIZE            4

/* MDIO timeouts */
#define MDIO_TIMEOUT_US             10000
#define MDIO_POLL_INTERVAL_US       100

/* DMA reset timeout */
#define DMA_RESET_TIMEOUT_US        10000

/* Default APMU base */
#define PMUA_BASE_REG               0xD4282800

/* YT8531 PHY ID (as seen in dmesg) */
#define YT8531_PHY_ID               0x4f51e91b

#endif /* __KERNEL_DEV_X1_EMAC_H */
