/*
 * iperfd — Kernel-resident iperf server daemon
 *
 * Uses lwIP's built-in lwiperf application to run an iperf2-compatible
 * TCP bandwidth measurement server on port 5001.
 *
 * lwiperf uses the raw TCP API and runs entirely within the tcpip thread,
 * so no separate kthread is needed.
 *
 * QEMU network: guest port 5001, reachable from host as localhost:5001
 *   via QEMU SLIRP hostfwd.
 *
 * Usage from host:
 *   iperf -c localhost -p 5001
 */

#include "types.h"
#include "param.h"
#include "printf.h"

#include "lwip/apps/lwiperf.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Report callback                                                             */
/* ──────────────────────────────────────────────────────────────────────────── */

static void iperf_report(void *arg, enum lwiperf_report_type report_type,
                         const ip_addr_t *local_addr, u16_t local_port,
                         const ip_addr_t *remote_addr, u16_t remote_port,
                         u32_t bytes_transferred, u32_t ms_duration,
                         u32_t bandwidth_kbitpsec)
{
    (void)arg;
    (void)local_addr;
    (void)local_port;

    const char *type_str;
    switch (report_type) {
    case LWIPERF_TCP_DONE_SERVER:    type_str = "done";           break;
    case LWIPERF_TCP_DONE_CLIENT:    type_str = "client done";    break;
    case LWIPERF_TCP_ABORTED_LOCAL:  type_str = "aborted local";  break;
    case LWIPERF_TCP_ABORTED_REMOTE: type_str = "aborted remote"; break;
    default:                         type_str = "error";          break;
    }

    printf("iperfd: %s — %d.%d.%d.%d:%d  %u bytes  %u ms  %u kbit/s\n",
           type_str,
           ip4_addr1_16(ip_2_ip4(remote_addr)),
           ip4_addr2_16(ip_2_ip4(remote_addr)),
           ip4_addr3_16(ip_2_ip4(remote_addr)),
           ip4_addr4_16(ip_2_ip4(remote_addr)),
           remote_port,
           bytes_transferred, ms_duration, bandwidth_kbitpsec);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void iperfd_init(void)
{
    void *session = lwiperf_start_tcp_server_default(iperf_report, NULL);
    if (session == NULL) {
        printf("iperfd: failed to start\n");
        return;
    }
    printf("iperfd: server started on port %d\n", LWIPERF_TCP_PORT_DEFAULT);
}
