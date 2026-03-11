#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/vfs/mount_flags.h"
#include "user/user.h"

/* strncmp is not in xv6 userlib, provide a local version */
static int my_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (unsigned char)a[i] - (unsigned char)b[i];
        if (a[i] == '\0') return 0;
    }
    return 0;
}

static unsigned long parse_options(const char *opts) {
    unsigned long flags = 0;
    if (opts == 0) return 0;

    const char *p = opts;
    while (*p) {
        const char *start = p;
        while (*p && *p != ',')
            p++;
        int len = p - start;

        if (len == 7 && my_strncmp(start, "remount", 7) == 0)
            flags |= MS_REMOUNT;
        else if (len == 4 && my_strncmp(start, "move", 4) == 0)
            flags |= MS_MOVE;
        else if (len == 2 && my_strncmp(start, "ro", 2) == 0)
            flags |= MS_RDONLY;
        else if (len == 2 && my_strncmp(start, "rw", 2) == 0)
            flags &= ~MS_RDONLY;
        else if (len == 7 && my_strncmp(start, "noatime", 7) == 0)
            flags |= MS_NOATIME;
        else if (len == 6 && my_strncmp(start, "nosuid", 6) == 0)
            flags |= MS_NOSUID;
        else if (len == 5 && my_strncmp(start, "nodev", 5) == 0)
            flags |= MS_NODEV;
        else if (len == 6 && my_strncmp(start, "noexec", 6) == 0)
            flags |= MS_NOEXEC;
        /* ignore "defaults" and unknown options */

        if (*p == ',')
            p++;
    }
    return flags;
}

int main(int argc, char *argv[]) {
    unsigned long flags = 0;
    const char *source = 0;
    const char *target = 0;
    const char *fstype = 0;
    const char *options = 0;

    /* Parse arguments: mount [-o opts] <source> <target> <fstype> */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            options = argv[++i];
            i++;
        } else if (source == 0) {
            source = argv[i++];
        } else if (target == 0) {
            target = argv[i++];
        } else if (fstype == 0) {
            fstype = argv[i++];
        } else {
            i++;
        }
    }

    if (target == 0) {
        fprintf(2, "Usage: mount [-o options] <source> <target> [fstype]\n");
        fprintf(2, "\n");
        fprintf(2, "  Options: remount, move, ro, rw, defaults\n");
        fprintf(2, "  source can be:\n");
        fprintf(2, "    /dev/disk1p1           block device path\n");
        fprintf(2, "    UUID=xxxx-xxxx-...     partition GUID\n");
        fprintf(2, "    /path/to/image.ext4    file (auto loop device)\n");
        fprintf(2, "    none                   pseudo fs (tmpfs, devtmpfs)\n");
        fprintf(2, "  target: mount point directory\n");
        fprintf(2, "  fstype: filesystem type (e.g., ext4, xv6fs, tmpfs)\n");
        exit(1);
    }

    flags = parse_options(options);
    if (fstype == 0)
        fstype = "";

    if (mount(source, target, fstype, flags, 0) < 0) {
        fprintf(2, "mount: failed to mount %s on %s\n",
                source ? source : "(none)", target);
        exit(1);
    }

    exit(0);
}
