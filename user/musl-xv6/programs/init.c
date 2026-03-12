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
#include <sys/mount.h>
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

/* xv6 kernel uses mkdev(major, minor) = ((major) << 20 | (minor)) */
#ifndef makedev
#define makedev(maj, min)  (((unsigned int)(maj) << 20) | (unsigned int)(min))
#endif

/*
 * xv6-specific syscalls not in POSIX: ioctl
 * musl provides ioctl(). mount() now works natively via musl's wrapper
 * since the xv6 kernel accepts the Linux-compatible 5-arg mount syscall.
 */

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

    printf("init: forking daemon: %s\n", tok_argv[0]);
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

    /* If execv fails and the file ends with ".sh", try running it via sh */
    {
        const char *name = tok_argv[0];
        int nlen = 0;
        while (name[nlen])
            nlen++;
        if (nlen >= 3 && name[nlen-3] == '.' && name[nlen-2] == 's' &&
            name[nlen-1] == 'h') {
            /* Rebuild argv as: /bin/sh <script> [original args...] */
            char *sh_argv[MAX_DAEMON_ARGS + 2];
            sh_argv[0] = "/bin/sh";
            for (int i = 0; i <= argc; i++)   /* includes NULL terminator */
                sh_argv[i + 1] = tok_argv[i];
            execv("/bin/sh", sh_argv);
        }
    }

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


/* ── /etc/fstab processing ───────────────────────────────────────────
 *
 * Reads /etc/fstab and mounts each entry.  Format (standard Linux):
 *
 *   # device      mountpoint   fstype   options   dump  pass
 *   /dev/vda      /            ext4     defaults  0     1
 *   devtmpfs      /dev         devtmpfs defaults  0     0
 *   proc          /proc        procfs   defaults  0     0
 *   tmpfs         /tmp         tmpfs    defaults  0     0
 *
 * Options: defaults, ro, rw, remount, move, noatime, nosuid, nodev,
 *          noexec, noauto.  Multiple options are comma-separated.
 *
 * Processing:
 *   1. All entries are parsed and collected.
 *   2. Entries are dependency-sorted so parent paths mount before children
 *      (e.g., "/" before "/dev", "/dev" before "/dev/pts").
 *   3. If a root ("/") entry specifies a different device from the current
 *      root, the new root is mounted at a staging directory, child
 *      filesystems are mounted/moved under it, then we chroot into it.
 *   4. Already-mounted targets are skipped (unless remount flag is set).
 */

#define FSTAB_PATH  "/etc/fstab"
#define FSTAB_MAX   4096
#define MAX_FSTAB_ENTRIES  32
#define NEWROOT_STAGING    "/mnt/newroot"

struct fstab_entry {
    char device[128];
    char mountpoint[128];
    char fstype[32];
    char options[128];
    unsigned long flags;
    int noauto;
    int depth;  /* number of '/' separators — for dependency sorting */
};

/* Parse comma-separated mount options into MS_* flags.
 * Returns the flags; sets *noauto=1 if "noauto" was present. */
static unsigned long parse_mount_options(const char *opts, int *noauto)
{
    unsigned long flags = 0;
    *noauto = 0;

    if (opts == NULL)
        return 0;

    const char *p = opts;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',')
            p++;
        int len = p - start;

        if (len == 8 && strncmp(start, "defaults", 8) == 0)
            /* nothing */;
        else if (len == 7 && strncmp(start, "remount", 7) == 0)
            flags |= MS_REMOUNT;
        else if (len == 4 && strncmp(start, "move", 4) == 0)
            flags |= MS_MOVE;
        else if (len == 2 && strncmp(start, "ro", 2) == 0)
            flags |= MS_RDONLY;
        else if (len == 2 && strncmp(start, "rw", 2) == 0)
            flags &= ~(unsigned long)MS_RDONLY;
        else if (len == 7 && strncmp(start, "noatime", 7) == 0)
            flags |= MS_NOATIME;
        else if (len == 6 && strncmp(start, "nosuid", 6) == 0)
            flags |= MS_NOSUID;
        else if (len == 5 && strncmp(start, "nodev", 5) == 0)
            flags |= MS_NODEV;
        else if (len == 6 && strncmp(start, "noexec", 6) == 0)
            flags |= MS_NOEXEC;
        else if (len == 6 && strncmp(start, "noauto", 6) == 0)
            *noauto = 1;
        /* ignore unknown options */

        if (*p == ',')
            p++;
    }
    return flags;
}

