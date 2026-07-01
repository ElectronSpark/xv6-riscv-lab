/*
 * netlink.c - AF_NETLINK socket implementation
 *
 * Provides a minimal netlink socket layer for kernel–userspace communication,
 * primarily for network configuration queries (NETLINK_ROUTE).
 *
 * Design:
 *   - When userspace sends a dump request (e.g. RTM_GETLINK), the kernel
 *     builds reply messages into a response buffer.
 *   - Subsequent recvfrom() calls drain the response buffer.
 *   - This is a simplified model: requests are handled synchronously
 *     (sendto fills the response buffer; recvfrom drains it).
 *
 * Supported message types:
 *   - RTM_GETLINK  → enumerates network interfaces
 *   - RTM_GETADDR  → enumerates interface addresses
 *   - RTM_GETROUTE → enumerates routes (default route only)
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "string.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "lock/mutex_types.h"
#include "netlink.h"
#include "vfs/file.h"
#include "vfs/vfs_types.h"
#include "vfs/fcntl.h"
#include "vfs/poll.h"
#include "kqueue_types.h"
#include "proc/tq.h"
#include "signal.h"
#include <mm/vm.h>
#include "mm/slab.h"

/* Socket type constants */
#include "vfs/unix_socket.h"

/* lwIP headers for querying interface state */
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/ip4_addr.h"

/* snprintf — implemented in sys_arch.c, declared in compat/stdio.h */
int snprintf(char *buf, size_t size, const char *fmt, ...);

/* From irq/syscall.c — argument fetching */
extern void argint(int n, int *ip);
extern void argaddr(int n, uint64 *ip);

/* ========================================================================== */
/* Internal netlink socket structure                                          */
/* ========================================================================== */

#define NETLINK_RESP_SIZE (PAGE_SIZE * 2) /* 8KB response buffer */
#define NETLINK_MSG_PEEK       0x02
#define NETLINK_MSG_DONTWAIT   0x40

struct netlink_sock {
    spinlock_t lock;
    int        protocol;    /* NETLINK_ROUTE, etc. */
    uint32     nl_pid;      /* port ID */
    uint32     nl_groups;   /* multicast groups */

    /* Response buffer: filled by sendto handlers, drained by recvfrom */
    char      *resp_buf;    /* allocated response buffer */
    size_t     resp_len;    /* total bytes of response data */
    size_t     resp_off;    /* current read offset */
    tq_t       rd_queue;    /* blocking recv/read waiters */

    /* Back-reference for kqueue */
    struct vfs_file *file;
};

/* ========================================================================== */
/* Slab cache                                                                 */
/* ========================================================================== */

static slab_cache_t __netlink_sock_cache = {0};
static uint32 __netlink_pid_counter = 1000;  /* PID auto-assign counter */

/* ========================================================================== */
/* Forward declarations                                                       */
/* ========================================================================== */

static ssize_t netlink_file_read(struct vfs_file *file, char *buf,
                                 size_t count, bool user);
static ssize_t netlink_file_write(struct vfs_file *file, const char *buf,
                                  size_t count, bool user);
static int netlink_file_release(struct vfs_inode *inode, struct vfs_file *file);
static int netlink_file_poll(struct vfs_file *file, short events);
static int netlink_file_ioctl(struct vfs_file *file, uint64 cmd, void *arg);

/* ========================================================================== */
/* File operations                                                            */
/* ========================================================================== */

struct vfs_file_ops netlink_socket_file_ops = {
    .flags   = VFS_FILE_OPS_F_POLL_NOTIFY_BACKED,
    .read    = netlink_file_read,
    .write   = netlink_file_write,
    .llseek  = NULL,
    .release = netlink_file_release,
    .fsync   = NULL,
    .fflush  = NULL,
    .poll    = netlink_file_poll,
    .ioctl   = netlink_file_ioctl,
    .fault   = NULL,
};

/* ========================================================================== */
/* Allocation helpers                                                         */
/* ========================================================================== */

static struct netlink_sock *netlink_sock_alloc(void)
{
    struct netlink_sock *sk = slab_alloc(&__netlink_sock_cache);
    if (sk == NULL)
        return NULL;
    memset(sk, 0, sizeof(*sk));
    spin_init(&sk->lock, "netlink_sock");
    tq_init(&sk->rd_queue, "netlink_rd", &sk->lock);
    return sk;
}

static void netlink_sock_free(struct netlink_sock *sk)
{
    if (sk->resp_buf != NULL) {
        kfree(sk->resp_buf);
        sk->resp_buf = NULL;
    }
    slab_free(sk);
}

