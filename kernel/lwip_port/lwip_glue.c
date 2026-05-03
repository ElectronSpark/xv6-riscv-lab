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
#include "lwip/dns.h"
#include "netif/ethernet.h"

#include "dev/netconf.h"
#include "dev/cdev.h"
#include "cmdline.h"
#include <mm/vm.h>
#include "errno.h"

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
    if (lwip_tx_count <= 200 || (lwip_tx_count % 500) == 0)
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
 *
 * The netif input function is set by netif_add() to tcpip_input().  Until
 * netif_add() has been called, xv6_netif.input is NULL and we must drop
 * packets silently to avoid calling into an uninitialised stack.
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

    /* Drop packets that arrive before the lwIP netif is ready */
    if (xv6_netif.input == NULL) {
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
    if (lwip_rx_count <= 200 || (lwip_rx_count % 500) == 0)
        LWIP_NET_DBG("RX #%lu len=%d -> tcpip_input\n",
                     lwip_rx_count, p->tot_len);
#endif

    /* Feed to lwIP — ethernet_input() handles ARP / IP demux */
    err_t err = xv6_netif.input(p, &xv6_netif);
    if (err != ERR_OK) {
#if LWIP_NET_DEBUG
        lwip_rx_input_err++;
        printf("net_rx: input error %d (total=%lu) — likely mbox full\n",
               err, lwip_rx_input_err);
#endif
        pbuf_free(p);
    }
}

/* --------------------------------------------------------------------------
 * tcpip_init done callback
 * -------------------------------------------------------------------------- */
static volatile int __lwip_tcpip_ready = 0;

/*
 * Set to 1 after netif_add() has been called (i.e. xv6_netif is valid
 * and the lwIP mbox is usable).  Guards the link-change callback and
 * the net_rx() input path against calls that arrive before lwIP is
 * ready (e.g. from the EMAC PHY polling kthread when the link comes up
 * during early boot).
 */
static volatile int __lwip_netif_ready = 0;

static void __tcpip_init_done(void *arg)
{
    (void)arg;
    __atomic_store_n(&__lwip_tcpip_ready, 1, __ATOMIC_RELEASE);
    printf("lwip: tcpip thread started\n");
}

/* --------------------------------------------------------------------------
 * Link-change callback: called by netdev_set_link() when the driver
 * detects a physical link state change.
 *
 * Guard: skip if the lwIP netif hasn't been set up yet.  The EMAC PHY
 * kthread may detect link-up before the lwIP kthread has called
 * netif_add() / tcpip_init().
 * -------------------------------------------------------------------------- */

#define GARP_COUNT       3   /* number of gratuitous ARPs on link-up */
#define GARP_INTERVAL_MS 500 /* ms between gratuitous ARPs */

