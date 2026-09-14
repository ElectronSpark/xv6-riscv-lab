// Exercise Rust socket ownership, checked packet buffers and DMA ring reuse.
#include "user/user.h"
#include "uabi/poll.h"
#include "uabi/stat.h"
#include "errno.h"

// The kernel's existing poll ABI uses this compact descriptor layout.
struct pollfd {
    int fd;
    short events;
    short revents;
};
extern int poll(struct pollfd *, int, int);

static int readiness(int fd, short events, int timeout_ms) {
    struct pollfd pfd = {.fd = fd, .events = events, .revents = 0};
    int result = poll(&pfd, 1, timeout_ms);
    return result < 0 ? result : pfd.revents;
}

static void fail(const char *message) {
    fprintf(2, "rustnettest: FAIL: %s\n", message);
    exit(1);
}

static volatile int caught_signal;

static void read_interrupt_handler(int signal) {
    (void)signal;
    caught_signal = 1;
    sigreturn();
}

static void interrupted_read(int fd) {
    int ready[2];
    if (pipe(ready) < 0)
        fail("interrupted-read readiness pipe");
    int child = fork();
    if (child < 0)
        fail("interrupted-read fork");
    if (child == 0) {
        close(ready[0]);
        struct sigaction action = {0}; // Deliberately no SA_RESTART.
        action.sa_handler = read_interrupt_handler;
        if (sigaction(SIGUSR1, &action, 0) < 0)
            fail("interrupted-read handler");
        if (write(ready[1], "r", 1) != 1)
            fail("interrupted-read readiness signal");
        close(ready[1]);
        char byte;
        if (read(fd, &byte, 1) != -EINTR || !caught_signal)
            fail("socket read did not return EINTR");
        close(fd);
        exit(0);
    }
    close(ready[1]);
    char byte;
    if (read(ready[0], &byte, 1) != 1)
        fail("interrupted-read child not ready");
    close(ready[0]);

    // Retry the signal if scheduling delivered it between the readiness
    // notification and entry into read. This avoids depending on a fixed
    // delay to prove that the child has actually parked in the kernel.
    for (int attempt = 0; attempt < 100; ++attempt) {
        sleep(2);
        kill(child, SIGUSR1);
        int status;
        int result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
                fail("interrupted-read child failed");
            return;
        }
        if (result < 0)
            fail("interrupted-read waitpid");
    }
    kill(child, SIGKILL);
    fail("interrupted socket read remained blocked");
}

int main(int argc, char **argv) {
    if (argc != 2)
        fail("expected host echo port");
    const uint32 host = 0x0a000202; // QEMU user-network host gateway.
    const int remote = atoi(argv[1]);
    const int local = 23001;
    int fd = connect(host, local, remote);
    if (fd < 0)
        fail("connect");
    if (connect(host, local, remote) >= 0)
        fail("duplicate endpoint accepted");

    if (readiness(fd, POLLIN | POLLRDNORM, 0) != 0)
        fail("empty socket reported readable");
    if (readiness(fd, POLLOUT | POLLWRNORM, 0) != (POLLOUT | POLLWRNORM))
        fail("socket not writable");

    char sent[2048], received[2048];
    if (write(fd, sent, sizeof(sent)) != -EMSGSIZE)
        fail("oversized write accepted");
    if (write(fd, sent, -1) != -EINVAL || read(fd, received, -1) != -EINVAL)
        fail("negative read or write count accepted");
    // The QEMU NIC advertises a 1500-byte MTU; IPv4 + UDP headers use 28.
    // 1473 bytes fit the backing mbuf but require unsupported fragmentation.
    if (write(fd, sent, 1473) != -EMSGSIZE)
        fail("UDP payload exceeding MTU accepted");
    if (write(fd, (void *)0x3fffffffffULL, 1) >= 0)
        fail("invalid user buffer accepted");

    // Both descriptor rings contain 16 slots; repeated request/reply cycles
    // exercise wraparound, reclaim, ownership transfer and receive wakeups.
    for (int packet = 0; packet < 64; ++packet) {
        int size = packet == 63 ? 1472 : 1 + (packet * 17) % 512;
        for (int i = 0; i < size; ++i)
            sent[i] = (char)(packet + i);
        if (write(fd, sent, size) != size)
            fail("UDP write");
        if (readiness(fd, POLLIN | POLLRDNORM, 1000) != (POLLIN | POLLRDNORM))
            fail("echo did not become readable");
        int got = read(fd, received, sizeof(received));
        if (got != size || memcmp(sent, received, size) != 0)
            fail("UDP payload mismatch");
    }
    if (readiness(fd, POLLIN, 0) != 0)
        fail("drained socket still readable");

    // A blocked read on a shared socket must not hold the VFS file mutex
    // needed by a different process writing that socket. Before the native
    // concurrent-I/O dispatch, this request/reply pair deadlocked.
    int ready[2];
    if (pipe(ready) < 0)
        fail("duplex readiness pipe");
    int child = fork();
    if (child < 0)
        fail("duplex fork");
    if (child == 0) {
        close(ready[0]);
        if (write(ready[1], "r", 1) != 1)
            fail("duplex readiness signal");
        close(ready[1]);
        char response[8];
        if (read(fd, response, sizeof(response)) != 6 || memcmp(response, "duplex", 6) != 0)
            fail("duplex response");
        close(fd);
        exit(0);
    }
    close(ready[1]);
    char ready_byte;
    if (read(ready[0], &ready_byte, 1) != 1)
        fail("duplex child not ready");
    close(ready[0]);
    sleep(5); // Give the child time to enter its blocking socket read.
    if (ioctl(fd, 0, 0) != -ENOTTY)
        fail("socket ioctl during blocked read");
    if (lseek(fd, 0, 0) != -EINVAL || ftruncate(fd, 0) != -EINVAL)
        fail("socket seek or truncate during blocked read");
    struct stat socket_stat;
    if (fstat(fd, &socket_stat) != 0)
        fail("socket stat during blocked read");
    if (write(fd, "duplex", 6) != 6)
        fail("duplex request");
    int status;
    if (wait(&status) != child || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("duplex child failed");

    // A signal wake leaves a channel-tree waiter for the sleeper to remove.
    // Its cleanup must regain the tree lock before touching those links.
    interrupted_read(fd);
    if (readiness(fd, POLLIN, 0) != 0)
        fail("interrupted read changed socket readiness");

    close(fd);
    // Closing a descriptor defers its last file-reference release through
    // RCU. Allow that existing reclamation protocol to finish before rebind.
    for (int attempt = 0; attempt < 100; ++attempt) {
        fd = connect(host, local, remote);
        if (fd >= 0)
            break;
        sleep(2);
    }
    if (fd < 0)
        fail("closed endpoint not released");
    close(fd);
    printf("rustnettest: ALL TESTS PASSED (64 UDP echoes)\n");
    exit(0);
}
