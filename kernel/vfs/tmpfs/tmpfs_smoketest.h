#ifndef KERNEL_VFS_TMPFS_SMOKETEST_H
#define KERNEL_VFS_TMPFS_SMOKETEST_H

void tmpfs_run_inode_smoketest(void);
void tmpfs_run_truncate_smoketest(void);
void tmpfs_run_namei_smoketest(void);
void tmpfs_run_dir_iter_mount_smoketest(void);
void tmpfs_run_file_ops_smoketest(void);

#endif // KERNEL_VFS_TMPFS_SMOKETEST_H
