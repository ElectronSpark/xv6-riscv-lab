/*
 * route — routing table display and manipulation for xv6
 *
 * Usage:
 *   route                                       Show routing table
 *   route -n                                    Show routing table (numeric)
 *   route add default gw <gateway> [dev <if>]   Add default route
 *   route add -net <dest> netmask <mask> gw <gw> [dev <if>]
 *   route del default                           Delete default route
 *   route del -net <dest>                       Delete route
 *
 * Since lwIP doesn't maintain a full routing table, the display is
 * synthesised from interface subnet routes + default gateway.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Netlink types for querying ──────────────────────────────────────── */

struct nl_msghdr {
    unsigned int   nlmsg_len;
    unsigned short nlmsg_type;
    unsigned short nlmsg_flags;
    unsigned int   nlmsg_seq;
    unsigned int   nlmsg_pid;
};

struct nl_sockaddr {
    unsigned short nl_family;
    unsigned short nl_pad;
    unsigned int   nl_pid;
    unsigned int   nl_groups;
};

struct nl_rtmsg {
    unsigned char rtm_family;
    unsigned char rtm_dst_len;
    unsigned char rtm_src_len;
    unsigned char rtm_tos;
    unsigned char rtm_table;
    unsigned char rtm_protocol;
    unsigned char rtm_scope;
    unsigned char rtm_type;
    unsigned int  rtm_flags;
};

struct nl_rtattr {
    unsigned short rta_len;
    unsigned short rta_type;
};

struct nl_ifinfomsg {
    unsigned char  ifi_family;
    unsigned char  __pad;
    unsigned short ifi_type;
    int            ifi_index;
    unsigned int   ifi_flags;
    unsigned int   ifi_change;
};

#define NL_AF_NETLINK    16
#define NL_NETLINK_ROUTE 0
#define NL_RTM_GETLINK   18
#define NL_RTM_GETROUTE  26
#define NL_NLM_F_REQUEST 0x0001
#define NL_NLM_F_DUMP    0x0300
#define NL_NLMSG_DONE    3
#define NL_NLMSG_ERROR   2
#define NL_NLMSG_HDRLEN  ((int)sizeof(struct nl_msghdr))
#define NL_NLMSG_ALIGN(l) (((l)+3)&~3)
#define NL_RTA_DST       1
#define NL_RTA_GATEWAY   5
#define NL_RTA_OIF       4
#define NL_IFLA_IFNAME   3

/* ── helpers ─────────────────────────────────────────────────────────── */

static int numeric_mode = 0;

/*
 * netlink_query - send a netlink dump request and read the response
 */
static int netlink_query(int type, void *payload, int payload_len,
                         char *resp, int resp_size)
{
    int fd = socket(NL_AF_NETLINK, SOCK_DGRAM, NL_NETLINK_ROUTE);
    if (fd < 0)
        return -1;

    char req[128];
    memset(req, 0, sizeof(req));
    struct nl_msghdr *nlh = (struct nl_msghdr *)req;
    nlh->nlmsg_len   = NL_NLMSG_HDRLEN + payload_len;
    nlh->nlmsg_type  = type;
    nlh->nlmsg_flags = NL_NLM_F_REQUEST | NL_NLM_F_DUMP;
    nlh->nlmsg_seq   = 1;
    nlh->nlmsg_pid   = 0;
    if (payload_len > 0)
        memcpy(req + NL_NLMSG_HDRLEN, payload, payload_len);

    struct nl_sockaddr sa;
    memset(&sa, 0, sizeof(sa));
    sa.nl_family = NL_AF_NETLINK;
    ssize_t sent = sendto(fd, req, nlh->nlmsg_len, 0,
                          (struct sockaddr *)&sa, sizeof(sa));
    if (sent < 0) {
        close(fd);
        return -1;
    }

    ssize_t nr = read(fd, resp, resp_size);
    close(fd);
    return (int)nr;
}

