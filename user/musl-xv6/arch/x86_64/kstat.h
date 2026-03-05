/*
 * kstat.h — kernel stat layout for musl on xv6 (x86_64)
 *
 * This MUST match what the xv6 kernel writes via copyout in stat/fstat/fstatat.
 * The xv6 kernel uses the riscv64 musl ABI layout for ALL architectures.
 * musl's fstatat_kstat() converts this to the userspace struct stat.
 */
struct kstat {
	dev_t st_dev;
	ino_t st_ino;
	mode_t st_mode;
	nlink_t st_nlink;
	uid_t st_uid;
	gid_t st_gid;
	dev_t st_rdev;
	unsigned long __pad;
	off_t st_size;
	blksize_t st_blksize;
	int __pad2;
	blkcnt_t st_blocks;
	long st_atime_sec;
	long st_atime_nsec;
	long st_mtime_sec;
	long st_mtime_nsec;
	long st_ctime_sec;
	long st_ctime_nsec;
	unsigned __unused[2];
};
