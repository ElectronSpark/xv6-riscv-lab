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
    
    const char *filename = "xv6fs_file_test";
    const size_t filename_len = sizeof("xv6fs_file_test") - 1;
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
 * Run all smoke tests
 ******************************************************************************/

void xv6fs_run_all_smoketests(void) {
    printf("\n========================================\n");
    printf("        xv6fs Smoke Tests\n");
    printf("========================================\n");
    
    xv6fs_run_inode_smoketest();
    xv6fs_run_file_ops_smoketest();
    
    printf("\n========================================\n");
    printf("        xv6fs Smoke Tests Complete\n");
    printf("========================================\n");
}