/* Check if target is already mounted by comparing the device of the
 * mountpoint to its parent.  stat(target).st_dev != stat(parent).st_dev
 * indicates a mount boundary. */
static int is_mounted(const char *target)
{
    struct stat st_target, st_parent;

    if (stat(target, &st_target) < 0)
        return 0;

    /* Build parent path: strip last component */
    char parent[256];
    int len = strlen(target);
    if (len >= (int)sizeof(parent))
        return 0;
    memcpy(parent, target, len + 1);

    /* Remove trailing slashes */
    while (len > 1 && parent[len - 1] == '/')
        parent[--len] = '\0';

    /* Find last '/' */
    char *slash = parent;
    char *last_slash = NULL;
    while (*slash) {
        if (*slash == '/')
            last_slash = slash;
        slash++;
    }
    if (last_slash == NULL)
        return 0;
    if (last_slash == parent)
        parent[1] = '\0';  /* parent is "/" */
    else
        *last_slash = '\0';

    if (stat(parent, &st_parent) < 0)
        return 0;

    return st_target.st_dev != st_parent.st_dev;
}

/* Count the depth of a mountpoint path (number of path components).
 * Used for dependency sorting: "/" = 0, "/dev" = 1, "/dev/pts" = 2. */
static int path_depth(const char *path)
{
    if (path[0] == '/' && path[1] == '\0')
        return 0;
    int depth = 0;
    const char *p = path;
    while (*p) {
        if (*p == '/' && p[1] != '\0' && p[1] != '/')
            depth++;
        p++;
    }
    return depth;
}

/* Simple insertion sort of fstab entries by depth (stable). */
static void sort_fstab_entries(struct fstab_entry *entries, int n)
{
    for (int i = 1; i < n; i++) {
        struct fstab_entry tmp = entries[i];
        int j = i - 1;
        while (j >= 0 && entries[j].depth > tmp.depth) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = tmp;
    }
}

/* Get the dev_t of the current root filesystem. */
static dev_t get_root_dev(void)
{
    struct stat st;
    if (stat("/", &st) < 0)
        return 0;
    return st.st_dev;
}

/* Get the dev_t of a block device node (or a pseudo name for virtual FS).
 * For real block devices like /dev/vda, returns stat().st_rdev.
 * For pseudo filesystems (devtmpfs, proc, tmpfs), returns 0. */
static dev_t get_device_dev(const char *device)
{
    struct stat st;
    if (device[0] == '/' && stat(device, &st) == 0) {
        /* Real device node */
        if (S_ISBLK(st.st_mode))
            return st.st_rdev;
    }
    return 0;
}

/* Recursively create directories for a path (like mkdir -p). */
static void mkdir_p(const char *path, mode_t mode)
{
    char tmp[256];
    int len = strlen(path);
    if (len >= (int)sizeof(tmp))
        return;
    memcpy(tmp, path, len + 1);

    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    mkdir(tmp, mode);
}