/* ========================================================================== */
/* Initialization                                                             */
/* ========================================================================== */

void netlink_init(void)
{
    int ret = slab_cache_init(&__netlink_sock_cache, "netlink_sock",
                             sizeof(struct netlink_sock), SLAB_FLAG_STATIC);
    assert(ret == 0, "netlink_sock slab init failed, errno=%d", ret);
}

/* ========================================================================== */
/* Socket fd allocation                                                       */
/* ========================================================================== */

static int netlink_fd_alloc(struct netlink_sock *sk, int file_flags)
{
    int fd = vfs_custom_fd_alloc(&netlink_socket_file_ops, sk, file_flags);
    if (fd < 0)
        return fd;

    struct vfs_file *f = vfs_fdtable_get_file(current->fdtable, fd);
    sk->file = f;
    vfs_fput(f);

    return fd;
}

/* ========================================================================== */
/* Check if a vfs_file is a netlink socket                                    */
/* ========================================================================== */

int netlink_sock_is_netlink(struct vfs_file *f)
{
    return (f != NULL && f->ops == &netlink_socket_file_ops);
}

static struct netlink_sock *netlink_from_fd(int fd)
{
    if (fd < 0 || fd >= NOFILE)
        return NULL;

    spin_lock(&current->fdtable->lock);
    struct vfs_file *f = current->fdtable->files[fd];
    spin_unlock(&current->fdtable->lock);

    if (f == NULL || f->ops != &netlink_socket_file_ops)
        return NULL;

    return (struct netlink_sock *)f->private_data;
}

/* ========================================================================== */
/* Response buffer helpers                                                    */
/* ========================================================================== */

static int resp_ensure_buf(struct netlink_sock *sk)
{
    if (sk->resp_buf == NULL) {
        /* Allocate 2 pages for response buffer */
        sk->resp_buf = kalloc();
        if (sk->resp_buf == NULL)
            return -ENOMEM;
        /* Zero initialization not strictly needed, but be safe */
    }
    return 0;
}

/*
 * resp_put - append raw bytes to the response buffer
 * Returns 0 on success, -ENOBUFS if buffer full.
 */
static int resp_put(struct netlink_sock *sk, const void *data, size_t len)
{
    if (sk->resp_len + len > PAGE_SIZE)  /* single page for now */
        return -ENOBUFS;
    memmove(sk->resp_buf + sk->resp_len, data, len);
    sk->resp_len += len;
    return 0;
}

static struct vfs_file *netlink_wake_readers_locked(struct netlink_sock *sk)
{
    struct vfs_file *file = NULL;

    if (sk->resp_len > sk->resp_off) {
        tq_wakeup_all(&sk->rd_queue, 0, 0);
        if (sk->file != NULL)
            file = vfs_fdup(sk->file);
    }
    return file;
}

static int netlink_wait_readable_locked(struct netlink_sock *sk,
                                        struct vfs_file *file, int flags)
{
    int nonblock = (file->f_flags & O_NONBLOCK) ||
                   (flags & NETLINK_MSG_DONTWAIT);

    if (nonblock)
        return -EAGAIN;

    int ret = tq_wait_in_state(&sk->rd_queue, &sk->lock, NULL,
                               THREAD_INTERRUPTIBLE);
    if (ret < 0)
        return ret;
    if (signal_pending(current))
        return -EINTR;
    return 0;
}

static int netlink_recv_from_file(struct vfs_file *file, uint64 buf, size_t len,
                                  int flags, uint64 usrc, uint64 uaddrlen,
                                  bool user)
{
    struct netlink_sock *sk = (struct netlink_sock *)file->private_data;
    if (sk == NULL)
        return -EBADF;
    if (len == 0)
        return 0;

    char *staging = NULL;
    size_t to_copy;

    spin_lock(&sk->lock);
    for (;;) {
        size_t avail = sk->resp_len - sk->resp_off;
        if (avail != 0) {
            to_copy = len < avail ? len : avail;
            staging = kvmalloc(to_copy);
            if (staging == NULL) {
                spin_unlock(&sk->lock);
                return -ENOMEM;
            }
            memmove(staging, sk->resp_buf + sk->resp_off, to_copy);
            if (!(flags & NETLINK_MSG_PEEK))
                sk->resp_off += to_copy;
            break;
        }

        int ret = netlink_wait_readable_locked(sk, file, flags);
        if (ret < 0) {
            spin_unlock(&sk->lock);
            return ret;
        }
    }
    spin_unlock(&sk->lock);

    int err = 0;
    if (user) {
        if (vm_copyout(current->vm, buf, staging, to_copy) < 0)
            err = -EFAULT;
    } else {
        memmove((void *)buf, staging, to_copy);
    }
    kvfree(staging);
    if (err != 0)
        return err;

    if (usrc != 0 && uaddrlen != 0) {
        struct sockaddr_nl sa;
        memset(&sa, 0, sizeof(sa));
        sa.nl_family = AF_NETLINK;
        sa.nl_pid = 0; /* from kernel */

        int alen = sizeof(sa);
        vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
        vm_copyout(current->vm, usrc, &sa, sizeof(sa));
    }

    return (int)to_copy;
}

