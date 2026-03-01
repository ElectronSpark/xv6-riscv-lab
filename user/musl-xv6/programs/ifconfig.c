/*
 * ifconfig — network interface configuration for xv6
 *
 * Usage:
 *   ifconfig                        Show all interfaces
 *   ifconfig <iface>                Show one interface
 *   ifconfig <iface> <addr>         Set IP address
 *   ifconfig <iface> netmask <mask> Set netmask
 *   ifconfig <iface> up             Bring interface up
 *   ifconfig <iface> down           Bring interface down
 *   ifconfig <iface> mtu <n>        Set MTU
 *
 * Multiple options can be combined:
 *   ifconfig en1 10.0.2.15 netmask 255.255.255.0 up
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── helpers ─────────────────────────────────────────────────────────── */

static int ctl_sock = -1;  /* socket used for ioctls */

static int open_ctl_sock(void)
{
    if (ctl_sock >= 0)
        return 0;
    ctl_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctl_sock < 0) {
        perror("ifconfig: socket");
        return -1;
    }
    return 0;
}

/*
 * get_ifaddr - retrieve an AF_INET sockaddr via ioctl
 * Returns the IP in host byte order, or 0 on failure.
 */
static in_addr_t get_ifaddr(const char *ifname, unsigned long req)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    if (ioctl(ctl_sock, req, &ifr) < 0)
        return 0;

    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    return sin->sin_addr.s_addr;  /* network byte order */
}

/*
 * print_interface - display full info for one interface
 */
static void print_interface(const char *ifname)
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);

    /* Flags */
    short flags = 0;
    if (ioctl(ctl_sock, SIOCGIFFLAGS, &ifr) == 0)
        flags = ifr.ifr_flags;

    printf("%s: flags=%d<", ifname, (int)(unsigned short)flags);
    {
        int first = 1;
        if (flags & IFF_UP)        { printf("%sUP", first ? "" : ","); first = 0; }
        if (flags & IFF_BROADCAST) { printf("%sBROADCAST", first ? "" : ","); first = 0; }
        if (flags & IFF_LOOPBACK)  { printf("%sLOOPBACK", first ? "" : ","); first = 0; }
        if (flags & IFF_RUNNING)   { printf("%sRUNNING", first ? "" : ","); first = 0; }
        if (flags & IFF_MULTICAST) { printf("%sMULTICAST", first ? "" : ","); first = 0; }
    }
    printf(">");

    /* MTU */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(ctl_sock, SIOCGIFMTU, &ifr) == 0)
        printf("  mtu %d", ifr.ifr_mtu);
    printf("\n");

    /* inet addr / netmask / broadcast */
    in_addr_t addr = get_ifaddr(ifname, SIOCGIFADDR);
    in_addr_t mask = get_ifaddr(ifname, SIOCGIFNETMASK);
    in_addr_t brd  = get_ifaddr(ifname, SIOCGIFBRDADDR);

    if (addr != 0 || mask != 0) {
        struct in_addr a;
        printf("        inet ");
        a.s_addr = addr;
        printf("%s", inet_ntoa(a));
        a.s_addr = mask;
        printf("  netmask %s", inet_ntoa(a));
        if (flags & IFF_BROADCAST) {
            a.s_addr = brd;
            printf("  broadcast %s", inet_ntoa(a));
        }
        printf("\n");
    }

    /* HW addr (MAC) */
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(ctl_sock, SIOCGIFHWADDR, &ifr) == 0) {
        unsigned char *hw = (unsigned char *)ifr.ifr_hwaddr.sa_data;
        if (ifr.ifr_hwaddr.sa_family == 1) { /* ARPHRD_ETHER */
            printf("        ether %02x:%02x:%02x:%02x:%02x:%02x\n",
                   hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
        }
    }

    printf("\n");
}

/*
 * enumerate_interfaces - list all interfaces using netlink or brute-force
 *
 * Since we know xv6 has at most a handful of interfaces, we just try
 * known names.  The kernel's netlink RTM_GETLINK also works, but ioctl
 * is simpler here.
 */
