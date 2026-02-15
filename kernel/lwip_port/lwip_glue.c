/**
 * @file lwip_glue.c
 * @brief lwIP ↔ xv6 kernel integration
 *
 * Provides:
 *  1. A lwIP netif driver that bridges the xv6 E1000 NIC with lwIP's
 *     pbuf-based packet processing (replacing the old net.c ethernet/IP/ARP).
 *  2. lwip_init_all() — top-level function to initialise the lwIP stack,
 *     register the E1000 network interface, configure IP, and start
 *     the tcpip thread.
 *
 * The e1000 HW transmit/receive functions (e1000_transmit, e1000_recv) remain
 * in e1000.c. This file replaces net.c's protocol processing with lwIP.
 */

#include "types.h"
#include "param.h"
#include "riscv.h"
#include "lock/spinlock.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "proc/sched.h"
#include "dev/e1000_dev.h"
#include "dev/net.h"

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

/* ========================================================================== */
/* E1000 netif for lwIP                                                       */
/* ========================================================================== */

static struct netif e1000_netif;

/* MAC address (must match what e1000_init programs into the HW filter) */
static const uint8 e1000_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};

/* --------------------------------------------------------------------------
 * TX path: lwIP → E1000 hardware
 * Called from the lwIP stack when a packet needs to be transmitted.
 * Converts a pbuf chain into an mbuf and calls e1000_transmit().
 * -------------------------------------------------------------------------- */
static err_t
e1000_netif_linkoutput(struct netif *netif, struct pbuf *p)
{
    (void)netif;
    struct mbuf *m;

    if (p->tot_len > MBUF_SIZE) {
        printf("lwip: packet too large (%d > %d)\n", p->tot_len, MBUF_SIZE);
        return ERR_MEM;
    }

    m = mbufalloc(0);
    if (m == NULL)
        return ERR_MEM;

    /* Copy (possibly chained) pbuf into contiguous mbuf */
    pbuf_copy_partial(p, mbufput(m, p->tot_len), p->tot_len, 0);

    if (e1000_transmit(m) != 0) {
        mbuffree(m);
        return ERR_IF;
    }

    return ERR_OK;
}

/* --------------------------------------------------------------------------
 * netif init callback
 * -------------------------------------------------------------------------- */
static err_t
e1000_netif_init(struct netif *netif)
{
    netif->name[0] = 'e';
    netif->name[1] = 'n';
    netif->linkoutput = e1000_netif_linkoutput;
    netif->output     = etharp_output;
    netif->mtu        = 1500;
    netif->hwaddr_len = 6;
    memmove(netif->hwaddr, e1000_mac, 6);
    netif->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
                   NETIF_FLAG_ETHERNET | NETIF_FLAG_LINK_UP;
    return ERR_OK;
}

/* --------------------------------------------------------------------------
 * RX path: E1000 hardware → lwIP
 * Called from e1000_recv() (ISR context via e1000_intr) for each received
 * packet. Replaces the old net_rx() entry point.
 *
 * We allocate a pbuf, copy the mbuf payload into it, free the mbuf, and
 * feed the pbuf into the lwIP stack.
 * -------------------------------------------------------------------------- */
void net_rx(struct mbuf *m)
{
    struct pbuf *p;

    if (m == NULL || m->len == 0) {
        if (m)
            mbuffree(m);
        return;
    }

    p = pbuf_alloc(PBUF_RAW, m->len, PBUF_POOL);
    if (p == NULL) {
        mbuffree(m);
        return;
    }

    /* Copy received data into pbuf (handles chained pbufs) */
    pbuf_take(p, m->head, m->len);
    mbuffree(m);

    /* Feed to lwIP — ethernet_input() handles ARP / IP demux */
    if (e1000_netif.input(p, &e1000_netif) != ERR_OK) {
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

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/**
 * lwip_net_init — Initialise the entire lwIP network stack.
 *
 * Must be called from a kernel thread context (not from ISR or early boot
 * before the scheduler is running). Typically called from
 * start_kernel_post_init().
 *
 * Sets up:
 *  - lwIP internal state (memp, pbuf, etc.)
 *  - The tcpip thread (runs lwIP timers and processes incoming packets)
 *  - The E1000 netif with a static IP matching QEMU's SLIRP gateway:
 *      IP  = 10.0.2.15
 *      GW  = 10.0.2.2
 *      Mask = 255.255.255.0
 */
void lwip_net_init(void)
{
    ip4_addr_t ipaddr, netmask, gw;

    printf("lwip: initialising network stack (lwIP %s)\n", LWIP_VERSION_STRING);

    /* Start the tcpip thread (calls lwip_init internally) */
    tcpip_init(__tcpip_init_done, NULL);

    /* Wait for the tcpip thread to be ready — must yield so it can run
     * (especially important on single-CPU configurations). */
    while (!__atomic_load_n(&__lwip_tcpip_ready, __ATOMIC_ACQUIRE)) {
        scheduler_yield();
    }

    /* Configure static IP suitable for QEMU user-mode networking (SLIRP) */
    IP4_ADDR(&ipaddr,  10, 0, 2, 15);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw,      10, 0, 2, 2);

    netif_add(&e1000_netif,
#if LWIP_IPV4
              &ipaddr, &netmask, &gw,
#endif
              NULL,                  /* state */
              e1000_netif_init,      /* init callback */
              tcpip_input);          /* input function */

    netif_set_default(&e1000_netif);
    netif_set_up(&e1000_netif);
    netif_set_link_up(&e1000_netif);

    printf("lwip: E1000 netif up — IP %d.%d.%d.%d\n",
           ip4_addr1_16(netif_ip4_addr(&e1000_netif)),
           ip4_addr2_16(netif_ip4_addr(&e1000_netif)),
           ip4_addr3_16(netif_ip4_addr(&e1000_netif)),
           ip4_addr4_16(netif_ip4_addr(&e1000_netif)));
}