static void __netdev_link_change(struct netdev *dev, int link_up)
{
    (void)dev;
    if (!__atomic_load_n(&__lwip_netif_ready, __ATOMIC_ACQUIRE))
        return; /* too early — netif not yet added */

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
/* Network configuration — syscall interface                                  */
/* ========================================================================== */

/**
 * Userspace provides network configuration via /dev/netconf (ioctl or write).
 * The lwIP kthread waits for configuration (or a timeout) before applying
 * the IP configuration and starting network services.
 *
 * The legacy SYS_netconf syscall is also supported but /dev/netconf is
 * the preferred interface since it requires no custom syscall plumbing.
 */
static volatile int __netconf_ready = 0;     /* set to 1 when config arrives */
static struct netconf_req __netconf_req;      /* filled by config path       */
static volatile int __lwip_initialized = 0;   /* set after kthread setup     */

/* Interval for polling __netconf_ready (ms) */
#define NETCONF_POLL_INTERVAL_MS  100
#define NETCONF_APPLY_TIMEOUT_MS  35000

static int __netconf_wait_boot_ready(void)
{
    int elapsed = 0;

    while (!__atomic_load_n(&__lwip_initialized, __ATOMIC_ACQUIRE)) {
        if (elapsed >= NETCONF_APPLY_TIMEOUT_MS)
            return -ETIMEDOUT;
        sleep_ms(NETCONF_POLL_INTERVAL_MS);
        elapsed += NETCONF_POLL_INTERVAL_MS;
    }

    return 0;
}

static int __netconf_wait_runtime_ready(int dhcp)
{
    int elapsed = 0;

    while (elapsed < NETCONF_APPLY_TIMEOUT_MS) {
        if (netif_is_up(&xv6_netif) && netif_is_link_up(&xv6_netif) &&
            !ip4_addr_isany_val(*netif_ip4_addr(&xv6_netif))) {
            if (!dhcp)
                return 0;

            const ip_addr_t *dns = dns_getserver(0);
            if (dns != NULL && !ip_addr_isany(dns))
                return 0;
        }

        sleep_ms(NETCONF_POLL_INTERVAL_MS);
        elapsed += NETCONF_POLL_INTERVAL_MS;
    }

    return -ETIMEDOUT;
}

/**
 * __apply_netconf — shared implementation for applying network configuration.
 *
 * Called from both the syscall handler and the /dev/netconf cdev paths.
 * @req: validated struct netconf_req (kernel-side copy).
 * Returns 0 on success, -errno on failure.
 */
static int __apply_netconf(struct netconf_req *req)
{
    /* Validate */
    if (req->mode != NETCONF_MODE_DHCP && req->mode != NETCONF_MODE_STATIC)
        return -EINVAL;
    req->hostname[NETCONF_HOSTNAME_MAX - 1] = '\0';

    /* Runtime reconfiguration (after kthread has finished boot setup) */
    if (__atomic_load_n(&__lwip_initialized, __ATOMIC_ACQUIRE)) {
        int wait_ret;

        if (req->mode == NETCONF_MODE_STATIC) {
            ip4_addr_t ip, mask, gw;
            ip.addr   = req->ip;
            mask.addr = req->netmask;
            gw.addr   = req->gateway;

            /* If DHCP was running, stop it first */
            dhcp_stop(&xv6_netif);
            netif_set_addr(&xv6_netif, &ip, &mask, &gw);
            printf("lwip: netconf runtime static %d.%d.%d.%d/%d.%d.%d.%d gw %d.%d.%d.%d\n",
                   ip4_addr1_16(&ip), ip4_addr2_16(&ip),
                   ip4_addr3_16(&ip), ip4_addr4_16(&ip),
                   ip4_addr1_16(&mask), ip4_addr2_16(&mask),
                   ip4_addr3_16(&mask), ip4_addr4_16(&mask),
                   ip4_addr1_16(&gw), ip4_addr2_16(&gw),
                   ip4_addr3_16(&gw), ip4_addr4_16(&gw));
        } else {
            /* DHCP mode — restart DHCP discovery */
            dhcp_release_and_stop(&xv6_netif);
            ip4_addr_t zero;
            IP4_ADDR(&zero, 0, 0, 0, 0);
            netif_set_addr(&xv6_netif, &zero, &zero, &zero);
            dhcp_start(&xv6_netif);
            printf("lwip: netconf runtime DHCP restart\n");
        }

        /* Update DNS if provided */
        if (req->dns != 0) {
            ip_addr_t dns_addr;
            dns_addr.addr = req->dns;
            dns_setserver(0, &dns_addr);
        }

        /* Update hostname if provided */
        if (req->hostname[0])
            netif_set_hostname(&xv6_netif, req->hostname);

        wait_ret = __netconf_wait_runtime_ready(req->mode == NETCONF_MODE_DHCP);
        if (wait_ret < 0)
            return wait_ret;

        return 0;
    }

    /* Boot-time: store and signal the lwIP kthread */
    __netconf_req = *req;
    __atomic_store_n(&__netconf_ready, 1, __ATOMIC_RELEASE);

    return __netconf_wait_boot_ready();
}

/* Syscall argument helpers (defined in arch/.../irq/syscall.c) */
extern void argaddr(int n, uint64 *ip);

/**
 * sys_netconf — SYS_netconf syscall handler (legacy path).
 *
 * /dev/netconf is preferred for new code.  This syscall is kept for
 * compatibility with the xv6-native userlib (init.c).
 */
uint64 sys_netconf(void)
{
    uint64 uaddr;
    argaddr(0, &uaddr);
    if (uaddr == 0)
        return (uint64)-EINVAL;

    struct netconf_req req;
    if (vm_copyin(current->vm, (char *)&req, uaddr, sizeof(req)) < 0)
        return (uint64)-EFAULT;

    return (uint64)__apply_netconf(&req);
}

/* ========================================================================== */
/* /dev/netconf — character device for network configuration                  */
/* ========================================================================== */

static int netconf_cdev_open(cdev_t *cdev)  { (void)cdev; return 0; }
static int netconf_cdev_release(cdev_t *cdev) { (void)cdev; return 0; }

/**
 * __get_active_netconf — fill a netconf_req with the active lwIP config.
 *
 * Returns 0 if lwIP is initialised (data valid), -EAGAIN if not yet ready.
 */
static int __get_active_netconf(struct netconf_req *out)
{
    if (!__atomic_load_n(&__lwip_initialized, __ATOMIC_ACQUIRE))
        return -EAGAIN;

    memset(out, 0, sizeof(*out));

    /* Determine mode from __netconf_req (initial config) */
    out->mode = __netconf_req.mode;

    /* Read live addresses from the lwIP netif */
    out->ip      = netif_ip4_addr(&xv6_netif)->addr;
    out->netmask = netif_ip4_netmask(&xv6_netif)->addr;
    out->gateway = netif_ip4_gw(&xv6_netif)->addr;

    /* DNS — from lwIP's dns module (slot 0) */
    const ip_addr_t *dns = dns_getserver(0);
    if (dns && !ip_addr_isany(dns))
        out->dns = ip_2_ip4(dns)->addr;

    /* Hostname */
    const char *hn = netif_get_hostname(&xv6_netif);
    if (hn)
        strncpy(out->hostname, hn, NETCONF_HOSTNAME_MAX - 1);

    return 0;
}

/**
 * netconf_cdev_read — return the active network config to userspace.
 *
 * Userspace: read(fd, &req, sizeof(req))
 * Returns sizeof(req) on success, -EAGAIN if lwIP not yet initialised.
 */
static int netconf_cdev_read(cdev_t *cdev, bool user, void *buf, size_t count)
{
    (void)cdev;
    if (count < sizeof(struct netconf_req))
        return -EINVAL;

    struct netconf_req req;
    int ret = __get_active_netconf(&req);
    if (ret < 0)
        return ret;

    if (user) {
        if (either_copyout(1, (uint64)buf, &req, sizeof(req)) < 0)
            return -EFAULT;
    } else {
        memmove(buf, &req, sizeof(req));
    }
    return (int)sizeof(struct netconf_req);
}

/**
 * netconf_cdev_write — accept a struct netconf_req via write().
 *
 * Userspace: write(fd, &req, sizeof(req))
 */
static int netconf_cdev_write(cdev_t *cdev, bool user, const void *buf,
                              size_t count)
{
    (void)cdev;
    if (count != sizeof(struct netconf_req))
        return -EINVAL;

    struct netconf_req req;
    if (user) {
        if (vm_copyin(current->vm, (char *)&req, (uint64)buf, sizeof(req)) < 0)
            return -EFAULT;
    } else {
        memmove(&req, buf, sizeof(req));
    }

    int ret = __apply_netconf(&req);
    return ret < 0 ? ret : (int)count;
}

/**
 * netconf_cdev_ioctl — handle SIOCNETCONF ioctl on /dev/netconf.
 *
 * Userspace: ioctl(fd, SIOCNETCONF, &req)
 * The arg pointer is a raw user-space pointer (from sys_vfs_ioctl default).
 */
static int netconf_cdev_ioctl(cdev_t *cdev, uint64 cmd, void *arg)
{
    (void)cdev;

    if (cmd == SIOCNETCONF) {
        struct netconf_req req;
        if (vm_copyin(current->vm, (char *)&req, (uint64)arg, sizeof(req)) < 0)
            return -EFAULT;
        return __apply_netconf(&req);
    }

    if (cmd == SIOCNETCONF_GET) {
        struct netconf_req req;
        int ret = __get_active_netconf(&req);
        if (ret < 0)
            return ret;
        if (vm_copyout(current->vm, (uint64)arg, &req, sizeof(req)) < 0)
            return -EFAULT;
        return 0;
    }

    return -ENOTTY;
}

static cdev_t netconf_cdev = {
    .dev = {
        .major   = NETCONF_MAJOR,
        .minor   = NETCONF_MINOR,
        .devname = "netconf",
        .devmode = S_IFCHR | 0666,
    },
    .readable = 1,
    .writable = 1,
    .ops = {
        .open    = netconf_cdev_open,
        .release = netconf_cdev_release,
        .write   = netconf_cdev_write,
        .ioctl   = netconf_cdev_ioctl,
        .read    = netconf_cdev_read,
        .poll    = NULL,
    },
};

/* --------------------------------------------------------------------------
 * Service startup — called once the interface has an IP address
 * -------------------------------------------------------------------------- */
static volatile int __services_started = 0;

static void __lwip_start_services(void)
{
    char netservices_opt[8];
    char gdbstub_opt[8];

    if (__atomic_exchange_n(&__services_started, 1, __ATOMIC_SEQ_CST))
        return;  /* already started */

    printf("lwip: netif up — IP %d.%d.%d.%d\n",
           ip4_addr1_16(netif_ip4_addr(&xv6_netif)),
           ip4_addr2_16(netif_ip4_addr(&xv6_netif)),
           ip4_addr3_16(netif_ip4_addr(&xv6_netif)),
           ip4_addr4_16(netif_ip4_addr(&xv6_netif)));

    if (cmdline_get_param("netservices", netservices_opt,
                          sizeof(netservices_opt)) == 0 &&
        strcmp(netservices_opt, "0") == 0) {
        printf("lwip: kernel network services disabled by cmdline\n");
        return;
    }

    /* Kernel-side telnetd disabled — user-space telnetd handles
     * connections with proper multi-user login authentication. */
    // extern void telnetd_init(void);
    // telnetd_init();
    extern void tftpd_init(void);
    tftpd_init();
    /* Kernel-side httpd disabled — user-space Flask handles HTTP. */
    // extern void httpd_daemon_init(void);
    // httpd_daemon_init();
    extern void iperfd_init(void);
    iperfd_init();
    extern void sntpd_init(void);
    sntpd_init();
    extern void netbiosd_init(void);
    netbiosd_init();
    extern void mdnsd_init(void);
    mdnsd_init();
    if (cmdline_get_param("gdbstub", gdbstub_opt, sizeof(gdbstub_opt)) == 0 &&
        strcmp(gdbstub_opt, "0") == 0) {
        printf("gdbstub: disabled by kernel cmdline\n");
    } else {
        extern void gdbstub_init(void);
        gdbstub_init();
    }
}

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

/**
 * __lwip_kthread — Kernel thread that initialises the lwIP network stack.
 *
 * Waits for userspace (init) to provide network configuration via
 * /dev/netconf before initialising the stack.  Network init is entirely
 * driven by init — the kthread will not proceed until init writes to
 * /dev/netconf.  After the netif is up (and has an IP), starts services
 * that depend on lwIP (telnetd, gdbstub, etc.).
 */
static void __lwip_kthread(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;
    ip4_addr_t ipaddr, netmask, gw;

    /* ---- Initialise the TCP/IP stack first ----
     * This creates the tcpip mbox so that any lwIP function called from
     * interrupt context (e.g. net_rx → pbuf_free_callback, or the EMAC
     * link-up path) won't crash on an uninitialised mbox.
     * tcpip_init() is independent of having a NIC or user configuration. */
    printf("lwip: initialising network stack (lwIP %s)\n", LWIP_VERSION_STRING);
    tcpip_init(__tcpip_init_done, NULL);

    /* Wait for the tcpip thread to be ready — must yield so it can run
     * (especially important on single-CPU configurations). */
    while (!__atomic_load_n(&__lwip_tcpip_ready, __ATOMIC_ACQUIRE)) {
        scheduler_yield();
    }

    /* ---- Wait for a NIC to be registered ---- */
    printf("lwip: waiting for network device...\n");
    while (netdev_get_default() == NULL) {
        sleep_ms(100);
    }
    printf("lwip: network device detected\n");

    /* ---- Wait for init to provide configuration via /dev/netconf ---- */
    printf("lwip: waiting for network configuration from init...\n");
    while (!__atomic_load_n(&__netconf_ready, __ATOMIC_ACQUIRE)) {
        sleep_ms(NETCONF_POLL_INTERVAL_MS);
    }
    printf("lwip: received configuration from init\n");

    /* Determine IP configuration (init always provides config) */
    int use_dhcp;
    if (__netconf_req.mode == NETCONF_MODE_STATIC) {
        ipaddr.addr  = __netconf_req.ip;
        netmask.addr = __netconf_req.netmask;
        gw.addr      = __netconf_req.gateway;
        use_dhcp = 0;
        printf("lwip: static config: %d.%d.%d.%d/%d.%d.%d.%d gw %d.%d.%d.%d\n",
               ip4_addr1_16(&ipaddr), ip4_addr2_16(&ipaddr),
               ip4_addr3_16(&ipaddr), ip4_addr4_16(&ipaddr),
               ip4_addr1_16(&netmask), ip4_addr2_16(&netmask),
               ip4_addr3_16(&netmask), ip4_addr4_16(&netmask),
               ip4_addr1_16(&gw), ip4_addr2_16(&gw),
               ip4_addr3_16(&gw), ip4_addr4_16(&gw));
    } else {
        /* DHCP — start with 0.0.0.0, address assigned later */
        IP4_ADDR(&ipaddr,  0, 0, 0, 0);
        IP4_ADDR(&netmask, 0, 0, 0, 0);
        IP4_ADDR(&gw,      0, 0, 0, 0);
        use_dhcp = 1;
        printf("lwip: using DHCP\n");
    }

    netif_add(&xv6_netif,
#if LWIP_IPV4
              &ipaddr, &netmask, &gw,
#endif
              NULL,                  /* state */
              xv6_netif_init,        /* init callback */
              tcpip_input);          /* input function */

    netif_set_default(&xv6_netif);

    /* Set hostname before starting DHCP (sent in DHCP DISCOVER) */
    if (__netconf_req.hostname[0]) {
        netif_set_hostname(&xv6_netif, __netconf_req.hostname);
    } else {
        netif_set_hostname(&xv6_netif, "xv6");
    }

    netif_set_up(&xv6_netif);

    /* Mark the netif as ready — this unblocks net_rx() and the link
     * callback so they can safely call into lwIP. */
    __atomic_store_n(&__lwip_netif_ready, 1, __ATOMIC_RELEASE);

    /* Set initial link state from the driver and register for changes */
    struct netdev *ndev = netdev_get_default();
    if (ndev) {
        netdev_set_link_callback(ndev, __netdev_link_change);
        if (ndev->link_up)
            netif_set_link_up(&xv6_netif);
    }

    if (use_dhcp) {
        /* Start DHCP — address will be assigned asynchronously */
        printf("lwip: starting DHCP discovery...\n");
        dhcp_start(&xv6_netif);

        /* Wait for DHCP to complete (or timeout after 30s) */
        int dhcp_timeout_ms = 30000;
        int dhcp_elapsed = 0;
        while (dhcp_elapsed < dhcp_timeout_ms) {
            const ip4_addr_t *assigned = netif_ip4_addr(&xv6_netif);
            if (!ip4_addr_isany_val(*assigned)) {
                printf("lwip: DHCP lease acquired\n");
                break;
            }
            sleep_ms(500);
            dhcp_elapsed += 500;
        }

        const ip4_addr_t *assigned = netif_ip4_addr(&xv6_netif);
        if (ip4_addr_isany_val(*assigned)) {
            printf("lwip: DHCP timeout after %d ms, using fallback\n",
                   dhcp_timeout_ms);
            /* Use platform-specific fallback */
            if (platform.has_emac) {
                IP4_ADDR(&ipaddr,  192, 168, 0, 201);
                IP4_ADDR(&netmask, 255, 255, 255, 0);
                IP4_ADDR(&gw,      192, 168, 0, 1);
            } else {
                IP4_ADDR(&ipaddr,  10, 0, 2, 15);
                IP4_ADDR(&netmask, 255, 255, 255, 0);
                IP4_ADDR(&gw,      10, 0, 2, 2);
            }
            dhcp_stop(&xv6_netif);
            netif_set_addr(&xv6_netif, &ipaddr, &netmask, &gw);
        }
    } else {
        /* Static IP — send gratuitous ARPs to announce our presence */
        if (ndev && ndev->link_up) {
            for (int i = 0; i < GARP_COUNT; i++) {
                etharp_gratuitous(&xv6_netif);
                if (i < GARP_COUNT - 1)
                    sleep_ms(GARP_INTERVAL_MS);
            }
        }
    }

    /*
     * DNS server handling:
     * - DHCP: lwIP's DHCP client automatically sets DNS from the
     *   DHCP ACK (option 6).  If the config also specifies dns=,
     *   it overrides the DHCP-provided server.
     * - Static: DNS must come from the config file.
     */
    if (use_dhcp) {
        /* Report DNS obtained from DHCP */
        const ip_addr_t *dhcp_dns = dns_getserver(0);
        if (!ip_addr_isany(dhcp_dns)) {
            printf("lwip: DNS from DHCP: %d.%d.%d.%d\n",
                   (ip_2_ip4(dhcp_dns)->addr >>  0) & 0xff,
                   (ip_2_ip4(dhcp_dns)->addr >>  8) & 0xff,
                   (ip_2_ip4(dhcp_dns)->addr >> 16) & 0xff,
                   (ip_2_ip4(dhcp_dns)->addr >> 24) & 0xff);
        }
    }
    /* Override with config-specified DNS if provided */
    if (__netconf_req.dns != 0) {
        ip_addr_t dns_addr;
        dns_addr.addr = __netconf_req.dns;
        dns_setserver(0, &dns_addr);
        printf("lwip: DNS server set to %d.%d.%d.%d (from config)\n",
               (__netconf_req.dns >>  0) & 0xff,
               (__netconf_req.dns >>  8) & 0xff,
               (__netconf_req.dns >> 16) & 0xff,
               (__netconf_req.dns >> 24) & 0xff);
    }

    /* Mark lwIP as initialized — sys_netconf() will apply config directly */
    __atomic_store_n(&__lwip_initialized, 1, __ATOMIC_RELEASE);

    /* Start network services */
    __lwip_start_services();
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
    /* Register /dev/netconf character device (devtmpfs auto-creates node) */
    int ret = cdev_register(&netconf_cdev);
    if (ret != 0)
        printf("lwip: failed to register /dev/netconf: %d\n", ret);

    struct thread *t = kthread_create("lwip", __lwip_kthread, 0, 0,
                                      KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(t)) {
        printf("lwip: failed to create init kthread\n");
        return;
    }
    wakeup(t);
}
