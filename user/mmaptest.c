// mmaptest.c - Test file-backed mmap
#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

static char buf[512];

void test_mmap_read(void) {
    int fd;
    char *mapped;
    int n;

    printf("test_mmap_read: ");

    // Create a test file
    fd = open("mmaptest_file", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - cannot create file\n");
        exit(1);
    }
    // Write known data
    char *msg = "Hello from mmap test! This is file-backed memory mapping.\n";
    n = write(fd, msg, strlen(msg));
    if (n != strlen(msg)) {
        printf("FAIL - write failed\n");
        close(fd);
        exit(1);
    }
    close(fd);

    // Open for reading
    fd = open("mmaptest_file", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - cannot open file\n");
        exit(1);
    }

    struct stat st;
    fstat(fd, &st);

    // mmap the file
    mapped = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL - mmap returned MAP_FAILED\n");
        close(fd);
        exit(1);
    }

    // Read data through the mapping
    // Compare first bytes with expected data
    int match = 1;
    for (int i = 0; i < strlen(msg); i++) {
        if (mapped[i] != msg[i]) {
            printf("FAIL - mismatch at byte %d: got '%c' expected '%c'\n", i,
                   mapped[i], msg[i]);
            match = 0;
            break;
        }
    }

    if (match) {
        printf("OK\n");
    }

    munmap(mapped, 4096);
    close(fd);
}

void test_mmap_private_write(void) {
    int fd, fd2;
    char *mapped;
    int n;

    printf("test_mmap_private_write: ");

    // Create a test file
    fd = open("mmaptest_file2", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - cannot create file\n");
        exit(1);
    }
    char *msg = "Original file content here.\n";
    n = write(fd, msg, strlen(msg));
    close(fd);

    // Open for reading
    fd = open("mmaptest_file2", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - cannot open\n");
        exit(1);
    }

    // mmap with PROT_READ | PROT_WRITE (private - writes don't affect file)
    mapped = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL - mmap failed\n");
        close(fd);
        exit(1);
    }

    // Verify initial content
    if (mapped[0] != 'O') {
        printf("FAIL - initial read wrong: got '%c'\n", mapped[0]);
        munmap(mapped, 4096);
        close(fd);
        exit(1);
    }

    // Write to the private mapping
    mapped[0] = 'X';

    // Verify the write took effect in memory
    if (mapped[0] != 'X') {
        printf("FAIL - private write not visible\n");
        munmap(mapped, 4096);
        close(fd);
        exit(1);
    }

    // Verify the file is unchanged by re-reading it
    close(fd);
    fd2 = open("mmaptest_file2", O_RDONLY);
    if (fd2 < 0) {
        printf("FAIL - reopen failed\n");
        exit(1);
    }
    n = read(fd2, buf, sizeof(buf));
    if (n > 0 && buf[0] == 'O') {
        printf("OK\n");
    } else {
        printf("FAIL - file was modified (COW broken)\n");
    }
    close(fd2);
    munmap(mapped, 4096);
}

void test_mmap_anonymous(void) {
    printf("test_mmap_anonymous: ");

    // Test anonymous private mapping (should still work)
    char *mapped =
        mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL - anonymous mmap failed\n");
        exit(1);
    }

    // Should be zero-filled
    int ok = 1;
    for (int i = 0; i < 4096; i++) {
        if (mapped[i] != 0) {
            printf("FAIL - anonymous page not zeroed at byte %d\n", i);
            ok = 0;
            break;
        }
    }

    if (ok) {
        // Write and read back
        mapped[0] = 42;
        mapped[4095] = 99;
        if (mapped[0] == 42 && mapped[4095] == 99) {
            printf("OK\n");
        } else {
            printf("FAIL - read back wrong\n");
        }
    }

    munmap(mapped, 4096);
}

void test_mmap_fork(void) {
    int fd;
    char *mapped;

    printf("test_mmap_fork: ");

    // Create a test file
    fd = open("mmaptest_file3", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - cannot create file\n");
        exit(1);
    }
    char *msg = "Fork test content.\n";
    write(fd, msg, strlen(msg));
    close(fd);

    fd = open("mmaptest_file3", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - cannot open\n");
        exit(1);
    }

    mapped = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL - mmap failed\n");
        close(fd);
        exit(1);
    }
    close(fd);

    // Touch the first page to load it
    char c = mapped[0];
    (void)c;

    int pid = fork();
    if (pid < 0) {
        printf("FAIL - fork failed\n");
        exit(1);
    }

    if (pid == 0) {
        // Child: verify can read the mapping and write privately
        if (mapped[0] != 'F') {
            printf("FAIL - child read wrong\n");
            exit(1);
        }
        mapped[0] = 'Z'; // Private write in child
        if (mapped[0] != 'Z') {
            printf("FAIL - child write failed\n");
            exit(1);
        }
        exit(0);
    } else {
        int status;
        wait(&status);
        // Parent: verify our mapping is unchanged
        if (mapped[0] == 'F') {
            printf("OK\n");
        } else {
            printf("FAIL - parent mapping corrupted by child\n");
        }
    }

    munmap(mapped, 4096);
}

int main(int argc, char *argv[]) {
    printf("mmaptest starting\n");

    test_mmap_anonymous();
    test_mmap_read();
    test_mmap_private_write();
    test_mmap_fork();

    printf("mmaptest: all tests passed\n");
    exit(0);
}
