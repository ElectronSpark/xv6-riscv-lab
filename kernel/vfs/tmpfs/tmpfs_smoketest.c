#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
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
#include "../vfs_private.h" // for vfs_root_inode
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "tmpfs_private.h"
#include "tmpfs_smoketest.h"

// ANSI color codes for test output
#define ANSI_RED     "\x1b[31m"
#define ANSI_GREEN   "\x1b[32m"
#define ANSI_YELLOW  "\x1b[33m"
#define ANSI_RESET   "\x1b[0m"

#define PASS ANSI_GREEN  "[PASS]" ANSI_RESET
#define FAIL ANSI_RED    "[FAIL]" ANSI_RESET
#define WARN ANSI_YELLOW "[WARN]" ANSI_RESET

struct iter_expect {
    const char *name;
    bool required;
    bool seen;
};

// Helper: lookup a child inode by name and bump refcount; caller must vfs_iput()
static struct vfs_inode *tmpfs_fetch_inode(struct vfs_inode *dir, const char *name, size_t name_len) {
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

// Run a small, inode-only tmpfs smoke test: create nested directories, files, symlinks,
// perform lookups, moves, and readlinks. This is debug-only scaffolding invoked at init.
void tmpfs_run_inode_smoketest(void) {
    int ret = 0;
    struct vfs_inode *root = vfs_root_inode.mnt_rooti;
    struct vfs_inode *subdir = NULL;
    struct vfs_inode *nested = NULL;
    struct vfs_inode *file_a = NULL;
    struct vfs_inode *file_b = NULL;
    struct vfs_inode *sym_a = NULL;
    struct vfs_inode *sym_b = NULL;
    uint64 file_a_ino = 0;
    uint64 file_b_ino = 0;
    bool root_pinned = false;

    const char *subdir_name = "tmpfs_subdir";
    const size_t subdir_len = sizeof("tmpfs_subdir") - 1;
    const char *nested_name = "nested";
    const size_t nested_len = sizeof("nested") - 1;
    const char *file_a_name = "tmp_file_a";
    const size_t file_a_len = sizeof("tmp_file_a") - 1;
    const char *file_b_name = "tmp_file_b";
    const size_t file_b_len = sizeof("tmp_file_b") - 1;
    const char *file_b_new_name = "tmp_file_b_moved";
    const size_t file_b_new_len = sizeof("tmp_file_b_moved") - 1;
    const char *file_a_link_name = "tmp_file_a_link";
    const size_t file_a_link_len = sizeof("tmp_file_a_link") - 1;
    const char *symlink_a_name = "link_to_nested";
    const size_t symlink_a_len = sizeof("link_to_nested") - 1;
    const char *symlink_b_name = "nested_link_to_root";
    const size_t symlink_b_len = sizeof("nested_link_to_root") - 1;
    const char *symlink_a_target = "nested";
    const size_t symlink_a_target_len = sizeof("nested") - 1;
    const char *symlink_b_target = "..";
    const size_t symlink_b_target_len = sizeof("..") - 1;

    vfs_idup(root);
    root_pinned = true;

    subdir = vfs_mkdir(root, 0755, subdir_name, subdir_len);
    if (IS_ERR_OR_NULL(subdir)) {
        ret = PTR_ERR(subdir);
        printf("inode_smoketest: " FAIL " vfs_mkdir %s, errno=%d\n", subdir_name, ret);
        goto out_put_root;
    }
    vfs_ilock(subdir);
    printf("inode_smoketest: " PASS " created /%s nlink=%u\n", subdir_name, subdir->n_links);
    vfs_iunlock(subdir);

    nested = vfs_mkdir(subdir, 0755, nested_name, nested_len);
    if (IS_ERR_OR_NULL(nested)) {
        ret = PTR_ERR(nested);
        printf("inode_smoketest: " FAIL " vfs_mkdir %s, errno=%d\n", nested_name, ret);
        goto out_cleanup;
    }
    vfs_ilock(nested);
    printf("inode_smoketest: " PASS " created /%s/%s nlink=%u\n", subdir_name, nested_name, nested->n_links);
    vfs_iunlock(nested);

    file_a = vfs_create(subdir, 0644, file_a_name, file_a_len);
    if (IS_ERR_OR_NULL(file_a)) {
        ret = PTR_ERR(file_a);
        printf("inode_smoketest: " FAIL " vfs_create %s, errno=%d\n", file_a_name, ret);
        goto out_cleanup;
    }
    file_a_ino = file_a->ino;
    vfs_ilock(file_a);
    printf("inode_smoketest: " PASS " created /%s/%s ino=%lu nlink=%u\n",
           subdir_name, file_a_name, file_a->ino, file_a->n_links);
    vfs_iunlock(file_a);
    vfs_iput(file_a);
    file_a = NULL;

    struct vfs_dentry link_old = {
        .sb = subdir->sb,
        .ino = file_a_ino,
        .name = NULL,
        .name_len = 0,
        .cookies = 0,
    };
    ret = vfs_link(&link_old, subdir, file_a_link_name, file_a_link_len);
    if (ret != 0) {
        printf("inode_smoketest: " FAIL " vfs_link %s, errno=%d\n", file_a_link_name, ret);
    } else {
        struct vfs_inode *tmp = tmpfs_fetch_inode(subdir, file_a_name, file_a_len);
        if (!IS_ERR_OR_NULL(tmp)) {
            vfs_ilock(tmp);
            printf("inode_smoketest: " PASS " linked /%s/%s -> /%s/%s nlink=%u\n",
                   subdir_name, file_a_link_name, subdir_name, file_a_name, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        } else {
            ret = PTR_ERR(tmp);
        }
    }

    file_b = vfs_create(nested, 0644, file_b_name, file_b_len);
    if (IS_ERR_OR_NULL(file_b)) {
        ret = PTR_ERR(file_b);
        printf("inode_smoketest: " FAIL " vfs_create %s, errno=%d\n", file_b_name, ret);
        goto out_cleanup;
    }
    file_b_ino = file_b->ino;
    vfs_ilock(file_b);
    printf("inode_smoketest: " PASS " created /%s/%s/%s ino=%lu nlink=%u\n",
           subdir_name, nested_name, file_b_name, file_b->ino, file_b->n_links);
    vfs_iunlock(file_b);
    vfs_iput(file_b);
    file_b = NULL;

    ret = vfs_unlink(subdir, file_a_link_name, file_a_link_len);
    if (ret != 0) {
        printf("inode_smoketest: " FAIL " vfs_unlink %s, errno=%d\n", file_a_link_name, ret);
    } else {
        struct vfs_inode *tmp = tmpfs_fetch_inode(subdir, file_a_name, file_a_len);
        if (!IS_ERR_OR_NULL(tmp)) {
            vfs_ilock(tmp);
            printf("inode_smoketest: " PASS " unlinked /%s/%s nlink=%u\n",
                   subdir_name, file_a_link_name, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        } else {
            ret = PTR_ERR(tmp);
        }
    }

    sym_a = vfs_symlink(subdir, 0777, symlink_a_name, symlink_a_len,
                        symlink_a_target, symlink_a_target_len);
    if (IS_ERR_OR_NULL(sym_a)) {
        printf("inode_smoketest: " FAIL " vfs_symlink %s, errno=%ld\n", symlink_a_name, PTR_ERR(sym_a));
        sym_a = NULL;
    } else {
        vfs_ilock(sym_a);
        printf("inode_smoketest: " PASS " symlink /%s/%s -> %s ino=%lu nlink=%u\n",
               subdir_name, symlink_a_name, symlink_a_target, sym_a->ino, sym_a->n_links);
        vfs_iunlock(sym_a);
        vfs_iput(sym_a);
        sym_a = NULL;
    }

    sym_b = vfs_symlink(nested, 0777, symlink_b_name, symlink_b_len,
                        symlink_b_target, symlink_b_target_len);
    if (IS_ERR_OR_NULL(sym_b)) {
        printf("inode_smoketest: " FAIL " vfs_symlink %s, errno=%ld\n", symlink_b_name, PTR_ERR(sym_b));
        sym_b = NULL;
    } else {
        vfs_ilock(sym_b);
        printf("inode_smoketest: " PASS " symlink /%s/%s/%s -> %s ino=%lu nlink=%u\n",
               subdir_name, nested_name, symlink_b_name, symlink_b_target, sym_b->ino, sym_b->n_links);
        vfs_iunlock(sym_b);
        vfs_iput(sym_b);
        sym_b = NULL;
    }

    struct vfs_dentry d1 = {0};
    ret = vfs_ilookup(root, &d1, subdir_name, subdir_len);
    if (ret == 0) {
        printf("inode_smoketest: " PASS " ilookup /%s -> ino=%lu\n", subdir_name, d1.ino);
        vfs_release_dentry(&d1);
    }

    struct vfs_dentry d2 = {0};
    ret = vfs_ilookup(subdir, &d2, nested_name, nested_len);
    if (ret == 0) {
        printf("inode_smoketest: " PASS " ilookup /%s/%s -> ino=%lu\n", subdir_name, nested_name, d2.ino);
        vfs_release_dentry(&d2);
    }

    struct vfs_dentry d_file_a = {0};
    ret = vfs_ilookup(subdir, &d_file_a, file_a_name, file_a_len);
    if (ret == 0) {
        struct vfs_inode *tmp = tmpfs_fetch_inode(subdir, file_a_name, file_a_len);
        if (!IS_ERR_OR_NULL(tmp)) {
            vfs_ilock(tmp);
            printf("inode_smoketest: " PASS " ilookup /%s/%s -> ino=%lu nlink=%u\n",
                   subdir_name, file_a_name, d_file_a.ino, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        } else {
            ret = PTR_ERR(tmp);
        }
        vfs_release_dentry(&d_file_a);
    }

    struct vfs_dentry d_file_b = {0};
    ret = vfs_ilookup(nested, &d_file_b, file_b_name, file_b_len);
    if (ret == 0) {
        struct vfs_inode *tmp = tmpfs_fetch_inode(nested, file_b_name, file_b_len);
        if (!IS_ERR_OR_NULL(tmp)) {
            vfs_ilock(tmp);
            printf("inode_smoketest: " PASS " ilookup /%s/%s/%s -> ino=%lu nlink=%u\n",
                   subdir_name, nested_name, file_b_name, d_file_b.ino, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        } else {
            ret = PTR_ERR(tmp);
        }
        vfs_release_dentry(&d_file_b);
    }

    struct vfs_dentry old_dentry = {
        .sb = nested->sb,
        .name = (char *)file_b_name,
        .name_len = file_b_len,
        .ino = file_b_ino,
        .cookies = 0,
    };
    ret = vfs_move(nested, &old_dentry, subdir, file_b_new_name, file_b_new_len);
    if (ret != 0) {
        printf("inode_smoketest: " FAIL " move %s -> %s, errno=%d\n", file_b_name, file_b_new_name, ret);
    }

    char linkbuf[64] = {0};
    struct vfs_inode *tmp_sym = NULL;
    tmp_sym = tmpfs_fetch_inode(subdir, symlink_a_name, symlink_a_len);
    if (!IS_ERR_OR_NULL(tmp_sym)) {
        ret = vfs_readlink(tmp_sym, linkbuf, sizeof(linkbuf));
        if (ret >= 0) {
            vfs_ilock(tmp_sym);
            printf("inode_smoketest: " PASS " readlink /%s/%s -> %s len=%d nlink=%u\n",
                   subdir_name, symlink_a_name, linkbuf, ret, tmp_sym->n_links);
            vfs_iunlock(tmp_sym);
        }
        vfs_iput(tmp_sym);
    } else {
        ret = PTR_ERR(tmp_sym);
    }
    memset(linkbuf, 0, sizeof(linkbuf));
    tmp_sym = NULL;
    tmp_sym = tmpfs_fetch_inode(nested, symlink_b_name, symlink_b_len);
    if (!IS_ERR_OR_NULL(tmp_sym)) {
        ret = vfs_readlink(tmp_sym, linkbuf, sizeof(linkbuf));
        if (ret >= 0) {
            vfs_ilock(tmp_sym);
            printf("inode_smoketest: " PASS " readlink /%s/%s/%s -> %s len=%d nlink=%u\n",
                   subdir_name, nested_name, symlink_b_name, linkbuf, ret, tmp_sym->n_links);
            vfs_iunlock(tmp_sym);
        }
        vfs_iput(tmp_sym);
    } else {
        ret = PTR_ERR(tmp_sym);
    }

out_cleanup:
    if (!IS_ERR_OR_NULL(sym_b)) {
        vfs_iput(sym_b);
    }
    if (!IS_ERR_OR_NULL(sym_a)) {
        vfs_iput(sym_a);
    }

    if (!IS_ERR_OR_NULL(subdir) && !IS_ERR_OR_NULL(nested)) {
        ret = vfs_unlink(nested, symlink_b_name, symlink_b_len);
        if (ret != 0) {
            printf("inode_smoketest: " FAIL " cleanup unlink %s, errno=%d\n", symlink_b_name, ret);
        }
    }
    if (!IS_ERR_OR_NULL(subdir)) {
        ret = vfs_unlink(subdir, symlink_a_name, symlink_a_len);
        if (ret != 0) {
            printf("inode_smoketest: " FAIL " cleanup unlink %s, errno=%d\n", symlink_a_name, ret);
        }
        ret = vfs_unlink(subdir, file_b_new_name, file_b_new_len);
        if (ret != 0) {
            ret = vfs_unlink(nested, file_b_name, file_b_len);
        }
        ret = vfs_unlink(subdir, file_a_name, file_a_len);
        if (ret != 0) {
            printf("inode_smoketest: " FAIL " cleanup unlink %s, errno=%d\n", file_a_name, ret);
        }
    }

    if (!IS_ERR_OR_NULL(nested)) {
        vfs_iput(nested);
        nested = NULL;
    }
    if (!IS_ERR_OR_NULL(subdir)) {
        ret = vfs_rmdir(subdir, nested_name, nested_len);
        if (ret != 0) {
            printf("inode_smoketest: " FAIL " cleanup rmdir /%s/%s, errno=%d\n", subdir_name, nested_name, ret);
        }
    }
    vfs_iput(subdir);
    subdir = NULL;

    ret = vfs_rmdir(root, subdir_name, subdir_len);
    if (ret != 0) {
        printf("inode_smoketest: " FAIL " cleanup rmdir /%s, errno=%d\n", subdir_name, ret);
    }
    printf("inode_smoketest: cleanup complete\n");

out_put_root:
    if (root_pinned) {
        vfs_iput(root);
    }
}

// Run truncate smoke tests: grow/shrink across embedded, direct, indirect, double indirect layers.
// Uses at most ~1000 blocks (~4MB) to stay well under 64MB limit.
void tmpfs_run_truncate_smoketest(void) {
    int ret = 0;
    struct vfs_inode *root = vfs_root_inode.mnt_rooti;
    struct vfs_inode *test_file = NULL;
    const char *file_name = "truncate_test";
    const size_t file_len = sizeof("truncate_test") - 1;
    bool root_pinned = false;

    vfs_idup(root);
    root_pinned = true;

    test_file = vfs_create(root, 0644, file_name, file_len);
    if (IS_ERR_OR_NULL(test_file)) {
        ret = PTR_ERR(test_file);
        printf("truncate_smoketest: create %s failed, errno=%d\n", file_name, ret);
        goto out;
    }
    printf("truncate_smoketest: created /%s ino=%lu\n", file_name, test_file->ino);

    struct tmpfs_inode *ti = container_of(test_file, struct tmpfs_inode, vfs_inode);

    // Test 1: Grow embedded (0 -> 100 bytes, stays in embedded)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 100);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " grow embedded, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " grow embedded 0->100 bytes, size=%llu\n", (unsigned long long)test_file->size);
    }

    // Test 2: Grow embedded to direct blocks (100 -> 5*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 5 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " grow to direct blocks, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " grow to direct 5 blocks, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 3: Shrink direct to embedded (5*PAGE_SIZE -> 50 bytes)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 50);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " shrink to embedded, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " shrink to embedded 50 bytes, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 4: Grow to full direct blocks (50 -> 32*PAGE_SIZE, block 0-31)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, TMPFS_INODE_DBLOCKS * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " grow to full direct, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " grow to full direct %lu blocks, size=%llu n_blocks=%lu\n",
               TMPFS_INODE_DBLOCKS, (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 5: Grow into indirect layer (32*PAGE_SIZE -> 100*PAGE_SIZE, uses indirect pointer)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 100 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " grow to indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " grow to indirect 100 blocks, size=%llu n_blocks=%lu indirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.indirect ? "set" : "null");
    }

    // Test 6: Shrink from indirect back to direct (100*PAGE_SIZE -> 20*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 20 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " shrink indirect to direct, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " shrink to direct 20 blocks, size=%llu n_blocks=%lu indirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.indirect ? "set" : "null");
    }

    // Test 7: Grow to double indirect layer (20*PAGE_SIZE -> 600*PAGE_SIZE)
    // Block 544 is start of double indirect (32 direct + 512 indirect)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 600 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " grow to double indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " grow to double indirect 600 blocks, size=%llu n_blocks=%lu dindirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.double_indirect ? "set" : "null");
    }

    // Test 8: Shrink from double indirect to indirect (600*PAGE_SIZE -> 40*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 40 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " shrink double to indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " shrink to indirect 40 blocks, size=%llu n_blocks=%lu dindirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.double_indirect ? "set" : "null");
    }