/*
 * ifindex_to_name - resolve an interface index to its name
 */
static void ifindex_to_name(int ifindex, char *out, int outlen)
{
    struct nl_ifinfomsg ifm;
    memset(&ifm, 0, sizeof(ifm));

    char resp[4096];
    int nr = netlink_query(NL_RTM_GETLINK, &ifm, sizeof(ifm),
                           resp, sizeof(resp));
    if (nr <= 0) {
        snprintf(out, outlen, "?");
        return;
    }

    int off = 0;
    int idx = 1;
    while (off + NL_NLMSG_HDRLEN <= nr) {
        const struct nl_msghdr *nlh = (const struct nl_msghdr *)(resp + off);
        if (nlh->nlmsg_len < (unsigned)NL_NLMSG_HDRLEN)
            break;
        if (nlh->nlmsg_type == NL_NLMSG_DONE)
            break;

        /* Skip non-link messages */
        if (nlh->nlmsg_type != 16 /* RTM_NEWLINK */) {
            off += NL_NLMSG_ALIGN(nlh->nlmsg_len);
            idx++;
            continue;
        }

        if (idx == ifindex) {
            /* Find IFLA_IFNAME attribute */
            int attr_off = NL_NLMSG_HDRLEN + NL_NLMSG_ALIGN(sizeof(struct nl_ifinfomsg));
            int attr_end = (int)nlh->nlmsg_len;
            int aoff = attr_off;
            while (aoff + (int)sizeof(struct nl_rtattr) <= attr_end) {
                const struct nl_rtattr *rta =
                    (const struct nl_rtattr *)(resp + off + aoff);
                if (rta->rta_len < sizeof(struct nl_rtattr))
                    break;
                if (rta->rta_type == NL_IFLA_IFNAME) {
                    const char *name = (const char *)rta +
                        NL_NLMSG_ALIGN(sizeof(struct nl_rtattr));
                    snprintf(out, outlen, "%s", name);
                    return;
                }
                aoff += NL_NLMSG_ALIGN(rta->rta_len);
            }
        }

        off += NL_NLMSG_ALIGN(nlh->nlmsg_len);
        idx++;
    }
    snprintf(out, outlen, "?");
}

/*
 * ip4_to_str - convert a network-byte-order IP to string
 */
static const char *ip4_to_str(uint32_t addr, char *buf, int len)
{
    struct in_addr a;
    a.s_addr = addr;
    snprintf(buf, len, "%s", inet_ntoa(a));
    return buf;
}

/*
 * show_routes - display the routing table via netlink RTM_GETROUTE
 */
