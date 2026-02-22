/**
 * @file lwip_glue.c
 * @brief lwIP ↔ xv6 kernel integration
 *
 * Provides:
 *  1. A lwIP netif driver that bridges the xv6 netdev abstraction with
 *     lwIP’s pbuf-based packet processing.
 *  2. lwip_init_all() — top-level function to initialise the lwIP stack,
 *     register the network interface, configure IP, and start
 *     the tcpip thread.
 *
 * The NIC-specific TX/RX functions live in the driver (e1000.c, x1_emac.c,
 * etc.).  This file is driver-agnostic and uses the netdev API.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "dev/e1000_dev.h"
#include "dev/net.h"
#include "dev/netdev.h"
#include "dev/fdt.h"

#include "lwip/opt.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include "lwip/etharp.h"
#include "lwip/dhcp.h"
#include "lwip/pbuf.h"
#include "lwip/err.h"
#include "lwip/ip4_addr.h"
#include "netif/ethernet.h"

/* Debug logging — set to 1 to enable verbose network tracing */
#define LWIP_NET_DEBUG 0

#if LWIP_NET_DEBUG
static uint64 lwip_rx_count = 0;
static uint64 lwip_rx_drop_count = 0;
static uint64 lwip_rx_input_err = 0;
static uint64 lwip_tx_count = 0;
static uint64 lwip_tx_err_count = 0;
#define LWIP_NET_DBG(fmt, ...) printf("lwip_net: " fmt, ##__VA_ARGS__)
#else
#define LWIP_NET_DBG(fmt, ...) do {} while(0)
#endif

/* ========================================================================== */
/* Generic netif for lwIP (works with any NIC via the netdev abstraction)      */
/* ========================================================================== */

static struct netif xv6_netif;

/* Fallback MAC address (matches QEMU E1000 default) */
static const uint8 fallback_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

/* --------------------------------------------------------------------------
 * TX path: lwIP → network device
 * Called from the lwIP stack when a packet needs to be transmitted.
 * Converts a pbuf chain into an mbuf and sends via the netdev abstraction.
 * -------------------------------------------------------------------------- */
static err_t
xv6_netif_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    struct mbuf *m;

    struct netdev *ndev = netdev_get_default();
    if (ndev == NULL) {
        LWIP_NET_DBG("TX: no netdev!\n");
        return ERR_IF;
    }

    if (p->tot_len > MBUF_SIZE) {
        printf("lwip: packet too large (%d > %d)\n", p->tot_len, MBUF_SIZE);
        return ERR_MEM;
    }

    m = mbufalloc(0);
    if (m == NULL) {
        LWIP_NET_DBG("TX: mbuf alloc failed\n");
        return ERR_MEM;
    }

    /* Copy (possibly chained) pbuf into contiguous mbuf */
    pbuf_copy_partial(p, mbufput(m, p->tot_len), p->tot_len, 0);

    if (ndev->ops->transmit(ndev, m) != 0) {
#if LWIP_NET_DEBUG
        lwip_tx_err_count++;
        if (lwip_tx_err_count <= 10 || (lwip_tx_err_count % 100) == 0)
            LWIP_NET_DBG("TX FAILED len=%d (total_err=%lu)\n",
                         p->tot_len, lwip_tx_err_count);
#endif
        mbuffree(m);
        return ERR_IF;
    }

#if LWIP_NET_DEBUG
    lwip_tx_count++;
    if (lwip_tx_count <= 10 || (lwip_tx_count % 500) == 0)
        LWIP_NET_DBG("TX ok #%lu len=%d\n", lwip_tx_count, p->tot_len);
#endif
    return ERR_OK;
}

/* --------------------------------------------------------------------------
 * netif init callback
 * -------------------------------------------------------------------------- */