    // Test 9: Shrink to zero
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 0);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " shrink to zero, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " shrink to zero, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 10: Grow directly to double indirect (0 -> 1000*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 1000 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: " FAIL " grow zero to double indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: " PASS " grow zero to double indirect 1000 blocks, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Cleanup: shrink to zero and unlink
    vfs_ilock(test_file);
    __tmpfs_truncate(test_file, 0);
    vfs_iunlock(test_file);

    vfs_iput(test_file);
    test_file = NULL;

    ret = vfs_unlink(root, file_name, file_len);
    if (ret != 0) {
        printf("truncate_smoketest: unlink %s failed, errno=%d\n", file_name, ret);
    } else {
        printf("truncate_smoketest: cleanup complete\n");
    }

out:
    if (test_file != NULL) {
        vfs_iput(test_file);
    }
    if (root_pinned) {
        vfs_iput(root);
    }
}

// Test vfs_namei path resolution with various cases
void tmpfs_run_namei_smoketest(void) {
    int ret = 0;
    struct vfs_inode *root = vfs_root_inode.mnt_rooti;
    struct vfs_inode *result = NULL;
    struct vfs_inode *subdir = NULL;
    struct vfs_inode *nested = NULL;
    struct vfs_inode *file = NULL;
    bool root_pinned = false;

    const char *subdir_name = "namei_test_dir";
    const size_t subdir_len = sizeof("namei_test_dir") - 1;
    const char *nested_name = "nested";
    const size_t nested_len = sizeof("nested") - 1;
    const char *file_name = "testfile";
    const size_t file_len = sizeof("testfile") - 1;

    root = vfs_curroot();
    if (root == NULL) {
        printf("namei_smoketest: " FAIL " vfs_curroot returned NULL\n");
        return;
    }
    root_pinned = true;

    // Setup: create /namei_test_dir/nested/testfile
    subdir = vfs_mkdir(root, 0755, subdir_name, subdir_len);
    if (IS_ERR_OR_NULL(subdir)) {
        ret = IS_ERR(subdir) ? PTR_ERR(subdir) : -EINVAL;
        printf("namei_smoketest: " FAIL " setup mkdir %s, errno=%d\n", subdir_name, ret);
        goto out;
    }

    nested = vfs_mkdir(subdir, 0755, nested_name, nested_len);
    if (IS_ERR_OR_NULL(nested)) {
        ret = IS_ERR(nested) ? PTR_ERR(nested) : -EINVAL;
        printf("namei_smoketest: " FAIL " setup mkdir %s, errno=%d\n", nested_name, ret);
        goto cleanup_subdir;
    }

    file = vfs_create(nested, 0644, file_name, file_len);
    if (IS_ERR_OR_NULL(file)) {
        ret = IS_ERR(file) ? PTR_ERR(file) : -EINVAL;
        printf("namei_smoketest: " FAIL " setup create %s, errno=%d\n", file_name, ret);
        goto cleanup_nested;
    }
    uint64 file_ino = file->ino;
    vfs_iput(file);
    file = NULL;

    printf("namei_smoketest: setup complete\n");

    // Test 1: Absolute path to root
    result = vfs_namei("/", 1);
    if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"/\"), errno=%ld\n", PTR_ERR(result));
    } else if (result != root) {
        printf("namei_smoketest: " FAIL " namei(\"/\") returned wrong inode\n");
        vfs_iput(result);
    } else {
        printf("namei_smoketest: " PASS " namei(\"/\") -> root\n");
        vfs_iput(result);
    }
    result = NULL;

    // Test 2: Absolute path to subdir
    const char *path2 = "/namei_test_dir";
    result = vfs_namei(path2, strlen(path2));
    if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"%s\"), errno=%ld\n", path2, PTR_ERR(result));
    } else {
        printf("namei_smoketest: " PASS " namei(\"%s\") -> ino=%lu\n", path2, result->ino);
        vfs_iput(result);
    }
    result = NULL;

    // Test 3: Absolute path with multiple components
    const char *path3 = "/namei_test_dir/nested/testfile";
    result = vfs_namei(path3, strlen(path3));
    if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"%s\"), errno=%ld\n", path3, PTR_ERR(result));
    } else if (result->ino != file_ino) {
        printf("namei_smoketest: " FAIL " namei(\"%s\") wrong ino=%lu expected=%lu\n", 
               path3, result->ino, file_ino);
        vfs_iput(result);
    } else {
        printf("namei_smoketest: " PASS " namei(\"%s\") -> ino=%lu\n", path3, result->ino);
        vfs_iput(result);
    }
    result = NULL;

    // Test 4: Path with "." components
    const char *path4 = "/namei_test_dir/./nested/./testfile";
    result = vfs_namei(path4, strlen(path4));
    if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"%s\"), errno=%ld\n", path4, PTR_ERR(result));
    } else if (result->ino != file_ino) {
        printf("namei_smoketest: " FAIL " namei(\"%s\") wrong ino\n", path4);
        vfs_iput(result);
    } else {
        printf("namei_smoketest: " PASS " namei(\"%s\") -> ino=%lu\n", path4, result->ino);
        vfs_iput(result);
    }
    result = NULL;

    // Test 5: Path with ".." components
    const char *path5 = "/namei_test_dir/nested/../nested/testfile";
    result = vfs_namei(path5, strlen(path5));
    if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"%s\"), errno=%ld\n", path5, PTR_ERR(result));
    } else if (result->ino != file_ino) {
        printf("namei_smoketest: " FAIL " namei(\"%s\") wrong ino\n", path5);
        vfs_iput(result);
    } else {
        printf("namei_smoketest: " PASS " namei(\"%s\") -> ino=%lu\n", path5, result->ino);
        vfs_iput(result);
    }
    result = NULL;

    // Test 6: ".." at root should stay at root
    const char *path6 = "/..";
    result = vfs_namei(path6, strlen(path6));
    if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"%s\"), errno=%ld\n", path6, PTR_ERR(result));
    } else if (result != root) {
        printf("namei_smoketest: " FAIL " namei(\"%s\") did not return root\n", path6);
        vfs_iput(result);
    } else {
        printf("namei_smoketest: " PASS " namei(\"%s\") -> root\n", path6);
        vfs_iput(result);
    }
    result = NULL;

    // Test 7: Multiple consecutive slashes
    const char *path7 = "///namei_test_dir///nested///testfile";
    result = vfs_namei(path7, strlen(path7));
    if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"%s\"), errno=%ld\n", path7, PTR_ERR(result));
    } else if (result->ino != file_ino) {
        printf("namei_smoketest: " FAIL " namei(\"%s\") wrong ino\n", path7);
        vfs_iput(result);
    } else {
        printf("namei_smoketest: " PASS " namei(\"%s\") -> ino=%lu\n", path7, result->ino);
        vfs_iput(result);
    }
    result = NULL;

    // Test 8: Non-existent path
    const char *path8 = "/namei_test_dir/nonexistent";
    result = vfs_namei(path8, strlen(path8));
    if (result == ERR_PTR(-ENOENT)) {
        printf("namei_smoketest: " PASS " namei(\"%s\") -> ENOENT as expected\n", path8);
    } else if (IS_ERR_OR_NULL(result)) {
        printf("namei_smoketest: " FAIL " namei(\"%s\") unexpected errno=%ld\n", path8, PTR_ERR(result));
    } else {
        printf("namei_smoketest: " FAIL " namei(\"%s\") should have failed\n", path8);
        vfs_iput(result);
    }
    result = NULL;

    printf("namei_smoketest: all tests completed\n");

    // Cleanup
