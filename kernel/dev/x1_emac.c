/**
 * @file x1_emac.c
 * @brief SpacemiT X1 EMAC Ethernet MAC/DMA driver for xv6
 *
 * Supports dual EMAC instances on the Orange Pi RV2 (SpacemiT X1 SoC).
 * Each EMAC is connected to a YT8531 PHY over RGMII.
 *
 * DMA is non-coherent on this platform, so Zicbom cache management
 * operations (cbo.clean, cbo.inval) are used around DMA transfers.
 *
 * Key data flow:
 *   TX: net.c → netdev_ops.transmit → x1_emac_transmit → DMA
 *   RX: IRQ → x1_emac_intr → x1_emac_recv → net_rx()
 */

#include "compiler.h"
#include "types.h"
#include "string.h"
#include "param.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "printf.h"
#include "dev/net.h"
#include "dev/netdev.h"
#include "dev/x1_emac.h"
#include "dev/yt8531.h"
#include "dev/fdt.h"
#include "cache.h"
#include "trap.h"
#include "timer/timer.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "errno.h"

/* Debug logging — set to 1 to enable verbose network packet tracing */
#define EMAC_DEBUG 0

#if EMAC_DEBUG
static uint64 emac_tx_count = 0;
static uint64 emac_rx_count = 0;
static uint64 emac_rx_err_count = 0;
static uint64 emac_tx_full_count = 0;
static uint64 emac_isr_count = 0;
#define EMAC_DBG(fmt, ...) printf("emac: " fmt, ##__VA_ARGS__)
#else
#define EMAC_DBG(fmt, ...) do {} while(0)
#endif

/* ======================================================================
 * Per-instance driver state
 * ====================================================================== */

struct x1_emac_softc {
    volatile uint32 *regs;            /* EMAC MMIO base */

    /* APMU registers (identity-mapped virtual = physical) */
    volatile uint32 *ctrl_reg;        /* APMU ctrl register */
    volatile uint32 *dline_reg;       /* APMU delay-line register */

    /* PHY */
    int phy_addr;                     /* MDIO address of the PHY */
    struct phy_state phy;             /* current link state */

    /* Clock tuning */
    uint32 tx_phase;
    uint32 rx_phase;

    /* GPIO for PHY reset */
    int reset_gpio;

    /* TX ring */
    struct x1_tx_desc *tx_ring;
    struct mbuf *tx_mbufs[X1_EMAC_TX_RING_SIZE];
    uint32 tx_head;                   /* next to transmit (producer) */
    uint32 tx_tail;                   /* next to reclaim (consumer) */

    /* RX ring */
    struct x1_rx_desc *rx_ring;
    struct mbuf *rx_mbufs[X1_EMAC_RX_RING_SIZE];
    uint32 rx_head;                   /* next to receive (consumer) */

    /* Lock */
    spinlock_t lock;

    /* Netdev */
    struct netdev ndev;

    /* IRQ number (raw PLIC hwirq, before PLIC_IRQ offset) */
    int irq;

    /* Instance index (0 or 1) */
    int index;
};

#define MAX_EMAC_INSTANCES 2
static struct x1_emac_softc emac_sc[MAX_EMAC_INSTANCES];
static int emac_count = 0;

/* Forward declarations */
static void x1_emac_intr(int irq, void *data, device_t *dev);
static int x1_emac_transmit_op(struct netdev *ndev, struct mbuf *m);

static struct netdev_ops x1_emac_netdev_ops = {
    .transmit = x1_emac_transmit_op,
};

/* ======================================================================
 * Low-level MMIO helpers
 * ====================================================================== */

static inline uint32 sc_rd(struct x1_emac_softc *sc, uint32 off)
{
    return *(volatile uint32 *)((char *)sc->regs + off);
}

static inline void sc_wr(struct x1_emac_softc *sc, uint32 off, uint32 val)
{
    *(volatile uint32 *)((char *)sc->regs + off) = val;
}

/* ======================================================================
 * APMU / Clock / Delay-Line Configuration
 * ====================================================================== */

/**
 * Configure the APMU control register for RGMII mode and AXI single-ID.
 */
