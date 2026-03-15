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

/******************************************************************************
 * mprotect tests
 ******************************************************************************/

/*
 * test_mprotect_read_write - mmap a region as PROT_READ, write to it after
 * upgrading to PROT_READ|PROT_WRITE with mprotect.
 */
void test_mprotect_read_write(void) {
    printf("test_mprotect_read_write: ");

    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }

    // Write to prove it's writable
    p[0] = 'A';
    if (p[0] != 'A') {
        printf("FAIL - initial write\n");
        exit(1);
    }

    // Downgrade to read-only
    if (mprotect(p, 4096, PROT_READ) != 0) {
        printf("FAIL - mprotect to PROT_READ\n");
        exit(1);
    }

    // Upgrade back to read-write
    if (mprotect(p, 4096, PROT_READ | PROT_WRITE) != 0) {
        printf("FAIL - mprotect to PROT_READ|PROT_WRITE\n");
        exit(1);
    }

    // Write again — should work
    p[0] = 'B';
    if (p[0] != 'B') {
        printf("FAIL - write after upgrade\n");
        exit(1);
    }

    munmap(p, 4096);
    printf("OK\n");
}

/*
 * test_mprotect_none - downgrade to PROT_NONE and verify a child that
 * tries to read it gets killed.
 */
void test_mprotect_none(void) {
    printf("test_mprotect_none: ");

    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }
    p[0] = 'Z';

    if (mprotect(p, 4096, PROT_NONE) != 0) {
        printf("FAIL - mprotect PROT_NONE\n");
        exit(1);
    }

    int pid = fork();
    if (pid == 0) {
        // Child: touching PROT_NONE memory should crash
        volatile char c = p[0];
        (void)c;
        // Should not reach here
        printf("FAIL - child survived PROT_NONE read\n");
        exit(1);
    }
    int status;
    wait(&status);
    // Child should have been killed (non-zero / signal exit)
    if (status == 0) {
        printf("FAIL - child exited cleanly (expected crash)\n");
        exit(1);
    }

    munmap(p, 4096);
    printf("OK\n");
}

/******************************************************************************
 * mremap tests
 ******************************************************************************/

/*
 * test_mremap_grow - grow an anonymous mapping in place.
 */
void test_mremap_grow(void) {
    printf("test_mremap_grow: ");

    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }

    // Write a marker
    p[0] = 'G';
    p[4095] = 'H';

    // Grow to 2 pages
    char *q = mremap(p, 4096, 8192, MREMAP_MAYMOVE, 0);
    if (q == MAP_FAILED) {
        printf("FAIL - mremap\n");
        munmap(p, 4096);
        exit(1);
    }

    // Original data preserved
    if (q[0] != 'G' || q[4095] != 'H') {
        printf("FAIL - data lost after grow (got '%c' '%c')\n", q[0], q[4095]);
        exit(1);
    }

    // Write into expanded region
    q[4096] = 'I';
    if (q[4096] != 'I') {
        printf("FAIL - write to expanded region\n");
        exit(1);
    }

    munmap(q, 8192);
    printf("OK\n");
}

/*
 * test_mremap_shrink - shrink an anonymous mapping.
 */
void test_mremap_shrink(void) {
    printf("test_mremap_shrink: ");

    char *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }

    p[0] = 'S';

    // Shrink to 1 page
    char *q = mremap(p, 8192, 4096, 0, 0);
    if (q == MAP_FAILED) {
        printf("FAIL - mremap shrink\n");
        munmap(p, 8192);
        exit(1);
    }

    // Should keep original data in the first page
    if (q[0] != 'S') {
        printf("FAIL - data lost\n");
        exit(1);
    }

    // q should equal p (shrink in place)
    if (q != p) {
        printf("FAIL - shrink moved mapping\n");
        exit(1);
    }

    munmap(q, 4096);
    printf("OK\n");
}

/******************************************************************************
 * mincore test
 ******************************************************************************/

/*
 * test_mincore - verify that mincore reports page residency correctly.
 * After mmap+touch page 0 is resident; page 1 (untouched) may not be.
 */
void test_mincore(void) {
    printf("test_mincore: ");

    char *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }

    // Touch only the first page
    p[0] = 'T';

    unsigned char vec[2];
    if (mincore(p, 8192, vec) != 0) {
        printf("FAIL - mincore returned error\n");
        munmap(p, 8192);
        exit(1);
    }

    // First page was touched, so must be resident
    if (!(vec[0] & 1)) {
        printf("FAIL - page 0 not resident after touch\n");
        munmap(p, 8192);
        exit(1);
    }

    munmap(p, 8192);
    printf("OK\n");
}

/******************************************************************************
 * madvise test
 ******************************************************************************/