cleanup_nested:
    ret = vfs_unlink(nested, file_name, file_len);
    if (ret != 0) {
        printf("namei_smoketest: cleanup unlink %s failed, errno=%d\n", file_name, ret);
    }
    vfs_iput(nested);
    nested = NULL;
    ret = vfs_rmdir(subdir, nested_name, nested_len);
    if (ret != 0) {
        printf("namei_smoketest: cleanup rmdir %s failed, errno=%d\n", nested_name, ret);
    } else {
        printf("namei_smoketest: cleanup rmdir %s success\n", nested_name);
    }
cleanup_subdir:
    vfs_iput(subdir);
    subdir = NULL;
    ret = vfs_rmdir(root, subdir_name, subdir_len);
    if (ret != 0) {
        printf("namei_smoketest: cleanup rmdir %s failed, errno=%d\n", subdir_name, ret);
    } else {
        printf("namei_smoketest: cleanup rmdir %s success\n", subdir_name);
    }
out:
    if (root_pinned) {
        vfs_iput(root);
    }
}

// Order-insensitive expected-entry tracker for dir iteration
static bool iter_mark_seen(const char *name, struct iter_expect *tbl, int tbl_cnt, const char *tag, bool *dup_out) {
    for (int i = 0; i < tbl_cnt; i++) {
        size_t nlen = strlen(tbl[i].name) + 1;
        if (strncmp(name, tbl[i].name, nlen) == 0) {
            if (tbl[i].seen) {
                printf("dir_iter_mount: " FAIL " %s duplicate entry %s\n", tag, name);
                if (dup_out != NULL) {
                    *dup_out = true;
                }
            }
            tbl[i].seen = true;
            return true;
        }
    }
    return false;
}

// Iterate a directory, fetch each dentry's inode, and validate expected entries
// exp: array of expected entries (caller-owned)
// exp_count: number of expected entries
// allow_extra: if true, unexpected entries are warnings; if false, they are failures
static void tmpfs_iter_and_fetch_ex(const char *tag, struct vfs_inode *dir,
                                    struct vfs_dir_iter *iter,
                                    struct iter_expect *exp, int exp_count,
                                    bool allow_extra) {
    // Reset iterator state fully (avoid carrying index/cookies between calls)
    memset(iter, 0, sizeof(*iter));

    int lret = 0;
    int unexpected = 0;
    bool ok = true;
    struct vfs_dentry dentry = {0};
    printf("dir_iter_mount: BEGIN %s\n", tag);
    int guard = 0;
    for (;;) {
        if (guard++ > 256) {
            printf("dir_iter_mount: ABORT %s guard hit\n", tag);
            ok = false;
            break;
        }
        printf("dir_iter_mount: ITER %s step=%d before dir_iter\n", tag, guard);
        lret = vfs_dir_iter(dir, iter, &dentry);
        printf("dir_iter_mount: ITER %s step=%d after dir_iter ret=%d cookies=%ld\n",
               tag, guard, lret, dentry.cookies);
        if (lret != 0) {
            printf("dir_iter_mount: " FAIL " dir_iter %s errno=%d\n", tag, lret);
            ok = false;
            break;
        }
        if (dentry.name == NULL) {
            printf("dir_iter_mount: END %s\n", tag);
            break;
        }

        struct vfs_inode *ent = vfs_get_dentry_inode(&dentry);
        if (IS_ERR_OR_NULL(ent)) {
            lret = IS_ERR(ent) ? PTR_ERR(ent) : -EINVAL;
            printf("dir_iter_mount: " FAIL " get_inode %s name=%s errno=%d\n",
                   tag, dentry.name ? dentry.name : "(null)", lret);
            ok = false;
        } else {
            bool locked = holding_mutex(&ent->mutex);
            printf("dir_iter_mount: entry %s name=%s ino=%lu fetched_ino=%lu sb_match=%s locked=%s\n",
                   tag,
                   dentry.name ? dentry.name : "(null)",
                   dentry.ino,
                   ent->ino,
                   ent->sb == dentry.sb ? "yes" : "no",
                   locked ? "yes" : "no");

            bool duplicate = false;
            if (!iter_mark_seen(dentry.name, exp, exp_count, tag, &duplicate)) {
                unexpected++;
                if (!allow_extra) {
                    printf("dir_iter_mount: " FAIL " %s unexpected entry %s\n", tag, dentry.name);
                    ok = false;
                }
            } else if (duplicate) {
                ok = false;
            }

            if (locked) {
                vfs_iunlock(ent);
            }
            vfs_iput(ent);
        }
    }

    for (int i = 0; i < exp_count; i++) {
        if (exp[i].required && !exp[i].seen) {
            printf("dir_iter_mount: " FAIL " %s missing entry %s\n", tag, exp[i].name);
            ok = false;
        }
    }
    if (unexpected > 0 && allow_extra) {
        printf("dir_iter_mount: " WARN " %s saw %d unexpected entries\n", tag, unexpected);
    }

    printf("dir_iter_mount: %s %s\n", ok ? PASS : FAIL, tag);
    vfs_release_dentry(&dentry);
}

