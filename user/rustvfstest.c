// End-to-end regression coverage for the Rust VFS path and ownership layer.
#include "uabi/stat.h"
#include "uabi/fcntl.h"
#include "errno.h"
#include "user/user.h"

static void fail(const char *message) {
    fprintf(2, "rustvfstest: FAIL: %s\n", message);
    exit(1);
}

static void expect_file(const char *path) {
    char byte = 0;
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        fail("open equivalent path");
    if (read(fd, &byte, 1) != 1 || byte != 'R')
        fail("read equivalent path");
    close(fd);
}

static void check_directory_records(const char *directory) {
    const char *linkpath = "/rvfstest/mnt/padding";
    char target[64];
    memset(target, 'P', sizeof(target) - 1);
    // This kernel stores symlink targets as absolute paths. Keep the fixture
    // absolute so normalization cannot prepend '/' and exceed the 64-byte
    // readlink buffer (the backend also reserves space for a terminator).
    target[0] = '/';
    target[sizeof(target) - 1] = 0;
    if (symlink(target, linkpath) < 0)
        fail("create padding-test symlink");

    int fd = open(directory, O_RDONLY);
    unsigned char bytes[64];
    if (fd < 0)
        fail("open directory for record checks");
    if (getdents(fd, bytes, -1) != -EINVAL)
        fail("negative getdents count must return EINVAL");
    if (getdents(fd, bytes, 0) != 0 || getdents(fd, bytes, 1) != 0)
        fail("short directory buffer changed behavior");

    int records = 0;
    for (;;) {
        // Dirty a scratch allocation in the same size class immediately
        // before getdents. Its previous raw slab buffer copied this old data
        // through the padding after each directory entry's name.
        char copied[64];
        if (readlink(linkpath, copied, sizeof(copied)) != 63 ||
            memcmp(copied, target, 63) != 0)
            fail("readlink scratch buffer contents");
        memset(bytes, 0xcc, sizeof(bytes));
        int n = getdents(fd, bytes, sizeof(bytes));
        if (n < 0 || n > sizeof(bytes))
            fail("directory read size");
        if (n == 0)
            break;
        for (int offset = 0; offset < n;) {
            if (n - offset < 19)
                fail("truncated directory record header");
            uint16 reclen;
            memmove(&reclen, bytes + offset + 16, sizeof(reclen));
            if (reclen < 20 || reclen % 8 || reclen > n - offset)
                fail("invalid directory record length");
            int end = offset + reclen;
            int name_end = offset + 19;
            while (name_end < end && bytes[name_end] != 0)
                name_end++;
            if (name_end == end)
                fail("directory name has no terminator");
            for (int i = name_end + 1; i < end; i++)
                if (bytes[i] != 0)
                    fail("directory record leaked nonzero padding");
            records++;
            offset = end;
        }
        for (int i = n; i < sizeof(bytes); i++)
            if (bytes[i] != 0xcc)
                fail("getdents wrote past returned byte count");
    }
    if (records < 3)
        fail("directory record checks did not cover multiple entries");
    close(fd);
    fd = open(directory, O_RDONLY);
    if (fd < 0 || getdents(fd, (void *)-1, sizeof(bytes)) != -EFAULT)
        fail("getdents invalid copy address");
    close(fd);
    if (readlink(linkpath, (char *)-1, 64) != -EFAULT)
        fail("readlink invalid copy address");
    if (unlink(linkpath) < 0)
        fail("remove padding-test symlink");
}