static void x1_emac_apmu_config(struct x1_emac_softc *sc)
{
    uint32 val;

    if (!sc->ctrl_reg) {
        printf("x1_emac%d: no ctrl_reg, skipping APMU config\n", sc->index);
        return;
    }

    val = *sc->ctrl_reg;

    /* Enable bus clock gate — required before any EMAC register access */
    val |= EMAC_BUS_CLK_EN;
    *sc->ctrl_reg = val;
    __sync_synchronize();

    /* Deassert reset (take EMAC out of reset) */
    val |= EMAC_RESET_DEASSERT;
    *sc->ctrl_reg = val;
    __sync_synchronize();
    scheduler_yield(); /* allow reset to propagate */

    /* Set RGMII interface mode + AXI single ID */
    val |= PHY_INTF_RGMII;
    val |= AXI_SINGLE_ID;
    *sc->ctrl_reg = val;
    __sync_synchronize();
}

/**
 * Program RGMII TX and RX delay-lines using the APMU dline register.
 * This is the CLK_TUNING_BY_DLINE method from the Linux driver.
 */
static void x1_emac_dline_config(struct x1_emac_softc *sc)
{
    uint32 val;

    if (!sc->dline_reg) {
        printf("x1_emac%d: no dline_reg, skipping delay-line config\n",
               sc->index);
        return;
    }

    val = *sc->dline_reg;

    /* TX delay line */
    val &= ~EMAC_TX_DLINE_CODE_MASK;
    val |= (sc->tx_phase << EMAC_TX_DLINE_CODE_OFFSET);
    val |= EMAC_TX_DLINE_EN;

    /* RX delay line */
    val &= ~EMAC_RX_DLINE_CODE_MASK;
    val |= (sc->rx_phase << EMAC_RX_DLINE_CODE_OFFSET);
    val |= EMAC_RX_DLINE_EN;

    *sc->dline_reg = val;
    __sync_synchronize();

    printf("x1_emac%d: delay-line tx=%d rx=%d\n",
           sc->index, sc->tx_phase, sc->rx_phase);
}

/* ======================================================================
 * DMA Reset
 * ====================================================================== */

static int x1_emac_dma_reset(struct x1_emac_softc *sc)
{
    /* Stop DMA */
    sc_wr(sc, DMA_CONTROL, 0);

    /* Assert software reset */
    sc_wr(sc, DMA_CONFIGURATION, DMA_CFG_SW_RESET);
    sleep_ms(10);
    sc_wr(sc, DMA_CONFIGURATION, 0);
    sleep_ms(10);

    return 0;
}

/* ======================================================================
 * MAC Initialisation (mirrors emac_init_hw in Linux)
 * ====================================================================== */

static void x1_emac_set_mac_addr(struct x1_emac_softc *sc, const uint8 *addr)
{
    sc_wr(sc, MAC_ADDRESS1_HIGH, ((uint32)addr[1] << 8) | addr[0]);
    sc_wr(sc, MAC_ADDRESS1_MED,  ((uint32)addr[3] << 8) | addr[2]);
    sc_wr(sc, MAC_ADDRESS1_LOW,  ((uint32)addr[5] << 8) | addr[4]);
}

