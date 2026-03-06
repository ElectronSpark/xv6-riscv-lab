/*
 * init.c — musl libc version for xv6
 *
 * The initial user-level program. Opens console, creates device nodes,
 * configures the network, and spawns the shell in a loop.
 *
 * Uses standard POSIX headers. Device major/minor numbers are
 * xv6-specific constants (not kernel headers).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <arpa/inet.h>

/*
 * xv6 device major/minor numbers — these are xv6-specific constants.
 * Copied here to avoid referencing kernel headers.
 */
#define CONSOLE_MAJOR   1
#define CONSOLE_MINOR   1
#define NULL_MAJOR      1
#define NULL_MINOR      2
#define RANDOM_MAJOR    1
#define RANDOM_MINOR    3
#define TTY_DEV_MAJOR   5
#define TTY_DEV_MINOR   1
#define DISK1_MAJOR     2
#define DISK1_MINOR     2

/* xv6 ioctl for setting controlling terminal */
#ifndef TIOCSCTTY
#define TIOCSCTTY       0x540E
#endif

/* xv6 uses mkdev(major, minor) = ((major) << 16) | (minor) */
#ifndef makedev
#define makedev(maj, min)  (((unsigned int)(maj) << 16) | (unsigned int)(min))
#endif

/*
 * xv6-specific syscalls not in POSIX: mount, ioctl
 * musl provides ioctl(). For mount(), we use a raw syscall.
 */

/* Forward declaration — mount is defined via syscall since musl's
 * mount() expects Linux-style args. We use a thin inline wrapper. */
static long xv6_mount(const char *source, const char *target, const char *fstype)
{
    /* xv6 SYS_mount = 34, args: a0=source, a1=target, a2=fstype */
    return syscall(34, source, target, fstype);
}

static char *argv[] = { "sh", NULL };

/* ── /etc/daemons launcher ─────────────────────────────────────────────
 *
 * Reads /etc/daemons line by line.  Each non-empty, non-comment line is
 * a command (optionally with arguments) to launch as a background daemon.
 *
 * Format:
 *   # comment
 *   /bin/telnetd
 *   /bin/sshd -D
 *
 * Each daemon is fork()+exec()'d into its own process group (via
 * setpgid(0,0)) so signals can be directed at individual daemons
 * without hitting the whole session.
 *
 * Daemon stdout/stderr go to /dev/console for diagnostics.
 */

#define DAEMONS_PATH   "/etc/daemons"
#define MAX_DAEMON_ARGS 16
#define MAX_LINE        256

static void start_daemon(const char *line)
{
    /* Tokenise the line into argv[] (modifies a local copy) */
    char buf[MAX_LINE];
    strncpy(buf, line, MAX_LINE - 1);
    buf[MAX_LINE - 1] = '\0';

    char *tok_argv[MAX_DAEMON_ARGS + 1];
    int argc = 0;
    char *p = buf;

    while (*p && argc < MAX_DAEMON_ARGS) {
        /* skip whitespace */
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '#')
            break;
        tok_argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t')
            p++;
        if (*p)
            *p++ = '\0';
    }
    if (argc == 0)
        return;
    tok_argv[argc] = NULL;

    pid_t pid = fork();
    if (pid < 0) {
        printf("init: fork failed for daemon: %s\n", tok_argv[0]);
        return;
    }
    if (pid != 0) {
        /* Parent — daemon launched */
        printf("init: started daemon %s (pid %d)\n", tok_argv[0], pid);
        return;
    }

    /* ── Child ─────────────────────────────────────────────────────── */

    /* Create a new process group for this daemon */
    setpgid(0, 0);

    /* Re-open stdio on /dev/console so daemon output is visible */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/console", O_RDWR);  /* fd 0 */
    dup(0);                        /* fd 1 */
    dup(0);                        /* fd 2 */

    execv(tok_argv[0], tok_argv);
    /* exec failed — only reaches here on error */
    printf("init: exec failed for daemon: %s\n", tok_argv[0]);
    _exit(1);
}

static void start_daemons(void)
{
    int fd = open(DAEMONS_PATH, O_RDONLY);
    if (fd < 0) {
        /* No /etc/daemons — nothing to start */
        return;
    }

    char filebuf[2048];
    ssize_t n = read(fd, filebuf, sizeof(filebuf) - 1);
    close(fd);

    if (n <= 0)
        return;
    filebuf[n] = '\0';

    /* Process line by line */
    char *line = filebuf;
    while (*line) {
        /* Find end of line */
        char *eol = line;
        while (*eol && *eol != '\n')
            eol++;

        char saved = *eol;
        *eol = '\0';

        /* Skip leading whitespace */
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        /* Skip empty lines and comments */
        if (*p != '\0' && *p != '#')
            start_daemon(p);

        if (saved == '\0')
            break;
        line = eol + 1;
    }
}


