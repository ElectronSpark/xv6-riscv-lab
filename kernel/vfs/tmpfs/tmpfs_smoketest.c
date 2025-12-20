#include "types.h"
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
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "tmpfs_private.h"
#include "tmpfs_smoketest.h"

// Helper: lookup a child inode by name and bump refcount; caller must vfs_iput()
static int tmpfs_fetch_inode(struct vfs_inode *dir, const char *name, size_t name_len,
                             bool user, struct vfs_inode **out) {
    struct vfs_dentry d = {0};
    int ret = vfs_ilookup(dir, &d, name, name_len, user);
    if (ret != 0) {
        return ret;
    }
    ret = vfs_get_dentry_inode(&d, out);
    if (ret == 0) {
        vfs_idup(*out);
        vfs_iunlock(*out);
    }
    vfs_release_dentry(&d);
    return ret;
}

// Run a small, inode-only tmpfs smoke test: create nested directories, files, symlinks,
// perform lookups, moves, and readlinks. This is debug-only scaffolding invoked at init.
void tmpfs_run_inode_smoketest(void) {
    const char *subdir_name = "tmpfs_subdir";
    const size_t subdir_len = sizeof("tmpfs_subdir") - 1;
    const char *nested_name = "nested";
    const size_t nested_len = sizeof("nested") - 1;
    const char *file_a_name = "tmp_file_a";
    const size_t file_a_len = sizeof("tmp_file_a") - 1;
    const char *file_b_name = "tmp_file_b";
    const size_t file_b_len = sizeof("tmp_file_b") - 1;
    const char *file_a_link_name = "tmp_file_a_hl";
    const size_t file_a_link_len = sizeof("tmp_file_a_hl") - 1;
    const char *file_b_new_name = "tmp_file_b_moved";
    const size_t file_b_new_len = sizeof("tmp_file_b_moved") - 1;
    const char *symlink_a_name = "link_to_nested";
    const size_t symlink_a_len = sizeof("link_to_nested") - 1;
    const char *symlink_b_name = "nested_link_to_root";
    const size_t symlink_b_len = sizeof("nested_link_to_root") - 1;
    const char *symlink_a_target = "nested";
    const size_t symlink_a_target_len = sizeof("nested") - 1;
    const char *symlink_b_target = "..";
    const size_t symlink_b_target_len = sizeof("..") - 1;

    int ret = 0;
    struct vfs_superblock *sb = vfs_root_inode.mnt_sb;
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


    vfs_idup(root); // Pin root inode
    root_pinned = true;

    ret = vfs_mkdir(root, 0755, &subdir, subdir_name, subdir_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL vfs_mkdir %s, errno=%d\n", subdir_name, ret);
        goto out_put_root;
    }
    vfs_ilock(subdir);
    printf("inode_smoketest: PASS created /%s nlink=%u\n", subdir_name, subdir->n_links);
    vfs_iunlock(subdir);
    vfs_iput(subdir);

    ret = vfs_mkdir(subdir, 0755, &nested, nested_name, nested_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL vfs_mkdir %s, errno=%d\n", nested_name, ret);
        goto out_put_subdir;
    }
    vfs_ilock(nested);
    printf("inode_smoketest: PASS created /%s/%s nlink=%u\n", subdir_name, nested_name, nested->n_links);
    vfs_iunlock(nested);
    vfs_iput(nested);

    ret = vfs_create(subdir, 0644, &file_a, file_a_name, file_a_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL vfs_create %s, errno=%d\n", file_a_name, ret);
    } else {
        file_a_ino = file_a->ino;
        vfs_ilock(file_a);
        printf("inode_smoketest: PASS created /%s/%s ino=%lu nlink=%u\n",
               subdir_name, file_a_name, file_a->ino, file_a->n_links);
        vfs_iunlock(file_a);
        vfs_iput(file_a);
        file_a = NULL;

        // Create a hard link to file_a inside the same directory
        struct vfs_dentry link_old = {
            .sb = sb,
            .ino = file_a_ino,
            .name = NULL,
            .name_len = 0,
            .cookies = 0,
        };
        ret = vfs_link(&link_old, subdir, file_a_link_name, file_a_link_len, false);
        if (ret != 0) {
            printf("inode_smoketest: FAIL vfs_link %s, errno=%d\n", file_a_link_name, ret);
        } else {
            struct vfs_inode *tmp = NULL;
            ret = tmpfs_fetch_inode(subdir, file_a_name, file_a_len, false, &tmp);
            if (ret == 0) {
                vfs_ilock(tmp);
                printf("inode_smoketest: PASS linked /%s/%s -> /%s/%s nlink=%u\n",
                       subdir_name, file_a_link_name, subdir_name, file_a_name, tmp->n_links);
                vfs_iunlock(tmp);
                vfs_iput(tmp);
            }
        }

        // Unlink the hard link
        ret = vfs_unlink(subdir, file_a_link_name, file_a_link_len, false);
        if (ret != 0) {
            printf("inode_smoketest: FAIL vfs_unlink %s, errno=%d\n", file_a_link_name, ret);
        } else {
            struct vfs_inode *tmp = NULL;
            ret = tmpfs_fetch_inode(subdir, file_a_name, file_a_len, false, &tmp);
            if (ret == 0) {
                vfs_ilock(tmp);
                printf("inode_smoketest: PASS unlinked /%s/%s nlink=%u\n",
                       subdir_name, file_a_link_name, tmp->n_links);
                vfs_iunlock(tmp);
                vfs_iput(tmp);
            }
        }
    }

    ret = vfs_create(nested, 0644, &file_b, file_b_name, file_b_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL vfs_create %s, errno=%d\n", file_b_name, ret);
    } else {
        file_b_ino = file_b->ino;
        vfs_ilock(file_b);
        printf("inode_smoketest: PASS created /%s/%s/%s ino=%lu nlink=%u\n",
               subdir_name, nested_name, file_b_name, file_b->ino, file_b->n_links);
        vfs_iunlock(file_b);
        vfs_iput(file_b);
        file_b = NULL;
    }

    ret = vfs_symlink(subdir, &sym_a, 0777, symlink_a_name, symlink_a_len,
                      symlink_a_target, symlink_a_target_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL vfs_symlink %s, errno=%d\n", symlink_a_name, ret);
    } else {
        vfs_ilock(sym_a);
        printf("inode_smoketest: PASS symlink /%s/%s -> %s ino=%lu nlink=%u\n",
               subdir_name, symlink_a_name, symlink_a_target, sym_a->ino, sym_a->n_links);
        vfs_iunlock(sym_a);
        vfs_iput(sym_a);
        sym_a = NULL;
    }

    ret = vfs_symlink(nested, &sym_b, 0777, symlink_b_name, symlink_b_len,
                      symlink_b_target, symlink_b_target_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL vfs_symlink %s, errno=%d\n", symlink_b_name, ret);
    } else {
        vfs_ilock(sym_b);
        printf("inode_smoketest: PASS symlink /%s/%s/%s -> %s ino=%lu nlink=%u\n",
               subdir_name, nested_name, symlink_b_name, symlink_b_target, sym_b->ino, sym_b->n_links);
        vfs_iunlock(sym_b);
        vfs_iput(sym_b);
        sym_b = NULL;
    }

    // Lookup a couple of created entries via inode-only paths
    struct vfs_dentry d1 = {0};
    ret = vfs_ilookup(root, &d1, subdir_name, subdir_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL ilookup /%s, errno=%d\n", subdir_name, ret);
    } else {
        printf("inode_smoketest: PASS ilookup /%s -> ino=%lu\n", subdir_name, d1.ino);
        vfs_release_dentry(&d1);
    }

    struct vfs_dentry d2 = {0};
    ret = vfs_ilookup(subdir, &d2, nested_name, nested_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL ilookup /%s/%s, errno=%d\n", subdir_name, nested_name, ret);
    } else {
        printf("inode_smoketest: PASS ilookup /%s/%s -> ino=%lu\n", subdir_name, nested_name, d2.ino);
        vfs_release_dentry(&d2);
    }

    struct vfs_dentry d3 = {0};
    ret = vfs_ilookup(subdir, &d3, symlink_a_name, symlink_a_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL ilookup /%s/%s, errno=%d\n", subdir_name, symlink_a_name, ret);
    } else {
        printf("inode_smoketest: PASS ilookup /%s/%s -> ino=%lu\n", subdir_name, symlink_a_name, d3.ino);
        vfs_release_dentry(&d3);
    }

    struct vfs_dentry d_file_a = {0};
    ret = vfs_ilookup(subdir, &d_file_a, file_a_name, file_a_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL ilookup /%s/%s, errno=%d\n", subdir_name, file_a_name, ret);
    } else {
        struct vfs_inode *tmp = NULL;
        ret = tmpfs_fetch_inode(subdir, file_a_name, file_a_len, false, &tmp);
        if (ret == 0) {
            vfs_ilock(tmp);
            printf("inode_smoketest: PASS ilookup /%s/%s -> ino=%lu nlink=%u\n",
                   subdir_name, file_a_name, d_file_a.ino, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        }
        vfs_release_dentry(&d_file_a);
    }

    struct vfs_dentry d_file_b = {0};
    ret = vfs_ilookup(nested, &d_file_b, file_b_name, file_b_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL ilookup /%s/%s/%s, errno=%d\n", subdir_name, nested_name, file_b_name, ret);
    } else {
        struct vfs_inode *tmp = NULL;
        ret = tmpfs_fetch_inode(nested, file_b_name, file_b_len, false, &tmp);
        if (ret == 0) {
            vfs_ilock(tmp);
            printf("inode_smoketest: PASS ilookup /%s/%s/%s -> ino=%lu nlink=%u\n",
                   subdir_name, nested_name, file_b_name, d_file_b.ino, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        }
        vfs_release_dentry(&d_file_b);
    }

    // Move a regular file from nested dir up to subdir
    struct vfs_dentry old_dentry = {
        .sb = sb,
        .name = (char *)file_b_name,
        .name_len = file_b_len,
        .ino = file_b_ino,
        .cookies = 0,
    };
    ret = vfs_move(nested, &old_dentry, subdir, file_b_new_name, file_b_new_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL move %s -> %s, errno=%d\n", file_b_name, file_b_new_name, ret);
    } else {
        struct vfs_inode *tmp = NULL;
        ret = tmpfs_fetch_inode(subdir, file_b_new_name, file_b_new_len, false, &tmp);
        if (ret == 0) {
            vfs_ilock(tmp);
            printf("inode_smoketest: PASS moved /%s/%s/%s -> /%s/%s nlink=%u\n",
                   subdir_name, nested_name, file_b_name, subdir_name, file_b_new_name, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        }
        struct vfs_dentry moved_lookup = {0};
        ret = vfs_ilookup(subdir, &moved_lookup, file_b_new_name, file_b_new_len, false);
        if (ret != 0) {
            printf("inode_smoketest: FAIL ilookup moved /%s/%s, errno=%d\n",
                   subdir_name, file_b_new_name, ret);
        } else {
            printf("inode_smoketest: PASS ilookup moved /%s/%s -> ino=%lu\n",
                   subdir_name, file_b_new_name, moved_lookup.ino);
            vfs_release_dentry(&moved_lookup);
        }
    }

    // Read back symlink targets through inode API
    char linkbuf[64] = {0};
    struct vfs_inode *tmp_sym = NULL;
    ret = tmpfs_fetch_inode(subdir, symlink_a_name, symlink_a_len, false, &tmp_sym);
    if (ret == 0) {
        ret = vfs_readlink(tmp_sym, linkbuf, sizeof(linkbuf), false);
        if (ret < 0) {
            printf("inode_smoketest: FAIL readlink %s, errno=%d\n", symlink_a_name, ret);
        } else {
            vfs_ilock(tmp_sym);
            printf("inode_smoketest: PASS readlink /%s/%s -> %s len=%d nlink=%u\n",
                   subdir_name, symlink_a_name, linkbuf, ret, tmp_sym->n_links);
            vfs_iunlock(tmp_sym);
        }
        vfs_iput(tmp_sym);
    }
    memset(linkbuf, 0, sizeof(linkbuf));
    tmp_sym = NULL;
    ret = tmpfs_fetch_inode(nested, symlink_b_name, symlink_b_len, false, &tmp_sym);
    if (ret == 0) {
        ret = vfs_readlink(tmp_sym, linkbuf, sizeof(linkbuf), false);
        if (ret < 0) {
            printf("inode_smoketest: FAIL readlink %s, errno=%d\n", symlink_b_name, ret);
        } else {
            vfs_ilock(tmp_sym);
            printf("inode_smoketest: PASS readlink /%s/%s/%s -> %s len=%d nlink=%u\n",
                   subdir_name, nested_name, symlink_b_name, linkbuf, ret, tmp_sym->n_links);
            vfs_iunlock(tmp_sym);
        }
        vfs_iput(tmp_sym);
    }

out_put_subdir:
    // Cleanup in reverse order: symlinks, files, dirs
    ret = vfs_unlink(nested, symlink_b_name, symlink_b_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL cleanup unlink %s, errno=%d\n", symlink_b_name, ret);
    }
    ret = vfs_unlink(subdir, symlink_a_name, symlink_a_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL cleanup unlink %s, errno=%d\n", symlink_a_name, ret);
    }
    // Remove moved file name if move succeeded, else original path
    ret = vfs_unlink(subdir, file_b_new_name, file_b_new_len, false);
    if (ret != 0) {
        ret = vfs_unlink(nested, file_b_name, file_b_len, false);
        if (ret != 0) {
            printf("inode_smoketest: FAIL cleanup unlink %s/%s, errno=%d\n", subdir_name, file_b_new_name, ret);
        }
    }
    ret = vfs_unlink(subdir, file_a_name, file_a_len, false);
    if (ret != 0) {
        printf("inode_smoketest: FAIL cleanup unlink %s, errno=%d\n", file_a_name, ret);
    }
    if (nested != NULL) {
        ret = vfs_rmdir(subdir, nested_name, nested_len, false);
        if (ret != 0) {
            printf("inode_smoketest: FAIL cleanup rmdir /%s/%s, errno=%d\n", subdir_name, nested_name, ret);
        }
    }
    if (subdir != NULL) {
        ret = vfs_rmdir(root, subdir_name, subdir_len, false);
        if (ret != 0) {
            printf("inode_smoketest: FAIL cleanup rmdir /%s, errno=%d\n", subdir_name, ret);
        }
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

    // Create test file
    ret = vfs_create(root, 0644, &test_file, file_name, file_len, false);
    if (ret != 0) {
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
        printf("truncate_smoketest: FAIL grow embedded, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS grow embedded 0->100 bytes, size=%llu\n", (unsigned long long)test_file->size);
    }

    // Test 2: Grow embedded to direct blocks (100 -> 5*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 5 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL grow to direct blocks, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS grow to direct 5 blocks, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 3: Shrink direct to embedded (5*PAGE_SIZE -> 50 bytes)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 50);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL shrink to embedded, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS shrink to embedded 50 bytes, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 4: Grow to full direct blocks (50 -> 32*PAGE_SIZE, block 0-31)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, TMPFS_INODE_DBLOCKS * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL grow to full direct, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS grow to full direct %lu blocks, size=%llu n_blocks=%lu\n",
               TMPFS_INODE_DBLOCKS, (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 5: Grow into indirect layer (32*PAGE_SIZE -> 100*PAGE_SIZE, uses indirect pointer)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 100 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL grow to indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS grow to indirect 100 blocks, size=%llu n_blocks=%lu indirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.indirect ? "set" : "null");
    }

    // Test 6: Shrink from indirect back to direct (100*PAGE_SIZE -> 20*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 20 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL shrink indirect to direct, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS shrink to direct 20 blocks, size=%llu n_blocks=%lu indirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.indirect ? "set" : "null");
    }

    // Test 7: Grow to double indirect layer (20*PAGE_SIZE -> 600*PAGE_SIZE)
    // Block 544 is start of double indirect (32 direct + 512 indirect)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 600 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL grow to double indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS grow to double indirect 600 blocks, size=%llu n_blocks=%lu dindirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.double_indirect ? "set" : "null");
    }

    // Test 8: Shrink from double indirect to indirect (600*PAGE_SIZE -> 40*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 40 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL shrink double to indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS shrink to indirect 40 blocks, size=%llu n_blocks=%lu dindirect=%s\n",
               (unsigned long long)test_file->size, test_file->n_blocks,
               ti->file.double_indirect ? "set" : "null");
    }

    // Test 9: Shrink to zero
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 0);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL shrink to zero, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS shrink to zero, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Test 10: Grow directly to double indirect (0 -> 1000*PAGE_SIZE)
    vfs_ilock(test_file);
    ret = __tmpfs_truncate(test_file, 1000 * PAGE_SIZE);
    vfs_iunlock(test_file);
    if (ret != 0) {
        printf("truncate_smoketest: FAIL grow zero to double indirect, errno=%d\n", ret);
    } else {
        printf("truncate_smoketest: PASS grow zero to double indirect 1000 blocks, size=%llu n_blocks=%lu\n",
               (unsigned long long)test_file->size, test_file->n_blocks);
    }

    // Cleanup: shrink to zero and unlink
    vfs_ilock(test_file);
    __tmpfs_truncate(test_file, 0);
    vfs_iunlock(test_file);

    vfs_iput(test_file);
    test_file = NULL;

    ret = vfs_unlink(root, file_name, file_len, false);
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