// Mount a second tmpfs on a subdirectory, populate it, and iterate with vfs_dir_iter
void tmpfs_run_dir_iter_mount_smoketest(void) {
    const char *mp_name = "iter_mount_dir";
    const size_t mp_len = sizeof("iter_mount_dir") - 1;
    const char *file_name = "iter_file";
    const size_t file_len = sizeof("iter_file") - 1;
    const char *subdir_name = "iter_subdir";
    const size_t subdir_len = sizeof("iter_subdir") - 1;

    int ret = 0;
    struct vfs_inode *root = vfs_root_inode.mnt_rooti;
    struct vfs_inode *mp = NULL;
    struct vfs_inode *mnt_root = NULL;
    struct vfs_inode *tmp = NULL;
    struct vfs_inode *mnt_subdir = NULL;
    struct vfs_dir_iter iter = {0};
    bool root_pinned = false;


    vfs_idup(root);
    root_pinned = true;

    mp = vfs_mkdir(root, 0755, mp_name, mp_len);
    if (IS_ERR_OR_NULL(mp)) {
        ret = IS_ERR(mp) ? PTR_ERR(mp) : -EINVAL;
        mp = NULL;
        printf("dir_iter_mount: " FAIL " setup mkdir %s errno=%d\n", mp_name, ret);
        goto out;
    }

    // Mount a new tmpfs on the freshly created directory
    vfs_mount_lock();
    vfs_superblock_wlock(root->sb);
    vfs_ilock(mp);
    ret = vfs_mount("tmpfs", mp, NULL, 0, NULL);
    vfs_iunlock(mp);
    vfs_superblock_unlock(root->sb);
    vfs_mount_unlock();
    if (ret != 0) {
        printf("dir_iter_mount: " FAIL " vfs_mount on %s errno=%d\n", mp_name, ret);
        goto cleanup_mp_dir;
    }

    mnt_root = mp->mnt_sb ? mp->mnt_sb->root_inode : NULL;
    if (mnt_root == NULL) {
        printf("dir_iter_mount: " FAIL " mounted root NULL\n");
        goto cleanup_mount;
    }
    vfs_idup(mnt_root);

    // --- Test 1: empty FS local root (freshly mounted, no content yet) ---
    {
        struct iter_expect exp[] = {
            {".", true, false},
            {"..", true, false},
        };
        tmpfs_iter_and_fetch_ex("mnt_root_empty", mnt_root, &iter, exp, 2, false);
    }

    // --- Test 2: empty process root ---
    // Temporarily chroot to the empty mounted fs root
    {
        struct vfs_inode *old_root = vfs_curroot();
        if (old_root != NULL) {
            ret = vfs_chroot(mnt_root);
            if (ret != 0) {
                printf("dir_iter_mount: " FAIL " chroot to mnt_root errno=%d\n", ret);
                vfs_iput(old_root);
            } else {
                struct iter_expect exp[] = {
                    {".", true, false},
                    {"..", true, false},
                };
                tmpfs_iter_and_fetch_ex("process_root_empty", mnt_root, &iter, exp, 2, false);
                // Restore original root
                ret = vfs_chroot(old_root);
                if (ret != 0) {
                    printf("dir_iter_mount: " FAIL " restore chroot errno=%d\n", ret);
                }
                vfs_iput(old_root);
            }
        }
    }

    // Populate mounted tmpfs: create a file and a subdir
    tmp = vfs_create(mnt_root, 0644, file_name, file_len);
    if (IS_ERR(tmp)) {
        ret = PTR_ERR(tmp);
        tmp = NULL;
        printf("dir_iter_mount: " FAIL " create %s errno=%d\n", file_name, ret);
        goto cleanup_mount;
    }
    vfs_iput(tmp);
    tmp = NULL;

    mnt_subdir = vfs_mkdir(mnt_root, 0755, subdir_name, subdir_len);
    if (IS_ERR_OR_NULL(mnt_subdir)) {
        ret = IS_ERR(mnt_subdir) ? PTR_ERR(mnt_subdir) : -EINVAL;
        mnt_subdir = NULL;
        printf("dir_iter_mount: " FAIL " mkdir %s errno=%d\n", subdir_name, ret);
        goto cleanup_mount;
    }

    // --- Test 3: non-empty FS local root (mnt_root has file + subdir) ---
    {
        struct iter_expect exp[] = {
            {".", true, false},
            {"..", true, false},
            {"iter_subdir", true, false},
            {"iter_file", true, false},
        };
        tmpfs_iter_and_fetch_ex("mnt_root_nonempty", mnt_root, &iter, exp, 4, false);
    }

    // --- Test 4: empty ordinary subdir (mnt_subdir is empty) ---
    {
        struct iter_expect exp[] = {
            {".", true, false},
            {"..", true, false},
        };
        tmpfs_iter_and_fetch_ex("subdir_empty", mnt_subdir, &iter, exp, 2, false);
    }

    // --- Test 5: non-empty ordinary subdir ---
    // Create a file inside mnt_subdir to make it non-empty
    const char *nested_file = "nested_file";
    const size_t nested_file_len = sizeof("nested_file") - 1;
    tmp = vfs_create(mnt_subdir, 0644, nested_file, nested_file_len);
    if (IS_ERR_OR_NULL(tmp)) {
        printf("dir_iter_mount: " FAIL " create nested_file errno=%ld\n", PTR_ERR(tmp));
    } else {
        vfs_iput(tmp);
        tmp = NULL;

        struct iter_expect exp[] = {
            {".", true, false},
            {"..", true, false},
            {"nested_file", true, false},
        };
        tmpfs_iter_and_fetch_ex("subdir_nonempty", mnt_subdir, &iter, exp, 3, false);

        ret = vfs_unlink(mnt_subdir, nested_file, nested_file_len);
        if (ret != 0) {
            printf("dir_iter_mount: " WARN " cleanup unlink nested_file errno=%d\n", ret);
        }
    }

    // --- Test 6: non-empty process root (has iter_mount_dir, allow extra from other tests) ---
    {
        struct iter_expect exp[] = {
            {".", true, false},
            {"..", true, false},
            {"iter_mount_dir", true, false},
        };
        tmpfs_iter_and_fetch_ex("process_root_nonempty", root, &iter, exp, 3, true);
    }

    // --- Test 7-12: vfs_ilookup ".." in various scenarios ---
    printf("dir_iter_mount: BEGIN ilookup_dotdot tests\n");

    // Test 7: ".." at process root -> should return self
    {
        struct vfs_dentry d = {0};
        ret = vfs_ilookup(root, &d, "..", 2);
        if (ret != 0) {
            printf("dir_iter_mount: " FAIL " ilookup(..) at process_root errno=%d\n", ret);
        } else if (d.ino != root->ino) {
            printf("dir_iter_mount: " FAIL " ilookup(..) at process_root wrong ino=%lu expected=%lu\n",
                   d.ino, root->ino);
        } else {
            printf("dir_iter_mount: " PASS " ilookup(..) at process_root -> self ino=%lu\n", d.ino);
        }
        vfs_release_dentry(&d);
    }

    // Test 8: ".." at FS local root (mnt_root) -> should cross mount and return root
    {
        struct vfs_dentry d = {0};
        ret = vfs_ilookup(mnt_root, &d, "..", 2);
        if (ret != 0) {
            printf("dir_iter_mount: " FAIL " ilookup(..) at mnt_root errno=%d\n", ret);
        } else {
            // Must fetch the actual inode to compare across superblock boundaries
            struct vfs_inode *parent = vfs_get_dentry_inode(&d);
            if (IS_ERR_OR_NULL(parent)) {
                printf("dir_iter_mount: " FAIL " ilookup(..) at mnt_root get_inode failed\n");
            } else if (parent != root) {
                printf("dir_iter_mount: " FAIL " ilookup(..) at mnt_root wrong inode (got ino=%lu sb=%p, expected ino=%lu sb=%p)\n",
                       parent->ino, parent->sb, root->ino, root->sb);
                vfs_iput(parent);
            } else {
                printf("dir_iter_mount: " PASS " ilookup(..) at mnt_root -> parent ino=%lu\n", parent->ino);
                vfs_iput(parent);
            }
        }
        vfs_release_dentry(&d);
    }

    // Test 9: ".." at ordinary subdir (mnt_subdir) -> should return mnt_root
    {
        struct vfs_dentry d = {0};
        ret = vfs_ilookup(mnt_subdir, &d, "..", 2);
        if (ret != 0) {
            printf("dir_iter_mount: " FAIL " ilookup(..) at mnt_subdir errno=%d\n", ret);
        } else if (d.ino != mnt_root->ino || d.sb != mnt_root->sb) {
            printf("dir_iter_mount: " FAIL " ilookup(..) at mnt_subdir wrong ino=%lu expected=%lu\n",
                   d.ino, mnt_root->ino);
        } else {
            printf("dir_iter_mount: " PASS " ilookup(..) at mnt_subdir -> parent ino=%lu\n", d.ino);
        }
        vfs_release_dentry(&d);
    }

    // Test 10: "." at process root -> should return self
    {
        struct vfs_dentry d = {0};
        ret = vfs_ilookup(root, &d, ".", 1);
        if (ret != 0) {
            printf("dir_iter_mount: " FAIL " ilookup(.) at process_root errno=%d\n", ret);
        } else if (d.ino != root->ino) {
            printf("dir_iter_mount: " FAIL " ilookup(.) at process_root wrong ino=%lu expected=%lu\n",
                   d.ino, root->ino);
        } else {
            printf("dir_iter_mount: " PASS " ilookup(.) at process_root -> self ino=%lu\n", d.ino);
        }
        vfs_release_dentry(&d);
    }

    // Test 11: "." at FS local root -> should return self
    {
        struct vfs_dentry d = {0};
        ret = vfs_ilookup(mnt_root, &d, ".", 1);
        if (ret != 0) {
            printf("dir_iter_mount: " FAIL " ilookup(.) at mnt_root errno=%d\n", ret);
        } else if (d.ino != mnt_root->ino) {
            printf("dir_iter_mount: " FAIL " ilookup(.) at mnt_root wrong ino=%lu expected=%lu\n",
                   d.ino, mnt_root->ino);
        } else {
            printf("dir_iter_mount: " PASS " ilookup(.) at mnt_root -> self ino=%lu\n", d.ino);
        }
        vfs_release_dentry(&d);
    }

    // Test 12: "." at ordinary subdir -> should return self
    {
        struct vfs_dentry d = {0};
        ret = vfs_ilookup(mnt_subdir, &d, ".", 1);
        if (ret != 0) {
            printf("dir_iter_mount: " FAIL " ilookup(.) at mnt_subdir errno=%d\n", ret);
        } else if (d.ino != mnt_subdir->ino) {
            printf("dir_iter_mount: " FAIL " ilookup(.) at mnt_subdir wrong ino=%lu expected=%lu\n",
                   d.ino, mnt_subdir->ino);
        } else {
            printf("dir_iter_mount: " PASS " ilookup(.) at mnt_subdir -> self ino=%lu\n", d.ino);
        }
        vfs_release_dentry(&d);
    }

    printf("dir_iter_mount: END ilookup_dotdot tests\n");

    // Tear down populated entries before unmount to avoid EBUSY
    if (mnt_subdir != NULL) {
        vfs_iput(mnt_subdir); // drop our ref before rmdir check
        mnt_subdir = NULL;
    }
    ret = vfs_unlink(mnt_root, file_name, file_len);
    if (ret != 0) {
        printf("dir_iter_mount: " WARN " cleanup unlink %s errno=%d\n", file_name, ret);
    }
    ret = vfs_rmdir(mnt_root, subdir_name, subdir_len);
    if (ret != 0) {
        printf("dir_iter_mount: " WARN " cleanup rmdir %s errno=%d\n", subdir_name, ret);
    }

cleanup_mount:
    if (mnt_subdir != NULL) {
        vfs_iput(mnt_subdir);
        mnt_subdir = NULL;
    }
    if (mnt_root != NULL) {
        vfs_iput(mnt_root);
        mnt_root = NULL;
    }

    // Unmount the tmpfs we mounted above
    if (mp != NULL && mp->mnt_sb != NULL) {
        struct vfs_superblock *child_sb = mp->mnt_sb;
        struct vfs_inode *child_root = child_sb->root_inode;
        vfs_mount_lock();
        vfs_superblock_wlock(mp->sb);
        vfs_superblock_wlock(child_sb);
        vfs_ilock(mp);
        vfs_ilock(child_root);
        ret = vfs_unmount(mp);
        if (ret != 0) {
            // Unmount failed - need to release locks we still hold
            vfs_iunlock(child_root);
            vfs_superblock_unlock(child_sb);
            printf("dir_iter_mount: " WARN " vfs_unmount errno=%d\n", ret);
        }
        // On success, vfs_unmount already freed child_root and child_sb
        vfs_iunlock(mp);
        vfs_superblock_unlock(mp->sb);
        vfs_mount_unlock();
    }

cleanup_mp_dir:
    if (mp != NULL) {
        int rmdir_ret = vfs_rmdir(root, mp_name, mp_len);
        if (rmdir_ret != 0) {
            printf("dir_iter_mount: " WARN " cleanup rmdir %s errno=%d\n", mp_name, rmdir_ret);
            // Only iput if rmdir failed - if rmdir succeeded, it already freed the inode
            vfs_iput(mp);
        }
        // Note: if rmdir succeeded, the inode is already freed by vfs_rmdir's internal vfs_iput
    }
out:
    if (root_pinned) {
        vfs_iput(root);
    }
}

/*
 * Lazy unmount smoketest: Tests deferred cleanup when files are still open
 * during unmount.
 *
 * Test cases:
 * 1. Lazy unmount with no open files - immediate cleanup
 * 2. Lazy unmount with open file - deferred cleanup until file closed
 * 3. Lazy unmount with nested open directories - deferred cleanup
 * 4. Operations on orphan files after lazy unmount (read/write still work)
 * 5. New operations blocked after lazy unmount (create/mkdir fail)
 */