static void x1_emac_init_hw(struct x1_emac_softc *sc)
{
    uint32 val;

    /* Disable TX and RX */
    sc_wr(sc, MAC_RECEIVE_CONTROL, 0);
    sc_wr(sc, MAC_TRANSMIT_CONTROL, 0);

    /* Reset MAC + statistic counters (matches U-Boot emac_reset_hw) */
    sc_wr(sc, MAC_GLOBAL_CONTROL, MAC_GC_RESET_RX_STATS | MAC_GC_RESET_TX_STATS);
    sc_wr(sc, MAC_GLOBAL_CONTROL, 0);

    /* Enable MAC address 1 filtering */
    sc_wr(sc, MAC_ADDRESS_CONTROL, MAC_AC_ADDR1_ENABLE);

    /* Zero the multicast hash table */
    sc_wr(sc, MAC_MULTICAST_HASH_TABLE1, 0);
    sc_wr(sc, MAC_MULTICAST_HASH_TABLE2, 0);
    sc_wr(sc, MAC_MULTICAST_HASH_TABLE3, 0);
    sc_wr(sc, MAC_MULTICAST_HASH_TABLE4, 0);

    /* TX FIFO almost full threshold */
    sc_wr(sc, MAC_TX_FIFO_ALMOST_FULL, 0x1f8);

    /* TX packet start threshold */
    sc_wr(sc, MAC_TX_PKT_START_THRESHOLD, X1_EMAC_DEFAULT_TX_THRESH);

    /* RX packet start threshold */
    sc_wr(sc, MAC_RX_PKT_START_THRESHOLD, X1_EMAC_DEFAULT_RX_THRESH);

    /* Flow control: decode enable */
    sc_wr(sc, MAC_FC_CONTROL, MAC_FC_DECODE_ENABLE);

    /* DMA reset */
    x1_emac_dma_reset(sc);

    /* DMA configuration: strict burst + 64-bit + 16-word burst length.
     * Matches the vendor Linux driver's register value (0x00060020).
     * dma-burst-len=5 in DT → bit 5 → BURST_16WORD. */
    val = DMA_CFG_STRICT_BURST | DMA_CFG_64BIT_MODE | DMA_CFG_BURST_16WORD;
    sc_wr(sc, DMA_CONFIGURATION, val);

    /* Disable all interrupts initially */
    sc_wr(sc, MAC_INTERRUPT_ENABLE, 0);
    sc_wr(sc, DMA_INTERRUPT_ENABLE, 0);
}

/**
 * Set MAC speed/duplex based on PHY link state.
 */
static void x1_emac_adjust_link(struct x1_emac_softc *sc)
{
    uint32 ctrl = sc_rd(sc, MAC_GLOBAL_CONTROL);

    ctrl &= ~MAC_GC_SPEED_MASK;
    switch (sc->phy.speed) {
    case PHY_SPEED_1000:
        ctrl |= MAC_GC_SPEED_1000M;
        break;
    case PHY_SPEED_100:
        ctrl |= MAC_GC_SPEED_100M;
        break;
    case PHY_SPEED_10:
    default:
        ctrl |= MAC_GC_SPEED_10M;
        break;
    }

    if (sc->phy.full_duplex)
        ctrl |= MAC_GC_FULL_DUPLEX;
    else
        ctrl &= ~MAC_GC_FULL_DUPLEX;

    sc_wr(sc, MAC_GLOBAL_CONTROL, ctrl);
}

/* ======================================================================
 * TX Ring Setup
 * ====================================================================== */

static int x1_emac_tx_ring_init(struct x1_emac_softc *sc)
{
    /* Allocate descriptor ring (needs to be page-aligned for DMA)
     * 64 entries × 64 bytes = 4096 = exactly 1 page */
    uint32 ring_size = X1_EMAC_TX_RING_SIZE * sizeof(struct x1_tx_desc);

    /* Use kalloc pages — identity mapped, so VA == PA */
    sc->tx_ring = kalloc();
    if (!sc->tx_ring)
        return -1;

    memset(sc->tx_ring, 0, ring_size);
    memset(sc->tx_mbufs, 0, sizeof(sc->tx_mbufs));
    sc->tx_head = 0;
    sc->tx_tail = 0;

    /* Set up chained descriptor pointers.
     * Each descriptor's buf_addr2 points to the next descriptor;
     * the last wraps back to the first. */
    for (int i = 0; i < X1_EMAC_TX_RING_SIZE; i++) {
        struct x1_tx_desc *d = &sc->tx_ring[i];
        int next = (i + 1) % X1_EMAC_TX_RING_SIZE;
        d->buf_addr2 = (uint32)(uint64)&sc->tx_ring[next];
        d->word1 = DESC_CHAINED;  /* SecondAddressChained */
    }

    /* Flush descriptors to memory */
    dma_cache_clean(sc->tx_ring, ring_size);

    return 0;
}

/* ======================================================================
 * RX Ring Setup
 * ====================================================================== */

