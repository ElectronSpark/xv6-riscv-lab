/*
 * xv6fs smoke tests
 *
 * Run a set of quick sanity tests for xv6fs VFS operations.
 * These tests mount xv6fs (which uses the disk backend), perform
 * various inode and file operations, and verify correct behavior.
 *
 * Tests cover:
 * - Directory creation/removal (mkdir, rmdir)
 * - File creation/removal (create, unlink)
 * - Hard links and symlinks
 * - File read/write operations
 * - Lookup and directory iteration
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/fcntl.h"
#include "vfs/stat.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "xv6fs_private.h"
#include "xv6fs_smoketest.h"

// ANSI color codes for test output
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_RESET   "\x1b[0m"

#define PASS ANSI_GREEN  "[PASS]" ANSI_RESET
#define FAIL ANSI_RED    "[FAIL]" ANSI_RESET
#define WARN ANSI_YELLOW "[WARN]" ANSI_RESET

// Helper: lookup a child inode by name and bump refcount; caller must vfs_iput()
static struct vfs_inode *xv6fs_fetch_inode(struct vfs_inode *dir, const char *name, size_t name_len) {
    struct vfs_dentry d = {0};
    int ret = vfs_ilookup(dir, &d, name, name_len);
    if (ret != 0) {
        return ERR_PTR(ret);
    }
    struct vfs_inode *inode = vfs_get_dentry_inode(&d);
    vfs_release_dentry(&d);
    if (IS_ERR_OR_NULL(inode)) {
        return inode;
    }
    return inode;
}

// Helper: get the xv6fs mount root from tmpfs root
// Returns the xv6fs root inode (caller must vfs_iput) or NULL if not mounted
static struct vfs_inode *xv6fs_get_disk_root(void) {
    struct vfs_inode *tmpfs_root = vfs_root_inode.mnt_rooti;
    if (tmpfs_root == NULL) {
        return NULL;
    }
    
    // Look for "disk" mount point in tmpfs root
    struct vfs_inode *disk_mp = xv6fs_fetch_inode(tmpfs_root, "disk", 4);
    if (IS_ERR_OR_NULL(disk_mp)) {
        return NULL;
    }
    
    // Get the mounted filesystem's root
    struct vfs_inode *disk_root = NULL;
    if (disk_mp->mnt_sb != NULL) {
        disk_root = disk_mp->mnt_sb->root_inode;
        if (disk_root != NULL) {
            vfs_idup(disk_root);
        }
    }
    
    vfs_iput(disk_mp);
    return disk_root;
}

/******************************************************************************
 * Inode smoke test
 ******************************************************************************/