/*
 * resp_put_attr - append an rtattr + payload to the response buffer
 */
static int resp_put_attr(struct netlink_sock *sk, uint16 type,
                         const void *data, size_t len)
{
    struct rtattr rta;
    rta.rta_len = (uint16)RTA_LENGTH(len);
    rta.rta_type = type;
    int ret = resp_put(sk, &rta, sizeof(rta));
    if (ret < 0) return ret;
    ret = resp_put(sk, data, len);
    if (ret < 0) return ret;
    /* Pad to 4-byte alignment */
    size_t total = sizeof(rta) + len;
    size_t aligned = RTA_ALIGN(total);
    if (aligned > total) {
        char pad[4] = {0};
        resp_put(sk, pad, aligned - total);
    }
    return 0;
}

/* ========================================================================== */
/* RTM_GETLINK handler — enumerate network interfaces                         */
/* ========================================================================== */

static int handle_getlink(struct netlink_sock *sk, struct nlmsghdr *req)
{
    sk->resp_len = 0;
    sk->resp_off = 0;

    /* Iterate lwIP netifs */
    struct netif *nif;
    int idx = 1;

    NETIF_FOREACH(nif) {
        /* Calculate message size (header + ifinfomsg + attributes) */
        size_t msg_start = sk->resp_len;

        /* Reserve space for nlmsghdr (fill length at end) */
        struct nlmsghdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.nlmsg_type = RTM_NEWLINK;
        hdr.nlmsg_flags = NLM_F_MULTI;
        hdr.nlmsg_seq = req->nlmsg_seq;
        hdr.nlmsg_pid = sk->nl_pid;
        resp_put(sk, &hdr, sizeof(hdr));

        /* ifinfomsg */
        struct ifinfomsg ifi;
        memset(&ifi, 0, sizeof(ifi));
        ifi.ifi_family = AF_UNSPEC;
        ifi.ifi_type = (nif->flags & NETIF_FLAG_ETHARP) ? ARPHRD_ETHER : ARPHRD_LOOPBACK;
        ifi.ifi_index = idx;
        ifi.ifi_flags = 0;
        if (nif->flags & NETIF_FLAG_UP)
            ifi.ifi_flags |= IFF_UP | IFF_RUNNING;
        if (nif->flags & NETIF_FLAG_BROADCAST)
            ifi.ifi_flags |= IFF_BROADCAST;
        if (nif->flags & NETIF_FLAG_IGMP)
            ifi.ifi_flags |= IFF_MULTICAST;
        if (!(nif->flags & NETIF_FLAG_ETHARP))
            ifi.ifi_flags |= IFF_LOOPBACK;
        ifi.ifi_change = 0xFFFFFFFF;
        resp_put(sk, &ifi, sizeof(ifi));

        /* IFLA_IFNAME attribute */
        char ifname[16];
        int namelen = snprintf(ifname, sizeof(ifname), "%c%c%d",
                              nif->name[0], nif->name[1], nif->num);
        resp_put_attr(sk, IFLA_IFNAME, ifname, namelen + 1);

        /* IFLA_MTU attribute */
        uint32 mtu = nif->mtu;
        resp_put_attr(sk, IFLA_MTU, &mtu, sizeof(mtu));

        /* IFLA_ADDRESS attribute (MAC) */
        if (nif->hwaddr_len > 0)
            resp_put_attr(sk, IFLA_ADDRESS, nif->hwaddr, nif->hwaddr_len);

        /* IFLA_OPERSTATE attribute */
        uint8 operstate = (nif->flags & NETIF_FLAG_LINK_UP) ? IF_OPER_UP : IF_OPER_DOWN;
        resp_put_attr(sk, IFLA_OPERSTATE, &operstate, sizeof(operstate));

        /* Fixup nlmsghdr length */
        size_t msg_len = sk->resp_len - msg_start;
        struct nlmsghdr *fix = (struct nlmsghdr *)(sk->resp_buf + msg_start);
        fix->nlmsg_len = (uint32)msg_len;

        idx++;
    }

    /* Also add loopback interface info */
    {
        size_t msg_start = sk->resp_len;
        struct nlmsghdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.nlmsg_type = RTM_NEWLINK;
        hdr.nlmsg_flags = NLM_F_MULTI;
        hdr.nlmsg_seq = req->nlmsg_seq;
        hdr.nlmsg_pid = sk->nl_pid;
        resp_put(sk, &hdr, sizeof(hdr));

        struct ifinfomsg ifi;
        memset(&ifi, 0, sizeof(ifi));
        ifi.ifi_family = AF_UNSPEC;
        ifi.ifi_type = ARPHRD_LOOPBACK;
        ifi.ifi_index = idx;
        ifi.ifi_flags = IFF_UP | IFF_RUNNING | IFF_LOOPBACK;
        ifi.ifi_change = 0xFFFFFFFF;
        resp_put(sk, &ifi, sizeof(ifi));

        char loname[] = "lo";
        resp_put_attr(sk, IFLA_IFNAME, loname, 3);
        uint32 lo_mtu = 65536;
        resp_put_attr(sk, IFLA_MTU, &lo_mtu, sizeof(lo_mtu));
        uint8 operstate = IF_OPER_UP;
        resp_put_attr(sk, IFLA_OPERSTATE, &operstate, sizeof(operstate));

        size_t msg_len = sk->resp_len - msg_start;
        struct nlmsghdr *fix = (struct nlmsghdr *)(sk->resp_buf + msg_start);
        fix->nlmsg_len = (uint32)msg_len;
    }

    /* Append NLMSG_DONE */
    struct nlmsghdr done;
    memset(&done, 0, sizeof(done));
    done.nlmsg_len = NLMSG_LENGTH(sizeof(int32));
    done.nlmsg_type = NLMSG_DONE;
    done.nlmsg_flags = 0;
    done.nlmsg_seq = req->nlmsg_seq;
    done.nlmsg_pid = sk->nl_pid;
    resp_put(sk, &done, sizeof(done));
    int32 zero_err = 0;
    resp_put(sk, &zero_err, sizeof(zero_err));

    return 0;
}

