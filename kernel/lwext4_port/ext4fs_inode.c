/*
 * ext4fs inode operations
 *
 * VFS inode callbacks implemented using lwext4's low-level APIs:
 *   ext4_dir_find_entry / add_entry / remove_entry  — directory ops
 *   ext4_dir_iterator_init / next / fini             — directory iteration
 *   ext4_fs_alloc_inode / free_inode                 — inode allocation
 *   ext4_fs_get_inode_ref / put_inode_ref            — inode access
 *   ext4_fs_truncate_inode                           — truncation
 *
 * All ext4 operations that access on-disk structures acquire ext4_inode_ref
 * on the stack.  This is lightweight — the underlying ext4 block cache
 * keeps the storage cached.  The filesystem-wide ext4fs_lock() serialises
 * concurrent access (lwext4 is not internally thread-safe).
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "lock/mutex_types.h"
#include <mm/vm.h>
#include "vfs/fs.h"
#include <mm/slab.h>
#include "ext4fs_private.h"
#include "kernel/vfs/vfs_private.h"

#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <ext4_super.h>

/* ──────────────────────────────────────────────────────────────────────────── */
/* Helpers                                                                     */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Convenience: get ext4_fs from a VFS inode */
static inline struct ext4_fs *inode_ext4fs(struct vfs_inode *vi)
{
    return ext4fs_get_fs(vi->sb);
}

/*
 * Build a VFS inode for an already-allocated on-disk ext4 inode.
 *
 * This replaces the broken pattern of calling vfs_alloc_inode() which would
 * allocate a SECOND on-disk inode and then overwrite the ino — corrupting
 * the inode hash table (the inode would sit in the wrong hash bucket).
 *
 * Instead we allocate the slab wrapper directly, set the correct ino upfront,
 * initialise refs/mutex via __vfs_inode_init, and insert into the hash via
 * vfs_add_inode.
 *
 * Caller must hold sb wlock.  Returns inode unlocked on success, or ERR_PTR.
 */