static int x1_emac_rx_ring_init(struct x1_emac_softc *sc)
{
    /* 64 entries × 64 bytes = 4096 = exactly 1 page */
    uint32 ring_size = X1_EMAC_RX_RING_SIZE * sizeof(struct x1_rx_desc);

    sc->rx_ring = kalloc();
    if (!sc->rx_ring)
        return -1;

    memset(sc->rx_ring, 0, ring_size);
    memset(sc->rx_mbufs, 0, sizeof(sc->rx_mbufs));
    sc->rx_head = 0;

    /* Allocate mbufs and set up each descriptor in chained mode */
    for (int i = 0; i < X1_EMAC_RX_RING_SIZE; i++) {
        struct mbuf *m = mbufalloc(0);
        if (!m) {
            printf("x1_emac%d: failed to alloc rx mbuf %d\n", sc->index, i);
            return -1;
        }
        sc->rx_mbufs[i] = m;

        struct x1_rx_desc *d = &sc->rx_ring[i];
        int next = (i + 1) % X1_EMAC_RX_RING_SIZE;
        d->buf_addr = (uint32)(uint64)m->head;
        d->buf_addr2 = (uint32)(uint64)&sc->rx_ring[next];
        d->word1 = RX_DESC_BUF1_SIZE(MBUF_SIZE) | DESC_CHAINED;
        d->word0 = RX_DESC_OWN;  /* give to DMA */

        /* Flush the mbuf buffer so DMA can write into it */
        dma_cache_clean(m->head, MBUF_SIZE);
    }

    /* Flush all descriptors to memory */
    dma_cache_clean(sc->rx_ring, ring_size);

    return 0;
}

/* ======================================================================
 * Transmit
 * ====================================================================== */

static int x1_emac_transmit(struct x1_emac_softc *sc, struct mbuf *m)
{
    spin_lock(&sc->lock);

    uint32 idx = sc->tx_head;
    struct x1_tx_desc *d = &sc->tx_ring[idx];

    /* Invalidate descriptor to read current state from memory */
    dma_cache_inval(d, sizeof(*d));

    /* Check if descriptor is still owned by DMA */
    if (d->word0 & TX_DESC_OWN) {
#if EMAC_DEBUG
        emac_tx_full_count++;
        if (emac_tx_full_count <= 5 || (emac_tx_full_count % 100) == 0)
            EMAC_DBG("TX ring full (idx=%d, total_full=%lu)\n",
                     idx, emac_tx_full_count);
#endif
        spin_unlock(&sc->lock);
        return -1;  /* ring full */
    }

    /* Free any previously transmitted mbuf */
    if (sc->tx_mbufs[idx]) {
        mbuffree(sc->tx_mbufs[idx]);
        sc->tx_mbufs[idx] = 0;
    }

    /* Set up the descriptor — chained mode: buf_addr2 already points
     * to the next descriptor (set during ring init, preserved here). */
    d->buf_addr = (uint32)(uint64)m->head;
    d->word1 = TX_DESC_BUF1_SIZE(m->len) |
               TX_DESC_FIRST_SEG | TX_DESC_LAST_SEG | TX_DESC_IOC |
               DESC_CHAINED;

    sc->tx_mbufs[idx] = m;

    /* Flush the packet data */
    dma_cache_clean(m->head, m->len);

    /* Write barrier before setting OWN bit */
    __sync_synchronize();
    d->word0 = TX_DESC_OWN;

    /* Flush the descriptor */
    dma_cache_clean(d, sizeof(*d));

    /* Advance head */
    sc->tx_head = (idx + 1) % X1_EMAC_TX_RING_SIZE;

    /* Kick DMA transmit poll */
    sc_wr(sc, DMA_TRANSMIT_POLL_DEMAND, 0xFF);

#if EMAC_DEBUG
    emac_tx_count++;
    if (emac_tx_count <= 10 || (emac_tx_count % 500) == 0)
        EMAC_DBG("TX pkt #%lu len=%d idx=%d head=%d tail=%d\n",
                 emac_tx_count, m->len, idx, sc->tx_head, sc->tx_tail);
#endif

    spin_unlock(&sc->lock);
    return 0;
}

/**
 * Netdev ops transmit wrapper.
 */
static int x1_emac_transmit_op(struct netdev *ndev, struct mbuf *m)
{
    struct x1_emac_softc *sc = (struct x1_emac_softc *)ndev->priv;
    return x1_emac_transmit(sc, m);
}

/* ======================================================================
 * Receive
 * ====================================================================== */

/*
 * RX budget: limit how many packets we process per interrupt to avoid
 * holding the CPU in ISR context for too long and consuming too many
 * pbufs before the tcpip thread gets a chance to process them.
 */
#define X1_EMAC_RX_BUDGET 32

