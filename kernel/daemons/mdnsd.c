/*
 * mdnsd — Kernel-resident mDNS (Multicast DNS) responder daemon
 *
 * Uses lwIP's built-in mDNS responder to advertise the system as
 * "xv6.local" on the local network, along with service records for
 * HTTP, Telnet, and TFTP.
 *
 * mDNS uses the raw UDP API (multicast, port 5353) and runs entirely
 * within the tcpip thread, so no separate kthread is needed.
 *
 * Requirements: LWIP_MDNS_RESPONDER=1, LWIP_IGMP=1 in lwipopts.h.
 *
 * Usage from host:
 *   ping xv6.local
 *   curl http://xv6.local:8080/    (via QEMU port forward)
 */

#include "types.h"
#include "param.h"
#include "printf.h"

#include "lwip/apps/mdns.h"
#include "lwip/netif.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void mdnsd_init(void)
{
    mdns_resp_init();

    /* Register the default network interface with hostname "xv6" →
     * makes the system reachable as "xv6.local" */
    if (netif_default == NULL) {
        printf("mdnsd: no default netif — skipping\n");
        return;
    }

    err_t err = mdns_resp_add_netif(netif_default, "xv6");
    if (err != ERR_OK) {
        printf("mdnsd: failed to add netif (err=%d)\n", err);
        return;
    }

    /* Advertise our services */
    mdns_resp_add_service(netif_default, "xv6 Web Server", "_http",
                          DNSSD_PROTO_TCP, 80, NULL, NULL);
    mdns_resp_add_service(netif_default, "xv6 Telnet", "_telnet",
                          DNSSD_PROTO_TCP, 23, NULL, NULL);
    mdns_resp_add_service(netif_default, "xv6 TFTP", "_tftp",
                          DNSSD_PROTO_UDP, 69, NULL, NULL);

    printf("mdnsd: responder started (xv6.local)\n");
}
