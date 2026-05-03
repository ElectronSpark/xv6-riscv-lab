/**
 * @file lwipopts.h
 * @brief lwIP configuration for xv6 kernel
 *
 * This is the main lwIP configuration file. It is included by lwip/opt.h
 * before any default values are set, so we can override any option here.
 */

#ifndef LWIP_LWIPOPTS_H
#define LWIP_LWIPOPTS_H

/* --------------------------------------------------------------------------
 * Platform / OS
 * -------------------------------------------------------------------------- */
#define NO_SYS                          0    /* We provide full OS support */
#define SYS_LIGHTWEIGHT_PROT            1    /* Enable interrupt protection */
#define LWIP_TIMERS                     1
#define LWIP_TCPIP_CORE_LOCKING         0

/* No C standard library available in kernel */
#define LWIP_NO_CTYPE_H                 1

/* --------------------------------------------------------------------------
 * Memory configuration
 * -------------------------------------------------------------------------- */
/* Use lwIP's internal heap (mem_malloc) */
#define MEM_LIBC_MALLOC                 0
#define MEMP_MEM_MALLOC                 0
#define MEM_ALIGNMENT                   8
#define MEM_SIZE                        (8 * 1024 * 1024) /* browser-sized TLS/network bursts */

/* Pool sizes
 *
 * PBUF_POOL_SIZE and MEMP_NUM_PBUF must be large enough to cover:
 *   - pbufs held in TCP recv buffers (TCP_WND/MSS per connection)
 *   - pbufs in-flight from ISR to tcpip thread (MEMP_NUM_TCPIP_MSG_INPKT),
 *     including several concurrent browser TCP streams with 128-MSS windows
 *   - pbufs for ARP, ICMP, UDP, etc.
 * If pbufs exhaust, ALL networking dies — even ping and ARP stop working.
 */
#define MEMP_NUM_PBUF                   1024
#define MEMP_NUM_UDP_PCB                16
#define MEMP_NUM_TCP_PCB                128
#define MEMP_NUM_TCP_PCB_LISTEN         16
#define MEMP_NUM_TCP_SEG                4096
#define MEMP_NUM_NETBUF                 32
#define MEMP_NUM_NETCONN                256
#define MEMP_NUM_TCPIP_MSG_API          64
#define MEMP_NUM_TCPIP_MSG_INPKT        1024
#define MEMP_NUM_ARP_QUEUE              32
#define MEMP_NUM_SYS_TIMEOUT            32

/* --------------------------------------------------------------------------
 * Pbuf options
 * -------------------------------------------------------------------------- */
#define PBUF_POOL_SIZE                  4096
#define PBUF_POOL_BUFSIZE               1536

/* --------------------------------------------------------------------------
 * ARP options
 * -------------------------------------------------------------------------- */
#define LWIP_ARP                        1
#define ARP_TABLE_SIZE                  16
#define ARP_QUEUEING                    1
#define ETHARP_SUPPORT_STATIC_ENTRIES   1

/* --------------------------------------------------------------------------
 * IP options
 * -------------------------------------------------------------------------- */
#define LWIP_IPV4                       1
#define LWIP_IPV6                       0    /* IPv4 only for now */
#define IP_FORWARD                      0
#define IP_REASSEMBLY                   1
#define IP_FRAG                         1
#define IP_REASS_MAX_PBUFS              32
#define IP_DEFAULT_TTL                  64

/* --------------------------------------------------------------------------
 * ICMP options
 * -------------------------------------------------------------------------- */
#define LWIP_ICMP                       1

/* --------------------------------------------------------------------------
 * DHCP options
 * -------------------------------------------------------------------------- */
#define LWIP_DHCP                       1
#define LWIP_AUTOIP                     0

/* --------------------------------------------------------------------------
 * UDP options
 * -------------------------------------------------------------------------- */
#define LWIP_UDP                        1
#define UDP_TTL                         64

/* --------------------------------------------------------------------------
 * TCP options
 * -------------------------------------------------------------------------- */