// Test basic inode operations: create, mkdir, unlink, rmdir, link, symlink
void xv6fs_run_inode_smoketest(void) {
    printf("\n=== xv6fs inode smoke test ===\n");
    
    int ret = 0;
    struct vfs_inode *root = xv6fs_get_disk_root();
    
    if (root == NULL) {
        printf("xv6fs_inode_test: " WARN " xv6fs not mounted at /disk, skipping\n");
        return;
    }
    
    struct vfs_inode *testdir = NULL;
    struct vfs_inode *file_a = NULL;
    struct vfs_inode *subdir = NULL;
    struct vfs_inode *sym = NULL;
    uint64 file_a_ino = 0;
    
    const char *testdir_name = "xv6fs_test";
    const size_t testdir_len = sizeof("xv6fs_test") - 1;
    const char *file_a_name = "testfile_a";
    const size_t file_a_len = sizeof("testfile_a") - 1;
    const char *subdir_name = "subdir";
    const size_t subdir_len = sizeof("subdir") - 1;
    const char *link_name = "hardlink";
    const size_t link_len = sizeof("hardlink") - 1;
    const char *sym_name = "symlink";
    const size_t sym_len = sizeof("symlink") - 1;
    const char *sym_target = "testfile_a";
    const size_t sym_target_len = sizeof("testfile_a") - 1;
    
    // Create test directory
    testdir = vfs_mkdir(root, 0755, testdir_name, testdir_len);
    if (IS_ERR_OR_NULL(testdir)) {
        ret = IS_ERR(testdir) ? PTR_ERR(testdir) : -EINVAL;
        printf("xv6fs_inode_test: " FAIL " mkdir %s errno=%d\n", testdir_name, ret);
        goto out;
    }
    vfs_ilock(testdir);
    printf("xv6fs_inode_test: " PASS " mkdir /%s ino=%lu nlink=%u\n",
           testdir_name, testdir->ino, testdir->n_links);
    vfs_iunlock(testdir);
    
    // Create a regular file
    file_a = vfs_create(testdir, 0644, file_a_name, file_a_len);
    if (IS_ERR_OR_NULL(file_a)) {
        ret = IS_ERR(file_a) ? PTR_ERR(file_a) : -EINVAL;
        printf("xv6fs_inode_test: " FAIL " create %s errno=%d\n", file_a_name, ret);
        goto cleanup;
    }
    file_a_ino = file_a->ino;
    vfs_ilock(file_a);
    printf("xv6fs_inode_test: " PASS " create %s ino=%lu nlink=%u\n",
           file_a_name, file_a->ino, file_a->n_links);
    vfs_iunlock(file_a);
    vfs_iput(file_a);
    file_a = NULL;
    
    // Create a subdirectory
    subdir = vfs_mkdir(testdir, 0755, subdir_name, subdir_len);
    if (IS_ERR_OR_NULL(subdir)) {
        ret = IS_ERR(subdir) ? PTR_ERR(subdir) : -EINVAL;
        printf("xv6fs_inode_test: " FAIL " mkdir %s errno=%d\n", subdir_name, ret);
        goto cleanup;
    }
    vfs_ilock(subdir);
    printf("xv6fs_inode_test: " PASS " mkdir %s ino=%lu nlink=%u\n",
           subdir_name, subdir->ino, subdir->n_links);
    vfs_iunlock(subdir);
    vfs_iput(subdir);
    subdir = NULL;
    
    // Create a hard link
    struct vfs_dentry link_old = {
        .sb = testdir->sb,
        .ino = file_a_ino,
        .name = NULL,
        .name_len = 0,
        .cookies = 0,
    };
    ret = vfs_link(&link_old, testdir, link_name, link_len);
    if (ret != 0) {
        printf("xv6fs_inode_test: " FAIL " link %s errno=%d\n", link_name, ret);
    } else {
        struct vfs_inode *tmp = xv6fs_fetch_inode(testdir, file_a_name, file_a_len);
        if (!IS_ERR_OR_NULL(tmp)) {
            vfs_ilock(tmp);
            printf("xv6fs_inode_test: " PASS " link %s -> %s nlink=%u\n",
                   link_name, file_a_name, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        }
    }
    
    // Create a symlink
    sym = vfs_symlink(testdir, 0777, sym_name, sym_len, sym_target, sym_target_len);
    if (IS_ERR_OR_NULL(sym)) {
        ret = IS_ERR(sym) ? PTR_ERR(sym) : -EINVAL;
        printf("xv6fs_inode_test: " FAIL " symlink %s errno=%d\n", sym_name, ret);
        sym = NULL;
    } else {
        vfs_ilock(sym);
        printf("xv6fs_inode_test: " PASS " symlink %s -> %s ino=%lu\n",
               sym_name, sym_target, sym->ino);
        vfs_iunlock(sym);
        vfs_iput(sym);
        sym = NULL;
    }
    
    // Read symlink
    struct vfs_inode *sym_inode = xv6fs_fetch_inode(testdir, sym_name, sym_len);
    if (!IS_ERR_OR_NULL(sym_inode)) {
        char linkbuf[64] = {0};
        ret = vfs_readlink(sym_inode, linkbuf, sizeof(linkbuf));
        if (ret >= 0) {
            printf("xv6fs_inode_test: " PASS " readlink %s -> %s len=%d\n",
                   sym_name, linkbuf, ret);
        } else {
            printf("xv6fs_inode_test: " FAIL " readlink %s errno=%d\n", sym_name, ret);
        }
        vfs_iput(sym_inode);
    }
    
    // Lookup test
    struct vfs_dentry d = {0};
    ret = vfs_ilookup(testdir, &d, file_a_name, file_a_len);
    if (ret == 0) {
        printf("xv6fs_inode_test: " PASS " lookup %s ino=%lu\n", file_a_name, d.ino);
        vfs_release_dentry(&d);
    } else {
        printf("xv6fs_inode_test: " FAIL " lookup %s errno=%d\n", file_a_name, ret);
    }
    
cleanup:
    // Remove symlink
    ret = vfs_unlink(testdir, sym_name, sym_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_inode_test: " WARN " cleanup unlink %s errno=%d\n", sym_name, ret);
    }
    
    // Remove hard link
    ret = vfs_unlink(testdir, link_name, link_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_inode_test: " WARN " cleanup unlink %s errno=%d\n", link_name, ret);
    }
    
    // Remove file
    ret = vfs_unlink(testdir, file_a_name, file_a_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_inode_test: " WARN " cleanup unlink %s errno=%d\n", file_a_name, ret);
    }
    
    // Remove subdirectory
    ret = vfs_rmdir(testdir, subdir_name, subdir_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_inode_test: " WARN " cleanup rmdir %s errno=%d\n", subdir_name, ret);
    }
    
    vfs_iput(testdir);
    testdir = NULL;
    
    // Remove test directory
    ret = vfs_rmdir(root, testdir_name, testdir_len);
    if (ret != 0) {
        printf("xv6fs_inode_test: " WARN " cleanup rmdir %s errno=%d\n", testdir_name, ret);
    }
    
    printf("xv6fs_inode_test: cleanup complete\n");
    
out:
    vfs_iput(root);
}

/******************************************************************************
 * File operations smoke test
 ******************************************************************************/

// Test file read/write operations
void xv6fs_run_file_ops_smoketest(void) {
    printf("\n=== xv6fs file ops smoke test ===\n");
    
    int ret = 0;
    struct vfs_inode *root = xv6fs_get_disk_root();
    
    if (root == NULL) {
        printf("xv6fs_file_test: " WARN " xv6fs not mounted at /disk, skipping\n");
        return;
    }
    
    struct vfs_inode *testfile = NULL;
    struct vfs_file *file = NULL;
    
    const char *filename = "xv6_ftest";
    const size_t filename_len = sizeof("xv6_ftest") - 1;
    const char *test_data = "Hello, xv6fs! This is a test message for file operations.";
    const size_t test_data_len = strlen(test_data);
    char read_buf[128] = {0};
    
    // Create test file
    testfile = vfs_create(root, 0644, filename, filename_len);
    if (IS_ERR_OR_NULL(testfile)) {
        ret = IS_ERR(testfile) ? PTR_ERR(testfile) : -EINVAL;
        printf("xv6fs_file_test: " FAIL " create %s errno=%d\n", filename, ret);
        goto out;
    }
    printf("xv6fs_file_test: " PASS " create %s ino=%lu\n", filename, testfile->ino);
    
    // Open file for writing using VFS API
    file = vfs_fileopen(testfile, O_WRONLY);
    if (IS_ERR_OR_NULL(file)) {
        ret = IS_ERR(file) ? PTR_ERR(file) : -EINVAL;
        printf("xv6fs_file_test: " FAIL " open for write errno=%d\n", ret);
        file = NULL;
        goto cleanup_inode;
    }
    
    // Write data
    ssize_t written = vfs_filewrite(file, test_data, test_data_len);
    if (written < 0) {
        printf("xv6fs_file_test: " FAIL " write errno=%ld\n", written);
    } else if ((size_t)written != test_data_len) {
        printf("xv6fs_file_test: " WARN " write incomplete: %ld/%lu\n", written, test_data_len);
    } else {
        printf("xv6fs_file_test: " PASS " write %ld bytes\n", written);
    }
    
    // Close file
    vfs_fileclose(file);
    file = NULL;
    
    // Open file for reading
    file = vfs_fileopen(testfile, O_RDONLY);
    if (IS_ERR_OR_NULL(file)) {
        ret = IS_ERR(file) ? PTR_ERR(file) : -EINVAL;
        printf("xv6fs_file_test: " FAIL " open for read errno=%d\n", ret);
        file = NULL;
        goto cleanup_inode;
    }
    
    // Read data
    ssize_t bytes_read = vfs_fileread(file, read_buf, sizeof(read_buf) - 1);
    if (bytes_read < 0) {
        printf("xv6fs_file_test: " FAIL " read errno=%ld\n", bytes_read);
    } else if ((size_t)bytes_read != test_data_len) {
        printf("xv6fs_file_test: " WARN " read incomplete: %ld/%lu\n", bytes_read, test_data_len);
    } else {
        if (strncmp(read_buf, test_data, test_data_len) == 0) {
            printf("xv6fs_file_test: " PASS " read %ld bytes, data verified\n", bytes_read);
        } else {
            printf("xv6fs_file_test: " FAIL " read data mismatch\n");
        }
    }
    
    // Test seek
    loff_t new_pos = vfs_filelseek(file, 0, SEEK_SET);
    if (new_pos == 0) {
        printf("xv6fs_file_test: " PASS " seek to beginning\n");
    } else {
        printf("xv6fs_file_test: " FAIL " seek errno=%lld\n", new_pos);
    }
    
    // Close file
    vfs_fileclose(file);
    file = NULL;
    
    // Check file size
    vfs_ilock(testfile);
    if ((loff_t)test_data_len == testfile->size) {
        printf("xv6fs_file_test: " PASS " file size=%lld\n", testfile->size);
    } else {
        printf("xv6fs_file_test: " WARN " file size=%lld expected=%lu\n", 
               testfile->size, test_data_len);
    }
    vfs_iunlock(testfile);
    
cleanup_inode:
    if (testfile != NULL) {
        vfs_iput(testfile);
        testfile = NULL;
    }
    
    // Remove test file
    ret = vfs_unlink(root, filename, filename_len);
    if (ret != 0) {
        printf("xv6fs_file_test: " WARN " cleanup unlink %s errno=%d\n", filename, ret);
    } else {
        printf("xv6fs_file_test: " PASS " cleanup unlink %s\n", filename);
    }
    
    printf("xv6fs_file_test: complete\n");
    
out:
    vfs_iput(root);
}

/******************************************************************************
 * Truncate smoke test
 ******************************************************************************/

// Test truncate operations within xv6fs transaction limits.
// xv6fs has MAXOPBLOCKS=10 per transaction, and each block allocation
// writes ~2 blocks (bitmap + data), so we can only grow by a few blocks
// at a time. Also, xv6fs only supports shrink to 0 (partial shrink = ENOSYS).
void xv6fs_run_truncate_smoketest(void) {
    printf("\n=== xv6fs truncate smoke test ===\n");
    
    int ret = 0;
    struct vfs_inode *root = xv6fs_get_disk_root();
    struct vfs_inode *testfile = NULL;
    struct vfs_file *file = NULL;
    (void)file; // suppress unused warning
    
    if (root == NULL) {
        printf("xv6fs_truncate: " WARN " xv6fs not mounted at /disk, skipping\n");
        return;
    }
    
    const char *filename = "trunctest";
    const size_t filename_len = sizeof("trunctest") - 1;
    
    // Create test file
    testfile = vfs_create(root, 0644, filename, filename_len);
    if (IS_ERR_OR_NULL(testfile)) {
        ret = IS_ERR(testfile) ? PTR_ERR(testfile) : -EINVAL;
        printf("xv6fs_truncate: " FAIL " create %s errno=%d\n", filename, ret);
        goto out;
    }
    printf("xv6fs_truncate: created %s ino=%lu\n", filename, testfile->ino);
    
    // Test 1: Grow to 512 bytes (within first direct block)
    ret = vfs_itruncate(testfile, 512);
    if (ret != 0) {
        printf("xv6fs_truncate: " FAIL " grow to 512 bytes, errno=%d\n", ret);
    } else {
        printf("xv6fs_truncate: " PASS " grow to 512 bytes, size=%lld\n", testfile->size);
    }
    
    // Test 2: Grow to 3 blocks
    ret = vfs_itruncate(testfile, 3 * BSIZE);
    if (ret != 0) {
        printf("xv6fs_truncate: " FAIL " grow to 3 blocks, errno=%d\n", ret);
    } else {
        printf("xv6fs_truncate: " PASS " grow to 3 blocks, size=%lld\n", testfile->size);
    }
    
    // Test 3: Partial shrink to 2 blocks
    ret = vfs_itruncate(testfile, 2 * BSIZE);
    if (ret != 0) {
        printf("xv6fs_truncate: " FAIL " partial shrink to 2 blocks, errno=%d\n", ret);
    } else {
        printf("xv6fs_truncate: " PASS " partial shrink to 2 blocks, size=%lld\n", testfile->size);
    }
    
    // Test 4: Partial shrink to 500 bytes (within 1 block)
    ret = vfs_itruncate(testfile, 500);
    if (ret != 0) {
        printf("xv6fs_truncate: " FAIL " partial shrink to 500 bytes, errno=%d\n", ret);
    } else {
        printf("xv6fs_truncate: " PASS " partial shrink to 500 bytes, size=%lld\n", testfile->size);
    }
    
    // Test 5: Truncate to zero
    ret = vfs_itruncate(testfile, 0);
    if (ret != 0) {
        printf("xv6fs_truncate: " FAIL " shrink to zero, errno=%d\n", ret);
    } else {
        printf("xv6fs_truncate: " PASS " shrink to zero, size=%lld\n", testfile->size);
    }
    
    // Test 6: Grow again after truncating to 0
    ret = vfs_itruncate(testfile, 2 * BSIZE);
    if (ret != 0) {
        printf("xv6fs_truncate: " FAIL " grow after shrink, errno=%d\n", ret);
    } else {
        printf("xv6fs_truncate: " PASS " grow after shrink, size=%lld\n", testfile->size);
    }
    
    // Test 7: Same size (no-op)
    ret = vfs_itruncate(testfile, 2 * BSIZE);
    if (ret != 0) {
        printf("xv6fs_truncate: " FAIL " same size no-op, errno=%d\n", ret);
    } else {
        printf("xv6fs_truncate: " PASS " same size no-op, size=%lld\n", testfile->size);
    }
    
    // Final truncate to 0 for cleanup
    vfs_itruncate(testfile, 0);
    
    // Cleanup
    vfs_iput(testfile);
    testfile = NULL;
    
    ret = vfs_unlink(root, filename, filename_len);
    if (ret != 0) {
        printf("xv6fs_truncate: " WARN " cleanup unlink %s errno=%d\n", filename, ret);
    } else {
        printf("xv6fs_truncate: cleanup complete\n");
    }
    
out:
    if (testfile != NULL) {
        vfs_iput(testfile);
    }
    vfs_iput(root);
}

/******************************************************************************
 * Namei (path resolution) smoke test
 ******************************************************************************/

// Test path resolution through the VFS layer
void xv6fs_run_namei_smoketest(void) {
    printf("\n=== xv6fs namei smoke test ===\n");
    
    int ret = 0;
    struct vfs_inode *root = xv6fs_get_disk_root();
    struct vfs_inode *result = NULL;
    struct vfs_inode *subdir = NULL;
    struct vfs_inode *nested = NULL;
    struct vfs_inode *file = NULL;
    
    if (root == NULL) {
        printf("xv6fs_namei: " WARN " xv6fs not mounted at /disk, skipping\n");
        return;
    }
    
    const char *subdir_name = "namei_dir";
    const size_t subdir_len = sizeof("namei_dir") - 1;
    const char *nested_name = "nested";
    const size_t nested_len = sizeof("nested") - 1;
    const char *file_name = "target";
    const size_t file_len = sizeof("target") - 1;
    
    // Setup: create /disk/namei_dir/nested/target
    subdir = vfs_mkdir(root, 0755, subdir_name, subdir_len);
    if (IS_ERR_OR_NULL(subdir)) {
        ret = IS_ERR(subdir) ? PTR_ERR(subdir) : -EINVAL;
        printf("xv6fs_namei: " FAIL " setup mkdir %s errno=%d\n", subdir_name, ret);
        goto out;
    }
    
    nested = vfs_mkdir(subdir, 0755, nested_name, nested_len);
    if (IS_ERR_OR_NULL(nested)) {
        ret = IS_ERR(nested) ? PTR_ERR(nested) : -EINVAL;
        printf("xv6fs_namei: " FAIL " setup mkdir %s errno=%d\n", nested_name, ret);
        goto cleanup_subdir;
    }
    
    file = vfs_create(nested, 0644, file_name, file_len);
    if (IS_ERR_OR_NULL(file)) {
        ret = IS_ERR(file) ? PTR_ERR(file) : -EINVAL;
        printf("xv6fs_namei: " FAIL " setup create %s errno=%d\n", file_name, ret);
        goto cleanup_nested;
    }
    uint64 file_ino = file->ino;
    vfs_iput(file);
    file = NULL;
    
    printf("xv6fs_namei: setup complete\n");
    
    // Test 1: Absolute path to disk root via /disk
    const char *path1 = "/disk";
    result = vfs_namei(path1, strlen(path1));
    if (IS_ERR_OR_NULL(result)) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") errno=%ld\n", path1, PTR_ERR(result));
    } else {
        printf("xv6fs_namei: " PASS " namei(\"%s\") -> ino=%lu\n", path1, result->ino);
        vfs_iput(result);
    }
    result = NULL;
    
    // Test 2: Path to subdir
    const char *path2 = "/disk/namei_dir";
    result = vfs_namei(path2, strlen(path2));
    if (IS_ERR_OR_NULL(result)) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") errno=%ld\n", path2, PTR_ERR(result));
    } else {
        printf("xv6fs_namei: " PASS " namei(\"%s\") -> ino=%lu\n", path2, result->ino);
        vfs_iput(result);
    }
    result = NULL;
    
    // Test 3: Full path to file
    const char *path3 = "/disk/namei_dir/nested/target";
    result = vfs_namei(path3, strlen(path3));
    if (IS_ERR_OR_NULL(result)) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") errno=%ld\n", path3, PTR_ERR(result));
    } else if (result->ino != file_ino) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") wrong ino=%lu expected=%lu\n",
               path3, result->ino, file_ino);
        vfs_iput(result);
    } else {
        printf("xv6fs_namei: " PASS " namei(\"%s\") -> ino=%lu\n", path3, result->ino);
        vfs_iput(result);
    }
    result = NULL;
    
    // Test 4: Path with "." components
    const char *path4 = "/disk/namei_dir/./nested/./target";
    result = vfs_namei(path4, strlen(path4));
    if (IS_ERR_OR_NULL(result)) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") errno=%ld\n", path4, PTR_ERR(result));
    } else if (result->ino != file_ino) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") wrong ino\n", path4);
        vfs_iput(result);
    } else {
        printf("xv6fs_namei: " PASS " namei(\"%s\") -> ino=%lu\n", path4, result->ino);
        vfs_iput(result);
    }
    result = NULL;
    
    // Test 5: Path with ".." components
    const char *path5 = "/disk/namei_dir/nested/../nested/target";
    result = vfs_namei(path5, strlen(path5));
    if (IS_ERR_OR_NULL(result)) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") errno=%ld\n", path5, PTR_ERR(result));
    } else if (result->ino != file_ino) {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") wrong ino\n", path5);
        vfs_iput(result);
    } else {
        printf("xv6fs_namei: " PASS " namei(\"%s\") -> ino=%lu\n", path5, result->ino);
        vfs_iput(result);
    }
    result = NULL;
    
    // Test 6: Non-existent path
    const char *path6 = "/disk/namei_dir/noexist";
    result = vfs_namei(path6, strlen(path6));
    if (IS_ERR(result)) {
        printf("xv6fs_namei: " PASS " namei(\"%s\") -> ENOENT errno=%ld\n", path6, PTR_ERR(result));
    } else if (result == NULL) {
        printf("xv6fs_namei: " PASS " namei(\"%s\") -> NULL (not found)\n", path6);
    } else {
        printf("xv6fs_namei: " FAIL " namei(\"%s\") should fail but got ino=%lu\n", path6, result->ino);
        vfs_iput(result);
    }
    result = NULL;
    
