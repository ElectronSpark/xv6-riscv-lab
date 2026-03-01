/*
 * netlink.h - AF_NETLINK socket declarations
 *
 * Netlink sockets provide communication between kernel and userspace
 * for network configuration. Supports a minimal subset of NETLINK_ROUTE
 * sufficient for tools like "ip link", "ip addr", "ip route".
 */

#ifndef __KERNEL_NETLINK_H
#define __KERNEL_NETLINK_H

#include "types.h"

struct vfs_file;
struct vfs_file_ops;

/* ========================================================================== */
/* Constants                                                                  */
/* ========================================================================== */

#define AF_NETLINK 16

/* Netlink protocol families */
#define NETLINK_ROUTE   0  /* Routing/link/address updates */

/* Netlink message types (rtnetlink) */
#define RTM_NEWLINK  16
#define RTM_DELLINK  17
#define RTM_GETLINK  18
#define RTM_NEWADDR  20
#define RTM_DELADDR  21
#define RTM_GETADDR  22
#define RTM_NEWROUTE 24
#define RTM_DELROUTE 25
#define RTM_GETROUTE 26

/* Netlink message flags */
#define NLM_F_REQUEST  0x0001
#define NLM_F_MULTI    0x0002
#define NLM_F_ACK      0x0004
#define NLM_F_ROOT     0x0100
#define NLM_F_MATCH    0x0200
#define NLM_F_DUMP     (NLM_F_ROOT | NLM_F_MATCH)

/* Netlink message done type */
#define NLMSG_DONE  3
#define NLMSG_ERROR 2
#define NLMSG_NOOP  1

/* ========================================================================== */
/* Netlink message header (same as Linux)                                     */
/* ========================================================================== */

struct nlmsghdr {
    uint32 nlmsg_len;    /* Length including header */
    uint16 nlmsg_type;   /* Message type */
    uint16 nlmsg_flags;  /* Flags */
    uint32 nlmsg_seq;    /* Sequence number */
    uint32 nlmsg_pid;    /* Sending port ID */
};

#define NLMSG_HDRLEN      ((int)sizeof(struct nlmsghdr))
#define NLMSG_ALIGN(len)  (((len) + 3) & ~3)
#define NLMSG_LENGTH(len) (NLMSG_HDRLEN + (len))
#define NLMSG_DATA(nlh)   ((void *)((char *)(nlh) + NLMSG_HDRLEN))

/* ========================================================================== */
/* Netlink socket address                                                     */
/* ========================================================================== */

struct sockaddr_nl {
    uint16 nl_family;   /* AF_NETLINK */
    uint16 nl_pad;      /* padding */
    uint32 nl_pid;      /* port ID (0 = kernel) */
    uint32 nl_groups;   /* multicast groups mask */
};

/* ========================================================================== */
/* Routing / ifinfo message structures (rtnetlink)                            */
/* ========================================================================== */

/* Interface address family values */
#define AF_INET_NL  2
#define AF_UNSPEC   0

/* ifinfomsg - for RTM_GETLINK / RTM_NEWLINK */
struct ifinfomsg {
    uint8  ifi_family;
    uint8  __ifi_pad;
    uint16 ifi_type;     /* ARPHRD_* */
    int32  ifi_index;    /* interface index */
    uint32 ifi_flags;    /* IFF_* */
    uint32 ifi_change;   /* IFF_* change mask */
};

/* ifaddrmsg - for RTM_GETADDR / RTM_NEWADDR */
struct ifaddrmsg {
    uint8  ifa_family;
    uint8  ifa_prefixlen;
    uint8  ifa_flags;
    uint8  ifa_scope;
    uint32 ifa_index;    /* interface index */
};

/* rtmsg - for RTM_GETROUTE / RTM_NEWROUTE */
struct rtmsg {
    uint8  rtm_family;
    uint8  rtm_dst_len;
    uint8  rtm_src_len;
    uint8  rtm_tos;
    uint8  rtm_table;
    uint8  rtm_protocol;
    uint8  rtm_scope;
    uint8  rtm_type;
    uint32 rtm_flags;
};

/* Routing table IDs */
#define RT_TABLE_MAIN   254

/* Route protocol */
#define RTPROT_BOOT     3
#define RTPROT_STATIC   4

/* Route scope */
#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_LINK     253

/* Route type */
#define RTN_UNICAST 1

/* Interface flags */
#define IFF_UP        0x1
#define IFF_BROADCAST 0x2
#define IFF_LOOPBACK  0x8
#define IFF_RUNNING   0x40
#define IFF_MULTICAST 0x1000

/* ARPHRD types */
#define ARPHRD_ETHER    1
#define ARPHRD_LOOPBACK 772

/* ========================================================================== */
/* Routing attributes (rtattr)                                                */
/* ========================================================================== */

struct rtattr {
    uint16 rta_len;
    uint16 rta_type;
};

#define RTA_ALIGNTO    4
#define RTA_ALIGN(len) (((len) + RTA_ALIGNTO - 1) & ~(RTA_ALIGNTO - 1))
#define RTA_LENGTH(len) (RTA_ALIGN(sizeof(struct rtattr)) + (len))
#define RTA_DATA(rta)  ((void *)((char *)(rta) + RTA_ALIGN(sizeof(struct rtattr))))

/* ifinfomsg attribute types (IFLA_*) */
#define IFLA_UNSPEC    0
#define IFLA_ADDRESS   1  /* hardware address */
#define IFLA_BROADCAST 2  /* broadcast address */
#define IFLA_IFNAME    3  /* interface name */
#define IFLA_MTU       4
#define IFLA_LINK      5
#define IFLA_QDISC     6
#define IFLA_OPERSTATE 16

/* ifaddrmsg attribute types (IFA_*) */
#define IFA_UNSPEC     0
#define IFA_ADDRESS    1
#define IFA_LOCAL      2
#define IFA_LABEL      3
#define IFA_BROADCAST  4

/* rtmsg attribute types (RTA_*) */
#define RTA_UNSPEC  0
#define RTA_DST     1
#define RTA_SRC     2
#define RTA_IIF     3
#define RTA_OIF     4
#define RTA_GATEWAY 5
#define RTA_TABLE   15

/* Interface operstate */
#define IF_OPER_DOWN 2
#define IF_OPER_UP   6

/* ========================================================================== */
/* Netlink error message                                                      */
/* ========================================================================== */

struct nlmsgerr {
    int32          error;
    struct nlmsghdr msg;  /* original request header */
};

/* ========================================================================== */
/* Public API                                                                 */
/* ========================================================================== */

void netlink_init(void);

/* Create an AF_NETLINK socket, returns fd or -errno */
int  netlink_sock_create(int type, int protocol, int file_flags);

/* Syscall-level operations on AF_NETLINK sockets */
int  netlink_sock_bind(int fd, uint64 uaddr, int addrlen);
int  netlink_sock_getsockname(int fd, uint64 uaddr, uint64 uaddrlen);
int  netlink_sock_sendto(int fd, uint64 ubuf, size_t len, int flags,
                         uint64 udest, int addrlen);
int  netlink_sock_recvfrom(int fd, uint64 ubuf, size_t len, int flags,
                           uint64 usrc, uint64 uaddrlen);

/* Check if a vfs_file is an AF_NETLINK socket */
int  netlink_sock_is_netlink(struct vfs_file *f);

/* Extern file_ops for AF_NETLINK sockets */
extern struct vfs_file_ops netlink_socket_file_ops;

#endif /* __KERNEL_NETLINK_H */