/* ── /dev/netconf integration ────────────────────────────────────────── */

/* Mirror of kernel/inc/dev/netconf.h (no kernel headers in musl builds) */
#define NETCONF_MODE_DHCP    0
#define NETCONF_MODE_STATIC  1
#define NETCONF_HOSTNAME_MAX 32

struct netconf_req {
    int          mode;
    unsigned int ip;
    unsigned int netmask;
    unsigned int gateway;
    unsigned int dns;
    char         hostname[NETCONF_HOSTNAME_MAX];
};

#define NET_CONF_PATH  "/etc/network.conf"

/* SYS_netconf = 161 (xv6-specific) — fallback if /dev/netconf missing */
#define SYS_netconf    161

/* ── config file parsing helpers ─────────────────────────────────────── */

/* Parse a dotted-quad IP string into a uint32 in network byte order.
 * Returns 0 on success, -1 on failure. */
static int parse_ip4(const char *s, unsigned int *out)
{
    struct in_addr addr;
    /* inet_aton handles dotted-quad → network byte order */
    if (inet_aton(s, &addr) == 0)
        return -1;
    *out = addr.s_addr;
    return 0;
}

/* Skip leading whitespace */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;
    return s;
}

/* Copy value string into dst, stripping trailing whitespace/comments */
static void copy_value(char *dst, int maxlen, const char *src)
{
    int i = 0;
    while (i < maxlen - 1 && src[i] != '\0' && src[i] != '\n' &&
           src[i] != '\r' && src[i] != '#')
        i++;
    while (i > 0 && (src[i - 1] == ' ' || src[i - 1] == '\t'))
        i--;
    memcpy(dst, src, i);
    dst[i] = '\0';
}

/* Find the first occurrence of a NUL-terminated string needle in the
 * first n bytes of s.  We avoid pulling in strstr(). */
static int starts_with(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s != *prefix)
            return 0;
        s++;
        prefix++;
    }
    return 1;
}

/* Read /etc/network.conf, parse it, and write to /dev/netconf device */
static void configure_network(void)
{
    struct netconf_req req;
    memset(&req, 0, sizeof(req));
    req.mode = NETCONF_MODE_DHCP;  /* default */
    strncpy(req.hostname, "xv6", NETCONF_HOSTNAME_MAX);

    int fd = open(NET_CONF_PATH, O_RDONLY);
    if (fd >= 0) {
        char buf[1024];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);

        if (n > 0) {
            buf[n] = '\0';

            /* Parse line by line */
            char *line = buf;
            while (*line) {
                char *eol = line;
                while (*eol && *eol != '\n')
                    eol++;

                char saved = *eol;
                *eol = '\0';

                const char *p = skip_ws(line);

                /* Skip empty lines and comments */
                if (*p != '\0' && *p != '#') {
                    if (starts_with(p, "mode=")) {
                        const char *val = skip_ws(p + 5);
                        if (starts_with(val, "static"))
                            req.mode = NETCONF_MODE_STATIC;
                        else
                            req.mode = NETCONF_MODE_DHCP;
                    } else if (starts_with(p, "ip=")) {
                        parse_ip4(skip_ws(p + 3), &req.ip);
                    } else if (starts_with(p, "netmask=")) {
                        parse_ip4(skip_ws(p + 8), &req.netmask);
                    } else if (starts_with(p, "gateway=")) {
                        parse_ip4(skip_ws(p + 8), &req.gateway);
                    } else if (starts_with(p, "dns=")) {
                        parse_ip4(skip_ws(p + 4), &req.dns);
                    } else if (starts_with(p, "hostname=")) {
                        copy_value(req.hostname, NETCONF_HOSTNAME_MAX,
                                   skip_ws(p + 9));
                    }
                }

                *eol = saved;
                if (saved == '\0')
                    break;
                line = eol + 1;
            }
        }
    }

    /* Apply configuration via /dev/netconf device (created by devtmpfs) */
    int nfd = open("/dev/netconf", O_WRONLY);
    if (nfd < 0) {
        /* Fall back to netconf() syscall if device not yet available */
        long ret = syscall(SYS_netconf, &req);
        if (ret < 0)
            printf("init: netconf syscall failed (%ld)\n", ret);
        else
            printf("init: network configured via syscall (%s)\n",
                   req.mode == NETCONF_MODE_DHCP ? "dhcp" : "static");
        return;
    }

    ssize_t ret = write(nfd, &req, sizeof(req));
    close(nfd);

    if (ret < 0)
        printf("init: /dev/netconf write failed\n");
    else
        printf("init: network configured (%s)\n",
               req.mode == NETCONF_MODE_DHCP ? "dhcp" : "static");
}