#define LWIP_TCP                        1
#define TCP_TTL                         64
#define TCP_MSL                         10000   /* 10s TIME_WAIT (default 60s) */
#define TCP_MSS                         1460
#define TCP_SND_BUF                     (64 * TCP_MSS)
#define TCP_SND_QUEUELEN                ((4 * (TCP_SND_BUF) + (TCP_MSS - 1))/(TCP_MSS))
#define TCP_WND                         (128 * TCP_MSS)
#define LWIP_TCP_KEEPALIVE              1
#define LWIP_WND_SCALE                  1
#define TCP_RCV_SCALE                   2
#define TCP_QUEUE_OOSEQ                 1
#define LWIP_TCP_SACK_OUT               1
#define LWIP_TCP_MAX_SACK_NUM           4

/* --------------------------------------------------------------------------
 * Network interfaces
 * -------------------------------------------------------------------------- */
#define LWIP_NETIF_STATUS_CALLBACK      1
#define LWIP_NETIF_LINK_CALLBACK        1
#define LWIP_NETIF_HOSTNAME             1
#define LWIP_NETIF_LOOPBACK             1
#define LWIP_LOOPBACK_MAX_PBUFS         16

/* --------------------------------------------------------------------------
 * Sequential / Socket API
 * -------------------------------------------------------------------------- */
#define LWIP_NETCONN                    1
#define LWIP_NETCONN_FULLDUPLEX         1    /* Allow concurrent read+write
                                                from different threads on the
                                                same netconn.  Required by
                                                telnetd (RX thread does recv,
                                                TX thread does write/close). */
#define LWIP_NETCONN_SEM_PER_THREAD     1    /* Required by FULLDUPLEX */
#define LWIP_SOCKET                     0    /* No BSD socket API (we use
                                                xv6's own socket layer) */
#define LWIP_DNS                        1
#define LWIP_SO_RCVTIMEO                1
#define LWIP_SO_SNDTIMEO                1
#define LWIP_SO_RCVBUF                  1
#define LWIP_SO_LINGER                  1

/* --------------------------------------------------------------------------
 * Raw API callbacks
 * -------------------------------------------------------------------------- */
#define LWIP_RAW                        1

/* --------------------------------------------------------------------------
 * Statistics
 * -------------------------------------------------------------------------- */
#define LWIP_STATS                      0
#define LWIP_STATS_DISPLAY              0

/* --------------------------------------------------------------------------
 * PPP / 6LoWPAN (disabled)
 * -------------------------------------------------------------------------- */
#define PPP_SUPPORT                     0
#define LWIP_6LOWPAN                    0

/* --------------------------------------------------------------------------
 * Checksum offload (none — do in software)
 * -------------------------------------------------------------------------- */
#define CHECKSUM_GEN_IP                 1
#define CHECKSUM_GEN_UDP                1
#define CHECKSUM_GEN_TCP                1
#define CHECKSUM_GEN_ICMP               1
#define CHECKSUM_CHECK_IP               1
#define CHECKSUM_CHECK_UDP              1
#define CHECKSUM_CHECK_TCP              1
#define CHECKSUM_CHECK_ICMP             1

/* --------------------------------------------------------------------------
 * Debugging (disabled by default, enable selectively)
 * -------------------------------------------------------------------------- */
#define LWIP_DBG_MIN_LEVEL              LWIP_DBG_LEVEL_ALL
#define LWIP_DBG_TYPES_ON               LWIP_DBG_OFF