static void x1_emac_recv(struct x1_emac_softc *sc)
{
    int budget = X1_EMAC_RX_BUDGET;
#if EMAC_DEBUG
    int rx_this_burst = 0;
#endif

    while (budget-- > 0) {
        uint32 idx = sc->rx_head;
        struct x1_rx_desc *d = &sc->rx_ring[idx];

        /* Invalidate descriptor to read from memory */
        dma_cache_inval(d, sizeof(*d));

        /* Is this descriptor owned by the CPU? */
        if (d->word0 & RX_DESC_OWN)
            break;

        /* Check for errors */
        uint32 app_status = RX_DESC_APP_STATUS(d->word0);
        if (app_status & RX_FRAME_ERROR_MASK) {
#if EMAC_DEBUG
            emac_rx_err_count++;
            if (emac_rx_err_count <= 10 || (emac_rx_err_count % 100) == 0)
                EMAC_DBG("RX err idx=%d status=0x%x word0=0x%x (total=%lu)\n",
                         idx, app_status, d->word0, emac_rx_err_count);
#endif
            /* Error frame — recycle the buffer */
            goto recycle;
        }

        /* Get frame length (includes FCS) */
        uint32 frame_len = RX_DESC_FRAME_LEN(d->word0);
        if (frame_len < 14 || frame_len > X1_EMAC_MAX_FRAME_SIZE + X1_EMAC_FCS_SIZE) {
            goto recycle;
        }

        /* Strip FCS (4 bytes) */
        frame_len -= X1_EMAC_FCS_SIZE;

        /* Invalidate the received data from cache */
        struct mbuf *m = sc->rx_mbufs[idx];
        dma_cache_inval(m->head, frame_len);

        /* Set the mbuf length */
        m->len = frame_len;

#if EMAC_DEBUG
        rx_this_burst++;
        emac_rx_count++;
        if (emac_rx_count <= 10 || (emac_rx_count % 500) == 0)
            EMAC_DBG("RX pkt #%lu len=%d idx=%d\n",
                     emac_rx_count, frame_len, idx);
#endif

        /* Deliver to network stack */
        net_rx(m);

        /* Allocate a replacement mbuf */
        struct mbuf *newm = mbufalloc(0);
        if (!newm) {
            printf("x1_emac%d: rx mbuf alloc failed\n", sc->index);
            /* Reuse the old mbuf (we already gave it to net_rx, so
             * allocate or we'll have a dangling pointer).
             * In practice this shouldn't happen on xv6. */
            newm = mbufalloc(0);
            if (!newm)
                panic("x1_emac: out of mbufs");
        }
        sc->rx_mbufs[idx] = newm;
        dma_cache_clean(newm->head, MBUF_SIZE);

        /* Set up descriptor with new buffer — chained mode preserved */
        d->buf_addr = (uint32)(uint64)newm->head;
        d->word1 = RX_DESC_BUF1_SIZE(MBUF_SIZE) | DESC_CHAINED;
        /* buf_addr2 already contains next-descriptor pointer from init */
        __sync_synchronize();
        d->word0 = RX_DESC_OWN;
        dma_cache_clean(d, sizeof(*d));

        sc->rx_head = (idx + 1) % X1_EMAC_RX_RING_SIZE;
        continue;

recycle:
        /* Give descriptor back to DMA with existing buffer — chained */
        d->word1 = RX_DESC_BUF1_SIZE(MBUF_SIZE) | DESC_CHAINED;
        /* buf_addr2 (next-desc ptr) already set from init */
        __sync_synchronize();
        d->word0 = RX_DESC_OWN;
        dma_cache_clean(d, sizeof(*d));
        sc->rx_head = (idx + 1) % X1_EMAC_RX_RING_SIZE;
    }

#if EMAC_DEBUG
    if (rx_this_burst > 0 && (emac_rx_count <= 20 || (emac_rx_count % 500) == 0))
        EMAC_DBG("RX burst: %d pkts (total=%lu)\n", rx_this_burst, emac_rx_count);
#endif

    /*
     * If we hit the budget limit, there may be more packets pending.
     * Kick DMA poll to trigger another interrupt so we process the rest.
     */
    if (budget < 0) {
        EMAC_DBG("RX budget exhausted, re-polling\n");
        sc_wr(sc, DMA_RECEIVE_POLL_DEMAND, 0xFF);
    }
}

