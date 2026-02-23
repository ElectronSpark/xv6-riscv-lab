struct kstat {
	/* Must match xv6 kernel struct stat first 32 bytes exactly:
	 *   int32 dev @0, padding @4, u64 ino @8,
	 *   u32 mode @16, u32 nlink @20, u64 size @24
	 */
	int st_dev;
	unsigned int __pad0;
	unsigned long st_ino;
	unsigned int st_mode;
	unsigned int st_nlink;
	long st_size;

	/* Fields not provided by xv6; left zeroed by callers. */
	unsigned int st_uid;
	unsigned int st_gid;
	unsigned long st_rdev;
	long st_blksize;
	long st_blocks;
	long st_atime_sec;
	long st_atime_nsec;
	long st_mtime_sec;
	long st_mtime_nsec;
	long st_ctime_sec;
	long st_ctime_nsec;
};