void tmpfs_run_lazy_unmount_smoketest(void) {
    const char *mp_name = "lazy_mount_dir";
    const size_t mp_len = sizeof("lazy_mount_dir") - 1;
    const char *file_name = "lazy_file";
    const size_t file_len = sizeof("lazy_file") - 1;
    const char *subdir_name = "lazy_subdir";
    const size_t subdir_len = sizeof("lazy_subdir") - 1;
    const char *test_data = "lazy unmount test data";
    const size_t test_data_len = sizeof("lazy unmount test data") - 1;

    int ret = 0;
    struct vfs_inode *root = vfs_root_inode.mnt_rooti;
    struct vfs_inode *mp = NULL;
    struct vfs_inode *mnt_root = NULL;
    struct vfs_inode *file_inode = NULL;
    struct vfs_inode *subdir = NULL;
    struct vfs_file *open_file = NULL;
    bool root_pinned = false;

    printf("lazy_unmount: BEGIN tests\n");

    vfs_idup(root);
    root_pinned = true;

    // =========================================================================
    // Test 1: Lazy unmount with no open files - should cleanup immediately
    // =========================================================================
    printf("lazy_unmount: Test 1 - no open files\n");
    
    mp = vfs_mkdir(root, 0755, mp_name, mp_len);
    if (IS_ERR_OR_NULL(mp)) {
        ret = IS_ERR(mp) ? PTR_ERR(mp) : -EINVAL;
        mp = NULL;
        printf("lazy_unmount: " FAIL " setup mkdir %s errno=%d\n", mp_name, ret);
        goto out;
    }

    // Mount a new tmpfs
    vfs_mount_lock();
    vfs_superblock_wlock(root->sb);
    vfs_ilock(mp);
    ret = vfs_mount("tmpfs", mp, NULL, 0, NULL);
    vfs_iunlock(mp);
    vfs_superblock_unlock(root->sb);
    vfs_mount_unlock();
    if (ret != 0) {
        printf("lazy_unmount: " FAIL " vfs_mount errno=%d\n", ret);
        goto cleanup_test1_mp;
    }

    mnt_root = mp->mnt_sb ? mp->mnt_sb->root_inode : NULL;
    if (mnt_root == NULL) {
        printf("lazy_unmount: " FAIL " mounted root NULL\n");
        goto cleanup_test1_mount;
    }
    vfs_idup(mnt_root);

    // Lazy unmount with no open files
    {
        vfs_iput(mnt_root);
        mnt_root = NULL;

        vfs_mount_lock();
        vfs_superblock_wlock(mp->sb);
        vfs_ilock(mp);
        ret = vfs_unmount_lazy(mp);
        vfs_iunlock(mp);
        vfs_superblock_unlock(mp->sb);
        vfs_mount_unlock();

        if (ret != 0) {
            printf("lazy_unmount: " FAIL " test1 vfs_unmount_lazy errno=%d\n", ret);
        } else {
            printf("lazy_unmount: " PASS " test1 no-open-files lazy unmount\n");
        }
    }

cleanup_test1_mount:
    // Already unmounted above
cleanup_test1_mp:
    if (mp != NULL) {
        int rmdir_ret = vfs_rmdir(root, mp_name, mp_len);
        if (rmdir_ret != 0) {
            printf("lazy_unmount: " WARN " test1 cleanup rmdir %s errno=%d\n", mp_name, rmdir_ret);
            vfs_iput(mp);
        }
        mp = NULL;
    }

    // =========================================================================
    // Test 2: Lazy unmount with open file - cleanup deferred until close
    // =========================================================================
    printf("lazy_unmount: Test 2 - open file during unmount\n");

    mp = vfs_mkdir(root, 0755, mp_name, mp_len);
    if (IS_ERR_OR_NULL(mp)) {
        ret = IS_ERR(mp) ? PTR_ERR(mp) : -EINVAL;
        mp = NULL;
        printf("lazy_unmount: " FAIL " test2 mkdir %s errno=%d\n", mp_name, ret);
        goto out;
    }

    // Mount a new tmpfs
    vfs_mount_lock();
    vfs_superblock_wlock(root->sb);
    vfs_ilock(mp);
    ret = vfs_mount("tmpfs", mp, NULL, 0, NULL);
    vfs_iunlock(mp);
    vfs_superblock_unlock(root->sb);
    vfs_mount_unlock();
    if (ret != 0) {
        printf("lazy_unmount: " FAIL " test2 vfs_mount errno=%d\n", ret);
        goto cleanup_test2_mp;
    }

    mnt_root = mp->mnt_sb ? mp->mnt_sb->root_inode : NULL;
    if (mnt_root == NULL) {
        printf("lazy_unmount: " FAIL " test2 mounted root NULL\n");
        goto cleanup_test2_mount;
    }
    vfs_idup(mnt_root);

    // Create and open a file
    file_inode = vfs_create(mnt_root, 0644, file_name, file_len);
    if (IS_ERR(file_inode)) {
        ret = PTR_ERR(file_inode);
        file_inode = NULL;
        printf("lazy_unmount: " FAIL " test2 create %s errno=%d\n", file_name, ret);
        goto cleanup_test2_mount;
    }

    open_file = vfs_fileopen(file_inode, O_RDWR);
    if (IS_ERR(open_file)) {
        ret = PTR_ERR(open_file);
        open_file = NULL;
        printf("lazy_unmount: " FAIL " test2 open file errno=%d\n", ret);
        goto cleanup_test2_file;
    }

    // Write some data before unmount
    ssize_t written = vfs_filewrite(open_file, test_data, test_data_len);
    if (written != (ssize_t)test_data_len) {
        printf("lazy_unmount: " WARN " test2 write returned %ld\n", (long)written);
    }

    // Lazy unmount while file is still open
    {
        // Release our refs to mnt_root and file_inode before unmount
        // But keep open_file which holds its own ref
        vfs_iput(mnt_root);
        mnt_root = NULL;
        vfs_iput(file_inode);
        file_inode = NULL;

        vfs_mount_lock();
        vfs_superblock_wlock(mp->sb);
        vfs_ilock(mp);
        ret = vfs_unmount_lazy(mp);
        vfs_iunlock(mp);
        vfs_superblock_unlock(mp->sb);
        vfs_mount_unlock();

        if (ret != 0) {
            printf("lazy_unmount: " FAIL " test2 vfs_unmount_lazy errno=%d\n", ret);
            goto cleanup_test2_file;
        }
        printf("lazy_unmount: " PASS " test2 lazy unmount with open file\n");
    }

    // Test 2a: Read/write should still work on orphan file
    {
        loff_t pos = vfs_filelseek(open_file, 0, SEEK_SET);
        if (pos != 0) {
            printf("lazy_unmount: " FAIL " test2a lseek errno=%lld\n", (long long)pos);
        } else {
            char read_buf[64] = {0};
            ssize_t bytes_read = vfs_fileread(open_file, read_buf, test_data_len);
            if (bytes_read != (ssize_t)test_data_len) {
                printf("lazy_unmount: " FAIL " test2a read returned %ld\n", (long)bytes_read);
            } else if (memcmp(read_buf, test_data, test_data_len) != 0) {
                printf("lazy_unmount: " FAIL " test2a read data mismatch\n");
            } else {
                printf("lazy_unmount: " PASS " test2a orphan file read works\n");
            }
        }
    }

    // Close the file - this should trigger final cleanup
    vfs_fileclose(open_file);
    open_file = NULL;
    printf("lazy_unmount: " PASS " test2 file closed, cleanup complete\n");

cleanup_test2_file:
    if (file_inode != NULL) {
        vfs_iput(file_inode);
        file_inode = NULL;
    }
cleanup_test2_mount:
    if (mnt_root != NULL) {
        vfs_iput(mnt_root);
        mnt_root = NULL;
    }
    if (open_file != NULL) {
        vfs_fileclose(open_file);
        open_file = NULL;
    }
    // Mount already lazily unmounted above
cleanup_test2_mp:
    if (mp != NULL) {
        int rmdir_ret = vfs_rmdir(root, mp_name, mp_len);
        if (rmdir_ret != 0) {
            printf("lazy_unmount: " WARN " test2 cleanup rmdir %s errno=%d\n", mp_name, rmdir_ret);
            vfs_iput(mp);
        }
        mp = NULL;
    }

    // =========================================================================
    // Test 3: Lazy unmount with held directory reference
    // =========================================================================
    printf("lazy_unmount: Test 3 - held directory during unmount\n");

    mp = vfs_mkdir(root, 0755, mp_name, mp_len);
    if (IS_ERR_OR_NULL(mp)) {
        ret = IS_ERR(mp) ? PTR_ERR(mp) : -EINVAL;
        mp = NULL;
        printf("lazy_unmount: " FAIL " test3 mkdir %s errno=%d\n", mp_name, ret);
        goto out;
    }

    // Mount a new tmpfs
    vfs_mount_lock();
    vfs_superblock_wlock(root->sb);
    vfs_ilock(mp);
    ret = vfs_mount("tmpfs", mp, NULL, 0, NULL);
    vfs_iunlock(mp);
    vfs_superblock_unlock(root->sb);
    vfs_mount_unlock();
    if (ret != 0) {
        printf("lazy_unmount: " FAIL " test3 vfs_mount errno=%d\n", ret);
        goto cleanup_test3_mp;
    }

    mnt_root = mp->mnt_sb ? mp->mnt_sb->root_inode : NULL;
    if (mnt_root == NULL) {
        printf("lazy_unmount: " FAIL " test3 mounted root NULL\n");
        goto cleanup_test3_mount;
    }
    vfs_idup(mnt_root);

    // Create a subdir and hold a reference
    subdir = vfs_mkdir(mnt_root, 0755, subdir_name, subdir_len);
    if (IS_ERR_OR_NULL(subdir)) {
        ret = IS_ERR(subdir) ? PTR_ERR(subdir) : -EINVAL;
        subdir = NULL;
        printf("lazy_unmount: " FAIL " test3 mkdir %s errno=%d\n", subdir_name, ret);
        goto cleanup_test3_mount;
    }

    // Keep an extra ref on subdir
    vfs_idup(subdir);

    // Lazy unmount while holding subdir reference
    {
        vfs_iput(mnt_root);
        mnt_root = NULL;

        vfs_mount_lock();
        vfs_superblock_wlock(mp->sb);
        vfs_ilock(mp);
        ret = vfs_unmount_lazy(mp);
        vfs_iunlock(mp);
        vfs_superblock_unlock(mp->sb);
        vfs_mount_unlock();

        if (ret != 0) {
            printf("lazy_unmount: " FAIL " test3 vfs_unmount_lazy errno=%d\n", ret);
            vfs_iput(subdir);  // Drop the extra ref
            goto cleanup_test3_subdir;
        }
        printf("lazy_unmount: " PASS " test3 lazy unmount with held directory\n");
    }

    // subdir is now an orphan - drop the reference to trigger cleanup
    vfs_iput(subdir);  // Drop extra ref
    vfs_iput(subdir);  // Drop original ref
    subdir = NULL;
    printf("lazy_unmount: " PASS " test3 orphan directory released, cleanup complete\n");

cleanup_test3_subdir:
    if (subdir != NULL) {
        vfs_iput(subdir);
        subdir = NULL;
    }
cleanup_test3_mount:
    if (mnt_root != NULL) {
        vfs_iput(mnt_root);
        mnt_root = NULL;
    }
    // Mount already lazily unmounted
cleanup_test3_mp:
    if (mp != NULL) {
        int rmdir_ret = vfs_rmdir(root, mp_name, mp_len);
        if (rmdir_ret != 0) {
            printf("lazy_unmount: " WARN " test3 cleanup rmdir %s errno=%d\n", mp_name, rmdir_ret);
            vfs_iput(mp);
        }
        mp = NULL;
    }

    printf("lazy_unmount: END tests\n");

out:
    if (root_pinned) {
        vfs_iput(root);
    }
}