/* ========================================================================== */
/* RTM_GETADDR handler — enumerate interface addresses                        */
/* ========================================================================== */

static int handle_getaddr(struct netlink_sock *sk, struct nlmsghdr *req)
{
    sk->resp_len = 0;
    sk->resp_off = 0;

    struct netif *nif;
    int idx = 1;

    NETIF_FOREACH(nif) {
        const ip4_addr_t *addr = netif_ip4_addr(nif);
        const ip4_addr_t *mask = netif_ip4_netmask(nif);

        if (ip4_addr_isany_val(*addr)) {
            idx++;
            continue;
        }

        size_t msg_start = sk->resp_len;

        struct nlmsghdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.nlmsg_type = RTM_NEWADDR;
        hdr.nlmsg_flags = NLM_F_MULTI;
        hdr.nlmsg_seq = req->nlmsg_seq;
        hdr.nlmsg_pid = sk->nl_pid;
        resp_put(sk, &hdr, sizeof(hdr));

        /* Compute prefix length from netmask */
        uint32 mask_val = ip4_addr_get_u32(mask);
        int prefixlen = 0;
        uint32 m = ntohl(mask_val);
        while (m & 0x80000000) {
            prefixlen++;
            m <<= 1;
        }

        struct ifaddrmsg ifa;
        memset(&ifa, 0, sizeof(ifa));
        ifa.ifa_family = AF_INET_NL; /* AF_INET */
        ifa.ifa_prefixlen = (uint8)prefixlen;
        ifa.ifa_flags = 0;
        ifa.ifa_scope = 0;
        ifa.ifa_index = (uint32)idx;
        resp_put(sk, &ifa, sizeof(ifa));

        /* IFA_LOCAL / IFA_ADDRESS attributes */
        uint32 addr_val = ip4_addr_get_u32(addr);
        resp_put_attr(sk, IFA_LOCAL, &addr_val, sizeof(addr_val));
        resp_put_attr(sk, IFA_ADDRESS, &addr_val, sizeof(addr_val));

        /* IFA_LABEL attribute */
        char ifname[16];
        int namelen = snprintf(ifname, sizeof(ifname), "%c%c%d",
                              nif->name[0], nif->name[1], nif->num);
        resp_put_attr(sk, IFA_LABEL, ifname, namelen + 1);

        /* Broadcast address */
        uint32 bcast_val = addr_val | ~mask_val;
        resp_put_attr(sk, IFA_BROADCAST, &bcast_val, sizeof(bcast_val));

        size_t msg_len = sk->resp_len - msg_start;
        struct nlmsghdr *fix = (struct nlmsghdr *)(sk->resp_buf + msg_start);
        fix->nlmsg_len = (uint32)msg_len;

        idx++;
    }

    /* Add loopback address 127.0.0.1/8 */
    {
        size_t msg_start = sk->resp_len;

        struct nlmsghdr hdr;
        memset(&hdr, 0, sizeof(hdr));
        hdr.nlmsg_type = RTM_NEWADDR;
        hdr.nlmsg_flags = NLM_F_MULTI;
        hdr.nlmsg_seq = req->nlmsg_seq;
        hdr.nlmsg_pid = sk->nl_pid;
        resp_put(sk, &hdr, sizeof(hdr));

        struct ifaddrmsg ifa;
        memset(&ifa, 0, sizeof(ifa));
        ifa.ifa_family = AF_INET_NL;
        ifa.ifa_prefixlen = 8;
        ifa.ifa_flags = 0;
        ifa.ifa_scope = 254; /* RT_SCOPE_HOST */
        ifa.ifa_index = (uint32)idx;
        resp_put(sk, &ifa, sizeof(ifa));

        uint32 lo_addr = htonl(0x7f000001); /* 127.0.0.1 */
        resp_put_attr(sk, IFA_LOCAL, &lo_addr, sizeof(lo_addr));
        resp_put_attr(sk, IFA_ADDRESS, &lo_addr, sizeof(lo_addr));
        char loname[] = "lo";
        resp_put_attr(sk, IFA_LABEL, loname, 3);

        size_t msg_len = sk->resp_len - msg_start;
        struct nlmsghdr *fix = (struct nlmsghdr *)(sk->resp_buf + msg_start);
        fix->nlmsg_len = (uint32)msg_len;
    }

    /* NLMSG_DONE */
    struct nlmsghdr done;
    memset(&done, 0, sizeof(done));
    done.nlmsg_len = NLMSG_LENGTH(sizeof(int32));
    done.nlmsg_type = NLMSG_DONE;
    done.nlmsg_flags = 0;
    done.nlmsg_seq = req->nlmsg_seq;
    done.nlmsg_pid = sk->nl_pid;
    resp_put(sk, &done, sizeof(done));
    int32 zero_err = 0;
    resp_put(sk, &zero_err, sizeof(zero_err));

    return 0;
}