static err_t
xv6_netif_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->linkoutput = xv6_netif_linkoutput;
    netif->output     = etharp_output;
    netif->mtu        = 1500;
    netif->hwaddr_len = 6;
    /* Use MAC from registered netdev, fall back to hardcoded default */
    struct netdev *ndev = netdev_get_default();
    if (ndev)
        memmove(netif->hwaddr, ndev->mac, 6);
    else
        memmove(netif->hwaddr, fallback_mac, 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET | NETIF_FLAG_IGMP;
    return ERR_OK;
}

/* --------------------------------------------------------------------------
 * RX path: NIC driver → lwIP
 * Called from the NIC driver ISR (e1000_recv / x1_emac_recv) for each
 * received packet. Allocates a pbuf, copies the mbuf payload, and feeds
 * the pbuf into the lwIP stack.
 * -------------------------------------------------------------------------- */
void net_rx(struct mbuf *m)
{
    struct pbuf *p;
    static int drop_count = 0;

    if (m == NULL || m->len == 0) {
        if (m)
            mbuffree(m);
        return;
    }

    p = pbuf_alloc(PBUF_RAW, m->len, PBUF_POOL);
    if (p == NULL) {
        mbuffree(m);
        drop_count++;
#if LWIP_NET_DEBUG
        lwip_rx_drop_count++;
#endif
        if (drop_count % 100 == 1)
            printf("net_rx: pbuf pool exhausted, dropped %d packets\n",
                   drop_count);
        return;
    }

    /* Copy received data into pbuf (handles chained pbufs) */
    pbuf_take(p, m->head, m->len);
    mbuffree(m);

#if LWIP_NET_DEBUG
    lwip_rx_count++;
    if (lwip_rx_count <= 10 || (lwip_rx_count % 500) == 0)
        LWIP_NET_DBG("RX #%lu len=%d -> tcpip_input\n",
                     lwip_rx_count, p->tot_len);
#endif

    /* Feed to lwIP — ethernet_input() handles ARP / IP demux */
    err_t err = xv6_netif.input(p, &xv6_netif);
    if (err != ERR_OK) {
#if LWIP_NET_DEBUG
        lwip_rx_input_err++;
        LWIP_NET_DBG("RX input error %d (total=%lu) — likely mbox full\n",
                     err, lwip_rx_input_err);
#endif
        pbuf_free(p);
    }
}

/* --------------------------------------------------------------------------
 * tcpip_init done callback
 * -------------------------------------------------------------------------- */
static volatile int __lwip_tcpip_ready = 0;

static void __tcpip_init_done(void *arg)
{
    (void)arg;
    __atomic_store_n(&__lwip_tcpip_ready, 1, __ATOMIC_RELEASE);
    printf("lwip: tcpip thread started\n");
}

/* --------------------------------------------------------------------------
 * Link-change callback: called by netdev_set_link() when the driver
 * detects a physical link state change.
 * -------------------------------------------------------------------------- */

#define GARP_COUNT       3   /* number of gratuitous ARPs on link-up */
#define GARP_INTERVAL_MS 500 /* ms between gratuitous ARPs */