static void enumerate_interfaces(void)
{
    /* Try common lwIP-style names: en0..en9, lo0 */
    const char *prefixes[] = { "en", "lo", "wl", "et", NULL };
    char name[IFNAMSIZ];
    int found = 0;

    for (int p = 0; prefixes[p] != NULL; p++) {
        for (int n = 0; n <= 9; n++) {
            snprintf(name, sizeof(name), "%s%d", prefixes[p], n);
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
            if (ioctl(ctl_sock, SIOCGIFFLAGS, &ifr) == 0) {
                print_interface(name);
                found++;
            }
        }
    }

    if (found == 0)
        printf("ifconfig: no interfaces found\n");
}

/* ── main ────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[])
{
    if (open_ctl_sock() < 0)
        return 1;

    /* No arguments: show all interfaces */
    if (argc == 1) {
        enumerate_interfaces();
        close(ctl_sock);
        return 0;
    }

    const char *ifname = argv[1];

    /* Single argument: show one interface */
    if (argc == 2) {
        /* Check interface exists */
        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
        if (ioctl(ctl_sock, SIOCGIFFLAGS, &ifr) < 0) {
            fprintf(stderr, "ifconfig: %s: No such interface\n", ifname);
            close(ctl_sock);
            return 1;
        }
        print_interface(ifname);
        close(ctl_sock);
        return 0;
    }

    /* Parse remaining arguments: addr, netmask, up, down, mtu */
    int i = 2;
    while (i < argc) {
        if (strcmp(argv[i], "up") == 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            ioctl(ctl_sock, SIOCGIFFLAGS, &ifr);
            ifr.ifr_flags |= IFF_UP;
            if (ioctl(ctl_sock, SIOCSIFFLAGS, &ifr) < 0)
                fprintf(stderr, "ifconfig: SIOCSIFFLAGS: %s\n", strerror(errno));
            i++;
        } else if (strcmp(argv[i], "down") == 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            ioctl(ctl_sock, SIOCGIFFLAGS, &ifr);
            ifr.ifr_flags &= ~IFF_UP;
            if (ioctl(ctl_sock, SIOCSIFFLAGS, &ifr) < 0)
                fprintf(stderr, "ifconfig: SIOCSIFFLAGS: %s\n", strerror(errno));
            i++;
        } else if (strcmp(argv[i], "netmask") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ifconfig: netmask requires an argument\n");
                close(ctl_sock);
                return 1;
            }
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_netmask;
            sin->sin_family = AF_INET;
            if (inet_aton(argv[i + 1], &sin->sin_addr) == 0) {
                fprintf(stderr, "ifconfig: bad netmask: %s\n", argv[i + 1]);
                close(ctl_sock);
                return 1;
            }
            if (ioctl(ctl_sock, SIOCSIFNETMASK, &ifr) < 0)
                fprintf(stderr, "ifconfig: SIOCSIFNETMASK: %s\n", strerror(errno));
            i += 2;
        } else if (strcmp(argv[i], "mtu") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "ifconfig: mtu requires an argument\n");
                close(ctl_sock);
                return 1;
            }
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            ifr.ifr_mtu = atoi(argv[i + 1]);
            if (ioctl(ctl_sock, SIOCSIFMTU, &ifr) < 0)
                fprintf(stderr, "ifconfig: SIOCSIFMTU: %s\n", strerror(errno));
            i += 2;
        } else {
            /* Treat as IP address */
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
            struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
            sin->sin_family = AF_INET;
            if (inet_aton(argv[i], &sin->sin_addr) == 0) {
                fprintf(stderr, "ifconfig: bad address: %s\n", argv[i]);
                close(ctl_sock);
                return 1;
            }
            if (ioctl(ctl_sock, SIOCSIFADDR, &ifr) < 0)
                fprintf(stderr, "ifconfig: SIOCSIFADDR: %s\n", strerror(errno));
            i++;
        }
    }

    close(ctl_sock);
    return 0;
}