cleanup_nested:
    // Cleanup file
    if (!IS_ERR_OR_NULL(nested)) {
        ret = vfs_unlink(nested, file_name, file_len);
        if (ret != 0 && ret != -ENOENT) {
            printf("xv6fs_namei: " WARN " cleanup unlink %s errno=%d\n", file_name, ret);
        }
        vfs_iput(nested);
    }
    
    // Cleanup nested directory
    if (!IS_ERR_OR_NULL(subdir)) {
        ret = vfs_rmdir(subdir, nested_name, nested_len);
        if (ret != 0 && ret != -ENOENT) {
            printf("xv6fs_namei: " WARN " cleanup rmdir %s errno=%d\n", nested_name, ret);
        }
    }
    
cleanup_subdir:
    if (!IS_ERR_OR_NULL(subdir)) {
        vfs_iput(subdir);
    }
    
    // Cleanup subdir
    ret = vfs_rmdir(root, subdir_name, subdir_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_namei: " WARN " cleanup rmdir %s errno=%d\n", subdir_name, ret);
    }
    
    printf("xv6fs_namei: complete\n");
    
out:
    vfs_iput(root);
}

/******************************************************************************
 * Directory iteration smoke test
 ******************************************************************************/

// Helper struct for tracking expected entries
struct xv6fs_iter_expect {
    const char *name;
    bool required;
    bool seen;
};