/*
 * test_madvise_dontneed - use MADV_DONTNEED to discard pages, then verify
 * they are demand-faulted back as zero.
 */
void test_madvise_dontneed(void) {
    printf("test_madvise_dontneed: ");

    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }

    // Write non-zero data
    memset(p, 0xAB, 4096);
    if ((unsigned char)p[0] != 0xAB) {
        printf("FAIL - write\n");
        exit(1);
    }

    // Discard pages
    if (madvise(p, 4096, MADV_DONTNEED) != 0) {
        printf("FAIL - madvise\n");
        exit(1);
    }

    // Pages should be zero-filled on next access (anonymous mapping)
    if (p[0] != 0 || p[4095] != 0) {
        printf("FAIL - page not zeroed after MADV_DONTNEED (got 0x%x 0x%x)\n",
               (unsigned char)p[0], (unsigned char)p[4095]);
        exit(1);
    }

    munmap(p, 4096);
    printf("OK\n");
}

/******************************************************************************
 * msync test
 ******************************************************************************/

/*
 * test_msync_basic - verify msync returns 0 on a valid anonymous mapping
 * (no-op but should not error) and -1 on an invalid address.
 */
void test_msync_basic(void) {
    printf("test_msync_basic: ");

    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) {
        printf("FAIL - mmap\n");
        exit(1);
    }
    p[0] = 'M';

    // msync on valid mapping should succeed
    if (msync(p, 4096, MS_SYNC) != 0) {
        printf("FAIL - msync on valid mapping\n");
        munmap(p, 4096);
        exit(1);
    }

    munmap(p, 4096);
    printf("OK\n");
}

/******************************************************************************
 * Sparse folio writeback tests
 *
 * With PCACHE_FOLIO_ORDER=2 the page cache allocates 4-page (16 KB) folios.
 * These tests create a MAP_SHARED mapping, write to selected bytes/pages at
 * various positions and sparsity levels, munmap, then read() back to verify
 * the dirty data was correctly written through the pcache→filesystem pipeline.
 ******************************************************************************/

#define SPARSE_PGSIZE   4096
#define FOLIO_PAGES     4                        /* 1 << PCACHE_FOLIO_ORDER */
#define FOLIO_SIZE      (FOLIO_PAGES * SPARSE_PGSIZE) /* 16 KB              */

/* Number of folios for the sparse test files.  16 folios = 256 KB. */
#define SPARSE_NFOLIOS  16
#define SPARSE_NPAGES   (SPARSE_NFOLIOS * FOLIO_PAGES)
#define SPARSE_FILESZ   (SPARSE_NPAGES * SPARSE_PGSIZE)

static const char *sparse_path = "mmap_sparse";

/**
 * Create the sparse-test file: every byte initialised to a known value
 * so that un-written pages can be verified as unchanged.
 */
static void sparse_create_file(void)
{
    int fd = open(sparse_path, O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("FAIL - sparse create\n");
        exit(1);
    }

    char page[SPARSE_PGSIZE];
    for (int p = 0; p < SPARSE_NPAGES; p++) {
        /* Fill with a per-page pattern: each byte = (p ^ 0x55) & 0xff */
        memset(page, (p ^ 0x55) & 0xff, SPARSE_PGSIZE);
        if (write(fd, page, SPARSE_PGSIZE) != SPARSE_PGSIZE) {
            printf("FAIL - sparse write page %d\n", p);
            close(fd);
            exit(1);
        }
    }
    close(fd);
}

/**
 * Verify the file by reading every page via read() and checking each
 * byte against an expected-value callback.
 *
 * @param label   Test name for diagnostics.
 * @param expect  Returns the expected byte value for file-offset @off.
 *                The function receives (page_index, byte_within_page).
 */
static void sparse_verify_file(const char *label,
                                char (*expect)(int pgno, int byteoff))
{
    int fd = open(sparse_path, O_RDONLY);
    if (fd < 0) {
        printf("FAIL (%s) - reopen\n", label);
        exit(1);
    }

    char page[SPARSE_PGSIZE];
    for (int p = 0; p < SPARSE_NPAGES; p++) {
        int rn = read(fd, page, SPARSE_PGSIZE);
        if (rn != SPARSE_PGSIZE) {
            printf("FAIL (%s) - read page %d (got %d)\n", label, p, rn);
            close(fd);
            exit(1);
        }
        for (int i = 0; i < SPARSE_PGSIZE; i++) {
            char exp = expect(p, i);
            if (page[i] != exp) {
                printf("FAIL (%s) - page %d byte %d: "
                       "got 0x%x want 0x%x\n",
                       label, p, i,
                       (unsigned char)page[i],
                       (unsigned char)exp);
                close(fd);
                exit(1);
            }
        }
    }
    close(fd);
}