static struct vfs_inode *ext4fs_build_vfs_inode(struct vfs_superblock *sb,
                                                uint32_t ino, mode_t mode,
                                                uint32_t n_links, loff_t size)
{
    struct ext4fs_inode *ei = slab_alloc(&ext4fs_inode_cache);
    if (ei == NULL)
        return ERR_PTR(-ENOMEM);
    memset(ei, 0, sizeof(*ei));

    ei->vfs_inode.ino     = ino;
    ei->vfs_inode.mode    = mode;
    ei->vfs_inode.n_links = n_links;
    ei->vfs_inode.size    = size;
    ei->vfs_inode.ops     = &ext4fs_inode_ops;
    __vfs_inode_init(&ei->vfs_inode);  /* ref_count = 1, init mutex */

    struct vfs_inode *existing = vfs_add_inode(sb, &ei->vfs_inode);
    if (IS_ERR(existing)) {
        slab_free(ei);
        return existing;  /* -EAGAIN or other error */
    }
    if (existing != &ei->vfs_inode) {
        /* Race: another thread inserted same ino.  Free our copy. */
        slab_free(ei);
        vfs_iunlock(existing);
        return existing;
    }
    /* vfs_add_inode returns locked; unlock for caller */
    vfs_iunlock(&ei->vfs_inode);
    return &ei->vfs_inode;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Inode sync / dirty                                                          */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_dirty_inode(struct vfs_inode *inode)
{
    if (inode == NULL)
        return -EINVAL;
    inode->dirty = 1;
    return 0;
}

static int ext4fs_sync_inode(struct vfs_inode *inode)
{
    if (inode == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    /* Push VFS fields back to the on-disk inode */
    ext4_inode_set_mode(&fs->sb, ref.inode,
                        vfs_mode_to_ext4_imode(inode->mode));
    ext4_inode_set_links_cnt(ref.inode, (uint16_t)inode->n_links);
    ext4_inode_set_size(ref.inode, (uint64_t)inode->size);
    ext4_inode_set_uid(ref.inode, inode->uid);
    ext4_inode_set_gid(ref.inode, inode->gid);
    ref.dirty = true;

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
    inode->dirty = 0;
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Directory lookup                                                            */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry,
                         const char *name, size_t name_len)
{
    if (dir == NULL || dentry == NULL || name == NULL)
        return -EINVAL;
    if (!S_ISDIR(dir->mode))
        return -ENOTDIR;

    struct ext4fs_superblock *esb = ext4fs_get_esb(dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref parent_ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dir->ino, &parent_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    struct ext4_dir_search_result result;
    r = ext4_dir_find_entry(&result, &parent_ref, name, (uint32_t)name_len);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return -r;
    }

    uint32_t child_ino = ext4_dir_en_get_inode(result.dentry);
    ext4_dir_destroy_result(&parent_ref, &result);
    ext4_fs_put_inode_ref(&parent_ref);
    ext4fs_unlock(esb);

    /* Fill the VFS dentry */
    dentry->ino      = child_ino;
    dentry->sb       = dir->sb;
    dentry->parent   = dir;
    dentry->name     = strndup(name, name_len);
    if (dentry->name == NULL)
        return -ENOMEM;
    dentry->name_len = name_len;

    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Directory iteration                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_dir_iter(struct vfs_inode *dir, struct vfs_dir_iter *iter,
                           struct vfs_dentry *ret_dentry)
{
    if (dir == NULL || iter == NULL || ret_dentry == NULL)
        return -EINVAL;
    if (!S_ISDIR(dir->mode))
        return -ENOTDIR;

    struct ext4fs_superblock *esb = ext4fs_get_esb(dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref dir_ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dir->ino, &dir_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    /* index == 1: VFS wants ".." */
    if (iter->index == 1) {
        /* Find ".." in the directory */
        struct ext4_dir_search_result result;
        r = ext4_dir_find_entry(&result, &dir_ref, "..", 2);
        if (r != EOK) {
            ext4_fs_put_inode_ref(&dir_ref);
            ext4fs_unlock(esb);
            return -r;
        }

        vfs_release_dentry(ret_dentry);
        ret_dentry->name = strndup("..", 2);
        if (ret_dentry->name == NULL) {
            ext4_dir_destroy_result(&dir_ref, &result);
            ext4_fs_put_inode_ref(&dir_ref);
            ext4fs_unlock(esb);
            return -ENOMEM;
        }
        ret_dentry->name_len = 2;
        ret_dentry->ino      = ext4_dir_en_get_inode(result.dentry);
        ret_dentry->sb       = dir->sb;

        ext4_dir_destroy_result(&dir_ref, &result);
        ext4_fs_put_inode_ref(&dir_ref);
        ext4fs_unlock(esb);
        return 0;
    }

    /* index > 1: iterate regular entries starting from cookies offset */
    uint64_t start_off = (uint64_t)iter->cookies;

    struct ext4_dir_iter dit;
    r = ext4_dir_iterator_init(&dit, &dir_ref, start_off);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&dir_ref);
        ext4fs_unlock(esb);
        return -r;
    }

    while (dit.curr != NULL) {
        uint32_t ino = ext4_dir_en_get_inode(dit.curr);
        if (ino == 0) {
            /* deleted entry — skip */
            r = ext4_dir_iterator_next(&dit);
            if (r != EOK)
                break;
            continue;
        }

        uint16_t nlen = ext4_dir_en_get_name_len(&fs->sb, dit.curr);
        const char *ename = (const char *)(dit.curr) + sizeof(struct ext4_fake_dir_entry);

        /* Skip "." and ".." — VFS handles them */
        if ((nlen == 1 && ename[0] == '.') ||
            (nlen == 2 && ename[0] == '.' && ename[1] == '.')) {
            r = ext4_dir_iterator_next(&dit);
            if (r != EOK)
                break;
            continue;
        }

        /* Found a real entry */
        vfs_release_dentry(ret_dentry);
        ret_dentry->name = strndup(ename, nlen);
        if (ret_dentry->name == NULL) {
            ext4_dir_iterator_fini(&dit);
            ext4_fs_put_inode_ref(&dir_ref);
            ext4fs_unlock(esb);
            return -ENOMEM;
        }
        ret_dentry->name_len = nlen;
        ret_dentry->ino      = ino;

        /* Advance past this entry for next call */
        uint64_t next_off = dit.curr_off +
                            ext4_dir_en_get_entry_len(dit.curr);
        ret_dentry->cookies = (int64_t)next_off;

        ext4_dir_iterator_fini(&dit);
        ext4_fs_put_inode_ref(&dir_ref);
        ext4fs_unlock(esb);
        return 0;
    }

    /* End of directory */
    ext4_dir_iterator_fini(&dit);
    ext4_fs_put_inode_ref(&dir_ref);
    ext4fs_unlock(esb);

    vfs_release_dentry(ret_dentry);
    ret_dentry->name     = NULL;
    ret_dentry->name_len = 0;
    ret_dentry->cookies  = 0; /* VFS_DENTRY_COOKIE_END */
    return 0;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Create regular file                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

static struct vfs_inode *ext4fs_create(struct vfs_inode *dir, mode_t mode,
                                       const char *name, size_t name_len)
{
    if (dir == NULL || name == NULL || name_len == 0)
        return ERR_PTR(-EINVAL);

    struct ext4fs_superblock *esb = ext4fs_get_esb(dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    struct ext4_inode_ref parent_ref;
    ext4fs_lock(esb);
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dir->ino, &parent_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Check if name already exists */
    struct ext4_dir_search_result check;
    r = ext4_dir_find_entry(&check, &parent_ref, name, (uint32_t)name_len);
    if (r == EOK) {
        ext4_dir_destroy_result(&parent_ref, &check);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-EEXIST);
    }

    /* Allocate new inode on disk */
    struct ext4_inode_ref child_ref;
    r = ext4_fs_alloc_inode(fs, &child_ref, EXT4_DE_REG_FILE);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Set inode fields */
    ext4_inode_set_mode(&fs->sb, child_ref.inode,
                        vfs_mode_to_ext4_imode(mode | S_IFREG));
    ext4_inode_set_links_cnt(child_ref.inode, 1);
    ext4_inode_set_size(child_ref.inode, 0);
    child_ref.dirty = true;

    /* Add directory entry */
    r = ext4_dir_add_entry(&parent_ref, name, (uint32_t)name_len, &child_ref);
    if (r != EOK) {
        ext4_fs_free_inode(&child_ref);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    uint32_t new_ino = child_ref.index;
    ext4_fs_put_inode_ref(&child_ref);
    ext4_fs_put_inode_ref(&parent_ref);
    ext4fs_unlock(esb);

    /* Build VFS inode directly — no vfs_alloc_inode to avoid double
     * on-disk allocation and hash corruption. */
    return ext4fs_build_vfs_inode(dir->sb, new_ino, mode | S_IFREG, 1, 0);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Create directory                                                            */
/* ──────────────────────────────────────────────────────────────────────────── */

static struct vfs_inode *ext4fs_mkdir(struct vfs_inode *dir, mode_t mode,
                                      const char *name, size_t name_len)
{
    if (dir == NULL || name == NULL || name_len == 0)
        return ERR_PTR(-EINVAL);

    struct ext4fs_superblock *esb = ext4fs_get_esb(dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    struct ext4_inode_ref parent_ref;
    ext4fs_lock(esb);
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dir->ino, &parent_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Check if name already exists */
    struct ext4_dir_search_result check;
    r = ext4_dir_find_entry(&check, &parent_ref, name, (uint32_t)name_len);
    if (r == EOK) {
        ext4_dir_destroy_result(&parent_ref, &check);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-EEXIST);
    }

    /* Allocate new directory inode */
    struct ext4_inode_ref child_ref;
    r = ext4_fs_alloc_inode(fs, &child_ref, EXT4_DE_DIR);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Set mode */
    ext4_inode_set_mode(&fs->sb, child_ref.inode,
                        vfs_mode_to_ext4_imode(mode | S_IFDIR));
    ext4_inode_set_links_cnt(child_ref.inode, 1);
    child_ref.dirty = true;

    /* Initialize blocks for the new directory */
    ext4_fs_inode_blocks_init(fs, &child_ref);

    /* Add "." and ".." entries */
    r = ext4_dir_add_entry(&child_ref, ".", 1, &child_ref);
    if (r != EOK) {
        ext4_fs_free_inode(&child_ref);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    r = ext4_dir_add_entry(&child_ref, "..", 2, &parent_ref);
    if (r != EOK) {
        ext4_fs_free_inode(&child_ref);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Increment parent's link count for ".." */
    ext4_fs_inode_links_count_inc(&parent_ref);
    parent_ref.dirty = true;

    /* Add entry in parent directory */
    r = ext4_dir_add_entry(&parent_ref, name, (uint32_t)name_len, &child_ref);
    if (r != EOK) {
        ext4_fs_inode_links_count_dec(&parent_ref);
        parent_ref.dirty = true;
        ext4_fs_free_inode(&child_ref);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    uint32_t new_ino = child_ref.index;
    ext4_fs_put_inode_ref(&child_ref);
    ext4_fs_put_inode_ref(&parent_ref);
    ext4fs_unlock(esb);

    /* Update link count for parent (for "..") */
    dir->n_links++;

    /* Build VFS inode directly — no vfs_alloc_inode to avoid double
     * on-disk allocation and hash corruption. */
    return ext4fs_build_vfs_inode(dir->sb, new_ino, mode | S_IFDIR, 1, 0);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Unlink / rmdir                                                              */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Inner helper — must be called with esb->lock held */
static int do_ext4fs_unlink(struct ext4_fs *fs, struct vfs_dentry *dentry,
                            struct vfs_inode *target)
{
    struct ext4_inode_ref parent_ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dentry->parent->ino,
                                  &parent_ref);
    if (r != EOK)
        return -r;

    /* Remove directory entry */
    r = ext4_dir_remove_entry(&parent_ref, dentry->name,
                              (uint32_t)dentry->name_len);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&parent_ref);
        return -r;
    }

    ext4_fs_put_inode_ref(&parent_ref);

    /* Decrement link count */
    target->n_links--;

    /* Write updated link count to disk */
    struct ext4_inode_ref target_ref;
    r = ext4_fs_get_inode_ref(fs, (uint32_t)target->ino, &target_ref);
    if (r == EOK) {
        ext4_fs_inode_links_count_dec(&target_ref);
        target_ref.dirty = true;
        ext4_fs_put_inode_ref(&target_ref);
    }

    return 0;
}

static int ext4fs_unlink(struct vfs_dentry *dentry, struct vfs_inode *target)
{
    if (dentry == NULL || target == NULL)
        return -EINVAL;
    if (dentry->sb == NULL || dentry->sb != target->sb)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(dentry->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    int r = do_ext4fs_unlink(fs, dentry, target);
    ext4fs_unlock(esb);
    return r;
}

static int ext4fs_rmdir(struct vfs_dentry *dentry, struct vfs_inode *target)
{
    if (dentry == NULL || target == NULL)
        return -EINVAL;
    if (dentry->sb == NULL || dentry->sb != target->sb)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(dentry->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    int r = do_ext4fs_unlink(fs, dentry, target);
    if (r == 0 && dentry->parent != NULL) {
        dentry->parent->n_links--;

        /* Update parent link count on disk */
        struct ext4_inode_ref parent_ref;
        int r2 = ext4_fs_get_inode_ref(
            fs, (uint32_t)dentry->parent->ino, &parent_ref);
        if (r2 == EOK) {
            ext4_fs_inode_links_count_dec(&parent_ref);
            parent_ref.dirty = true;
            ext4_fs_put_inode_ref(&parent_ref);
        }
    }
    ext4fs_unlock(esb);
    return r;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Hard link                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_link(struct vfs_inode *old, struct vfs_inode *dir,
                       const char *name, size_t name_len)
{
    if (old == NULL || dir == NULL || name == NULL)
        return -EINVAL;
    if (S_ISDIR(old->mode))
        return -EPERM;

    struct ext4fs_superblock *esb = ext4fs_get_esb(dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref parent_ref, child_ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dir->ino, &parent_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    r = ext4_fs_get_inode_ref(fs, (uint32_t)old->ino, &child_ref);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return -r;
    }

    /* Check if name exists */
    struct ext4_dir_search_result check;
    r = ext4_dir_find_entry(&check, &parent_ref, name, (uint32_t)name_len);
    if (r == EOK) {
        ext4_dir_destroy_result(&parent_ref, &check);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return -EEXIST;
    }

    /* Increment link count */
    ext4_fs_inode_links_count_inc(&child_ref);
    child_ref.dirty = true;
    old->n_links++;

    /* Add directory entry */
    r = ext4_dir_add_entry(&parent_ref, name, (uint32_t)name_len, &child_ref);
    if (r != EOK) {
        ext4_fs_inode_links_count_dec(&child_ref);
        child_ref.dirty = true;
        old->n_links--;
    }

    ext4_fs_put_inode_ref(&child_ref);
    ext4_fs_put_inode_ref(&parent_ref);
    ext4fs_unlock(esb);
    return r == EOK ? 0 : -r;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Truncate                                                                    */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_truncate(struct vfs_inode *inode, loff_t new_size)
{
    if (inode == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    r = ext4_fs_truncate_inode(&ref, (uint64_t)new_size);
    if (r == EOK) {
        ext4_inode_set_size(ref.inode, (uint64_t)new_size);
        ref.dirty = true;
        inode->size = new_size;
    }

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
    return r == EOK ? 0 : -r;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Symlink operations                                                          */
/* ──────────────────────────────────────────────────────────────────────────── */

static ssize_t ext4fs_readlink(struct vfs_inode *inode, char *buf,
                               size_t buflen)
{
    if (inode == NULL || buf == NULL)
        return -EINVAL;
    if (!S_ISLNK(inode->mode))
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    uint64_t link_len = ext4_inode_get_size(&fs->sb, ref.inode);
    if (link_len + 1 > buflen) {
        ext4_fs_put_inode_ref(&ref);
        ext4fs_unlock(esb);
        return -ENAMETOOLONG;
    }

    /*
     * Short symlinks store the target in the inode's block pointers area.
     * Long symlinks store it in data blocks.
     */
    if (link_len < sizeof(ref.inode->blocks)) {
        /* Fast symlink — target is in the blocks area */
        memcpy(buf, ref.inode->blocks, (uint)link_len);
    } else {
        /* Slow symlink — read from data blocks */
        uint64_t bytes_read = 0;
        while (bytes_read < link_len) {
            ext4_lblk_t iblock = (ext4_lblk_t)(bytes_read / block_size);
            uint off = (uint)(bytes_read % block_size);
            uint n = block_size - off;
            if (n > link_len - bytes_read)
                n = (uint)(link_len - bytes_read);

            ext4_fsblk_t fblock;
            r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, false);
            if (r != EOK || fblock == 0) {
                ext4_fs_put_inode_ref(&ref);
                ext4fs_unlock(esb);
                return -EIO;
            }

            struct ext4_block blk;
            r = ext4_block_get(&esb->bdev, &blk, fblock);
            if (r != EOK) {
                ext4_fs_put_inode_ref(&ref);
                ext4fs_unlock(esb);
                return -EIO;
            }

            memcpy(buf + bytes_read, blk.data + off, n);
            ext4_block_set(&esb->bdev, &blk);

            bytes_read += n;
        }
    }

    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
    buf[link_len] = '\0';
    return (ssize_t)link_len;
}

static struct vfs_inode *ext4fs_symlink(struct vfs_inode *dir, mode_t mode,
                                        const char *name, size_t name_len,
                                        const char *target, size_t target_len)
{
    if (dir == NULL || name == NULL || target == NULL ||
        name_len == 0 || target_len == 0)
        return ERR_PTR(-EINVAL);

    struct ext4fs_superblock *esb = ext4fs_get_esb(dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    uint32_t block_size = ext4_sb_get_block_size(&fs->sb);

    ext4fs_lock(esb);
    struct ext4_inode_ref parent_ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dir->ino, &parent_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Check for existing name */
    struct ext4_dir_search_result check;
    r = ext4_dir_find_entry(&check, &parent_ref, name, (uint32_t)name_len);
    if (r == EOK) {
        ext4_dir_destroy_result(&parent_ref, &check);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-EEXIST);
    }

    /* Allocate symlink inode */
    struct ext4_inode_ref child_ref;
    r = ext4_fs_alloc_inode(fs, &child_ref, EXT4_DE_SYMLINK);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    ext4_inode_set_mode(&fs->sb, child_ref.inode,
                        vfs_mode_to_ext4_imode(S_IFLNK | 0777));
    ext4_inode_set_links_cnt(child_ref.inode, 1);
    child_ref.dirty = true;

    ext4_fs_inode_blocks_init(fs, &child_ref);

    /* Write symlink target */
    if (target_len < sizeof(child_ref.inode->blocks)) {
        /* Fast symlink */
        memcpy(child_ref.inode->blocks, target, target_len);
        ext4_inode_set_size(child_ref.inode, target_len);
    } else {
        /* Slow symlink — write to data blocks */
        uint64_t written = 0;
        while (written < target_len) {
            ext4_fsblk_t fblock;
            ext4_lblk_t iblock;
            r = ext4_fs_append_inode_dblk(&child_ref, &fblock, &iblock);
            if (r != EOK) {
                ext4_fs_free_inode(&child_ref);
                ext4_fs_put_inode_ref(&child_ref);
                ext4_fs_put_inode_ref(&parent_ref);
                ext4fs_unlock(esb);
                return ERR_PTR(-r);
            }

            struct ext4_block blk;
            r = ext4_block_get(&esb->bdev, &blk, fblock);
            if (r != EOK) {
                ext4_fs_free_inode(&child_ref);
                ext4_fs_put_inode_ref(&child_ref);
                ext4_fs_put_inode_ref(&parent_ref);
                ext4fs_unlock(esb);
                return ERR_PTR(-EIO);
            }

            uint n = block_size;
            if (n > target_len - written)
                n = (uint)(target_len - written);
            memcpy(blk.data, target + written, n);
            if (n < block_size)
                memset(blk.data + n, 0, block_size - n);

            blk.buf->flags |= 0x02; /* BC_DIRTY */
            ext4_block_set(&esb->bdev, &blk);
            written += n;
        }
        ext4_inode_set_size(child_ref.inode, target_len);
    }
    child_ref.dirty = true;

    /* Add directory entry */
    r = ext4_dir_add_entry(&parent_ref, name, (uint32_t)name_len, &child_ref);
    if (r != EOK) {
        ext4_fs_free_inode(&child_ref);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    uint32_t new_ino = child_ref.index;
    ext4_fs_put_inode_ref(&child_ref);
    ext4_fs_put_inode_ref(&parent_ref);
    ext4fs_unlock(esb);

    /* Build VFS inode directly — no vfs_alloc_inode to avoid double
     * on-disk allocation and hash corruption. */
    return ext4fs_build_vfs_inode(dir->sb, new_ino, S_IFLNK | 0777, 1,
                                  (loff_t)target_len);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Device node (mknod)                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

static struct vfs_inode *ext4fs_mknod(struct vfs_inode *dir, mode_t mode,
                                      dev_t dev, const char *name,
                                      size_t name_len)
{
    if (dir == NULL || name == NULL || name_len == 0)
        return ERR_PTR(-EINVAL);
    if (!S_ISBLK(mode) && !S_ISCHR(mode))
        return ERR_PTR(-EINVAL);

    struct ext4fs_superblock *esb = ext4fs_get_esb(dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref parent_ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)dir->ino, &parent_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    int de_type = S_ISCHR(mode) ? EXT4_DE_CHRDEV : EXT4_DE_BLKDEV;
    struct ext4_inode_ref child_ref;
    r = ext4_fs_alloc_inode(fs, &child_ref, de_type);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    ext4_inode_set_mode(&fs->sb, child_ref.inode,
                        vfs_mode_to_ext4_imode(mode));
    ext4_inode_set_links_cnt(child_ref.inode, 1);
    ext4_inode_set_dev(child_ref.inode, (uint32_t)dev);
    child_ref.dirty = true;

    r = ext4_dir_add_entry(&parent_ref, name, (uint32_t)name_len, &child_ref);
    if (r != EOK) {
        ext4_fs_free_inode(&child_ref);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    uint32_t new_ino = child_ref.index;
    ext4_fs_put_inode_ref(&child_ref);
    ext4_fs_put_inode_ref(&parent_ref);
    ext4fs_unlock(esb);

    /* Build VFS inode directly — no vfs_alloc_inode to avoid double
     * on-disk allocation and hash corruption. */
    struct vfs_inode *new_inode =
        ext4fs_build_vfs_inode(dir->sb, new_ino, mode, 1, 0);
    if (IS_ERR(new_inode))
        return new_inode;

    if (S_ISCHR(mode))
        new_inode->cdev = dev;
    else
        new_inode->bdev = dev;

    return new_inode;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Inode lifecycle                                                             */
/* ──────────────────────────────────────────────────────────────────────────── */

static void ext4fs_destroy_inode(struct vfs_inode *inode)
{
    if (inode == NULL)
        return;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    ext4fs_lock(esb);
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return;
    }

    /* Truncate all data and free the inode on disk */
    ext4_fs_truncate_inode(&ref, 0);
    ext4_fs_free_inode(&ref);
    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
}

static void ext4fs_free_inode(struct vfs_inode *inode)
{
    if (inode == NULL)
        return;
    struct ext4fs_inode *ei =
        container_of(inode, struct ext4fs_inode, vfs_inode);
    slab_free(ei);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Open callback                                                               */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_open(struct vfs_inode *inode, struct vfs_file *file,
                       int f_flags)
{
    if (inode == NULL || file == NULL)
        return -EINVAL;

    if (S_ISREG(inode->mode) || S_ISDIR(inode->mode) ||
        S_ISLNK(inode->mode)) {
        file->ops = &ext4fs_file_ops;
        return 0;
    }

    /* char/block devices handled by VFS core */
    if (S_ISCHR(inode->mode) || S_ISBLK(inode->mode))
        return -EINVAL;

    return -ENOSYS;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Getattr / setattr                                                           */
/* ──────────────────────────────────────────────────────────────────────────── */

static int ext4fs_getattr(struct vfs_inode *inode, struct stat *stat)
{
    if (inode == NULL || stat == NULL)
        return -EINVAL;

    vfs_ilock(inode);
    memset(stat, 0, sizeof(*stat));

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    stat->dev   = ext4fs_sb_dev(esb);
    stat->ino   = inode->ino;
    stat->mode  = inode->mode;
    stat->nlink = inode->n_links;
    stat->size  = inode->size;

    vfs_iunlock(inode);
    return 0;
}

static int ext4fs_setattr(struct vfs_inode *inode, const struct stat *stat)
{
    (void)inode;
    (void)stat;
    return -EOPNOTSUPP;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* VFS inode operations table                                                  */
/* ──────────────────────────────────────────────────────────────────────────── */

struct vfs_inode_ops ext4fs_inode_ops = {
    .lookup        = ext4fs_lookup,
    .dir_iter      = ext4fs_dir_iter,
    .readlink      = ext4fs_readlink,
    .getattr       = ext4fs_getattr,
    .setattr       = ext4fs_setattr,
    .create        = ext4fs_create,
    .link          = ext4fs_link,
    .unlink        = ext4fs_unlink,
    .mkdir         = ext4fs_mkdir,
    .rmdir         = ext4fs_rmdir,
    .mknod         = ext4fs_mknod,
    .move          = NULL,
    .symlink       = ext4fs_symlink,
    .truncate      = ext4fs_truncate,
    .destroy_inode = ext4fs_destroy_inode,
    .free_inode    = ext4fs_free_inode,
    .dirty_inode   = ext4fs_dirty_inode,
    .sync_inode    = ext4fs_sync_inode,
    .open          = ext4fs_open,
};
