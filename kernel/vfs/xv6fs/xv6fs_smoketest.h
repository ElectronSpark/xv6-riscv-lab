#ifndef KERNEL_VFS_XV6FS_SMOKETEST_H
#define KERNEL_VFS_XV6FS_SMOKETEST_H

void xv6fs_run_inode_smoketest(void);
void xv6fs_run_file_ops_smoketest(void);
void xv6fs_run_truncate_smoketest(void);
void xv6fs_run_namei_smoketest(void);
void xv6fs_run_dir_iter_smoketest(void);
void xv6fs_run_large_file_smoketest(void);
void xv6fs_run_all_smoketests(void);

#endif // KERNEL_VFS_XV6FS_SMOKETEST_H
