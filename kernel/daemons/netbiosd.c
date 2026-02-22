/*
 * netbiosd — Kernel-resident NetBIOS Name Service responder
 *
 * Uses lwIP's built-in netbiosns application to make the xv6 system
 * discoverable on the local network by its NetBIOS name.
 *
 * netbiosns uses the raw UDP API (port 137) and runs entirely within
 * the tcpip thread, so no separate kthread is needed.
 *
 * The system is discoverable as "XV6" on the local network.
 */

#include "types.h"
#include "param.h"
#include "printf.h"

#include "lwip/apps/netbiosns.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void netbiosd_init(void)
{
    netbiosns_init();
    netbiosns_set_name("XV6");
    printf("netbiosd: NetBIOS name service started (name: XV6)\n");
}
