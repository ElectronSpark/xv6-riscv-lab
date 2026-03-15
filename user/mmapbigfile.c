/**
 * @file mmapbigfile.c
 * @brief Large-file mmap test exercising zero-copy page cache and dirty
 *        bit tracking.
 *
 * Tests:
 *  1. MAP_PRIVATE read   — map a big file read-only, verify every page
 *     (exercises zero-copy pcache page mapping).
 *  2. MAP_SHARED  write  — write through a shared mapping, unmap, then
 *     read() back to confirm data persisted (exercises dirty tracking
 *     and writeback).
 *  3. fork + MAP_PRIVATE — map a file, fork, child and parent verify
 *     independent COW copies of pcache pages.
 *  4. mprotect cycle     — switch a shared mapping between PROT_READ
 *     and PROT_READ|PROT_WRITE, verify dirty propagation across
 *     protection changes.
 *  5. MADV_DONTNEED      — advise away pages, re-fault them, verify
 *     data still correct (dirty pages should have been written back).
 */

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "kernel/inc/vfs/fcntl.h"

#define PGSIZE 4096

/*
 * Number of pages for the large file.  Keep it big enough to stress the
 * page cache but small enough to finish in reasonable time.
 * 256 pages = 1 MiB.
 */
#define NPAGES 256
#define FILESZ (NPAGES * PGSIZE)

static const char *bigpath = "mmapbig.dat";

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

/**
 * Fill page buffer with a deterministic pattern based on page number.
 * Each byte = (pgno * PGSIZE + byte_offset) & 0xff.
 */
static void fill_pattern(char *buf, int pgno)
{
	for (int i = 0; i < PGSIZE; i++)
		buf[i] = (char)((pgno * PGSIZE + i) & 0xff);
}

/**
 * Create (or overwrite) the test file with NPAGES pages of patterned data
 * using ordinary write().
 */
static void create_big_file(void)
{
	int fd = open(bigpath, O_CREAT | O_WRONLY);
	if (fd < 0) {
		printf("FAIL - open for create\n");
		exit(1);
	}

	char page[PGSIZE];
	for (int p = 0; p < NPAGES; p++) {
		fill_pattern(page, p);
		if (write(fd, page, PGSIZE) != PGSIZE) {
			printf("FAIL - write page %d\n", p);
			close(fd);
			exit(1);
		}
	}
	close(fd);
}

/* ------------------------------------------------------------------ */
/* test 1: MAP_PRIVATE read of big file (zero-copy pcache path)       */
/* ------------------------------------------------------------------ */