static void __netdev_link_change(struct netdev *dev, int link_up)
{
    (void)dev;
    if (link_up) {
        netif_set_link_up(&xv6_netif);
        printf("lwip: link up (%s)\n", dev->name);

        /* Send a burst of gratuitous ARPs so that all neighbours learn
         * our MAC immediately, even if they missed the first one.      */
        for (int i = 0; i < GARP_COUNT; i++) {
            etharp_gratuitous(&xv6_netif);
            if (i < GARP_COUNT - 1)
                sleep_ms(GARP_INTERVAL_MS);
        }
    } else {
        netif_set_link_down(&xv6_netif);
        printf("lwip: link down (%s)\n", dev->name);
    }
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/**
 * __lwip_kthread — Kernel thread that initialises the lwIP network stack.
 *
 * Waits for a network device to be registered (the NIC driver kthread may
 * still be running PHY autonegotiation), then brings up the lwIP stack with
 * a platform-appropriate static IP:
 *
 *   Orange Pi (has_emac):  192.168.0.201 / 255.255.255.0 / 192.168.0.1
 *   QEMU SLIRP (default):  10.0.2.15    / 255.255.255.0 / 10.0.2.2
 *
 * After the netif is up, starts services that depend on lwIP (telnetd,
 * gdbstub).
 */
static void __lwip_kthread(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;
    ip4_addr_t ipaddr, netmask, gw;

    /* ---- Wait for a NIC to be registered ---- */
    printf("lwip: waiting for network device...\n");
    while (netdev_get_default() == NULL) {
        sleep_ms(100);
    }
    printf("lwip: network device detected\n");

    printf("lwip: initialising network stack (lwIP %s)\n", LWIP_VERSION_STRING);

    /* Start the tcpip thread (calls lwip_init internally) */
    tcpip_init(__tcpip_init_done, NULL);

    /* Wait for the tcpip thread to be ready — must yield so it can run
     * (especially important on single-CPU configurations). */
    while (!__atomic_load_n(&__lwip_tcpip_ready, __ATOMIC_ACQUIRE)) {
        scheduler_yield();
    }

    /* Configure static IP based on platform */
    if (platform.has_emac) {
        /* Orange Pi / SpacemiT X1 — local network */
        IP4_ADDR(&ipaddr,  192, 168, 0, 201);
        IP4_ADDR(&netmask, 255, 255, 255, 0);
        IP4_ADDR(&gw,      192, 168, 0, 1);
    } else {
        /* QEMU user-mode networking (SLIRP) */
        IP4_ADDR(&ipaddr,  10, 0, 2, 15);
        IP4_ADDR(&netmask, 255, 255, 255, 0);
        IP4_ADDR(&gw,      10, 0, 2, 2);
    }

    netif_add(&xv6_netif,
#if LWIP_IPV4
              &ipaddr, &netmask, &gw,
#endif
              NULL,                  /* state */
              xv6_netif_init,        /* init callback */
              tcpip_input);          /* input function */

    netif_set_default(&xv6_netif);
    netif_set_up(&xv6_netif);

    /* Set initial link state from the driver and register for changes */
    struct netdev *ndev = netdev_get_default();
    if (ndev) {
        netdev_set_link_callback(ndev, __netdev_link_change);
        if (ndev->link_up) {
            netif_set_link_up(&xv6_netif);
            /* Announce our IP on the network */
            for (int i = 0; i < GARP_COUNT; i++) {
                etharp_gratuitous(&xv6_netif);
                if (i < GARP_COUNT - 1)
                    sleep_ms(GARP_INTERVAL_MS);
            }
        }
    }

    printf("lwip: netif up — IP %d.%d.%d.%d\n",
           ip4_addr1_16(netif_ip4_addr(&xv6_netif)),
           ip4_addr2_16(netif_ip4_addr(&xv6_netif)),
           ip4_addr3_16(netif_ip4_addr(&xv6_netif)),
           ip4_addr4_16(netif_ip4_addr(&xv6_netif)));

    /* Start services that depend on the network stack */
    extern void telnetd_init(void);
    telnetd_init();
    extern void tftpd_init(void);
    tftpd_init();
    extern void httpd_daemon_init(void);
    httpd_daemon_init();
    extern void iperfd_init(void);
    iperfd_init();
    extern void sntpd_init(void);
    sntpd_init();
    extern void netbiosd_init(void);
    netbiosd_init();
    extern void mdnsd_init(void);
    mdnsd_init();
    extern void gdbstub_init(void);
    gdbstub_init();
}

/**
 * lwip_net_init — Spawn the lwIP initialisation kthread.
 *
 * Called from start_kernel_post_init(). The actual initialisation happens
 * asynchronously in __lwip_kthread so it can wait for the NIC driver
 * (which may itself be running as a kthread doing PHY bring-up).
 */
void lwip_net_init(void)
{
    struct thread *t = kthread_create("lwip", __lwip_kthread, 0, 0,
                                      KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("lwip: failed to create init kthread\n");
        return;
    }
    wakeup(t);
}