/* ========================================================================== */
/* RTM_GETROUTE handler — enumerate routes                                    */
/* ========================================================================== */

static int handle_getroute(struct netlink_sock *sk, struct nlmsghdr *req)
{
    sk->resp_len = 0;
    sk->resp_off = 0;

    struct netif *nif;
    int idx = 1;

    NETIF_FOREACH(nif) {
        const ip4_addr_t *addr = netif_ip4_addr(nif);
        const ip4_addr_t *mask = netif_ip4_netmask(nif);
        const ip4_addr_t *gw_addr = netif_ip4_gw(nif);

        if (ip4_addr_isany_val(*addr)) {
            idx++;
            continue;
        }

        /* Compute prefix length */
        uint32 mask_val = ip4_addr_get_u32(mask);
        int prefixlen = 0;
        uint32 m = ntohl(mask_val);
        while (m & 0x80000000) {
            prefixlen++;
            m <<= 1;
        }

        /* Subnet route */
        {
            size_t msg_start = sk->resp_len;

            struct nlmsghdr hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.nlmsg_type = RTM_NEWROUTE;
            hdr.nlmsg_flags = NLM_F_MULTI;
            hdr.nlmsg_seq = req->nlmsg_seq;
            hdr.nlmsg_pid = sk->nl_pid;
            resp_put(sk, &hdr, sizeof(hdr));

            struct rtmsg rt;
            memset(&rt, 0, sizeof(rt));
            rt.rtm_family = AF_INET_NL;
            rt.rtm_dst_len = (uint8)prefixlen;
            rt.rtm_table = RT_TABLE_MAIN;
            rt.rtm_protocol = RTPROT_BOOT;
            rt.rtm_scope = RT_SCOPE_LINK;
            rt.rtm_type = RTN_UNICAST;
            resp_put(sk, &rt, sizeof(rt));

            /* Subnet = addr & mask */
            uint32 subnet = ip4_addr_get_u32(addr) & mask_val;
            resp_put_attr(sk, RTA_DST, &subnet, sizeof(subnet));

            int32 oif = idx;
            resp_put_attr(sk, RTA_OIF, &oif, sizeof(oif));

            size_t msg_len = sk->resp_len - msg_start;
            struct nlmsghdr *fix = (struct nlmsghdr *)(sk->resp_buf + msg_start);
            fix->nlmsg_len = (uint32)msg_len;
        }

        /* Default route via gateway (if gateway is set) */
        if (!ip4_addr_isany_val(*gw_addr)) {
            size_t msg_start = sk->resp_len;

            struct nlmsghdr hdr;
            memset(&hdr, 0, sizeof(hdr));
            hdr.nlmsg_type = RTM_NEWROUTE;
            hdr.nlmsg_flags = NLM_F_MULTI;
            hdr.nlmsg_seq = req->nlmsg_seq;
            hdr.nlmsg_pid = sk->nl_pid;
            resp_put(sk, &hdr, sizeof(hdr));

            struct rtmsg rt;
            memset(&rt, 0, sizeof(rt));
            rt.rtm_family = AF_INET_NL;
            rt.rtm_dst_len = 0; /* default route */
            rt.rtm_table = RT_TABLE_MAIN;
            rt.rtm_protocol = RTPROT_STATIC;
            rt.rtm_scope = RT_SCOPE_UNIVERSE;
            rt.rtm_type = RTN_UNICAST;
            resp_put(sk, &rt, sizeof(rt));

            uint32 gw_val = ip4_addr_get_u32(gw_addr);
            resp_put_attr(sk, RTA_GATEWAY, &gw_val, sizeof(gw_val));

            int32 oif = idx;
            resp_put_attr(sk, RTA_OIF, &oif, sizeof(oif));

            size_t msg_len = sk->resp_len - msg_start;
            struct nlmsghdr *fix = (struct nlmsghdr *)(sk->resp_buf + msg_start);
            fix->nlmsg_len = (uint32)msg_len;
        }

        idx++;
    }

    /* NLMSG_DONE */
    struct nlmsghdr done;
    memset(&done, 0, sizeof(done));
    done.nlmsg_len = NLMSG_LENGTH(sizeof(int32));
    done.nlmsg_type = NLMSG_DONE;
    done.nlmsg_flags = 0;
    done.nlmsg_seq = req->nlmsg_seq;
    done.nlmsg_pid = sk->nl_pid;
    resp_put(sk, &done, sizeof(done));
    int32 zero_err = 0;
    resp_put(sk, &zero_err, sizeof(zero_err));

    return 0;
}