int main(void) {
    const char *base = "/rvfstest";
    const char *mounted = "/rvfstest/mnt";
    if (mkdir(base) < 0 || mkdir(mounted) < 0)
        fail("create test directories");
    if (dumpinode("/rvfstest/missing") >= 0)
        fail("diagnostic lookup accepted missing path");

    // Previously strncmp used the requested length, so "tmp" matched
    // the registered "tmpfs" type. Valid names must still mount normally.
    if (mount("", mounted, "tmp") >= 0) {
        umount(mounted);
        fail("filesystem type prefix accepted");
    }
    if (mount("", mounted, "tmpfs") < 0)
        fail("mount tmpfs");

    // A reference to an otherwise empty mounted root must block unmount.
    // Counting only cached inodes missed both open directories and cwd.
    int rootfd = open(mounted, O_RDONLY);
    if (rootfd < 0)
        fail("open mounted root");
    if (umount(mounted) >= 0)
        fail("unmounted an open root directory");
    close(rootfd);
    if (chdir(mounted) < 0)
        fail("chdir mounted root");
    if (umount(mounted) >= 0)
        fail("unmounted the working directory");
    if (chdir("/") < 0)
        fail("leave mounted root");

    int sibling = open("/rvfstest/mnt/sibling", O_CREAT | O_RDWR);
    if (sibling < 0 || write(sibling, "R", 1) != 1)
        fail("create closed sibling");
    close(sibling);
    int fd = open("/rvfstest//mnt/./file", O_CREAT | O_RDWR);
    if (fd < 0 || write(fd, "R", 1) != 1)
        fail("create using parent path components");
    if (umount(mounted) >= 0)
        fail("unmounted a filesystem with an open file");
    // A rejected unmount must leave the existing file descriptor usable.
    if (lseek(fd, 0, SEEK_SET) < 0)
        fail("seek file after rejected unmount");
    char byte = 0;
    if (read(fd, &byte, 1) != 1 || byte != 'R')
        fail("open file damaged by rejected unmount");
    expect_file("/rvfstest/mnt/sibling");
    close(fd);
    if (mkdir("/rvfstest/mnt/sub///") < 0)
        fail("mkdir with trailing slashes");
    expect_file("///rvfstest//mnt/sub/.././file");
    if (chdir("/rvfstest//mnt/sub") < 0)
        fail("chdir through repeated separators");
    expect_file("../file");
    if (chdir("/") < 0)
        fail("restore working directory");

    check_directory_records(mounted);

    int pid = fork();
    if (pid < 0)
        fail("fork");
    if (pid == 0) {
        if (chroot("/rvfstest/mnt/file") >= 0)
            fail("chroot accepted a regular file");
        expect_file("/rvfstest/mnt/file");
        if (chroot(mounted) < 0)
            fail("chroot to tmpfs");
        expect_file("/file");
        expect_file("../../../file");
        if (open("/rvfstest/mnt/file", O_RDONLY) >= 0)
            fail("chroot escaped process root");
        exit(0);
    }
    int status = -1;
    if (wait(&status) != pid || status != 0)
        fail("chroot child");

    // All references held by the child's cwd and root must be released.
    if (unlink("/rvfstest/mnt/file") < 0 || unlink("/rvfstest/mnt/sub") < 0 ||
        unlink("/rvfstest/mnt/sibling") < 0)
        fail("remove tmpfs contents");
    // Keep a nested, closed tree to distinguish internal parent references
    // from open directory/cwd references during final unmount.
    if (mkdir("/rvfstest/mnt/left") < 0 || mkdir("/rvfstest/mnt/right") < 0 ||
        mkdir("/rvfstest/mnt/left/deep") < 0)
        fail("create closed directory tree");
    if (chdir("/rvfstest/mnt/left/deep") < 0 || chdir("/") < 0)
        fail("populate cached directory parent references");
    // fdtable close can defer its last file put until an RCU grace period.
    // Retry only that busy condition, with a bounded wait for reclamation.
    int unmount_result = -EBUSY;
    const struct timespec pause = {.tv_sec = 0, .tv_nsec = 10000000};
    for (int attempt = 0; attempt < 100 && unmount_result == -EBUSY; attempt++) {
        unmount_result = umount(mounted);
        if (unmount_result == -EBUSY)
            nanosleep(&pause, 0);
    }
    if (unmount_result < 0) {
        dumpinode(mounted);
        fail("unmount after chroot child exited");
    }
    if (unlink(mounted) < 0 || unlink(base) < 0)
        fail("remove test directories");
    printf("rustvfstest: ALL TESTS PASSED\n");
    exit(0);
}