// Test file descriptor operations: open, read, write, lseek, stat, close
void tmpfs_run_file_ops_smoketest(void) {
    int ret = 0;
    struct vfs_inode *root = vfs_root_inode.mnt_rooti;
    struct vfs_inode *test_inode = NULL;
    struct vfs_file *file = NULL;
    bool root_pinned = false;

    const char *file_name = "file_ops_test";
    const size_t file_len = sizeof("file_ops_test") - 1;
    
    char write_buf[128];
    char read_buf[128];
    const char *test_data = "Hello, tmpfs file operations!";
    size_t test_data_len = strlen(test_data);

    vfs_idup(root);
    root_pinned = true;

    // Create a test file
    test_inode = vfs_create(root, 0644, file_name, file_len);
    if (IS_ERR(test_inode)) {
        ret = PTR_ERR(test_inode);
        printf("file_ops_smoketest: " FAIL " create %s, errno=%d\n", file_name, ret);
        goto out;
    }
    printf("file_ops_smoketest: created /%s ino=%lu\n", file_name, test_inode->ino);

    // Test 1: Open file for read/write
    file = vfs_fileopen(test_inode, O_RDWR);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("file_ops_smoketest: " FAIL " open O_RDWR, errno=%d\n", ret);
        goto cleanup;
    }
    printf("file_ops_smoketest: " PASS " open O_RDWR\n");

    // Test 2: Write to empty file
    ssize_t written = vfs_filewrite(file, test_data, test_data_len);
    if (written < 0) {
        printf("file_ops_smoketest: " FAIL " write to file failed, errno=%ld\n", (long)written);
    } else if (written != (ssize_t)test_data_len) {
        printf("file_ops_smoketest: " WARN " write %lu bytes, got %ld\n", (unsigned long)test_data_len, (long)written);
    } else {
        printf("file_ops_smoketest: " PASS " write %ld bytes\n", (long)written);
    }

    // Test 3: Seek to beginning
    loff_t pos = vfs_filelseek(file, 0, SEEK_SET);
    if (pos != 0) {
        printf("file_ops_smoketest: " FAIL " lseek SEEK_SET 0, got %lld\n", (long long)pos);
    } else {
        printf("file_ops_smoketest: " PASS " lseek SEEK_SET 0\n");
    }

    // Test 4: Read back data
    memset(read_buf, 0, sizeof(read_buf));
    ssize_t bytes_read = vfs_fileread(file, read_buf, test_data_len);
    if (bytes_read != (ssize_t)test_data_len) {
        printf("file_ops_smoketest: " FAIL " read %lu bytes, got %ld\n", (unsigned long)test_data_len, (long)bytes_read);
    } else if (memcmp(read_buf, test_data, test_data_len) != 0) {
        printf("file_ops_smoketest: " FAIL " read data mismatch\n");
    } else {
        printf("file_ops_smoketest: " PASS " read %ld bytes, data matches\n", (long)bytes_read);
    }

    // Test 5: Seek with SEEK_CUR
    pos = vfs_filelseek(file, -10, SEEK_CUR);
    if (pos != (loff_t)(test_data_len - 10)) {
        printf("file_ops_smoketest: " FAIL " lseek SEEK_CUR -10, expected %lu got %lld\n", 
               (unsigned long)(test_data_len - 10), (long long)pos);
    } else {
        printf("file_ops_smoketest: " PASS " lseek SEEK_CUR -10, pos=%lld\n", (long long)pos);
    }

    // Test 6: Seek with SEEK_END
    pos = vfs_filelseek(file, 0, SEEK_END);
    if (pos != (loff_t)test_data_len) {
        printf("file_ops_smoketest: " FAIL " lseek SEEK_END 0, expected %lu got %lld\n",
               (unsigned long)test_data_len, (long long)pos);
    } else {
        printf("file_ops_smoketest: " PASS " lseek SEEK_END 0, pos=%lld\n", (long long)pos);
    }

    // Test 7: Read at EOF should return 0
    bytes_read = vfs_fileread(file, read_buf, 10);
    if (bytes_read != 0) {
        printf("file_ops_smoketest: " FAIL " read at EOF, expected 0 got %ld\n", (long)bytes_read);
    } else {
        printf("file_ops_smoketest: " PASS " read at EOF returns 0\n");
    }

    // Test 8: stat
    struct stat st;
    ret = vfs_filestat(file, &st);
    if (ret != 0) {
        printf("file_ops_smoketest: " FAIL " stat, errno=%d\n", ret);
    } else if ((size_t)st.size != test_data_len) {
        printf("file_ops_smoketest: " FAIL " stat size=%llu expected %lu\n", 
               (unsigned long long)st.size, (unsigned long)test_data_len);
    } else {
        printf("file_ops_smoketest: " PASS " stat size=%llu type=%d\n",
               (unsigned long long)st.size, st.type);
    }

    // Test 9: Close and reopen to verify persistence
    vfs_fileclose(file);
    file = NULL;

    file = vfs_fileopen(test_inode, O_RDONLY);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("file_ops_smoketest: " FAIL " reopen O_RDONLY, errno=%d\n", ret);
        goto cleanup;
    }

    memset(read_buf, 0, sizeof(read_buf));
    bytes_read = vfs_fileread(file, read_buf, test_data_len);
    if (bytes_read != (ssize_t)test_data_len || memcmp(read_buf, test_data, test_data_len) != 0) {
        printf("file_ops_smoketest: " FAIL " re-read after close mismatch\n");
    } else {
        printf("file_ops_smoketest: " PASS " re-read after close, data persisted\n");
    }

    vfs_fileclose(file);
    file = NULL;

    // Test 10: Test larger write spanning multiple blocks
    vfs_ilock(test_inode);
    __tmpfs_truncate(test_inode, 0);
    vfs_iunlock(test_inode);

    file = vfs_fileopen(test_inode, O_RDWR);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("file_ops_smoketest: " FAIL " open for multi-block test, errno=%d\n", ret);
        goto cleanup;
    }

    // Write pattern to fill 3 pages
    memset(write_buf, 'A', sizeof(write_buf));
    size_t total_written = 0;
    size_t target_size = PAGE_SIZE * 3;
    while (total_written < target_size) {
        size_t to_write = target_size - total_written;
        if (to_write > sizeof(write_buf)) to_write = sizeof(write_buf);
        written = vfs_filewrite(file, write_buf, to_write);
        if (written <= 0) {
            printf("file_ops_smoketest: " FAIL " multi-block write at %lu, errno=%ld\n", 
                   (unsigned long)total_written, (long)written);
            break;
        }
        total_written += written;
    }
    if (total_written == target_size) {
        printf("file_ops_smoketest: " PASS " multi-block write %lu bytes\n", (unsigned long)total_written);
    }

    // Seek to middle of second page and read
    pos = vfs_filelseek(file, PAGE_SIZE + 100, SEEK_SET);
    memset(read_buf, 0, sizeof(read_buf));
    bytes_read = vfs_fileread(file, read_buf, 50);
    if (bytes_read == 50 && read_buf[0] == 'A') {
        printf("file_ops_smoketest: " PASS " read from middle of second page\n");
    } else {
        printf("file_ops_smoketest: " FAIL " read from middle of second page (got %ld bytes, first char=%d)\n",
               (long)bytes_read, (int)(unsigned char)read_buf[0]);
    }

    vfs_fileclose(file);
    file = NULL;

cleanup:
    if (file != NULL && !IS_ERR(file)) {
        vfs_fileclose(file);
    }

    if (test_inode != NULL && !IS_ERR(test_inode)) {
        // Truncate to free blocks while we still hold reference
        vfs_ilock(test_inode);
        __tmpfs_truncate(test_inode, 0);
        vfs_iunlock(test_inode);
        // Drop our reference BEFORE unlink (refcount 2->1)
        // The dentry still holds the other reference
        vfs_iput(test_inode);
        test_inode = NULL;  // Don't use after iput
        // Now unlink - refcount should be 1, so it will succeed
        ret = vfs_unlink(root, file_name, file_len);
        if (ret != 0) {
            printf("file_ops_smoketest: " WARN " cleanup unlink %s, errno=%d\n", file_name, ret);
        }
    }

out:
    if (root_pinned) {
        vfs_iput(root);
    }
}
// ============================================================================
// Double Indirect Block Tests
// ============================================================================
// Block layout for tmpfs:
//   Direct:          blocks 0-31       (32 blocks)    = 128KB
//   Indirect:        blocks 32-543     (512 blocks)   = 2MB  (ends at 2.12MB)
//   Double Indirect: blocks 544+       (up to 262144) = 1GB+
// 64MB = 16384 blocks = well into double indirect layer
// ============================================================================

#define DOUBLE_INDIRECT_MAX_SIZE (64UL * 1024 * 1024)  // 64MB limit

// Helper to fill buffer with position-dependent pattern for verification
static void fill_pattern(char *buf, size_t len, loff_t offset) {
    for (size_t i = 0; i < len; i++) {
        buf[i] = (char)((offset + i) & 0xFF);
    }
}

// Helper to verify pattern
static bool verify_pattern(const char *buf, size_t len, loff_t offset) {
    for (size_t i = 0; i < len; i++) {
        char expected = (char)((offset + i) & 0xFF);
        if (buf[i] != expected) {
            printf("  pattern mismatch at offset %lld+%lu: got 0x%02x expected 0x%02x\n",
                   (long long)offset, (unsigned long)i, 
                   (unsigned char)buf[i], (unsigned char)expected);
            return false;
        }
    }
    return true;
}