/* ========================================================================== */
/* Request dispatch                                                           */
/* ========================================================================== */

static int netlink_handle_msg(struct netlink_sock *sk, char *msg, size_t len)
{
    if (len < NLMSG_HDRLEN)
        return -EINVAL;

    struct nlmsghdr *nlh = (struct nlmsghdr *)msg;
    if (nlh->nlmsg_len < NLMSG_HDRLEN || nlh->nlmsg_len > len)
        return -EINVAL;

    /* Ensure response buffer is allocated */
    int ret = resp_ensure_buf(sk);
    if (ret < 0)
        return ret;

    switch (nlh->nlmsg_type) {
    case RTM_GETLINK:
        return handle_getlink(sk, nlh);
    case RTM_GETADDR:
        return handle_getaddr(sk, nlh);
    case RTM_GETROUTE:
        return handle_getroute(sk, nlh);
    default:
        /* Unsupported message type: return NLMSG_ERROR with -EOPNOTSUPP */
        sk->resp_len = 0;
        sk->resp_off = 0;

        struct nlmsghdr ehdr;
        memset(&ehdr, 0, sizeof(ehdr));
        ehdr.nlmsg_len = NLMSG_LENGTH(sizeof(struct nlmsgerr));
        ehdr.nlmsg_type = NLMSG_ERROR;
        ehdr.nlmsg_flags = 0;
        ehdr.nlmsg_seq = nlh->nlmsg_seq;
        ehdr.nlmsg_pid = sk->nl_pid;
        resp_put(sk, &ehdr, sizeof(ehdr));

        struct nlmsgerr nerr;
        memset(&nerr, 0, sizeof(nerr));
        nerr.error = -EOPNOTSUPP;
        nerr.msg = *nlh;
        resp_put(sk, &nerr, sizeof(nerr));
        return 0;
    }
}

