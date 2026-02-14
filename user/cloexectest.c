#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static int check_fd_closed_after_exec(int fd) {
    char ch;
    int n = read(fd, &ch, 1);
    if (n >= 0) {
        fprintf(2, "cloexectest: fd %d still open after exec\n", fd);
        return -1;
    }
    return 0;
}

static int run_exec_check(int fd) {
    char fdstr[16];
    char *argv[4];

    snprintf(fdstr, sizeof(fdstr), "%d", fd);
    argv[0] = "cloexectest";
    argv[1] = "check";
    argv[2] = fdstr;
    argv[3] = 0;

    exec("/bin/cloexectest", argv);
    fprintf(2, "cloexectest: exec failed\n");
    return -1;
}

static int case_open_cloexec(void) {
    int fd = open("/bin/sh", O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(2, "cloexectest: open(O_CLOEXEC) failed\n");
        return -1;
    }

    int fdf = fcntl(fd, F_GETFD, 0);
    if (fdf < 0 || !(fdf & FD_CLOEXEC)) {
        fprintf(2, "cloexectest: F_GETFD missing FD_CLOEXEC\n");
        close(fd);
        return -1;
    }

    return run_exec_check(fd);
}

static int case_setfd_cloexec(void) {
    int fd = open("/bin/sh", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "cloexectest: open failed\n");
        return -1;
    }

    if (fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
        fprintf(2, "cloexectest: F_SETFD failed\n");
        close(fd);
        return -1;
    }

    return run_exec_check(fd);
}

static int case_dupfd_cloexec(void) {
    int fd = open("/bin/sh", O_RDONLY);
    if (fd < 0) {
        fprintf(2, "cloexectest: open failed\n");
        return -1;
    }

    int newfd = fcntl(fd, F_DUPFD_CLOEXEC, 5);
    close(fd);
    if (newfd < 0) {
        fprintf(2, "cloexectest: F_DUPFD_CLOEXEC failed\n");
        return -1;
    }

    return run_exec_check(newfd);
}

static int run_case(int which) {
    int pid = fork();
    if (pid < 0) {
        fprintf(2, "cloexectest: fork failed\n");
        return -1;
    }
    if (pid == 0) {
        int rc = -1;
        if (which == 0) {
            rc = case_open_cloexec();
        } else if (which == 1) {
            rc = case_setfd_cloexec();
        } else if (which == 2) {
            rc = case_dupfd_cloexec();
        }
        exit(rc == 0 ? 0 : 1);
    }

    int st = 0;
    if (wait(&st) < 0) {
        return -1;
    }
    return st;
}

int main(int argc, char **argv) {
    if (argc == 3 && strcmp(argv[1], "check") == 0) {
        int fd = atoi(argv[2]);
        int rc = check_fd_closed_after_exec(fd);
        exit(rc == 0 ? 0 : 1);
    }

    if (run_case(0) != 0) {
        fprintf(2, "cloexectest: O_CLOEXEC case failed\n");
        exit(1);
    }
    if (run_case(1) != 0) {
        fprintf(2, "cloexectest: F_SETFD case failed\n");
        exit(1);
    }
    if (run_case(2) != 0) {
        fprintf(2, "cloexectest: F_DUPFD_CLOEXEC case failed\n");
        exit(1);
    }

    printf("cloexectest: ok\n");
    exit(0);
}