/*
 * update_resolv_conf — wait for lwIP to finish configuring, read back
 * the active DNS server from /dev/netconf, and write /etc/resolv.conf.
 *
 * The kernel returns -EAGAIN from read() until lwIP has an IP and DNS.
 * We poll with a short sleep, giving DHCP time to complete.
 *
 * This runs in a forked child so the shell starts immediately.
 */
#define SIOCNETCONF_GET  0x89F1
#define RESOLV_POLL_MS   2000
#define RESOLV_TIMEOUT   120000   /* 120 s — plenty for link + DHCP */

static void update_resolv_conf(void)
{
    int pid = fork();
    if (pid != 0)
        return;  /* parent continues immediately */

    /* Child: wait for DNS and update resolv.conf, then exit */
    int nfd = open("/dev/netconf", O_RDONLY);
    if (nfd < 0)
        _exit(0);

    struct netconf_req active;
    int elapsed = 0;

    while (elapsed < RESOLV_TIMEOUT) {
        ssize_t n = read(nfd, &active, sizeof(active));
        if (n == (ssize_t)sizeof(active) && active.dns != 0)
            break;
        /* -EAGAIN or no DNS yet — keep polling */
        usleep(RESOLV_POLL_MS * 1000);
        elapsed += RESOLV_POLL_MS;
    }
    close(nfd);

    if (elapsed >= RESOLV_TIMEOUT || active.dns == 0) {
        printf("init: timeout waiting for DNS (resolv.conf not updated)\n");
        _exit(0);
    }

    /* Format DNS IP address */
    unsigned char *d = (unsigned char *)&active.dns;
    char line[64];
    int len = snprintf(line, sizeof(line), "nameserver %d.%d.%d.%d\n",
                       d[0], d[1], d[2], d[3]);

    /* Rename the original resolv.conf (ignore errors — may not exist) */
    rename("/etc/resolv.conf", "/etc/resolv.conf.bak");

    /* Write the new resolv.conf */
    int fd = open("/etc/resolv.conf", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        write(fd, line, len);
        close(fd);
    }
    printf("init: resolv.conf updated (DNS %d.%d.%d.%d)\n",
           d[0], d[1], d[2], d[3]);
    _exit(0);
}

int main(void)
{
    int pid, wpid;

    /* Open console as stdin (fd 0), stdout (fd 1), stderr (fd 2) */
    if (open("/dev/console", O_RDWR) < 0) {
        mknod("/dev/console", S_IFCHR | 0666, makedev(CONSOLE_MAJOR, CONSOLE_MINOR));
        open("/dev/console", O_RDWR);
    }
    dup(0);  /* stdout (fd 1) */
    dup(0);  /* stderr (fd 2) */

    /* Set the console as the controlling terminal */
    if (ioctl(0, TIOCSCTTY, (void *)0) < 0)
        printf("init: warning: TIOCSCTTY failed\n");

    /* Ensure device nodes exist (devtmpfs creates them automatically;
     * mknod errors are silently ignored) */
    mknod("/dev/null",    S_IFCHR | 0666, makedev(NULL_MAJOR, NULL_MINOR));
    mknod("/dev/random",  S_IFCHR | 0666, makedev(RANDOM_MAJOR, RANDOM_MINOR));
    mknod("/dev/urandom", S_IFCHR | 0666, makedev(RANDOM_MAJOR, RANDOM_MINOR));
    mknod("/dev/tty",     S_IFCHR | 0666, makedev(TTY_DEV_MAJOR, TTY_DEV_MINOR));

    /* Configure network (reads /etc/network.conf, writes to /dev/netconf) */
    configure_network();

    /* Wait for DHCP/static config to complete, then update /etc/resolv.conf
     * with the DNS server obtained from DHCP or the config file. */
    update_resolv_conf();

    /* Ensure /var/empty is owned by root with correct permissions.
     * sshd requires this for privilege separation. */
    chown("/var/empty", 0, 0);
    chmod("/var/empty", 0755);

    /* Start daemon processes listed in /etc/daemons (each in its own pgrp) */
    start_daemons();

    for (;;) {
        printf("init: starting getty on /dev/console\n");
        pid = fork();
        if (pid < 0) {
            printf("init: fork failed\n");
            _exit(1);
        }
        if (pid == 0) {
            char *getty_argv[] = { "getty", "/dev/console", NULL };
            execv("/bin/getty", getty_argv);
            /* If getty not found, fall back to login */
            char *login_argv[] = { "login", NULL };
            execv("/bin/login", login_argv);
            /* Last resort: shell */
            execv("/bin/sh", argv);
            printf("init: exec failed\n");
            _exit(1);
        }

        for (;;) {
            wpid = wait(NULL);
            if (wpid == pid) {
                break;
            } else if (wpid < 0) {
                printf("init: wait returned an error\n");
                _exit(1);
            }
            /* else: reaped a parentless process, continue */
        }
    }
}