/* ========================================================================== */
/* File operations implementation                                             */
/* ========================================================================== */

/*
 * netlink_file_read - read response data from a netlink socket
 */
static ssize_t netlink_file_read(struct vfs_file *file, char *buf,
                                 size_t count, bool user)
{
    return netlink_recv_from_file(file, (uint64)buf, count, 0, 0, 0, user);
}

/*
 * netlink_file_write - send a netlink request
 */
static ssize_t netlink_file_write(struct vfs_file *file, const char *buf,
                                  size_t count, bool user)
{
    struct netlink_sock *sk = (struct netlink_sock *)file->private_data;
    if (sk == NULL)
        return -EBADF;

    if (count > PAGE_SIZE)
        return -EMSGSIZE;

    char kbuf[512];
    char *msg = kbuf;
    if (count > sizeof(kbuf)) {
        msg = kalloc();
        if (msg == NULL)
            return -ENOMEM;
    }

    if (user) {
        if (vm_copyin(current->vm, msg, (uint64)buf, count) < 0) {
            if (msg != kbuf) kfree(msg);
            return -EFAULT;
        }
    } else {
        memmove(msg, buf, count);
    }

    spin_lock(&sk->lock);
    int ret = netlink_handle_msg(sk, msg, count);
    struct vfs_file *notify_file = (ret < 0) ? NULL :
        netlink_wake_readers_locked(sk);
    spin_unlock(&sk->lock);

    if (notify_file != NULL) {
        vfs_file_knote_notify(notify_file, EVFILT_READ, 0);
        vfs_fput(notify_file);
    }

    if (msg != kbuf)
        kfree(msg);

    if (ret < 0)
        return (ssize_t)ret;
    return (ssize_t)count;
}

/*
 * netlink_file_release - close a netlink socket
 */
static int netlink_file_release(struct vfs_inode *inode, struct vfs_file *file)
{
    (void)inode;
    struct netlink_sock *sk = (struct netlink_sock *)file->private_data;
    if (sk == NULL)
        return 0;

    spin_lock(&sk->lock);
    tq_wakeup_all(&sk->rd_queue, -EBADF, 0);
    spin_unlock(&sk->lock);

    sk->file = NULL;
    netlink_sock_free(sk);
    file->private_data = NULL;
    return 0;
}

/*
 * netlink_file_poll - check socket readiness
 */
static int netlink_file_poll(struct vfs_file *file, short events)
{
    struct netlink_sock *sk = (struct netlink_sock *)file->private_data;
    if (sk == NULL)
        return POLLNVAL;

    short revents = 0;

    spin_lock(&sk->lock);
    /* Always writable (we handle requests synchronously) */
    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND))
        revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));

    /* Readable if there's response data */
    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        if (sk->resp_len > sk->resp_off)
            revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
    }
    spin_unlock(&sk->lock);

    return revents;
}

/*
 * netlink_file_ioctl - ioctl on netlink socket
 */
static int netlink_file_ioctl(struct vfs_file *file, uint64 cmd, void *arg)
{
    struct netlink_sock *sk = (struct netlink_sock *)file->private_data;
    if (sk == NULL)
        return -EBADF;

    switch (cmd) {
    case 0x541B: { /* FIONREAD */
        spin_lock(&sk->lock);
        int count = (int)(sk->resp_len - sk->resp_off);
        spin_unlock(&sk->lock);
        if (vm_copyout(current->vm, (uint64)arg, &count, sizeof(count)) < 0)
            return -EFAULT;
        return 0;
    }
    case 0x5421: { /* FIONBIO */
        int val;
        if (vm_copyin(current->vm, &val, (uint64)arg, sizeof(val)) < 0)
            return -EFAULT;
        if (val)
            file->f_flags |= O_NONBLOCK;
        else
            file->f_flags &= ~O_NONBLOCK;
        return 0;
    }
    default:
        return -ENOTTY;
    }
}

/* ========================================================================== */
/* Syscall-level operations                                                   */
/* ========================================================================== */

/*
 * netlink_sock_create - create a new AF_NETLINK socket
 */