static void show_routes(void)
{
    struct nl_rtmsg rtm;
    memset(&rtm, 0, sizeof(rtm));
    rtm.rtm_family = 2; /* AF_INET */

    char resp[4096];
    int nr = netlink_query(NL_RTM_GETROUTE, &rtm, sizeof(rtm),
                           resp, sizeof(resp));
    if (nr <= 0) {
        fprintf(stderr, "route: cannot get routing table\n");
        return;
    }

    printf("Kernel IP routing table\n");
    printf("%-16s %-16s %-16s %-6s %-5s %s\n",
           "Destination", "Gateway", "Genmask", "Flags", "Iface", "");

    int off = 0;
    while (off + NL_NLMSG_HDRLEN <= nr) {
        const struct nl_msghdr *nlh = (const struct nl_msghdr *)(resp + off);
        if (nlh->nlmsg_len < (unsigned)NL_NLMSG_HDRLEN)
            break;
        if (nlh->nlmsg_type == NL_NLMSG_DONE)
            break;
        if (nlh->nlmsg_type == NL_NLMSG_ERROR)
            break;

        /* Parse rtmsg */
        if ((int)nlh->nlmsg_len < NL_NLMSG_HDRLEN + (int)sizeof(struct nl_rtmsg)) {
            off += NL_NLMSG_ALIGN(nlh->nlmsg_len);
            continue;
        }

        const struct nl_rtmsg *rt =
            (const struct nl_rtmsg *)(resp + off + NL_NLMSG_HDRLEN);

        uint32_t dst = 0, gw = 0;
        int oif = 0;

        /* Parse attributes */
        int attr_off = NL_NLMSG_HDRLEN + NL_NLMSG_ALIGN(sizeof(struct nl_rtmsg));
        int attr_end = (int)nlh->nlmsg_len;
        int aoff = attr_off;
        while (aoff + (int)sizeof(struct nl_rtattr) <= attr_end) {
            const struct nl_rtattr *rta =
                (const struct nl_rtattr *)(resp + off + aoff);
            if (rta->rta_len < sizeof(struct nl_rtattr))
                break;
            const void *data = (const char *)rta +
                NL_NLMSG_ALIGN(sizeof(struct nl_rtattr));
            switch (rta->rta_type) {
            case NL_RTA_DST:
                memcpy(&dst, data, 4);
                break;
            case NL_RTA_GATEWAY:
                memcpy(&gw, data, 4);
                break;
            case NL_RTA_OIF:
                memcpy(&oif, data, 4);
                break;
            }
            aoff += NL_NLMSG_ALIGN(rta->rta_len);
        }

        /* Compute genmask from prefix length */
        uint32_t mask = 0;
        if (rt->rtm_dst_len > 0 && rt->rtm_dst_len <= 32)
            mask = htonl(~((1U << (32 - rt->rtm_dst_len)) - 1));

        /* Flags */
        char flags[8] = "";
        int fi = 0;
        flags[fi++] = 'U';  /* Up */
        if (gw != 0)
            flags[fi++] = 'G';  /* Gateway */
        if (rt->rtm_dst_len == 32)
            flags[fi++] = 'H';  /* Host */
        flags[fi] = '\0';

        /* Interface name */
        char ifname[16] = "*";
        if (oif > 0)
            ifindex_to_name(oif, ifname, sizeof(ifname));

        char dst_str[20], gw_str[20], mask_str[20];
        if (dst == 0 && rt->rtm_dst_len == 0)
            snprintf(dst_str, sizeof(dst_str), numeric_mode ? "0.0.0.0" : "default");
        else
            ip4_to_str(dst, dst_str, sizeof(dst_str));

        if (gw == 0)
            snprintf(gw_str, sizeof(gw_str), numeric_mode ? "0.0.0.0" : "*");
        else
            ip4_to_str(gw, gw_str, sizeof(gw_str));

        ip4_to_str(mask, mask_str, sizeof(mask_str));

        printf("%-16s %-16s %-16s %-6s %-5s\n",
               dst_str, gw_str, mask_str, flags, ifname);

        off += NL_NLMSG_ALIGN(nlh->nlmsg_len);
    }
}

/* ── route add / del ─────────────────────────────────────────────────── */