/* ======================================================================
 * TX Reclaim (clean completed TX descriptors)
 * ====================================================================== */

static void x1_emac_tx_reclaim(struct x1_emac_softc *sc)
{
    while (sc->tx_tail != sc->tx_head) {
        uint32 idx = sc->tx_tail;
        struct x1_tx_desc *d = &sc->tx_ring[idx];

        dma_cache_inval(d, sizeof(*d));
        if (d->word0 & TX_DESC_OWN)
            break; /* still owned by DMA */

        if (sc->tx_mbufs[idx]) {
            mbuffree(sc->tx_mbufs[idx]);
            sc->tx_mbufs[idx] = 0;
        }

        sc->tx_tail = (idx + 1) % X1_EMAC_TX_RING_SIZE;
    }
}

/* ======================================================================
 * Interrupt Handler
 * ====================================================================== */

static void x1_emac_intr(int irq, void *data, device_t *dev)
{
    struct x1_emac_softc *sc = (struct x1_emac_softc *)data;

    /* Read and acknowledge DMA status */
    uint32 status = sc_rd(sc, DMA_STATUS_IRQ);

    /* Acknowledge all bits by writing them back */
    sc_wr(sc, DMA_STATUS_IRQ, status);

#if EMAC_DEBUG
    emac_isr_count++;
    /* Log every ISR for the first 20, then every 500th */
    if (emac_isr_count <= 20 || (emac_isr_count % 500) == 0)
        EMAC_DBG("ISR #%lu status=0x%x%s%s%s%s%s\n",
                 emac_isr_count, status,
                 (status & DMA_IRQ_RX_DONE)         ? " RX" : "",
                 (status & DMA_IRQ_TX_DONE)         ? " TX" : "",
                 (status & DMA_IRQ_RX_DESC_UNAVAIL) ? " RX_UNAVAIL" : "",
                 (status & DMA_IRQ_TX_STOPPED)      ? " TX_STOP" : "",
                 (status & DMA_IRQ_RX_STOPPED)      ? " RX_STOP" : "");
#endif

    if (status & DMA_IRQ_RX_DONE)
        x1_emac_recv(sc);

    if (status & DMA_IRQ_TX_DONE)
        x1_emac_tx_reclaim(sc);

    if (status & DMA_IRQ_RX_DESC_UNAVAIL) {
        /* RX ring ran out of descriptors.  Kick RX DMA to resume. */
        EMAC_DBG("RX desc unavail, re-polling\n");
        sc_wr(sc, DMA_RECEIVE_POLL_DEMAND, 0xFF);
    }

    if (status & (DMA_IRQ_TX_STOPPED | DMA_IRQ_RX_STOPPED)) {
        printf("x1_emac%d: DMA stopped! status=0x%x\n", sc->index, status);
    }
}

/* ======================================================================
 * Start TX/RX DMA
 * ====================================================================== */

static void x1_emac_start(struct x1_emac_softc *sc)
{
    uint32 val;

    /* Set TX base address */
    sc_wr(sc, DMA_TRANSMIT_BASE_ADDRESS, (uint32)(uint64)sc->tx_ring);

    /* Enable TX */
    val = sc_rd(sc, MAC_TRANSMIT_CONTROL);
    val |= MAC_TC_TX_ENABLE | MAC_TC_TX_AUTO_RETRY;
    sc_wr(sc, MAC_TRANSMIT_CONTROL, val);

    /* Disable auto-poll (we use poll demand on transmit) */
    sc_wr(sc, DMA_TRANSMIT_AUTO_POLL_COUNTER, 0);

    /* Start TX DMA */
    val = sc_rd(sc, DMA_CONTROL);
    val |= DMA_CTRL_START_TX;
    sc_wr(sc, DMA_CONTROL, val);

    /* Set RX base address */
    sc_wr(sc, DMA_RECEIVE_BASE_ADDRESS, (uint32)(uint64)sc->rx_ring);

    /* Enable RX: receive + store-and-forward */
    val = sc_rd(sc, MAC_RECEIVE_CONTROL);
    val |= MAC_RC_RX_ENABLE | MAC_RC_STORE_FORWARD;
    sc_wr(sc, MAC_RECEIVE_CONTROL, val);

    /* Start RX DMA */
    val = sc_rd(sc, DMA_CONTROL);
    val |= DMA_CTRL_START_RX;
    sc_wr(sc, DMA_CONTROL, val);

    /* Enable DMA interrupts */
    val = DMA_IE_TX_DONE | DMA_IE_RX_DONE |
          DMA_IE_RX_DESC_UNAVAIL | DMA_IE_TX_STOPPED | DMA_IE_RX_STOPPED;
    sc_wr(sc, DMA_INTERRUPT_ENABLE, val);
}