static void process_fstab(void)
{
    int fd = open(FSTAB_PATH, O_RDONLY);
    if (fd < 0)
        return;

    char buf[FSTAB_MAX];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return;
    buf[n] = '\0';

    /* ── Phase 1: Parse all fstab entries ─────────────────────────── */
    struct fstab_entry entries[MAX_FSTAB_ENTRIES];
    int nentries = 0;

    char *line = buf;
    while (*line && nentries < MAX_FSTAB_ENTRIES) {
        char *eol = line;
        while (*eol && *eol != '\n')
            eol++;
        char saved = *eol;
        *eol = '\0';

        const char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;

        if (*p != '\0' && *p != '#') {
            /* Tokenize: device mountpoint fstype options dump pass */
            const char *fields[6];
            int field_lens[6];
            int nfields = 0;
            const char *fp = p;

            while (*fp && nfields < 6) {
                while (*fp == ' ' || *fp == '\t')
                    fp++;
                if (*fp == '\0')
                    break;
                fields[nfields] = fp;
                const char *fs = fp;
                while (*fp && *fp != ' ' && *fp != '\t')
                    fp++;
                field_lens[nfields] = fp - fs;
                nfields++;
            }

            if (nfields >= 3) {
                struct fstab_entry *e = &entries[nentries];

                int dlen = field_lens[0] < 127 ? field_lens[0] : 127;
                memcpy(e->device, fields[0], dlen);
                e->device[dlen] = '\0';

                int mlen = field_lens[1] < 127 ? field_lens[1] : 127;
                memcpy(e->mountpoint, fields[1], mlen);
                e->mountpoint[mlen] = '\0';

                int flen = field_lens[2] < 31 ? field_lens[2] : 31;
                memcpy(e->fstype, fields[2], flen);
                e->fstype[flen] = '\0';

                if (nfields >= 4) {
                    int olen = field_lens[3] < 127 ? field_lens[3] : 127;
                    memcpy(e->options, fields[3], olen);
                    e->options[olen] = '\0';
                } else {
                    strcpy(e->options, "defaults");
                }

                e->flags = parse_mount_options(e->options, &e->noauto);
                e->depth = path_depth(e->mountpoint);
                nentries++;
            }
        }

        *eol = saved;
        if (saved == '\0')
            break;
        line = eol + 1;
    }

    if (nentries == 0)
        return;

    /* ── Phase 2: Sort by dependency (path depth) ─────────────────── */
    sort_fstab_entries(entries, nentries);

    /* ── Phase 3: Detect root change ──────────────────────────────── */
    /* Check if any entry mounts "/" and whether it's a different device
     * from the currently running root filesystem. */
    int root_idx = -1;
    int need_chroot = 0;

    for (int i = 0; i < nentries; i++) {
        if (strcmp(entries[i].mountpoint, "/") == 0) {
            root_idx = i;
            break;
        }
    }

    if (root_idx >= 0 && !entries[root_idx].noauto) {
        dev_t cur_root = get_root_dev();
        dev_t new_root = get_device_dev(entries[root_idx].device);

        if (new_root != 0 && new_root != cur_root) {
            /* Root device positively differs — mount at staging and chroot */
            need_chroot = 1;
            printf("init: fstab: root change detected: current dev %u:%u "
                   "-> new dev %u:%u (%s)\n",
                   (cur_root >> 20) & 0xFFF, cur_root & 0xFFFFF,
                   (new_root >> 20) & 0xFFF, new_root & 0xFFFFF,
                   entries[root_idx].device);
        }
        /* If new_root == 0 (can't determine device) or new_root == cur_root
         * (same device), assume root is already correct — no chroot needed. */
    }

    /* ── Phase 4: Mount filesystems ───────────────────────────────── */
    if (need_chroot) {
        /*
         * Root change workflow:
         *   1. Create staging directory (NEWROOT_STAGING)
         *   2. Mount new root at staging
         *   3. For each child mount: if already mounted on current root,
         *      use MS_MOVE to move it under staging; otherwise mount fresh
         *   4. chdir + chroot into staging
         */
        mkdir_p(NEWROOT_STAGING, 0755);

        /* Mount new root at staging */
        struct fstab_entry *re = &entries[root_idx];
        int ret = mount(re->device, NEWROOT_STAGING, re->fstype,
                        re->flags & ~(unsigned long)MS_REMOUNT, NULL);
        if (ret < 0) {
            printf("init: fstab: failed to mount new root %s at %s (%s)\n",
                   re->device, NEWROOT_STAGING, re->fstype);
            /* Fall through to mount remaining entries normally */
            need_chroot = 0;
        } else {
            printf("init: fstab: mounted new root %s at %s (%s)\n",
                   re->device, NEWROOT_STAGING, re->fstype);
        }
    }

    /* Process non-root entries (or all entries if no chroot needed) */
    for (int i = 0; i < nentries; i++) {
        struct fstab_entry *e = &entries[i];

        if (e->noauto)
            continue;

        /* Skip the root entry — already handled above */
        if (strcmp(e->mountpoint, "/") == 0) {
            if (!need_chroot && !(e->flags & MS_REMOUNT)) {
                /* Same root device, nothing to do */
                continue;
            }
            if (!need_chroot && (e->flags & MS_REMOUNT)) {
                /* Remount current root with new flags */
                int ret = mount(e->device, "/", e->fstype,
                                e->flags, NULL);
                if (ret < 0)
                    printf("init: fstab: failed to remount /\n");
                else
                    printf("init: fstab: remounted /\n");
                continue;
            }
            /* If need_chroot, root was already mounted at staging */
            continue;
        }

        if (need_chroot) {
            /* Build the target path under the staging root */
            char staged_target[256];
            int slen = snprintf(staged_target, sizeof(staged_target),
                                "%s%s", NEWROOT_STAGING, e->mountpoint);
            if (slen >= (int)sizeof(staged_target))
                continue;

            /* Ensure the mount directory exists under the new root */
            mkdir_p(staged_target, 0755);

            /* If this filesystem is already mounted on the *current* root,
             * move it to the new root instead of mounting a second instance.
             * This is important for /dev and /proc which the kernel already
             * mounted and which should not be mounted twice. */
            if (is_mounted(e->mountpoint)) {
                int ret = mount(e->mountpoint, staged_target, NULL,
                                MS_MOVE, NULL);
                if (ret == 0) {
                    printf("init: fstab: moved %s -> %s\n",
                           e->mountpoint, staged_target);
                } else {
                    printf("init: fstab: move %s -> %s failed, "
                           "trying fresh mount\n",
                           e->mountpoint, staged_target);
                    ret = mount(e->device, staged_target, e->fstype,
                                e->flags, NULL);
                    if (ret == 0)
                        printf("init: fstab: mounted %s on %s (%s)\n",
                               e->device, staged_target, e->fstype);
                    else
                        printf("init: fstab: failed to mount %s on %s\n",
                               e->device, staged_target);
                }
            } else {
                int ret = mount(e->device, staged_target, e->fstype,
                                e->flags, NULL);
                if (ret < 0)
                    printf("init: fstab: failed to mount %s on %s (%s)\n",
                           e->device, staged_target, e->fstype);
                else
                    printf("init: fstab: mounted %s on %s (%s)\n",
                           e->device, staged_target, e->fstype);
            }
        } else {
            /* Normal (no chroot) — mount directly */
            if (is_mounted(e->mountpoint) &&
                !(e->flags & MS_REMOUNT) && !(e->flags & MS_MOVE)) {
                printf("init: fstab: %s already mounted, skipping\n",
                       e->mountpoint);
                continue;
            }

            int ret = mount(e->device, e->mountpoint, e->fstype,
                            e->flags, NULL);
            if (ret < 0)
                printf("init: fstab: failed to mount %s on %s (%s)\n",
                       e->device, e->mountpoint, e->fstype);
            else
                printf("init: fstab: mounted %s on %s (%s)\n",
                       e->device, e->mountpoint, e->fstype);
        }
    }

    /* ── Phase 5: chroot into new root ────────────────────────────── */
    if (need_chroot) {
        /* Switch working directory and root to the new root */
        if (chdir(NEWROOT_STAGING) < 0) {
            printf("init: fstab: chdir(%s) failed\n", NEWROOT_STAGING);
            return;
        }
        if (chroot(NEWROOT_STAGING) < 0) {
            printf("init: fstab: chroot(%s) failed\n", NEWROOT_STAGING);
            return;
        }
        /* After chroot, "/" is now the new root */
        chdir("/");
        printf("init: fstab: chroot to new root successful\n");

        /* Re-open console so fd 0/1/2 point into the new root */
        close(STDIN_FILENO);
        close(STDOUT_FILENO);
        close(STDERR_FILENO);
        open("/dev/console", O_RDWR);
        dup(0);
        dup(0);
    }
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

    /* Process /etc/fstab — mount filesystems as configured.
     * The kernel only mounts devtmpfs at /dev during boot.
     * Other filesystems (/, /proc, /tmp, etc.) are handled here.
     * Already-mounted targets are skipped unless "remount" is specified. */
    process_fstab();

    /* Configure network (reads /etc/network.conf, writes to /dev/netconf) */
    configure_network();

    /* Wait for DHCP/static config to complete, then update /etc/resolv.conf
     * with the DNS server obtained from DHCP or the config file. */
    update_resolv_conf();

    /* Ensure /var/empty is owned by root with correct permissions.
     * sshd requires this for privilege separation. */
    chown("/var/empty", 0, 0);
    chmod("/var/empty", 0755);

    /* Set up environment variables inherited by all children (daemons,
     * getty, login shells).  The kernel starts init with an empty environ. */
    setenv("PATH",    "/bin:/sbin:/usr/bin:/usr/local/bin", 1);
    setenv("HOME",    "/root", 1);
    setenv("TERM",    "xterm", 1);
    setenv("SHELL",   "/bin/sh", 1);
    setenv("LOGNAME", "root", 1);
    setenv("USER",    "root", 1);
    setenv("LANG",    "C.UTF-8", 1);
    /* Python looks for /lib/python3.12 (symlinked to /usr/local/lib/python3.12) */
    setenv("PYTHONHOME", "/", 1);
    /* Mozilla CA bundle for TLS certificate verification */
    setenv("SSL_CERT_FILE", "/etc/ssl/certs/ca-certificates.crt", 1);

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