#define ETHARP_DEBUG                    LWIP_DBG_OFF
#define NETIF_DEBUG                     LWIP_DBG_OFF
#define PBUF_DEBUG                      LWIP_DBG_OFF
#define API_LIB_DEBUG                   LWIP_DBG_OFF
#define API_MSG_DEBUG                   LWIP_DBG_OFF
#define SOCKETS_DEBUG                   LWIP_DBG_OFF
#define ICMP_DEBUG                      LWIP_DBG_OFF
#define IGMP_DEBUG                      LWIP_DBG_OFF
#define INET_DEBUG                      LWIP_DBG_OFF
#define IP_DEBUG                        LWIP_DBG_OFF
#define IP_REASS_DEBUG                  LWIP_DBG_OFF
#define RAW_DEBUG                       LWIP_DBG_OFF
#define MEM_DEBUG                       LWIP_DBG_OFF
#define MEMP_DEBUG                      LWIP_DBG_OFF
#define SYS_DEBUG                       LWIP_DBG_OFF
#define TIMERS_DEBUG                    LWIP_DBG_OFF
#define TCP_DEBUG                       LWIP_DBG_OFF
#define TCP_INPUT_DEBUG                 LWIP_DBG_OFF
#define TCP_FR_DEBUG                    LWIP_DBG_OFF
#define TCP_RTO_DEBUG                   LWIP_DBG_OFF
#define TCP_CWND_DEBUG                  LWIP_DBG_OFF
#define TCP_WND_DEBUG                   LWIP_DBG_OFF
#define TCP_OUTPUT_DEBUG                LWIP_DBG_OFF
#define TCP_RST_DEBUG                   LWIP_DBG_OFF
#define TCP_QLEN_DEBUG                  LWIP_DBG_OFF
#define UDP_DEBUG                       LWIP_DBG_OFF
#define TCPIP_DEBUG                     LWIP_DBG_OFF
#define SLIP_DEBUG                      LWIP_DBG_OFF
#define DHCP_DEBUG                      LWIP_DBG_OFF
#define AUTOIP_DEBUG                    LWIP_DBG_OFF
#define DNS_DEBUG                       LWIP_DBG_OFF

/* --------------------------------------------------------------------------
 * Thread options
 * -------------------------------------------------------------------------- */
#define TCPIP_THREAD_NAME               "lwip_tcpip"
#define TCPIP_THREAD_STACKSIZE          0    /* ignored by xv6 port */
#define TCPIP_THREAD_PRIO               ((0 << 2) | 2)  /* major=0, minor=2 */
#define TCPIP_MBOX_SIZE                 SYS_MBOX_SIZE
#define DEFAULT_THREAD_NAME             "lwip"
#define DEFAULT_THREAD_STACKSIZE        0
#define DEFAULT_THREAD_PRIO             0
#define DEFAULT_ACCEPTMBOX_SIZE         8
#define DEFAULT_RAW_RECVMBOX_SIZE       8
#define DEFAULT_UDP_RECVMBOX_SIZE       8
#define DEFAULT_TCP_RECVMBOX_SIZE       256

/* --------------------------------------------------------------------------
 * Misc
 * -------------------------------------------------------------------------- */
/* Use kernel's errno.h (found via -I${KERNEL_DIR}/inc) */
#define LWIP_ERRNO_STDINCLUDE
#define LWIP_SKIP_PACKING_CHECK         1   /* Our packing works, skip the
                                               compile-time struct size test */

/* snprintf / vsnprintf — xv6 doesn't have these, use lwIP defaults */
#define LWIP_NO_CTYPE_H                 1

/* --------------------------------------------------------------------------
 * IGMP (required by mDNS multicast)
 * -------------------------------------------------------------------------- */
#define LWIP_IGMP                       1

/* --------------------------------------------------------------------------
 * Netif client data (required by mDNS per-netif storage)
 * -------------------------------------------------------------------------- */
#define LWIP_NUM_NETIF_CLIENT_DATA      1

/* --------------------------------------------------------------------------
 * Application options
 * -------------------------------------------------------------------------- */

/* TFTP server */
#define TFTP_MAX_FILENAME_LEN           128

/* HTTP server — disabled, user-space Flask handles HTTP */
// #define LWIP_HTTPD_CUSTOM_FILES         1
// #define LWIP_HTTPD_DYNAMIC_FILE_READ    1
// #define LWIP_HTTPD_DYNAMIC_HEADERS      1
// #define LWIP_HTTPD_FILE_EXTENSION       1

/* SNTP client — time synchronisation */
#define SNTP_SERVER_DNS                 1
void sntp_set_system_time_us(unsigned int sec, unsigned int us);
#define SNTP_SET_SYSTEM_TIME_US(sec, us) sntp_set_system_time_us((sec), (us))

/* mDNS responder */
#define LWIP_MDNS_RESPONDER             1
#define MDNS_MAX_SERVICES               4

#endif /* LWIP_LWIPOPTS_H */