// Mark an entry as seen, check for duplicates
static bool xv6fs_iter_mark_seen(const char *name, struct xv6fs_iter_expect *tbl, 
                                  int tbl_cnt, bool *dup_out) {
    for (int i = 0; i < tbl_cnt; i++) {
        if (strncmp(name, tbl[i].name, strlen(tbl[i].name) + 1) == 0) {
            if (tbl[i].seen) {
                if (dup_out != NULL) {
                    *dup_out = true;
                }
                return true;
            }
            tbl[i].seen = true;
            return true;
        }
    }
    return false;
}

// Test directory iteration
void xv6fs_run_dir_iter_smoketest(void) {
    printf("\n=== xv6fs dir_iter smoke test ===\n");
    
    int ret = 0;
    struct vfs_inode *root = xv6fs_get_disk_root();
    struct vfs_inode *testdir = NULL;
    struct vfs_inode *tmp = NULL;
    struct vfs_dir_iter iter = {0};
    struct vfs_dentry dentry = {0};
    
    if (root == NULL) {
        printf("xv6fs_dir_iter: " WARN " xv6fs not mounted at /disk, skipping\n");
        return;
    }
    
    const char *testdir_name = "iter_dir";
    const size_t testdir_len = sizeof("iter_dir") - 1;
    const char *file1_name = "file1";
    const size_t file1_len = sizeof("file1") - 1;
    const char *file2_name = "file2";
    const size_t file2_len = sizeof("file2") - 1;
    const char *subdir_name = "subdir";
    const size_t subdir_len = sizeof("subdir") - 1;
    
    // Create test directory
    testdir = vfs_mkdir(root, 0755, testdir_name, testdir_len);
    if (IS_ERR_OR_NULL(testdir)) {
        ret = IS_ERR(testdir) ? PTR_ERR(testdir) : -EINVAL;
        printf("xv6fs_dir_iter: " FAIL " mkdir %s errno=%d\n", testdir_name, ret);
        goto out;
    }
    
    // Test 1: Empty directory (just . and ..)
    printf("xv6fs_dir_iter: Test 1 - empty directory\n");
    {
        struct xv6fs_iter_expect exp[] = {
            {".", true, false},
            {"..", true, false},
        };
        int exp_count = 2;
        bool ok = true;
        int count = 0;
        
        memset(&iter, 0, sizeof(iter));
        for (;;) {
            ret = vfs_dir_iter(testdir, &iter, &dentry);
            if (ret != 0) {
                printf("xv6fs_dir_iter: " FAIL " dir_iter errno=%d\n", ret);
                ok = false;
                break;
            }
            if (dentry.name == NULL) {
                break;
            }
            
            bool dup = false;
            if (!xv6fs_iter_mark_seen(dentry.name, exp, exp_count, &dup)) {
                printf("xv6fs_dir_iter: " WARN " unexpected entry: %s\n", dentry.name);
            } else if (dup) {
                printf("xv6fs_dir_iter: " FAIL " duplicate entry: %s\n", dentry.name);
                ok = false;
            }
            count++;
            vfs_release_dentry(&dentry);
            memset(&dentry, 0, sizeof(dentry));
        }
        
        for (int i = 0; i < exp_count; i++) {
            if (exp[i].required && !exp[i].seen) {
                printf("xv6fs_dir_iter: " FAIL " missing entry: %s\n", exp[i].name);
                ok = false;
            }
        }
        
        if (ok && count == 2) {
            printf("xv6fs_dir_iter: " PASS " empty dir has . and .. only\n");
        }
        vfs_release_dentry(&dentry);
    }
    
    // Add files and subdirectory
    tmp = vfs_create(testdir, 0644, file1_name, file1_len);
    if (IS_ERR_OR_NULL(tmp)) {
        ret = IS_ERR(tmp) ? PTR_ERR(tmp) : -EINVAL;
        printf("xv6fs_dir_iter: " FAIL " create %s errno=%d\n", file1_name, ret);
        goto cleanup;
    }
    vfs_iput(tmp);
    tmp = NULL;
    
    tmp = vfs_create(testdir, 0644, file2_name, file2_len);
    if (IS_ERR_OR_NULL(tmp)) {
        ret = IS_ERR(tmp) ? PTR_ERR(tmp) : -EINVAL;
        printf("xv6fs_dir_iter: " FAIL " create %s errno=%d\n", file2_name, ret);
        goto cleanup;
    }
    vfs_iput(tmp);
    tmp = NULL;
    
    tmp = vfs_mkdir(testdir, 0755, subdir_name, subdir_len);
    if (IS_ERR_OR_NULL(tmp)) {
        ret = IS_ERR(tmp) ? PTR_ERR(tmp) : -EINVAL;
        printf("xv6fs_dir_iter: " FAIL " mkdir %s errno=%d\n", subdir_name, ret);
        goto cleanup;
    }
    vfs_iput(tmp);
    tmp = NULL;
    
    // Test 2: Directory with entries
    printf("xv6fs_dir_iter: Test 2 - populated directory\n");
    {
        struct xv6fs_iter_expect exp[] = {
            {".", true, false},
            {"..", true, false},
            {"file1", true, false},
            {"file2", true, false},
            {"subdir", true, false},
        };
        int exp_count = 5;
        bool ok = true;
        int count = 0;
        
        memset(&iter, 0, sizeof(iter));
        for (;;) {
            ret = vfs_dir_iter(testdir, &iter, &dentry);
            if (ret != 0) {
                printf("xv6fs_dir_iter: " FAIL " dir_iter errno=%d\n", ret);
                ok = false;
                break;
            }
            if (dentry.name == NULL) {
                break;
            }
            
            bool dup = false;
            if (!xv6fs_iter_mark_seen(dentry.name, exp, exp_count, &dup)) {
                printf("xv6fs_dir_iter: " WARN " unexpected entry: %s\n", dentry.name);
            } else if (dup) {
                printf("xv6fs_dir_iter: " FAIL " duplicate entry: %s\n", dentry.name);
                ok = false;
            }
            count++;
            vfs_release_dentry(&dentry);
            memset(&dentry, 0, sizeof(dentry));
        }
        
        for (int i = 0; i < exp_count; i++) {
            if (exp[i].required && !exp[i].seen) {
                printf("xv6fs_dir_iter: " FAIL " missing entry: %s\n", exp[i].name);
                ok = false;
            }
        }
        
        if (ok && count == 5) {
            printf("xv6fs_dir_iter: " PASS " found all 5 entries\n");
        } else {
            printf("xv6fs_dir_iter: " WARN " found %d entries (expected 5)\n", count);
        }
        vfs_release_dentry(&dentry);
    }
    
    // Test 3: Fetch inode for each entry
    printf("xv6fs_dir_iter: Test 3 - fetch inodes during iteration\n");
    {
        bool ok = true;
        int fetched = 0;
        
        memset(&iter, 0, sizeof(iter));
        for (;;) {
            ret = vfs_dir_iter(testdir, &iter, &dentry);
            if (ret != 0) {
                printf("xv6fs_dir_iter: " FAIL " dir_iter errno=%d\n", ret);
                ok = false;
                break;
            }
            if (dentry.name == NULL) {
                break;
            }
            
            struct vfs_inode *ent = vfs_get_dentry_inode(&dentry);
            if (IS_ERR_OR_NULL(ent)) {
                printf("xv6fs_dir_iter: " FAIL " get_inode %s errno=%ld\n", 
                       dentry.name, PTR_ERR(ent));
                ok = false;
            } else {
                fetched++;
                vfs_iput(ent);
            }
            vfs_release_dentry(&dentry);
            memset(&dentry, 0, sizeof(dentry));
        }
        
        if (ok && fetched == 5) {
            printf("xv6fs_dir_iter: " PASS " fetched all 5 inodes successfully\n");
        }
        vfs_release_dentry(&dentry);
    }
    
cleanup:
    // Remove entries
    ret = vfs_rmdir(testdir, subdir_name, subdir_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_dir_iter: " WARN " cleanup rmdir %s errno=%d\n", subdir_name, ret);
    }
    ret = vfs_unlink(testdir, file2_name, file2_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_dir_iter: " WARN " cleanup unlink %s errno=%d\n", file2_name, ret);
    }
    ret = vfs_unlink(testdir, file1_name, file1_len);
    if (ret != 0 && ret != -ENOENT) {
        printf("xv6fs_dir_iter: " WARN " cleanup unlink %s errno=%d\n", file1_name, ret);
    }
    
    vfs_iput(testdir);
    testdir = NULL;
    
    ret = vfs_rmdir(root, testdir_name, testdir_len);
    if (ret != 0) {
        printf("xv6fs_dir_iter: " WARN " cleanup rmdir %s errno=%d\n", testdir_name, ret);
    }
    
    printf("xv6fs_dir_iter: complete\n");
    
out:
    vfs_iput(root);
}