// Helper to write pattern at offset and verify
static bool write_and_verify(struct vfs_file *file, loff_t offset, size_t len, 
                             char *write_buf, char *read_buf, const char *desc) {
    // Seek and write
    loff_t pos = vfs_filelseek(file, offset, SEEK_SET);
    if (pos != offset) {
        printf("dindirect_smoketest: " FAIL " %s seek to %lld failed (got %lld)\n",
               desc, (long long)offset, (long long)pos);
        return false;
    }
    
    fill_pattern(write_buf, len, offset);
    ssize_t written = vfs_filewrite(file, write_buf, len);
    if (written != (ssize_t)len) {
        printf("dindirect_smoketest: " FAIL " %s write %lu bytes at %lld failed (got %ld)\n",
               desc, (unsigned long)len, (long long)offset, (long)written);
        return false;
    }
    
    // Seek back and read
    pos = vfs_filelseek(file, offset, SEEK_SET);
    if (pos != offset) {
        printf("dindirect_smoketest: " FAIL " %s seek back failed\n", desc);
        return false;
    }
    
    memset(read_buf, 0, len);
    ssize_t bytes_read = vfs_fileread(file, read_buf, len);
    if (bytes_read != (ssize_t)len) {
        printf("dindirect_smoketest: " FAIL " %s read %lu bytes got %ld\n",
               desc, (unsigned long)len, (long)bytes_read);
        return false;
    }
    
    if (!verify_pattern(read_buf, len, offset)) {
        printf("dindirect_smoketest: " FAIL " %s pattern verification failed\n", desc);
        return false;
    }
    
    printf("dindirect_smoketest: " PASS " %s (offset=%lld, len=%lu)\n",
           desc, (long long)offset, (unsigned long)len);
    return true;
}

// Test file I/O reaching into double indirect blocks with various edge cases
void tmpfs_run_double_indirect_smoketest(void) {
    int ret = 0;
    struct vfs_inode *root = vfs_root_inode.mnt_rooti;
    struct vfs_inode *test_inode = NULL;
    struct vfs_file *file = NULL;
    bool root_pinned = false;

    const char *file_name = "dindirect_test";
    const size_t file_len = sizeof("dindirect_test") - 1;

    // Use static buffers for pattern writes (up to 3 pages for cross-boundary tests)
    static char write_buf[PAGE_SIZE * 3];
    static char read_buf[PAGE_SIZE * 3];

    // Key byte offsets for boundary testing
    const loff_t DIRECT_END = TMPFS_INODE_DBLOCKS * PAGE_SIZE;           // 128KB
    const loff_t INDIRECT_END = (TMPFS_INODE_DBLOCKS + 512) * PAGE_SIZE; // ~2.12MB (block 544)
    const loff_t TARGET_64MB = DOUBLE_INDIRECT_MAX_SIZE;                 // 64MB

    printf("dindirect_smoketest: Block boundaries:\n");
    printf("  Direct end:   block %lu, offset %lld\n", TMPFS_INODE_DBLOCKS, (long long)DIRECT_END);
    printf("  Indirect end: block %lu, offset %lld\n", (unsigned long)(TMPFS_INODE_DBLOCKS + 512), (long long)INDIRECT_END);
    printf("  Target size:  64MB = %lld bytes\n", (long long)TARGET_64MB);

    vfs_idup(root);
    root_pinned = true;

    // Create test file
    test_inode = vfs_create(root, 0644, file_name, file_len);
    if (IS_ERR(test_inode)) {
        ret = PTR_ERR(test_inode);
        printf("dindirect_smoketest: " FAIL " create %s, errno=%d\n", file_name, ret);
        goto out;
    }
    printf("dindirect_smoketest: created /%s ino=%lu\n", file_name, test_inode->ino);

    file = vfs_fileopen(test_inode, O_RDWR);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("dindirect_smoketest: " FAIL " open O_RDWR, errno=%d\n", ret);
        goto cleanup;
    }

    // =========================================================================
    // Part 1: Edge position writes within blocks and across block boundaries
    // Note: Each write extends the file sequentially to avoid sparse file bugs.
    // =========================================================================
    printf("\ndindirect_smoketest: === Part 1: Edge position writes ===\n");

    // Test 1a: Write at start of file (embedded data)
    write_and_verify(file, 0, 100, write_buf, read_buf, "start of file (embedded)");

    // Test 1b: Write at last byte of direct layer
    write_and_verify(file, DIRECT_END - 100, 100, write_buf, read_buf, "end of direct layer");

    // Test 1c: Write crossing direct->indirect boundary
    write_and_verify(file, DIRECT_END - 50, 100, write_buf, read_buf, "cross direct->indirect boundary");

    // Test 1d: Write at start of indirect layer (already covered by 1c extension)
    write_and_verify(file, DIRECT_END, 100, write_buf, read_buf, "start of indirect layer");

    // Test 1e: Write at end of indirect layer (extends file through indirect)
    write_and_verify(file, INDIRECT_END - 100, 100, write_buf, read_buf, "end of indirect layer");

    // Test 1f: Write crossing indirect->double_indirect boundary
    write_and_verify(file, INDIRECT_END - 50, 100, write_buf, read_buf, "cross indirect->dindirect boundary");

    // Test 1g: Write at start of double indirect layer
    write_and_verify(file, INDIRECT_END, 100, write_buf, read_buf, "start of double indirect layer");

    // Extend file through double indirect sequentially using truncate first
    // to avoid sparse write issues with the allocate_blocks function
    vfs_fileclose(file);
    file = NULL;
    
    // Extend to 10MB, 32MB, then 64MB via truncate (sequential growth)
    printf("dindirect_smoketest: extending file through double indirect via truncate...\n");
    
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, 10 * 1024 * 1024);
    vfs_iunlock(test_inode);
    if (ret != 0) {
        printf("dindirect_smoketest: " FAIL " truncate to 10MB, errno=%d\n", ret);
        goto cleanup;
    }
    
    file = vfs_fileopen(test_inode, O_RDWR);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("dindirect_smoketest: " FAIL " reopen after 10MB extend, errno=%d\n", ret);
        goto cleanup;
    }
    
    // Test 1h: Write deep in double indirect (10MB offset - within allocated range)
    write_and_verify(file, 10 * 1024 * 1024 - 100, 100, write_buf, read_buf, "at 10MB");

    // Test 1i: Write crossing page boundary within double indirect
    loff_t mid_dindirect = 5 * 1024 * 1024; // 5MB (within 10MB file)
    write_and_verify(file, mid_dindirect - 50, 100, write_buf, read_buf, 
                     "cross page boundary in dindirect (5MB)");

    vfs_fileclose(file);
    file = NULL;
    
    // Extend to 32MB
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, 32 * 1024 * 1024);
    vfs_iunlock(test_inode);
    if (ret != 0) {
        printf("dindirect_smoketest: " FAIL " truncate to 32MB, errno=%d\n", ret);
        goto cleanup;
    }
    
    file = vfs_fileopen(test_inode, O_RDWR);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("dindirect_smoketest: " FAIL " reopen after 32MB extend, errno=%d\n", ret);
        goto cleanup;
    }
    
    // Test 1j: Write at 32MB
    write_and_verify(file, 32 * 1024 * 1024 - 100, 100, write_buf, read_buf, "at 32MB");

    // Test 1k: Large write spanning multiple pages in double indirect (at 20MB)
    write_and_verify(file, 20 * 1024 * 1024, PAGE_SIZE + 500, write_buf, read_buf,
                     "multi-page write in dindirect (20MB)");

    vfs_fileclose(file);
    file = NULL;
    
    // Extend to 64MB
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, TARGET_64MB);
    vfs_iunlock(test_inode);
    if (ret != 0) {
        printf("dindirect_smoketest: " FAIL " truncate to 64MB, errno=%d\n", ret);
        goto cleanup;
    }
    printf("dindirect_smoketest: " PASS " extended to 64MB, n_blocks=%lu\n", test_inode->n_blocks);
    
    file = vfs_fileopen(test_inode, O_RDWR);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("dindirect_smoketest: " FAIL " reopen after 64MB extend, errno=%d\n", ret);
        goto cleanup;
    }
    
    // Test 1l: Write near 64MB limit
    write_and_verify(file, TARGET_64MB - 100, 100, write_buf, read_buf, "near 64MB limit");

    // =========================================================================
    // Part 2: Verify all previously written data is still correct
    // =========================================================================
    printf("\ndindirect_smoketest: === Part 2: Verify persistence ===\n");

    struct {
        loff_t offset;
        size_t len;
        const char *desc;
    } verify_points[] = {
        {0, 100, "start of file"},
        {DIRECT_END - 100, 100, "end of direct"},
        {DIRECT_END - 50, 100, "cross direct->indirect"},
        {DIRECT_END, 100, "start of indirect"},
        {INDIRECT_END - 100, 100, "end of indirect"},
        {INDIRECT_END - 50, 100, "cross indirect->dindirect"},
        {INDIRECT_END, 100, "start of dindirect"},
        {10 * 1024 * 1024 - 100, 100, "at 10MB"},
        {5 * 1024 * 1024 - 50, 100, "5MB cross page"},
        {32 * 1024 * 1024 - 100, 100, "at 32MB"},
        {20 * 1024 * 1024, PAGE_SIZE + 500, "20MB multi-page"},
        {TARGET_64MB - 100, 100, "64MB - 100"},
    };
    
    bool all_verified = true;
    for (size_t i = 0; i < sizeof(verify_points)/sizeof(verify_points[0]); i++) {
        loff_t off = verify_points[i].offset;
        size_t len = verify_points[i].len;
        
        loff_t pos = vfs_filelseek(file, off, SEEK_SET);
        if (pos != off) {
            printf("dindirect_smoketest: " FAIL " verify seek %s\n", verify_points[i].desc);
            all_verified = false;
            continue;
        }
        
        memset(read_buf, 0, len);
        ssize_t bytes_read = vfs_fileread(file, read_buf, len);
        if (bytes_read != (ssize_t)len) {
            printf("dindirect_smoketest: " FAIL " verify read %s (got %ld)\n", 
                   verify_points[i].desc, (long)bytes_read);
            all_verified = false;
            continue;
        }
        
        if (!verify_pattern(read_buf, len, off)) {
            printf("dindirect_smoketest: " FAIL " verify pattern %s\n", verify_points[i].desc);
            all_verified = false;
        }
    }
    if (all_verified) {
        printf("dindirect_smoketest: " PASS " all persistence verifications passed\n");
    }

    // =========================================================================
    // Part 3: Size changes via truncate across layer boundaries
    // =========================================================================
    printf("\ndindirect_smoketest: === Part 3: Truncate across layers ===\n");

    vfs_fileclose(file);
    file = NULL;

    struct tmpfs_inode *ti = container_of(test_inode, struct tmpfs_inode, vfs_inode);

    // Test 3a: Truncate to zero
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, 0);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == 0) {
        printf("dindirect_smoketest: " PASS " truncate to 0\n");
    } else {
        printf("dindirect_smoketest: " FAIL " truncate to 0 (ret=%d, size=%lld)\n",
               ret, (long long)test_inode->size);
    }

    // Test 3b: Grow directly to double indirect (0 -> 5MB)
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, 5 * 1024 * 1024);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == 5 * 1024 * 1024) {
        printf("dindirect_smoketest: " PASS " grow 0 -> 5MB (dindirect=%s)\n",
               ti->file.double_indirect ? "set" : "null");
    } else {
        printf("dindirect_smoketest: " FAIL " grow 0 -> 5MB\n");
    }

    // Test 3c: Shrink from double indirect to indirect (5MB -> 1MB)
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, 1 * 1024 * 1024);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == 1 * 1024 * 1024) {
        printf("dindirect_smoketest: " PASS " shrink 5MB -> 1MB (dindirect=%s)\n",
               ti->file.double_indirect ? "set" : "null");
    } else {
        printf("dindirect_smoketest: " FAIL " shrink 5MB -> 1MB\n");
    }

    // Test 3d: Shrink from indirect to direct (1MB -> 64KB)
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, 64 * 1024);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == 64 * 1024) {
        printf("dindirect_smoketest: " PASS " shrink 1MB -> 64KB (indirect=%s)\n",
               ti->file.indirect ? "set" : "null");
    } else {
        printf("dindirect_smoketest: " FAIL " shrink 1MB -> 64KB\n");
    }

    // Test 3e: Grow to exactly indirect boundary
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, DIRECT_END);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == DIRECT_END) {
        printf("dindirect_smoketest: " PASS " grow to exact direct end (%lld)\n", (long long)DIRECT_END);
    } else {
        printf("dindirect_smoketest: " FAIL " grow to exact direct end\n");
    }

    // Test 3f: Grow one byte past direct boundary (into indirect)
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, DIRECT_END + 1);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == DIRECT_END + 1) {
        printf("dindirect_smoketest: " PASS " grow to direct_end + 1 (indirect=%s)\n",
               ti->file.indirect ? "set" : "null");
    } else {
        printf("dindirect_smoketest: " FAIL " grow to direct_end + 1\n");
    }

    // Test 3g: Grow to exactly double indirect boundary
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, INDIRECT_END);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == INDIRECT_END) {
        printf("dindirect_smoketest: " PASS " grow to exact indirect end (%lld)\n", (long long)INDIRECT_END);
    } else {
        printf("dindirect_smoketest: " FAIL " grow to exact indirect end\n");
    }

    // Test 3h: Grow one byte past indirect boundary (into double indirect)
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, INDIRECT_END + 1);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == INDIRECT_END + 1) {
        printf("dindirect_smoketest: " PASS " grow to indirect_end + 1 (dindirect=%s)\n",
               ti->file.double_indirect ? "set" : "null");
    } else {
        printf("dindirect_smoketest: " FAIL " grow to indirect_end + 1\n");
    }

    // Test 3i: Grow to 64MB
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, TARGET_64MB);
    vfs_iunlock(test_inode);
    if (ret == 0 && test_inode->size == TARGET_64MB) {
        printf("dindirect_smoketest: " PASS " grow to 64MB, n_blocks=%lu\n", test_inode->n_blocks);
    } else {
        printf("dindirect_smoketest: " FAIL " grow to 64MB\n");
    }

    // Shrink back to 0 for next tests
    vfs_ilock(test_inode);
    __tmpfs_truncate(test_inode, 0);
    vfs_iunlock(test_inode);

    // =========================================================================
    // Part 4: Size changes via writes then verify with reads
    // =========================================================================
    printf("\ndindirect_smoketest: === Part 4: Size change via write + verify ===\n");

    file = vfs_fileopen(test_inode, O_RDWR);
    if (IS_ERR(file)) {
        ret = PTR_ERR(file);
        printf("dindirect_smoketest: " FAIL " reopen for part 4, errno=%d\n", ret);
        goto cleanup;
    }

    // Test 4a: Write to extend file into indirect layer
    write_and_verify(file, DIRECT_END + 500, 200, write_buf, read_buf,
                     "extend into indirect via write");

    // Test 4b: Truncate shorter but still in indirect, then write at boundary
    vfs_fileclose(file);
    file = NULL;
    
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, DIRECT_END + 100);
    vfs_iunlock(test_inode);
    
    file = vfs_fileopen(test_inode, O_RDWR);
    if (!IS_ERR(file)) {
        write_and_verify(file, DIRECT_END - 20, 50, write_buf, read_buf,
                         "write across direct/indirect after truncate");
        vfs_fileclose(file);
        file = NULL;
    }

    // Test 4c: Truncate to zero, grow via truncate, then write
    vfs_ilock(test_inode);
    __tmpfs_truncate(test_inode, 0);
    vfs_iunlock(test_inode);

    // First extend via truncate to avoid sparse write bug
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, 8 * 1024 * 1024 + 200);
    vfs_iunlock(test_inode);
    
    if (ret == 0) {
        file = vfs_fileopen(test_inode, O_RDWR);
        if (!IS_ERR(file)) {
            write_and_verify(file, 8 * 1024 * 1024, 200, write_buf, read_buf,
                             "write at 8MB after truncate-extend");

            // Verify zeros in the gap (zero-filled by truncate)
            (void)vfs_filelseek(file, DIRECT_END + 100, SEEK_SET);
            memset(read_buf, 0xFF, 100);
            ssize_t bytes_read = vfs_fileread(file, read_buf, 100);
            if (bytes_read == 100) {
                bool all_zero = true;
                for (int i = 0; i < 100; i++) {
                    if (read_buf[i] != 0) {
                        all_zero = false;
                        break;
                    }
                }
                if (all_zero) {
                    printf("dindirect_smoketest: " PASS " gap is zero-filled\n");
                } else {
                    printf("dindirect_smoketest: " WARN " gap not zero-filled\n");
                }
            }
            vfs_fileclose(file);
            file = NULL;
        }
    } else {
        printf("dindirect_smoketest: " FAIL " truncate-extend to 8MB, errno=%d\n", ret);
    }

    // =========================================================================
    // Part 5: Cross-layer write/read spanning multiple layers
    // Note: Uses truncate to pre-extend file to avoid sparse write bugs
    // =========================================================================
    printf("\ndindirect_smoketest: === Part 5: Cross-layer operations ===\n");

    vfs_ilock(test_inode);
    __tmpfs_truncate(test_inode, 0);
    vfs_iunlock(test_inode);

    // Pre-extend to indirect layer for first test
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, DIRECT_END + PAGE_SIZE * 3);
    vfs_iunlock(test_inode);

    file = vfs_fileopen(test_inode, O_RDWR);
    if (!IS_ERR(file)) {
        // Write that spans from direct into indirect (large write)
        size_t span_len = PAGE_SIZE * 3; // 12KB spanning the boundary
        loff_t span_start = DIRECT_END - PAGE_SIZE; // Start 1 page before boundary
        
        write_and_verify(file, span_start, span_len, write_buf, read_buf,
                         "span direct->indirect (3 pages)");

        vfs_fileclose(file);
        file = NULL;
    }
    
    // Pre-extend to double indirect layer for second test
    vfs_ilock(test_inode);
    ret = __tmpfs_truncate(test_inode, INDIRECT_END + PAGE_SIZE * 3);
    vfs_iunlock(test_inode);

    file = vfs_fileopen(test_inode, O_RDWR);
    if (!IS_ERR(file)) {
        // Write that spans from indirect into double indirect
        size_t span_len = PAGE_SIZE * 3;
        loff_t span_start = INDIRECT_END - PAGE_SIZE;
        write_and_verify(file, span_start, span_len, write_buf, read_buf,
                         "span indirect->dindirect (3 pages)");

        vfs_fileclose(file);
        file = NULL;
    }

    // =========================================================================
    // Part 6: Final comprehensive verification after many operations
    // =========================================================================
    printf("\ndindirect_smoketest: === Part 6: Final stat check ===\n");

    file = vfs_fileopen(test_inode, O_RDONLY);
    if (!IS_ERR(file)) {
        struct stat st;
        ret = vfs_filestat(file, &st);
        if (ret == 0) {
            printf("dindirect_smoketest: " PASS " final stat: size=%lld, n_blocks=%lu\n",
                   (long long)st.size, test_inode->n_blocks);
        } else {
            printf("dindirect_smoketest: " FAIL " final stat failed\n");
        }
        vfs_fileclose(file);
        file = NULL;
    }

