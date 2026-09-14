// Demonstrate that moving the "acquire" in iderw after the loop that
// appends to the idequeue results in a race.

// For this to work, you should also add a spin within iderw's
// idequeue traversal loop.  Adding the following demonstrated a panic
// after about 5 runs of stressfs in QEMU on a 2.1GHz CPU:
//    for (i = 0; i < 40000; i++)
//      asm volatile("");

#include "uabi/stat.h"
#include "user/user.h"
#include "uabi/fcntl.h"

static void fail(const char *operation) {
    fprintf(2, "stressfs: FAIL: %s\n", operation);
    exit(1);
}

int main(int argc, char *argv[]) {
    int fd, i;
    char path[] = "stressfs0";
    char data[512];

    printf("stressfs starting\n");
    memset(data, 'a', sizeof(data));

    int child = -1;
    for (i = 0; i < 32; i++) {
        child = fork();
        if (child < 0)
            fail("fork");
        if (child > 0)
            break;
    }
    int worker = i;

    printf("write %d\n", i);

    path[8] += i;
    fd = open(path, O_CREAT | O_TRUNC | O_RDWR);
    if (fd < 0)
        fail("open for write");
    for (i = 0; i < 20; i++)
        if (write(fd, data, sizeof(data)) != sizeof(data))
            fail("write");
    close(fd);

    printf("read\n");

    fd = open(path, O_RDONLY);
    if (fd < 0)
        fail("open for read");
    for (i = 0; i < 20; i++) {
        memset(data, 0, sizeof(data));
        if (read(fd, data, sizeof(data)) != sizeof(data))
            fail("read");
        for (int byte = 0; byte < sizeof(data); byte++)
            if (data[byte] != 'a')
                fail("data mismatch");
    }
    if (read(fd, data, 1) != 0)
        fail("file length");
    close(fd);

    if (unlink(path) < 0)
        fail("unlink");
    if (child > 0) {
        int status = -1;
        if (wait(&status) != child || status != 0)
            fail("child status");
    }
    if (worker == 0)
        printf("stressfs: ALL TESTS PASSED (33 workers)\n");

    exit(0);
}