/* ======================================================================
 * Instance Initialisation
 * ====================================================================== */

/**
 * x1_emac_init_one - Initialise a single EMAC instance.
 *
 * @idx:  Index into platform_info.emac[] (0 or 1)
 * Returns 0 on success, -1 on failure.
 */
static int x1_emac_init_one(int idx)
{
    struct x1_emac_softc *sc = &emac_sc[emac_count];

    memset(sc, 0, sizeof(*sc));
    spin_init(&sc->lock, "x1_emac");
    sc->index = idx;
    sc->irq = platform.emac[idx].irq;

    /* Map EMAC registers (already identity-mapped by vm.c) */
    sc->regs = (volatile uint32 *)(uint64)platform.emac[idx].base;
    printf("x1_emac%d: MMIO base 0x%lx, IRQ %d\n", idx,
           platform.emac[idx].base, platform.emac[idx].irq);

    /* Map APMU ctrl and dline registers */
    if (platform.emac[idx].apmu_base && platform.emac[idx].ctrl_reg) {
        sc->ctrl_reg = (volatile uint32 *)(uint64)(
            platform.emac[idx].apmu_base + platform.emac[idx].ctrl_reg);
        printf("x1_emac%d: ctrl_reg @ 0x%lx\n", idx,
               (uint64)platform.emac[idx].apmu_base + platform.emac[idx].ctrl_reg);
    }
    if (platform.emac[idx].apmu_base && platform.emac[idx].dline_reg) {
        sc->dline_reg = (volatile uint32 *)(uint64)(
            platform.emac[idx].apmu_base + platform.emac[idx].dline_reg);
        printf("x1_emac%d: dline_reg @ 0x%lx\n", idx,
               (uint64)platform.emac[idx].apmu_base + platform.emac[idx].dline_reg);
    }

    sc->tx_phase = platform.emac[idx].tx_phase;
    sc->rx_phase = platform.emac[idx].rx_phase;
    sc->reset_gpio = platform.emac[idx].reset_gpio;
    sc->phy_addr = -1; /* auto-detect */

    /* Configure APMU: RGMII mode + AXI single ID */
    x1_emac_apmu_config(sc);

    /* Configure RGMII delay lines */
    x1_emac_dline_config(sc);

    /* Init MAC hardware */
    x1_emac_init_hw(sc);

    /* Init PHY via MDIO */
    int phy = yt8531_init(sc->regs, sc->phy_addr);
    if (phy < 0) {
        printf("x1_emac%d: PHY init failed\n", idx);
        return -1;
    }
    sc->phy_addr = phy; /* use the auto-detected PHY address */

    /* Wait for auto-negotiation (up to 5 seconds) */
    if (yt8531_wait_autoneg(sc->regs, sc->phy_addr, &sc->phy, 5000) == 0) {
        printf("x1_emac%d: link up %dMbps %s-duplex\n",
               idx, sc->phy.speed,
               sc->phy.full_duplex ? "full" : "half");
    } else {
        printf("x1_emac%d: no link\n", idx);
        /* Continue anyway — link may come up later */
        sc->phy.speed = PHY_SPEED_1000;
        sc->phy.full_duplex = 1;
    }

    /* Set MAC speed/duplex to match PHY */
    x1_emac_adjust_link(sc);

    /* Set MAC address for this instance */
    /* Generate a locally-administered MAC based on instance index */
    uint8 mac[6] = {0x02, 0x00, 0x00, 0x00, 0x00, (uint8)(idx + 1)};
    x1_emac_set_mac_addr(sc, mac);

    /* Init TX ring */
    if (x1_emac_tx_ring_init(sc) < 0) {
        printf("x1_emac%d: TX ring init failed\n", idx);
        return -1;
    }

    /* Init RX ring */
    if (x1_emac_rx_ring_init(sc) < 0) {
        printf("x1_emac%d: RX ring init failed\n", idx);
        return -1;
    }

    /* Register IRQ handler */
    struct irq_desc emac_irq = {
        .handler = x1_emac_intr,
        .data = sc,
        .dev = NULL,
    };
    int ret = register_irq_handler(PLIC_IRQ(sc->irq), &emac_irq);
    if (ret != 0) {
        printf("x1_emac%d: failed to register IRQ %d\n", idx, sc->irq);
        return -1;
    }

    /* Start DMA */
    x1_emac_start(sc);

    /* Register with netdev layer */
    sc->ndev.name[0] = 'e';
    sc->ndev.name[1] = 't';
    sc->ndev.name[2] = 'h';
    sc->ndev.name[3] = '0' + idx;
    sc->ndev.name[4] = '\0';
    memmove(sc->ndev.mac, mac, 6);
    sc->ndev.mtu = 1500;
    sc->ndev.link_up = sc->phy.link_up;
    sc->ndev.speed = sc->phy.speed;
    sc->ndev.full_duplex = sc->phy.full_duplex;
    sc->ndev.ops = &x1_emac_netdev_ops;
    sc->ndev.priv = sc;
    netdev_register(&sc->ndev);

    printf("x1_emac%d: initialised as %s (MAC %x:%x:%x:%x:%x:%x)\n",
           idx, sc->ndev.name, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    emac_count++;
    return 0;
}

/* ======================================================================
 * Top-level init — spawns a kthread for post-init probing
 * ====================================================================== */

/**
 * Kernel thread entry point: probes and initialises all EMAC instances,
 * then enters a link-polling loop that runs for the lifetime of the system.
 *
 * Running in kthread context allows sleep_ms() / scheduler_yield()
 * instead of busy-waiting during PHY reset and auto-negotiation.
 */
static void x1_emac_kthread(uint64 arg1 __attribute__((unused)),
                            uint64 arg2 __attribute__((unused)))
{
    printf("x1_emac: found %d EMAC instance(s)\n", platform.emac_count);

    for (int i = 0; i < platform.emac_count && i < MAX_EMAC_INSTANCES; i++) {
        if (x1_emac_init_one(i) < 0)
            printf("x1_emac%d: init failed, skipping\n", i);
    }

    /* ---- Link-state polling loop ----
     * PHY link-change interrupts are unreliable on many SoCs, so we
     * poll the PHY status register every 2 seconds.  When the link
     * state changes we update the netdev and notify upper layers
     * (e.g. lwIP) via the link-change callback.
     */
    for (;;) {
        sleep_ms(2000);

        for (int i = 0; i < emac_count; i++) {
            struct x1_emac_softc *sc = &emac_sc[i];
            if (sc->phy_addr < 0)
                continue; /* PHY init failed for this instance */

            struct phy_state ps;
            if (yt8531_poll_link(sc->regs, sc->phy_addr, &ps) != 0)
                continue;

            int old_link = sc->ndev.link_up;
            int new_link = ps.link_up;

            if (old_link != new_link) {
                sc->phy = ps;
                sc->ndev.speed = ps.speed;
                sc->ndev.full_duplex = ps.full_duplex;

                if (new_link) {
                    x1_emac_adjust_link(sc);
                    printf("x1_emac%d: link up %dMbps %s-duplex\n",
                           sc->index, ps.speed,
                           ps.full_duplex ? "full" : "half");
                } else {
                    printf("x1_emac%d: link down\n", sc->index);
                }

                netdev_set_link(&sc->ndev, new_link);
            }
        }
    }
}

/**
 * x1_emac_init — schedule EMAC probing as a post-init kthread.
 *
 * Called from start_kernel.  The actual hardware probing, PHY init,
 * and auto-negotiation run in a dedicated kernel thread so that:
 *  1. The scheduler is already running → sleep_ms() works.
 *  2. Boot proceeds without blocking on slow PHY negotiation.
 */
void x1_emac_init(void)
{
    if (!platform.has_emac)
        return;

    struct thread *t = kthread_create("x1_emac", x1_emac_kthread,
                                      0, 0, KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("x1_emac: failed to create init kthread\n");
        return;
    }
    wakeup(t);
}