/******************************************************************************
 * Large file smoke test
 ******************************************************************************/

// Test read/write of larger files that span multiple blocks
void xv6fs_run_large_file_smoketest(void) {
    printf("\n=== xv6fs large file smoke test ===\n");
    
    int ret = 0;
    struct vfs_inode *root = xv6fs_get_disk_root();
    struct vfs_inode *testfile = NULL;
    struct vfs_file *file = NULL;
    
    if (root == NULL) {
        printf("xv6fs_largefile: " WARN " xv6fs not mounted at /disk, skipping\n");
        return;
    }
    
    const char *filename = "largefile";
    const size_t filename_len = sizeof("largefile") - 1;
    
    // Create test file
    testfile = vfs_create(root, 0644, filename, filename_len);
    if (IS_ERR_OR_NULL(testfile)) {
        ret = IS_ERR(testfile) ? PTR_ERR(testfile) : -EINVAL;
        printf("xv6fs_largefile: " FAIL " create %s errno=%d\n", filename, ret);
        goto out;
    }
    printf("xv6fs_largefile: created %s ino=%lu\n", filename, testfile->ino);
    
    // Write 10 blocks of data (10KB)
    file = vfs_fileopen(testfile, O_WRONLY);
    if (IS_ERR_OR_NULL(file)) {
        ret = IS_ERR(file) ? PTR_ERR(file) : -EINVAL;
        printf("xv6fs_largefile: " FAIL " open for write errno=%d\n", ret);
        file = NULL;
        goto cleanup_inode;
    }
    
    // Create a pattern buffer
    char write_buf[BSIZE];
    ssize_t total_written = 0;
    for (int block = 0; block < 10; block++) {
        // Fill with pattern: block number in each byte
        memset(write_buf, 'A' + block, BSIZE);
        ssize_t written = vfs_filewrite(file, write_buf, BSIZE);
        if (written != BSIZE) {
            printf("xv6fs_largefile: " FAIL " write block %d: %ld/%d\n", block, written, BSIZE);
            break;
        }
        total_written += written;
    }
    
    if (total_written == 10 * BSIZE) {
        printf("xv6fs_largefile: " PASS " wrote %ld bytes (10 blocks)\n", total_written);
    }
    
    vfs_fileclose(file);
    file = NULL;
    
    // Verify file size
    vfs_ilock(testfile);
    if (testfile->size == 10 * BSIZE) {
        printf("xv6fs_largefile: " PASS " file size=%lld\n", testfile->size);
    } else {
        printf("xv6fs_largefile: " FAIL " file size=%lld expected=%d\n", 
               testfile->size, 10 * BSIZE);
    }
    vfs_iunlock(testfile);
    
    // Read back and verify
    file = vfs_fileopen(testfile, O_RDONLY);
    if (IS_ERR_OR_NULL(file)) {
        ret = IS_ERR(file) ? PTR_ERR(file) : -EINVAL;
        printf("xv6fs_largefile: " FAIL " open for read errno=%d\n", ret);
        file = NULL;
        goto cleanup_inode;
    }
    
    char read_buf[BSIZE];
    bool read_ok = true;
    for (int block = 0; block < 10; block++) {
        memset(read_buf, 0, BSIZE);
        ssize_t bytes_read = vfs_fileread(file, read_buf, BSIZE);
        if (bytes_read != BSIZE) {
            printf("xv6fs_largefile: " FAIL " read block %d: %ld/%d\n", block, bytes_read, BSIZE);
            read_ok = false;
            break;
        }
        // Verify pattern
        char expected = 'A' + block;
        for (int i = 0; i < BSIZE; i++) {
            if (read_buf[i] != expected) {
                printf("xv6fs_largefile: " FAIL " block %d byte %d: got 0x%02x expected 0x%02x\n",
                       block, i, (unsigned char)read_buf[i], (unsigned char)expected);
                read_ok = false;
                break;
            }
        }
        if (!read_ok) break;
    }
    
    if (read_ok) {
        printf("xv6fs_largefile: " PASS " read and verified 10 blocks\n");
    }
    
    // Test seek to middle and read
    loff_t new_pos = vfs_filelseek(file, 5 * BSIZE, SEEK_SET);
    if (new_pos == 5 * BSIZE) {
        memset(read_buf, 0, BSIZE);
        ssize_t bytes_read = vfs_fileread(file, read_buf, BSIZE);
        if (bytes_read == BSIZE && read_buf[0] == 'F') { // Block 5 has pattern 'F'
            printf("xv6fs_largefile: " PASS " seek to block 5 and read verified\n");
        } else {
            printf("xv6fs_largefile: " FAIL " seek+read: read=%ld first_byte=0x%02x\n",
                   bytes_read, (unsigned char)read_buf[0]);
        }
    } else {
        printf("xv6fs_largefile: " FAIL " seek to block 5: pos=%lld\n", new_pos);
    }
    
    vfs_fileclose(file);
    file = NULL;
    
cleanup_inode:
    if (testfile != NULL) {
        vfs_iput(testfile);
        testfile = NULL;
    }
    
    ret = vfs_unlink(root, filename, filename_len);
    if (ret != 0) {
        printf("xv6fs_largefile: " WARN " cleanup unlink %s errno=%d\n", filename, ret);
    } else {
        printf("xv6fs_largefile: cleanup complete\n");
    }
    
out:
    vfs_iput(root);
}