int netlink_sock_create(int type, int protocol, int file_flags)
{
    if (type != SOCK_DGRAM && type != SOCK_RAW)
        return -EPROTOTYPE;

    if (protocol != NETLINK_ROUTE)
        return -EPROTONOSUPPORT;

    struct netlink_sock *sk = netlink_sock_alloc();
    if (sk == NULL)
        return -ENOMEM;

    sk->protocol = protocol;
    sk->nl_pid = __atomic_fetch_add(&__netlink_pid_counter, 1,
                                    __ATOMIC_RELAXED);

    int fd = netlink_fd_alloc(sk, file_flags);
    if (fd < 0) {
        netlink_sock_free(sk);
        return fd;
    }

    return fd;
}

/* Socket type constants (must match sys_socket.c) */
#define SOCK_STREAM 1
#define SOCK_DGRAM  2
#define SOCK_RAW    3

/*
 * netlink_sock_bind - bind a netlink socket
 */
int netlink_sock_bind(int fd, uint64 uaddr, int addrlen)
{
    struct netlink_sock *sk = netlink_from_fd(fd);
    if (sk == NULL)
        return -EBADF;

    if (addrlen < (int)sizeof(struct sockaddr_nl))
        return -EINVAL;

    struct sockaddr_nl sa;
    if (vm_copyin(current->vm, &sa, uaddr, sizeof(sa)) < 0)
        return -EFAULT;

    if (sa.nl_family != AF_NETLINK)
        return -EAFNOSUPPORT;

    spin_lock(&sk->lock);
    if (sa.nl_pid != 0)
        sk->nl_pid = sa.nl_pid;
    sk->nl_groups = sa.nl_groups;
    spin_unlock(&sk->lock);

    return 0;
}

/*
 * netlink_sock_getsockname - get netlink address
 */
int netlink_sock_getsockname(int fd, uint64 uaddr, uint64 uaddrlen)
{
    struct netlink_sock *sk = netlink_from_fd(fd);
    if (sk == NULL)
        return -EBADF;

    struct sockaddr_nl sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;

    spin_lock(&sk->lock);
    sa.nl_pid = sk->nl_pid;
    sa.nl_groups = sk->nl_groups;
    spin_unlock(&sk->lock);

    int alen = sizeof(sa);
    vm_copyout(current->vm, uaddrlen, &alen, sizeof(alen));
    vm_copyout(current->vm, uaddr, &sa, sizeof(sa));
    return 0;
}

/*
 * netlink_sock_sendto - send a netlink request message
 */
int netlink_sock_sendto(int fd, uint64 ubuf, size_t len, int flags,
                        uint64 udest, int addrlen)
{
    (void)flags;
    (void)udest;
    (void)addrlen;

    struct netlink_sock *sk = netlink_from_fd(fd);
    if (sk == NULL)
        return -EBADF;

    if (len > PAGE_SIZE)
        return -EMSGSIZE;

    char kbuf[512];
    char *msg = kbuf;
    if (len > sizeof(kbuf)) {
        msg = kalloc();
        if (msg == NULL)
            return -ENOMEM;
    }

    if (vm_copyin(current->vm, msg, ubuf, len) < 0) {
        if (msg != kbuf) kfree(msg);
        return -EFAULT;
    }

    spin_lock(&sk->lock);
    int ret = netlink_handle_msg(sk, msg, len);
    struct vfs_file *notify_file = (ret < 0) ? NULL :
        netlink_wake_readers_locked(sk);
    spin_unlock(&sk->lock);

    if (notify_file != NULL) {
        vfs_file_knote_notify(notify_file, EVFILT_READ, 0);
        vfs_fput(notify_file);
    }

    if (msg != kbuf)
        kfree(msg);

    if (ret < 0)
        return ret;
    return (int)len;
}

/*
 * netlink_sock_recvfrom - receive netlink response data
 */
int netlink_sock_recvfrom(int fd, uint64 ubuf, size_t len, int flags,
                          uint64 usrc, uint64 uaddrlen)
{
    return netlink_sock_recv(fd, ubuf, len, flags, usrc, uaddrlen, true);
}

int netlink_sock_recv(int fd, uint64 buf, size_t len, int flags,
                      uint64 usrc, uint64 uaddrlen, bool user)
{
    struct vfs_file *file = vfs_fdtable_get_file(current->fdtable, fd);
    if (file == NULL)
        return -EBADF;
    if (file->ops != &netlink_socket_file_ops) {
        vfs_fput(file);
        return -EBADF;
    }

    int ret = netlink_recv_from_file(file, buf, len, flags, usrc, uaddrlen,
                                     user);
    vfs_fput(file);
    return ret;
}