static void test_mmap_read_big(void)
{
	printf("test_mmap_read_big: ");

	create_big_file();

	int fd = open(bigpath, O_RDONLY);
	if (fd < 0) {
		printf("FAIL - open\n");
		exit(1);
	}

	char *mapped = mmap(0, FILESZ, PROT_READ, MAP_PRIVATE, fd, 0);
	if (mapped == MAP_FAILED) {
		printf("FAIL - mmap\n");
		close(fd);
		exit(1);
	}
	close(fd);

	/* verify every byte through the mapping */
	for (int p = 0; p < NPAGES; p++) {
		for (int i = 0; i < PGSIZE; i++) {
			char expected = (char)((p * PGSIZE + i) & 0xff);
			if (mapped[p * PGSIZE + i] != expected) {
				printf("FAIL - page %d byte %d: got 0x%x want 0x%x\n",
				       p, i,
				       (unsigned char)mapped[p * PGSIZE + i],
				       (unsigned char)expected);
				exit(1);
			}
		}
	}

	munmap(mapped, FILESZ);
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* test 2: MAP_SHARED write then read() back (dirty tracking)         */
/* ------------------------------------------------------------------ */

static void test_mmap_write_big(void)
{
	printf("test_mmap_write_big: ");

	create_big_file();

	int fd = open(bigpath, O_RDWR);
	if (fd < 0) {
		printf("FAIL - open rdwr\n");
		exit(1);
	}

	char *mapped = mmap(0, FILESZ, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		printf("FAIL - mmap\n");
		close(fd);
		exit(1);
	}
	close(fd);

	/*
	 * Overwrite every page with inverted pattern.
	 * This exercises the dirty-bit fast path: first store to each
	 * clean pcache page triggers a store fault (D=0), which must set
	 * D and call pcache_mark_page_dirty().
	 */
	for (int p = 0; p < NPAGES; p++) {
		for (int i = 0; i < PGSIZE; i++)
			mapped[p * PGSIZE + i] = (char)(~((p * PGSIZE + i) & 0xff));
	}

	/* unmap — should propagate dirty to pcache and eventually flush */
	munmap(mapped, FILESZ);

	/* re-open and read() back — data must match the inverted pattern */
	fd = open(bigpath, O_RDONLY);
	if (fd < 0) {
		printf("FAIL - reopen\n");
		exit(1);
	}

	char page[PGSIZE];
	for (int p = 0; p < NPAGES; p++) {
		int rn = read(fd, page, PGSIZE);
		if (rn != PGSIZE) {
			printf("FAIL - read page %d (got %d)\n", p, rn);
			close(fd);
			exit(1);
		}
		for (int i = 0; i < PGSIZE; i++) {
			char expected = (char)(~((p * PGSIZE + i) & 0xff));
			if (page[i] != expected) {
				printf("FAIL - readback page %d byte %d: "
				       "got 0x%x want 0x%x\n",
				       p, i,
				       (unsigned char)page[i],
				       (unsigned char)expected);
				close(fd);
				exit(1);
			}
		}
	}

	close(fd);
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* test 3: fork with MAP_PRIVATE pcache pages (COW)                   */
/* ------------------------------------------------------------------ */

static void test_mmap_fork_big(void)
{
	printf("test_mmap_fork_big: ");

	create_big_file();

	int fd = open(bigpath, O_RDONLY);
	if (fd < 0) {
		printf("FAIL - open\n");
		exit(1);
	}

	char *mapped = mmap(0, FILESZ, PROT_READ | PROT_WRITE,
			    MAP_PRIVATE, fd, 0);
	if (mapped == MAP_FAILED) {
		printf("FAIL - mmap\n");
		close(fd);
		exit(1);
	}
	close(fd);

	/* touch every page so they are faulted in before fork */
	for (int p = 0; p < NPAGES; p++) {
		char expected = (char)((p * PGSIZE) & 0xff);
		if (mapped[p * PGSIZE] != expected) {
			printf("FAIL - pre-fork page %d\n", p);
			exit(1);
		}
	}

	int pid = fork();
	if (pid < 0) {
		printf("FAIL - fork\n");
		exit(1);
	}

	if (pid == 0) {
		/* child: write to mapping (triggers COW on pcache pages) */
		for (int p = 0; p < NPAGES; p++)
			mapped[p * PGSIZE] = (char)(p & 0xff);

		/* verify child's own writes */
		for (int p = 0; p < NPAGES; p++) {
			if (mapped[p * PGSIZE] != (char)(p & 0xff)) {
				printf("FAIL - child write page %d\n", p);
				exit(1);
			}
		}

		munmap(mapped, FILESZ);
		exit(0);
	}

	/* parent: wait for child, then verify mapping is unchanged */
	int status;
	wait(&status);
	if (status != 0) {
		printf("FAIL - child exit %d\n", status);
		exit(1);
	}

	for (int p = 0; p < NPAGES; p++) {
		char expected = (char)((p * PGSIZE) & 0xff);
		if (mapped[p * PGSIZE] != expected) {
			printf("FAIL - parent page %d after fork: "
			       "got 0x%x want 0x%x\n", p,
			       (unsigned char)mapped[p * PGSIZE],
			       (unsigned char)expected);
			exit(1);
		}
	}

	munmap(mapped, FILESZ);
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* test 4: mprotect cycle on shared mapping (dirty propagation)       */
/* ------------------------------------------------------------------ */

static void test_mprotect_big(void)
{
	printf("test_mprotect_big: ");

	create_big_file();

	int fd = open(bigpath, O_RDWR);
	if (fd < 0) {
		printf("FAIL - open\n");
		exit(1);
	}

	char *mapped = mmap(0, FILESZ, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		printf("FAIL - mmap\n");
		close(fd);
		exit(1);
	}
	close(fd);

	/* write first half */
	for (int p = 0; p < NPAGES / 2; p++)
		for (int i = 0; i < PGSIZE; i++)
			mapped[p * PGSIZE + i] = 'A';

	/* demote to read-only — dirty pages must be captured */
	if (mprotect(mapped, FILESZ, PROT_READ) < 0) {
		printf("FAIL - mprotect RO\n");
		exit(1);
	}

	/* promote back to read-write */
	if (mprotect(mapped, FILESZ, PROT_READ | PROT_WRITE) < 0) {
		printf("FAIL - mprotect RW\n");
		exit(1);
	}

	/* write second half */
	for (int p = NPAGES / 2; p < NPAGES; p++)
		for (int i = 0; i < PGSIZE; i++)
			mapped[p * PGSIZE + i] = 'B';

	munmap(mapped, FILESZ);

	/* verify via read() */
	fd = open(bigpath, O_RDONLY);
	if (fd < 0) {
		printf("FAIL - reopen\n");
		exit(1);
	}

	char page[PGSIZE];
	for (int p = 0; p < NPAGES; p++) {
		int rn = read(fd, page, PGSIZE);
		if (rn != PGSIZE) {
			printf("FAIL - read page %d (got %d)\n", p, rn);
			close(fd);
			exit(1);
		}
		char expected = (p < NPAGES / 2) ? 'A' : 'B';
		for (int i = 0; i < PGSIZE; i++) {
			if (page[i] != expected) {
				printf("FAIL - page %d byte %d: "
				       "got 0x%x want '%c'\n",
				       p, i, (unsigned char)page[i], expected);
				close(fd);
				exit(1);
			}
		}
	}

	close(fd);
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* test 5: MADV_DONTNEED + re-fault on shared dirty pages             */
/* ------------------------------------------------------------------ */

static void test_madvise_big(void)
{
	printf("test_madvise_big: ");

	create_big_file();

	int fd = open(bigpath, O_RDWR);
	if (fd < 0) {
		printf("FAIL - open\n");
		exit(1);
	}

	char *mapped = mmap(0, FILESZ, PROT_READ | PROT_WRITE,
			    MAP_SHARED, fd, 0);
	if (mapped == MAP_FAILED) {
		printf("FAIL - mmap\n");
		close(fd);
		exit(1);
	}
	close(fd);

	/* write a marker into every page */
	for (int p = 0; p < NPAGES; p++)
		mapped[p * PGSIZE] = (char)(p & 0xff);

	/*
	 * MADV_DONTNEED discards the pages; because the mapping is
	 * MAP_SHARED and dirty, the kernel must have propagated D→pcache
	 * dirty before dropping. Re-faulting should bring back the
	 * on-disk (written-back) data.
	 */
	if (madvise(mapped, FILESZ, MADV_DONTNEED) < 0) {
		printf("FAIL - madvise\n");
		exit(1);
	}

	/* re-read through mapping — should re-fault from pcache/disk */
	for (int p = 0; p < NPAGES; p++) {
		if (mapped[p * PGSIZE] != (char)(p & 0xff)) {
			printf("FAIL - re-fault page %d: "
			       "got 0x%x want 0x%x\n",
			       p,
			       (unsigned char)mapped[p * PGSIZE],
			       (unsigned char)(p & 0xff));
			exit(1);
		}
	}

	munmap(mapped, FILESZ);
	printf("OK\n");
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(void)
{
	printf("mmapbigfile test\n");

	test_mmap_read_big();
	test_mmap_write_big();
	test_mmap_fork_big();
	test_mprotect_big();
	test_madvise_big();

	unlink(bigpath);
	printf("mmapbigfile: all tests passed\n");
	exit(0);
}
