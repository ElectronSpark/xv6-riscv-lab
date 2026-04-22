/**
 * @file netconf.h
 * @brief Network configuration interface shared between kernel and userspace.
 *
 * Userspace programs configure the network by opening /dev/netconf and
 * issuing ioctl(fd, SIOCNETCONF, &req).  The device node is auto-created
 * by devtmpfs when the kernel registers the netconf cdev at boot.
 *
 * init reads /etc/network.conf and writes the parsed struct netconf_req
 * to /dev/netconf via write() (equivalent to ioctl).  ifconfig can also
 * use ioctl(SIOCNETCONF) at runtime for DHCP/static reconfiguration.
 */

#ifndef __NETCONF_H__
#define __NETCONF_H__

#define NETCONF_MODE_DHCP   0
#define NETCONF_MODE_STATIC 1

#define NETCONF_HOSTNAME_MAX 32

/* ioctl command for /dev/netconf — apply struct netconf_req */
#define SIOCNETCONF         0x89F0
/* ioctl command to read active config (IP/mask/gw/dns from lwIP stack) */
#define SIOCNETCONF_GET     0x89F1

/* Device numbers for /dev/netconf */
#define NETCONF_MAJOR       10
#define NETCONF_MINOR       0

/**
 * struct netconf_req — network configuration request.
 *
 * Passed from userspace to kernel via /dev/netconf (write or ioctl).
 * IP addresses are in network byte order (big-endian).
 */
struct netconf_req {
    int  mode;                              /* NETCONF_MODE_DHCP or _STATIC */
    unsigned int ip;                        /* IPv4 address (network order) */
    unsigned int netmask;                   /* subnet mask (network order)  */
    unsigned int gateway;                   /* default gw (network order)   */
    unsigned int dns;                       /* DNS server, 0 = none         */
    char   hostname[NETCONF_HOSTNAME_MAX];  /* hostname (NUL-terminated)    */
};

#endif /* __NETCONF_H__ */