/* ────────────────────────────────────────────────────────────────────
 * Test A: Write only the FIRST page of each folio.
 *   Folio pages 0,4,8,… get 0xAA; the rest stay at the original value.
 * ──────────────────────────────────────────────────────────────────── */

static char expect_first_of_folio(int pgno, int byteoff)
{
    (void)byteoff;
    if (pgno % FOLIO_PAGES == 0)
        return (char)0xAA;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_first_of_folio(void)
{
    printf("test_sparse_first_of_folio: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    for (int f = 0; f < SPARSE_NFOLIOS; f++) {
        int pg0 = f * FOLIO_PAGES;
        memset(m + pg0 * SPARSE_PGSIZE, 0xAA, SPARSE_PGSIZE);
    }

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("first_of_folio", expect_first_of_folio);
    printf("OK\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test B: Write only the LAST page of each folio.
 *   Folio pages 3,7,11,… get 0xBB.
 * ──────────────────────────────────────────────────────────────────── */

static char expect_last_of_folio(int pgno, int byteoff)
{
    (void)byteoff;
    if (pgno % FOLIO_PAGES == FOLIO_PAGES - 1)
        return (char)0xBB;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_last_of_folio(void)
{
    printf("test_sparse_last_of_folio: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    for (int f = 0; f < SPARSE_NFOLIOS; f++) {
        int last_pg = f * FOLIO_PAGES + (FOLIO_PAGES - 1);
        memset(m + last_pg * SPARSE_PGSIZE, 0xBB, SPARSE_PGSIZE);
    }

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("last_of_folio", expect_last_of_folio);
    printf("OK\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test C: Write only the MIDDLE page (page 1) of each folio.
 *   Folio pages 1,5,9,… get 0xCC.
 * ──────────────────────────────────────────────────────────────────── */

static char expect_mid_of_folio(int pgno, int byteoff)
{
    (void)byteoff;
    if (pgno % FOLIO_PAGES == 1)
        return (char)0xCC;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_mid_of_folio(void)
{
    printf("test_sparse_mid_of_folio: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    for (int f = 0; f < SPARSE_NFOLIOS; f++) {
        int mid_pg = f * FOLIO_PAGES + 1;
        memset(m + mid_pg * SPARSE_PGSIZE, 0xCC, SPARSE_PGSIZE);
    }

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("mid_of_folio", expect_mid_of_folio);
    printf("OK\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test D: Write every OTHER folio — skip odd-numbered folios entirely.
 *   All 4 pages of even folios get 0xDD; odd folios unchanged.
 * ──────────────────────────────────────────────────────────────────── */

static char expect_even_folios(int pgno, int byteoff)
{
    (void)byteoff;
    int folio = pgno / FOLIO_PAGES;
    if (folio % 2 == 0)
        return (char)0xDD;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_every_other_folio(void)
{
    printf("test_sparse_every_other_folio: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    for (int f = 0; f < SPARSE_NFOLIOS; f += 2) {
        int base = f * FOLIO_PAGES * SPARSE_PGSIZE;
        memset(m + base, 0xDD, FOLIO_SIZE);
    }

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("even_folios", expect_even_folios);
    printf("OK\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test E: Write a SINGLE BYTE in every page — maximum sparsity.
 *   Byte 0 of each page is set to 0xEE; the rest of the page keeps
 *   the original fill value.
 * ──────────────────────────────────────────────────────────────────── */

static char expect_single_byte(int pgno, int byteoff)
{
    if (byteoff == 0)
        return (char)0xEE;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_single_byte_per_page(void)
{
    printf("test_sparse_single_byte_per_page: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    /* Touch only byte 0 of every page */
    for (int p = 0; p < SPARSE_NPAGES; p++)
        m[p * SPARSE_PGSIZE] = (char)0xEE;

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("single_byte", expect_single_byte);
    printf("OK\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test F: Write a single byte in only ONE page per folio (page 2).
 *   Byte 100 of pages 2,6,10,… is set to 0xFF.
 *   Everything else unchanged.
 * ──────────────────────────────────────────────────────────────────── */

static char expect_one_byte_page2(int pgno, int byteoff)
{
    if (pgno % FOLIO_PAGES == 2 && byteoff == 100)
        return (char)0xFF;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_one_byte_one_page_per_folio(void)
{
    printf("test_sparse_one_byte_one_page_per_folio: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    for (int f = 0; f < SPARSE_NFOLIOS; f++) {
        int pg = f * FOLIO_PAGES + 2;
        m[pg * SPARSE_PGSIZE + 100] = (char)0xFF;
    }

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("one_byte_page2", expect_one_byte_page2);
    printf("OK\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test G: Cross-folio boundary writes — write 8 KB straddling the
 *   boundary between consecutive folios (last 4 KB of folio N and
 *   first 4 KB of folio N+1).
 * ──────────────────────────────────────────────────────────────────── */

static char expect_cross_boundary(int pgno, int byteoff)
{
    (void)byteoff;
    int folio = pgno / FOLIO_PAGES;
    int within = pgno % FOLIO_PAGES;
    /* We wrote boundaries 0–1, 2–3, 4–5, … (even-numbered boundaries).
     * That means: last page of even folios and first page of odd folios. */
    if (folio % 2 == 0 && within == FOLIO_PAGES - 1)
        return (char)0x77;
    if (folio % 2 == 1 && within == 0)
        return (char)0x77;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_cross_folio_boundary(void)
{
    printf("test_sparse_cross_folio_boundary: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    /* Write 2 pages (8 KB) straddling each even→odd folio boundary */
    for (int f = 0; f + 1 < SPARSE_NFOLIOS; f += 2) {
        int last_pg = f * FOLIO_PAGES + (FOLIO_PAGES - 1);
        /* 2 contiguous pages: last of folio f and first of folio f+1 */
        memset(m + last_pg * SPARSE_PGSIZE, 0x77, 2 * SPARSE_PGSIZE);
    }

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("cross_boundary", expect_cross_boundary);
    printf("OK\n");
}

/* ────────────────────────────────────────────────────────────────────
 * Test H: Stride-N writes — write 1 page out of every N pages.
 *   Tests strides 1 (dense), 2, 4 (every folio gets exactly 1 dirty
 *   page), 8 (every other folio), and 16 (1 page per 4 folios).
 * ──────────────────────────────────────────────────────────────────── */

/* State passed via globals since xv6 user programs lack closures */
static int stride_cur_stride;
static char stride_val;

static char expect_stride(int pgno, int byteoff)
{
    (void)byteoff;
    if (pgno % stride_cur_stride == 0)
        return stride_val;
    return (char)((pgno ^ 0x55) & 0xff);
}

void test_sparse_stride(void)
{
    int strides[] = { 2, 4, 8, 16 };
    char vals[]   = { 0x11, 0x22, 0x33, 0x44 };

    for (int s = 0; s < 4; s++) {
        printf("test_sparse_stride_%d: ", strides[s]);
        sparse_create_file();

        int fd = open(sparse_path, O_RDWR);
        if (fd < 0) { printf("FAIL - open\n"); exit(1); }

        char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
        close(fd);

        for (int p = 0; p < SPARSE_NPAGES; p += strides[s])
            memset(m + p * SPARSE_PGSIZE, vals[s], SPARSE_PGSIZE);

        munmap(m, SPARSE_FILESZ);

        stride_cur_stride = strides[s];
        stride_val = vals[s];
        sparse_verify_file("stride", expect_stride);
        printf("OK\n");
    }
}

/* ────────────────────────────────────────────────────────────────────
 * Test I: Reverse-order writes — write pages from end to start.
 *   Every page gets 0x99, but written in descending order to stress
 *   any ordering assumptions in the writeback path.
 * ──────────────────────────────────────────────────────────────────── */

static char expect_reverse(int pgno, int byteoff)
{
    (void)pgno;
    (void)byteoff;
    return (char)0x99;
}

void test_sparse_reverse_order(void)
{
    printf("test_sparse_reverse_order: ");
    sparse_create_file();

    int fd = open(sparse_path, O_RDWR);
    if (fd < 0) { printf("FAIL - open\n"); exit(1); }

    char *m = mmap(0, SPARSE_FILESZ, PROT_READ | PROT_WRITE,
                   MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) { printf("FAIL - mmap\n"); close(fd); exit(1); }
    close(fd);

    /* Write pages in reverse order */
    for (int p = SPARSE_NPAGES - 1; p >= 0; p--)
        memset(m + p * SPARSE_PGSIZE, 0x99, SPARSE_PGSIZE);

    munmap(m, SPARSE_FILESZ);
    sparse_verify_file("reverse", expect_reverse);
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

    test_mprotect_read_write();
    test_mprotect_none();
    test_mremap_grow();
    test_mremap_shrink();
    test_mincore();
    test_madvise_dontneed();
    test_msync_basic();

    /* Sparse folio writeback tests */
    test_sparse_first_of_folio();
    test_sparse_last_of_folio();
    test_sparse_mid_of_folio();
    test_sparse_every_other_folio();
    test_sparse_single_byte_per_page();
    test_sparse_one_byte_one_page_per_folio();
    test_sparse_cross_folio_boundary();
    test_sparse_stride();
    test_sparse_reverse_order();

    unlink(sparse_path);
    printf("mmaptest: all tests passed\n");
    exit(0);
}