/******************************************************************************
 * Run all smoke tests
 ******************************************************************************/

// Helper to shrink all caches and check for leaks
static void xv6fs_shrink_all_caches(void) {
    xv6fs_shrink_caches();
    __vfs_shrink_caches();
    kmm_shrink_all();
}

void xv6fs_run_all_smoketests(void) {
    printf("\n========================================\n");
    printf("        xv6fs Smoke Tests\n");
    printf("========================================\n");
    
    // Shrink all caches before baseline to get a clean state
    xv6fs_shrink_all_caches();
    
    uint64 before_pages, after_pages;
    
    // Memory leak check for inode smoketest
    before_pages = get_total_free_pages();
    xv6fs_run_inode_smoketest();
    xv6fs_shrink_all_caches();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: xv6fs_inode_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("xv6fs_inode_smoketest: no memory leak detected\n");
    }
    
    // Memory leak check for file_ops smoketest
    before_pages = get_total_free_pages();
    xv6fs_run_file_ops_smoketest();
    xv6fs_shrink_all_caches();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: xv6fs_file_ops_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("xv6fs_file_ops_smoketest: no memory leak detected\n");
    }
    
    // Memory leak check for truncate smoketest
    before_pages = get_total_free_pages();
    xv6fs_run_truncate_smoketest();
    xv6fs_shrink_all_caches();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: xv6fs_truncate_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("xv6fs_truncate_smoketest: no memory leak detected\n");
    }
    
    // Memory leak check for namei smoketest
    before_pages = get_total_free_pages();
    xv6fs_run_namei_smoketest();
    xv6fs_shrink_all_caches();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: xv6fs_namei_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("xv6fs_namei_smoketest: no memory leak detected\n");
    }
    
    // Memory leak check for dir_iter smoketest
    before_pages = get_total_free_pages();
    xv6fs_run_dir_iter_smoketest();
    xv6fs_shrink_all_caches();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: xv6fs_dir_iter_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("xv6fs_dir_iter_smoketest: no memory leak detected\n");
    }
    
    // Memory leak check for large_file smoketest
    before_pages = get_total_free_pages();
    xv6fs_run_large_file_smoketest();
    xv6fs_shrink_all_caches();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: xv6fs_large_file_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("xv6fs_large_file_smoketest: no memory leak detected\n");
    }
    
    printf("\n========================================\n");
    printf("        xv6fs Smoke Tests Complete\n");
    printf("========================================\n");
}