static int do_route_add(int argc, char *argv[], int start)
{
    struct rtentry rt;
    memset(&rt, 0, sizeof(rt));
    rt.rt_flags = RTF_UP;

    struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
    struct sockaddr_in *gw  = (struct sockaddr_in *)&rt.rt_gateway;
    struct sockaddr_in *mask = (struct sockaddr_in *)&rt.rt_genmask;
    dst->sin_family  = AF_INET;
    gw->sin_family   = AF_INET;
    mask->sin_family = AF_INET;

    char *devname = NULL;
    int i = start;

    while (i < argc) {
        if (strcmp(argv[i], "default") == 0) {
            /* default route: dst = 0.0.0.0, mask = 0.0.0.0 */
            dst->sin_addr.s_addr = 0;
            mask->sin_addr.s_addr = 0;
            i++;
        } else if (strcmp(argv[i], "-net") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "route: -net requires a destination\n");
                return 1;
            }
            if (inet_aton(argv[i + 1], &dst->sin_addr) == 0) {
                fprintf(stderr, "route: bad destination: %s\n", argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "-host") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "route: -host requires a destination\n");
                return 1;
            }
            if (inet_aton(argv[i + 1], &dst->sin_addr) == 0) {
                fprintf(stderr, "route: bad destination: %s\n", argv[i + 1]);
                return 1;
            }
            rt.rt_flags |= RTF_HOST;
            i += 2;
        } else if (strcmp(argv[i], "gw") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "route: gw requires a gateway address\n");
                return 1;
            }
            if (inet_aton(argv[i + 1], &gw->sin_addr) == 0) {
                fprintf(stderr, "route: bad gateway: %s\n", argv[i + 1]);
                return 1;
            }
            rt.rt_flags |= RTF_GATEWAY;
            i += 2;
        } else if (strcmp(argv[i], "netmask") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "route: netmask requires a mask\n");
                return 1;
            }
            if (inet_aton(argv[i + 1], &mask->sin_addr) == 0) {
                fprintf(stderr, "route: bad netmask: %s\n", argv[i + 1]);
                return 1;
            }
            i += 2;
        } else if (strcmp(argv[i], "dev") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "route: dev requires a device name\n");
                return 1;
            }
            devname = argv[i + 1];
            i += 2;
        } else {
            fprintf(stderr, "route: unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    rt.rt_dev = devname;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("route: socket");
        return 1;
    }

    if (ioctl(fd, SIOCADDRT, &rt) < 0) {
        perror("route: SIOCADDRT");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int do_route_del(int argc, char *argv[], int start)
{
    struct rtentry rt;
    memset(&rt, 0, sizeof(rt));

    struct sockaddr_in *dst = (struct sockaddr_in *)&rt.rt_dst;
    struct sockaddr_in *gw  = (struct sockaddr_in *)&rt.rt_gateway;
    struct sockaddr_in *mask = (struct sockaddr_in *)&rt.rt_genmask;
    dst->sin_family  = AF_INET;
    gw->sin_family   = AF_INET;
    mask->sin_family = AF_INET;

    int i = start;
    while (i < argc) {
        if (strcmp(argv[i], "default") == 0) {
            dst->sin_addr.s_addr = 0;
            mask->sin_addr.s_addr = 0;
            i++;
        } else if (strcmp(argv[i], "-net") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "route: -net requires a destination\n");
                return 1;
            }
            inet_aton(argv[i + 1], &dst->sin_addr);
            i += 2;
        } else if (strcmp(argv[i], "netmask") == 0) {
            if (i + 1 >= argc) return 1;
            inet_aton(argv[i + 1], &mask->sin_addr);
            i += 2;
        } else {
            fprintf(stderr, "route: unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        perror("route: socket");
        return 1;
    }

    if (ioctl(fd, SIOCDELRT, &rt) < 0) {
        perror("route: SIOCDELRT");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

/* ── main ────────────────────────────────────────────────────────────── */

static void usage(void)
{
    fprintf(stderr,
        "Usage: route [-n]\n"
        "       route add default gw <gateway> [dev <iface>]\n"
        "       route add -net <dest> netmask <mask> gw <gw> [dev <iface>]\n"
        "       route add -host <dest> gw <gw> [dev <iface>]\n"
        "       route del default\n"
        "       route del -net <dest>\n");
}

int main(int argc, char *argv[])
{
    if (argc == 1) {
        show_routes();
        return 0;
    }

    /* Handle -h / --help */
    if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
        usage();
        return 0;
    }

    int i = 1;

    /* Check for -n flag */
    if (strcmp(argv[1], "-n") == 0) {
        numeric_mode = 1;
        i++;
        if (i >= argc) {
            show_routes();
            return 0;
        }
    }

    if (strcmp(argv[i], "add") == 0) {
        return do_route_add(argc, argv, i + 1);
    } else if (strcmp(argv[i], "del") == 0 || strcmp(argv[i], "delete") == 0) {
        return do_route_del(argc, argv, i + 1);
    } else {
        usage();
        return 1;
    }
}
