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
        printf("tmpfs_run_inode_smoketest: vfs_mkdir %s failed, errno=%d\n", subdir_name, ret);
        goto out_put_root;
    }
    printf("tmpfs_run_inode_smoketest: created /%s directory\n", subdir_name);
    vfs_ilock(subdir);
    printf("tmpfs_run_inode_smoketest: /%s nlink=%u\n", subdir_name, subdir->n_links);
    vfs_iunlock(subdir);
    vfs_iput(subdir);

    ret = vfs_mkdir(subdir, 0755, &nested, nested_name, nested_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: vfs_mkdir %s failed, errno=%d\n", nested_name, ret);
        goto out_put_subdir;
    }
    printf("tmpfs_run_inode_smoketest: created /%s/%s directory\n", subdir_name, nested_name);
    vfs_ilock(nested);
    printf("tmpfs_run_inode_smoketest: /%s/%s nlink=%u\n", subdir_name, nested_name, nested->n_links);
    vfs_iunlock(nested);
    vfs_iput(nested);

    ret = vfs_create(subdir, 0644, &file_a, file_a_name, file_a_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: vfs_create %s failed, errno=%d\n", file_a_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: created file /%s/%s ino=%lu\n",
               subdir_name, file_a_name, file_a->ino);
        file_a_ino = file_a->ino;
        vfs_ilock(file_a);
        printf("tmpfs_run_inode_smoketest: /%s/%s nlink=%u\n", subdir_name, file_a_name, file_a->n_links);
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
            printf("tmpfs_run_inode_smoketest: vfs_link %s failed, errno=%d\n", file_a_link_name, ret);
        } else {
            struct vfs_inode *tmp = NULL;
            ret = tmpfs_fetch_inode(subdir, file_a_name, file_a_len, false, &tmp);
            if (ret == 0) {
                vfs_ilock(tmp);
                printf("tmpfs_run_inode_smoketest: linked /%s/%s -> /%s/%s nlink=%u\n",
                       subdir_name, file_a_link_name, subdir_name, file_a_name, tmp->n_links);
                vfs_iunlock(tmp);
                vfs_iput(tmp);
            }
        }

        // Unlink the hard link
        ret = vfs_unlink(subdir, file_a_link_name, file_a_link_len, false);
        if (ret != 0) {
            printf("tmpfs_run_inode_smoketest: vfs_unlink %s failed, errno=%d\n", file_a_link_name, ret);
        } else {
            struct vfs_inode *tmp = NULL;
            ret = tmpfs_fetch_inode(subdir, file_a_name, file_a_len, false, &tmp);
            if (ret == 0) {
                vfs_ilock(tmp);
                printf("tmpfs_run_inode_smoketest: unlinked /%s/%s nlink=%u\n",
                       subdir_name, file_a_link_name, tmp->n_links);
                vfs_iunlock(tmp);
                vfs_iput(tmp);
            }
        }
    }

    ret = vfs_create(nested, 0644, &file_b, file_b_name, file_b_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: vfs_create %s failed, errno=%d\n", file_b_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: created file /%s/%s/%s ino=%lu\n",
               subdir_name, nested_name, file_b_name, file_b->ino);
        file_b_ino = file_b->ino;
        vfs_ilock(file_b);
        printf("tmpfs_run_inode_smoketest: /%s/%s/%s nlink=%u\n", subdir_name, nested_name, file_b_name, file_b->n_links);
        vfs_iunlock(file_b);
        vfs_iput(file_b);
        file_b = NULL;
    }

    ret = vfs_symlink(subdir, &sym_a, 0777, symlink_a_name, symlink_a_len,
                      symlink_a_target, symlink_a_target_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: vfs_symlink %s failed, errno=%d\n", symlink_a_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: created symlink /%s/%s -> %s ino=%lu\n",
               subdir_name, symlink_a_name, symlink_a_target, sym_a->ino);
        vfs_ilock(sym_a);
        printf("tmpfs_run_inode_smoketest: /%s/%s nlink=%u\n", subdir_name, symlink_a_name, sym_a->n_links);
        vfs_iunlock(sym_a);
        vfs_iput(sym_a);
        sym_a = NULL;
    }

    ret = vfs_symlink(nested, &sym_b, 0777, symlink_b_name, symlink_b_len,
                      symlink_b_target, symlink_b_target_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: vfs_symlink %s failed, errno=%d\n", symlink_b_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: created symlink /%s/%s/%s -> %s ino=%lu\n",
               subdir_name, nested_name, symlink_b_name, symlink_b_target, sym_b->ino);
        vfs_ilock(sym_b);
        printf("tmpfs_run_inode_smoketest: /%s/%s/%s nlink=%u\n", subdir_name, nested_name, symlink_b_name, sym_b->n_links);
        vfs_iunlock(sym_b);
        vfs_iput(sym_b);
        sym_b = NULL;
    }

    // Lookup a couple of created entries via inode-only paths
    struct vfs_dentry d1 = {0};
    ret = vfs_ilookup(root, &d1, subdir_name, subdir_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: ilookup /%s failed, errno=%d\n", subdir_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: ilookup /%s -> ino=%lu\n", subdir_name, d1.ino);
        vfs_release_dentry(&d1);
    }

    struct vfs_dentry d2 = {0};
    ret = vfs_ilookup(subdir, &d2, nested_name, nested_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: ilookup /%s/%s failed, errno=%d\n", subdir_name, nested_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: ilookup /%s/%s -> ino=%lu\n", subdir_name, nested_name, d2.ino);
        vfs_release_dentry(&d2);
    }

    struct vfs_dentry d3 = {0};
    ret = vfs_ilookup(subdir, &d3, symlink_a_name, symlink_a_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: ilookup symlink /%s/%s failed, errno=%d\n", subdir_name, symlink_a_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: ilookup /%s/%s -> ino=%lu\n", subdir_name, symlink_a_name, d3.ino);
        vfs_release_dentry(&d3);
    }

    struct vfs_dentry d_file_a = {0};
    ret = vfs_ilookup(subdir, &d_file_a, file_a_name, file_a_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: ilookup file /%s/%s failed, errno=%d\n", subdir_name, file_a_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: ilookup /%s/%s -> ino=%lu\n", subdir_name, file_a_name, d_file_a.ino);
        struct vfs_inode *tmp = NULL;
        ret = tmpfs_fetch_inode(subdir, file_a_name, file_a_len, false, &tmp);
        if (ret == 0) {
            vfs_ilock(tmp);
            printf("tmpfs_run_inode_smoketest: /%s/%s nlink=%u\n", subdir_name, file_a_name, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        }
        vfs_release_dentry(&d_file_a);
    }

    struct vfs_dentry d_file_b = {0};
    ret = vfs_ilookup(nested, &d_file_b, file_b_name, file_b_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: ilookup file /%s/%s/%s failed, errno=%d\n", subdir_name, nested_name, file_b_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: ilookup /%s/%s/%s -> ino=%lu\n", subdir_name, nested_name, file_b_name, d_file_b.ino);
        struct vfs_inode *tmp = NULL;
        ret = tmpfs_fetch_inode(nested, file_b_name, file_b_len, false, &tmp);
        if (ret == 0) {
            vfs_ilock(tmp);
            printf("tmpfs_run_inode_smoketest: /%s/%s/%s nlink=%u\n", subdir_name, nested_name, file_b_name, tmp->n_links);
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
        printf("tmpfs_run_inode_smoketest: move %s -> %s failed, errno=%d\n", file_b_name, file_b_new_name, ret);
    } else {
        printf("tmpfs_run_inode_smoketest: moved /%s/%s/%s to /%s/%s\n",
               subdir_name, nested_name, file_b_name, subdir_name, file_b_new_name);
        struct vfs_inode *tmp = NULL;
        ret = tmpfs_fetch_inode(subdir, file_b_new_name, file_b_new_len, false, &tmp);
        if (ret == 0) {
            vfs_ilock(tmp);
            printf("tmpfs_run_inode_smoketest: post-move /%s/%s nlink=%u\n",
                   subdir_name, file_b_new_name, tmp->n_links);
            vfs_iunlock(tmp);
            vfs_iput(tmp);
        }
        struct vfs_dentry moved_lookup = {0};
        ret = vfs_ilookup(subdir, &moved_lookup, file_b_new_name, file_b_new_len, false);
        if (ret != 0) {
            printf("tmpfs_run_inode_smoketest: ilookup moved file /%s/%s failed, errno=%d\n",
                   subdir_name, file_b_new_name, ret);
        } else {
            printf("tmpfs_run_inode_smoketest: ilookup /%s/%s -> ino=%lu\n",
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
            printf("tmpfs_run_inode_smoketest: readlink %s failed, errno=%d\n", symlink_a_name, ret);
        } else {
            printf("tmpfs_run_inode_smoketest: readlink /%s/%s -> %s (len=%d)\n",
                   subdir_name, symlink_a_name, linkbuf, ret);
            vfs_ilock(tmp_sym);
            printf("tmpfs_run_inode_smoketest: /%s/%s nlink=%u\n", subdir_name, symlink_a_name, tmp_sym->n_links);
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
            printf("tmpfs_run_inode_smoketest: readlink %s failed, errno=%d\n", symlink_b_name, ret);
        } else {
            printf("tmpfs_run_inode_smoketest: readlink /%s/%s/%s -> %s (len=%d)\n",
                   subdir_name, nested_name, symlink_b_name, linkbuf, ret);
            vfs_ilock(tmp_sym);
            printf("tmpfs_run_inode_smoketest: /%s/%s/%s nlink=%u\n", subdir_name, nested_name, symlink_b_name, tmp_sym->n_links);
            vfs_iunlock(tmp_sym);
        }
        vfs_iput(tmp_sym);
    }

out_put_subdir:
    // Cleanup in reverse order: symlinks, files, dirs
    ret = vfs_unlink(nested, symlink_b_name, symlink_b_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: unlink %s failed, errno=%d\n", symlink_b_name, ret);
    }
    ret = vfs_unlink(subdir, symlink_a_name, symlink_a_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: unlink %s failed, errno=%d\n", symlink_a_name, ret);
    }
    // Remove moved file name if move succeeded, else original path
    ret = vfs_unlink(subdir, file_b_new_name, file_b_new_len, false);
    if (ret != 0) {
        ret = vfs_unlink(nested, file_b_name, file_b_len, false);
        if (ret != 0) {
            printf("tmpfs_run_inode_smoketest: unlink %s/%s failed, errno=%d\n", subdir_name, file_b_new_name, ret);
        }
    }
    ret = vfs_unlink(subdir, file_a_name, file_a_len, false);
    if (ret != 0) {
        printf("tmpfs_run_inode_smoketest: unlink %s failed, errno=%d\n", file_a_name, ret);
    }
    if (nested != NULL) {
        ret = vfs_rmdir(subdir, nested_name, nested_len, false);
        if (ret != 0) {
            printf("tmpfs_run_inode_smoketest: rmdir /%s/%s failed, errno=%d\n", subdir_name, nested_name, ret);
        }
    }
    if (subdir != NULL) {
        ret = vfs_rmdir(root, subdir_name, subdir_len, false);
        if (ret != 0) {
            printf("tmpfs_run_inode_smoketest: rmdir /%s failed, errno=%d\n", subdir_name, ret);
        }
    }
out_put_root:
    if (root_pinned) {
        vfs_iput(root);
    }
}