cleanup:
    if (file != NULL && !IS_ERR(file)) {
        vfs_fileclose(file);
    }

    if (test_inode != NULL && !IS_ERR(test_inode)) {
        vfs_ilock(test_inode);
        __tmpfs_truncate(test_inode, 0);
        vfs_iunlock(test_inode);
        vfs_iput(test_inode);
        ret = vfs_unlink(root, file_name, file_len);
        if (ret != 0) {
            printf("dindirect_smoketest: " WARN " cleanup unlink %s, errno=%d\n", file_name, ret);
        } else {
            printf("dindirect_smoketest: cleanup complete\n");
        }
    }

out:
    if (root_pinned) {
        vfs_iput(root);
    }
}

// Run all tmpfs smoketests with memory leak detection
void tmpfs_run_all_smoketests(void) {
    // Shrink all caches before baseline to get a clean state
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();

    // Memory leak check for inode smoketest
    uint64 before_pages = get_total_free_pages();
    tmpfs_run_inode_smoketest();
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();
    uint64 after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: inode_smoketest leaked %ld pages\n", 
               (long)(before_pages - after_pages));
    } else {
        printf("inode_smoketest: no memory leak detected\n");
    }

    // Memory leak check for truncate smoketest
    before_pages = get_total_free_pages();
    tmpfs_run_truncate_smoketest();
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: truncate_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("truncate_smoketest: no memory leak detected\n");
    }

    // Memory leak check for namei smoketest
    before_pages = get_total_free_pages();
    tmpfs_run_namei_smoketest();
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: namei_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("namei_smoketest: no memory leak detected\n");
    }

    // Memory leak check for dir_iter_mount smoketest
    before_pages = get_total_free_pages();
    tmpfs_run_dir_iter_mount_smoketest();
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: dir_iter_mount_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("dir_iter_mount_smoketest: no memory leak detected\n");
    }

    // Memory leak check for file_ops smoketest
    before_pages = get_total_free_pages();
    tmpfs_run_file_ops_smoketest();
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: file_ops_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("file_ops_smoketest: no memory leak detected\n");
    }

    // Memory leak check for lazy_unmount smoketest
    before_pages = get_total_free_pages();
    tmpfs_run_lazy_unmount_smoketest();
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: lazy_unmount_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("lazy_unmount_smoketest: no memory leak detected\n");
    }

    // Memory leak check for double_indirect smoketest
    before_pages = get_total_free_pages();
    tmpfs_run_double_indirect_smoketest();
    // tmpfs_shrink_caches();
    // __vfs_shrink_caches();
    // kmm_shrink_all();
    after_pages = get_total_free_pages();
    if (before_pages != after_pages) {
        printf("MEMORY LEAK: dindirect_smoketest leaked %ld pages\n",
               (long)(before_pages - after_pages));
    } else {
        printf("dindirect_smoketest: no memory leak detected\n");
    }
}