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
    char *mapped = mmap(0, 4096, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
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

/*
 * test_mmap_file_multipage - map a file spanning multiple pages and
 * verify every byte matches what was written.
 */
void test_mmap_file_multipage(void) {
    printf("test_mmap_file_multipage: ");

    int fd = open("mmap_multi", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - create\n");
        exit(1);
    }

    // Write 3 pages of patterned data
    char page[4096];
    for (int p = 0; p < 3; p++) {
        for (int i = 0; i < 4096; i++)
            page[i] = (char)((p * 4096 + i) & 0xff);
        if (write(fd, page, 4096) != 4096) {
            printf("FAIL - write page %d\n", p);
            close(fd);
            exit(1);
        }
    }
    close(fd);

    fd = open("mmap_multi", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - open\n");
        exit(1);
    }

    char *mapped = mmap(0, 3 * 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL - mmap\n");
        close(fd);
        exit(1);
    }
    close(fd);

    for (int i = 0; i < 3 * 4096; i++) {
        char expected = (char)(i & 0xff);
        if (mapped[i] != expected) {
            printf("FAIL - byte %d: got 0x%x want 0x%x\n", i,
                   (unsigned char)mapped[i], (unsigned char)expected);
            exit(1);
        }
    }

    munmap(mapped, 3 * 4096);
    printf("OK\n");
}

/*
 * test_mmap_file_offset - map starting at a non-zero page offset
 * into the file and verify correct data is returned.
 */
void test_mmap_file_offset(void) {
    printf("test_mmap_file_offset: ");

    int fd = open("mmap_off", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - create\n");
        exit(1);
    }

    // Write 2 pages: page 0 filled with 'A', page 1 filled with 'B'
    char page[4096];
    memset(page, 'A', 4096);
    if (write(fd, page, 4096) != 4096) {
        printf("FAIL - write0\n");
        close(fd);
        exit(1);
    }
    memset(page, 'B', 4096);
    if (write(fd, page, 4096) != 4096) {
        printf("FAIL - write1\n");
        close(fd);
        exit(1);
    }
    close(fd);

    fd = open("mmap_off", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - open\n");
        exit(1);
    }

    // Map only the second page (offset = 4096)
    char *mapped = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 4096);
    if (mapped == MAP_FAILED) {
        printf("FAIL - mmap\n");
        close(fd);
        exit(1);
    }
    close(fd);

    // Every byte should be 'B'
    for (int i = 0; i < 4096; i++) {
        if (mapped[i] != 'B') {
            printf("FAIL - byte %d: got '%c' want 'B'\n", i, mapped[i]);
            exit(1);
        }
    }

    munmap(mapped, 4096);
    printf("OK\n");
}

/*
 * test_mmap_file_read_after_close - mapping stays valid after the
 * file descriptor is closed.
 */
void test_mmap_file_read_after_close(void) {
    printf("test_mmap_file_read_after_close: ");

    int fd = open("mmap_close", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - create\n");
        exit(1);
    }

    char *msg = "Still readable after close!";
    int len = strlen(msg);
    if (write(fd, msg, len) != len) {
        printf("FAIL - write\n");
        close(fd);
        exit(1);
    }
    close(fd);

    fd = open("mmap_close", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - open\n");
        exit(1);
    }

    char *mapped = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL - mmap\n");
        close(fd);
        exit(1);
    }

    // Close fd *before* accessing the mapping
    close(fd);

    // Data should still be accessible via the mapping
    for (int i = 0; i < len; i++) {
        if (mapped[i] != msg[i]) {
            printf("FAIL - byte %d: got '%c' want '%c'\n", i, mapped[i],
                   msg[i]);
            exit(1);
        }
    }

    munmap(mapped, 4096);
    printf("OK\n");
}

/*
 * test_mmap_file_two_mappings - two independent mappings of the same
 * file, each with MAP_PRIVATE, don't interfere with each other.
 */
void test_mmap_file_two_mappings(void) {
    printf("test_mmap_file_two_mappings: ");

    int fd = open("mmap_two", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - create\n");
        exit(1);
    }

    char page[4096];
    memset(page, 'M', 4096);
    if (write(fd, page, 4096) != 4096) {
        printf("FAIL - write\n");
        close(fd);
        exit(1);
    }
    close(fd);

    fd = open("mmap_two", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - open\n");
        exit(1);
    }

    char *m1 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    char *m2 = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    close(fd);

    if (m1 == MAP_FAILED || m2 == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }
    if (m1 == m2) {
        printf("FAIL - same address\n");
        exit(1);
    }

    // Both should read 'M'
    if (m1[0] != 'M' || m2[0] != 'M') {
        printf("FAIL - initial read\n");
        exit(1);
    }

    // Private write to m1 should not affect m2
    m1[0] = 'X';
    if (m2[0] != 'M') {
        printf("FAIL - m2 corrupted by m1 write\n");
        exit(1);
    }

    // Private write to m2 should not affect m1
    m2[0] = 'Y';
    if (m1[0] != 'X') {
        printf("FAIL - m1 corrupted by m2 write\n");
        exit(1);
    }

    munmap(m1, 4096);
    munmap(m2, 4096);
    printf("OK\n");
}

/*
 * test_mmap_file_boundary - map a file whose size is not page-aligned.
 * Bytes past EOF within the mapped page must be zero.
 */
void test_mmap_file_boundary(void) {
    printf("test_mmap_file_boundary: ");

    int fd = open("mmap_bnd", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - create\n");
        exit(1);
    }

    // Write exactly 100 bytes of 0xff
    char data[100];
    memset(data, 0xff, 100);
    if (write(fd, data, 100) != 100) {
        printf("FAIL - write\n");
        close(fd);
        exit(1);
    }
    close(fd);

    fd = open("mmap_bnd", O_RDONLY);
    if (fd < 0) {
        printf("FAIL - open\n");
        exit(1);
    }

    char *mapped = mmap(0, 4096, PROT_READ, MAP_PRIVATE, fd, 0);
    if (mapped == MAP_FAILED) {
        printf("FAIL - mmap\n");
        close(fd);
        exit(1);
    }
    close(fd);

    // First 100 bytes should be 0xff
    for (int i = 0; i < 100; i++) {
        if ((unsigned char)mapped[i] != 0xff) {
            printf("FAIL - byte %d: got 0x%x want 0xff\n", i,
                   (unsigned char)mapped[i]);
            exit(1);
        }
    }

    // Bytes 100..4095 should be zero (past EOF zero-fill)
    for (int i = 100; i < 4096; i++) {
        if (mapped[i] != 0) {
            printf("FAIL - byte %d past EOF: got 0x%x want 0x00\n", i,
                   (unsigned char)mapped[i]);
            exit(1);
        }
    }

    munmap(mapped, 4096);
    printf("OK\n");
}

int main(int argc, char *argv[]) {
    printf("mmaptest starting\n");

    test_mmap_anonymous();
    test_mmap_read();
    test_mmap_private_write();
    test_mmap_fork();
    test_mmap_file_multipage();
    test_mmap_file_offset();
    test_mmap_file_read_after_close();
    test_mmap_file_two_mappings();
    test_mmap_file_boundary();

    printf("mmaptest: all tests passed\n");
    exit(0);
}
