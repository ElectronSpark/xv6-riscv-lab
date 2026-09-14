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
