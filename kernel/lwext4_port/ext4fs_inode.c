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
#include <mm/pcache.h>
#include "vfs/fs.h"
#include "vfs/fcntl.h"
#include <mm/slab.h>
#include "ext4fs_private.h"
#include "kernel/vfs/vfs_private.h"
#include "proc/cred.h"

#include <ext4_errno.h>
#include <ext4_fs.h>
#include <ext4_inode.h>
#include <ext4_dir.h>
#include <ext4_dir_idx.h>
#include <ext4_super.h>
#include <ext4_trans.h>

#include "timer/goldfish_rtc.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Helpers                                                                     */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Timestamp helper: set mtime+ctime on a VFS inode AND the on-disk
 * ext4_inode_ref (which must already be acquired).  Marks ref dirty.
 */
static inline void ext4fs_stamp_mctime(struct vfs_inode *vi,
                                       struct ext4_inode_ref *ref)
{
    uint32_t now = (uint32_t)goldfish_rtc_read_sec();
    vi->mtime = now;
    vi->ctime = now;
    ext4_inode_set_modif_time(ref->inode, now);
    ext4_inode_set_change_inode_time(ref->inode, now);
    ref->dirty = true;
}

/*
 * Stamp mtime+ctime on a VFS inode given only its ino (acquires
 * ext4_inode_ref internally).  Caller must hold ext4fs_lock.
 */
static void ext4fs_stamp_inode_mc(struct ext4_fs *fs, struct vfs_inode *vi)
{
    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)vi->ino, &ref);
    if (r != EOK)
        return;
    ext4fs_stamp_mctime(vi, &ref);
    ext4_fs_put_inode_ref(&ref);
}

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
    ext4fs_inode_map_cache_init(ei);

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

    /* Timestamps */
    ext4_inode_set_access_time(ref.inode, (uint32_t)inode->atime);
    ext4_inode_set_modif_time(ref.inode, (uint32_t)inode->mtime);
    ext4_inode_set_change_inode_time(ref.inode, (uint32_t)inode->ctime);
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
        ret_dentry->d_type   = EXT4_DE_DIR;
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
        ret_dentry->d_type   = ext4_dir_en_get_inode_type(&fs->sb, dit.curr);

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
    ext4_inode_set_uid(child_ref.inode, current_euid());
    ext4_inode_set_gid(child_ref.inode, current_egid());
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_access_time(child_ref.inode, now);
        ext4_inode_set_modif_time(child_ref.inode, now);
        ext4_inode_set_change_inode_time(child_ref.inode, now);
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

    /* Stamp parent directory mtime/ctime */
    ext4fs_stamp_mctime(dir, &parent_ref);

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
    ext4_inode_set_uid(child_ref.inode, current_euid());
    ext4_inode_set_gid(child_ref.inode, current_egid());
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_access_time(child_ref.inode, now);
        ext4_inode_set_modif_time(child_ref.inode, now);
        ext4_inode_set_change_inode_time(child_ref.inode, now);
    }
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

    /* Stamp parent directory mtime/ctime */
    ext4fs_stamp_mctime(dir, &parent_ref);

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

    /* Stamp parent directory mtime/ctime */
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_modif_time(parent_ref.inode, now);
        ext4_inode_set_change_inode_time(parent_ref.inode, now);
        parent_ref.dirty = true;
        if (dentry->parent) {
            dentry->parent->mtime = now;
            dentry->parent->ctime = now;
        }
    }
    ext4_fs_put_inode_ref(&parent_ref);

    /* Decrement link count */
    target->n_links--;

    /* Write updated link count to disk, stamp ctime */
    struct ext4_inode_ref target_ref;
    r = ext4_fs_get_inode_ref(fs, (uint32_t)target->ino, &target_ref);
    if (r == EOK) {
        ext4_fs_inode_links_count_dec(&target_ref);
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_change_inode_time(target_ref.inode, now);
        target->ctime = now;
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
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_change_inode_time(child_ref.inode, now);
        old->ctime = now;
    }
    child_ref.dirty = true;
    old->n_links++;

    /* Add directory entry */
    r = ext4_dir_add_entry(&parent_ref, name, (uint32_t)name_len, &child_ref);
    if (r != EOK) {
        ext4_fs_inode_links_count_dec(&child_ref);
        child_ref.dirty = true;
        old->n_links--;
    } else {
        /* Stamp parent directory mtime/ctime */
        ext4fs_stamp_mctime(dir, &parent_ref);
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

    ext4fs_inode_map_cache_invalidate(inode);

    if (inode->i_data.active)
        pcache_teardown(&inode->i_data);

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
        ext4fs_stamp_mctime(inode, &ref);
        inode->size = new_size;
        inode->n_blocks = ext4_inode_get_blocks_count(&fs->sb, ref.inode);
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
            r = ext4_fs_get_inode_dblk_idx(&ref, iblock, &fblock, true);
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
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_access_time(child_ref.inode, now);
        ext4_inode_set_modif_time(child_ref.inode, now);
        ext4_inode_set_change_inode_time(child_ref.inode, now);
    }
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

    /* Stamp parent directory mtime/ctime */
    ext4fs_stamp_mctime(dir, &parent_ref);

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
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_access_time(child_ref.inode, now);
        ext4_inode_set_modif_time(child_ref.inode, now);
        ext4_inode_set_change_inode_time(child_ref.inode, now);
    }
    child_ref.dirty = true;

    r = ext4_dir_add_entry(&parent_ref, name, (uint32_t)name_len, &child_ref);
    if (r != EOK) {
        ext4_fs_free_inode(&child_ref);
        ext4_fs_put_inode_ref(&child_ref);
        ext4_fs_put_inode_ref(&parent_ref);
        ext4fs_unlock(esb);
        return ERR_PTR(-r);
    }

    /* Stamp parent directory mtime/ctime */
    ext4fs_stamp_mctime(dir, &parent_ref);

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

    if (inode->i_data.active) {
        /* The inode is being permanently deleted (nlink==0).  Suppress
         * the dirty-page flush inside pcache_teardown: writing data to
         * blocks that will be freed immediately by truncate_inode is
         * wasteful and can trigger ext4 bcache reference-count
         * assertions when the write-back path and the truncation path
         * touch the same bcache buffers.  Setting active=0 before
         * teardown makes pcache_flush() return early while still
         * properly releasing all cached pages. */
        inode->i_data.active = 0;
        pcache_teardown(&inode->i_data);
    }

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
    if (inode->i_data.active)
        pcache_teardown(&inode->i_data);
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

    if (S_ISREG(inode->mode)) {
        file->ops = &ext4fs_file_ops;
        if ((f_flags & O_ACCMODE) == O_RDONLY && !inode->i_data.active)
            ext4fs_inode_pcache_init(inode);
        return 0;
    }

    if (S_ISDIR(inode->mode) || S_ISLNK(inode->mode) || S_ISFIFO(inode->mode)) {
        file->ops = &ext4fs_file_ops;
        return 0;
    }

    /* char/block devices handled by VFS core */
    if (S_ISCHR(inode->mode) || S_ISBLK(inode->mode))
        return -EINVAL;

    return -ENOSYS;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Move / rename                                                               */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Move (rename) a file or directory within the same ext4 filesystem.
 *
 * VFS guarantees: both old_dir and new_dir are locked (via
 * vfs_ilock_two_directories); same superblock; sb wlock held.
 *
 * Algorithm:
 *   1. Get inode refs for old parent, new parent, and child.
 *   2. Check if the target name already exists in new_dir.
 *      - If the existing target is the same inode → nothing to do.
 *      - If types conflict (file over dir or vice versa) → error.
 *      - Otherwise, remove the existing target entry and adjust links.
 *   3. Add a new directory entry in new_dir.
 *   4. Remove the old directory entry from old_dir.
 *   5. For directories: update ".." to point to the new parent,
 *      and adjust parent link counts.
 *   6. Stamp mtime/ctime on both parent directories.
 */
static int ext4fs_move(struct vfs_inode *old_dir,
                       struct vfs_dentry *old_dentry,
                       struct vfs_inode *new_dir,
                       const char *name, size_t name_len)
{
    if (old_dir == NULL || old_dentry == NULL || new_dir == NULL ||
        name == NULL || name_len == 0)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(old_dir->sb);
    struct ext4_fs *fs = &esb->ext4fs;
    int is_dir = S_ISDIR(old_dentry->sb ? /* use target inode mode */
                old_dir->mode : 0); /* fallback — refined below */

    ext4fs_lock(esb);

    /* Get inode refs */
    struct ext4_inode_ref old_parent_ref, new_parent_ref, child_ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)old_dir->ino, &old_parent_ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        return -r;
    }

    r = ext4_fs_get_inode_ref(fs, (uint32_t)new_dir->ino, &new_parent_ref);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&old_parent_ref);
        ext4fs_unlock(esb);
        return -r;
    }

    r = ext4_fs_get_inode_ref(fs, (uint32_t)old_dentry->ino, &child_ref);
    if (r != EOK) {
        ext4_fs_put_inode_ref(&new_parent_ref);
        ext4_fs_put_inode_ref(&old_parent_ref);
        ext4fs_unlock(esb);
        return -r;
    }

    /* Determine if child is a directory from its on-disk mode */
    uint32_t child_mode = ext4_inode_get_mode(&fs->sb, child_ref.inode);
    is_dir = (child_mode & 0xF000) == EXT4_INODE_MODE_DIRECTORY;

    /* Check if target name already exists in new_dir */
    struct ext4_dir_search_result existing;
    r = ext4_dir_find_entry(&existing, &new_parent_ref, name,
                            (uint32_t)name_len);
    if (r == EOK) {
        uint32_t existing_ino = ext4_dir_en_get_inode(existing.dentry);
        ext4_dir_destroy_result(&new_parent_ref, &existing);

        if (existing_ino == old_dentry->ino) {
            /* Same inode — nothing to do */
            ext4_fs_put_inode_ref(&child_ref);
            ext4_fs_put_inode_ref(&new_parent_ref);
            ext4_fs_put_inode_ref(&old_parent_ref);
            ext4fs_unlock(esb);
            return 0;
        }

        /* Remove the existing target entry */
        struct ext4_inode_ref target_ref;
        r = ext4_fs_get_inode_ref(fs, existing_ino, &target_ref);
        if (r != EOK)
            goto fail;

        uint32_t target_mode = ext4_inode_get_mode(&fs->sb, target_ref.inode);
        int target_is_dir = (target_mode & 0xF000) == EXT4_INODE_MODE_DIRECTORY;

        /* Type conflict checks */
        if (is_dir && !target_is_dir) {
            ext4_fs_put_inode_ref(&target_ref);
            r = ENOTDIR;
            goto fail;
        }
        if (!is_dir && target_is_dir) {
            ext4_fs_put_inode_ref(&target_ref);
            r = EISDIR;
            goto fail;
        }

        /* Remove existing target */
        r = ext4_dir_remove_entry(&new_parent_ref, name, (uint32_t)name_len);
        if (r != EOK) {
            ext4_fs_put_inode_ref(&target_ref);
            goto fail;
        }

        /* Decrement target link count */
        ext4_fs_inode_links_count_dec(&target_ref);
        if (target_is_dir) {
            /* Also decrement new_dir's link count (removing ".." backref) */
            ext4_fs_inode_links_count_dec(&new_parent_ref);
            new_parent_ref.dirty = true;
            new_dir->n_links--;
        }
        target_ref.dirty = true;
        ext4_fs_put_inode_ref(&target_ref);
    }

    /* Add new directory entry in new_dir */
    r = ext4_dir_add_entry(&new_parent_ref, name, (uint32_t)name_len,
                           &child_ref);
    if (r != EOK)
        goto fail;

    /* Remove old directory entry from old_dir */
    r = ext4_dir_remove_entry(&old_parent_ref, old_dentry->name,
                              (uint32_t)old_dentry->name_len);
    if (r != EOK) {
        /* Rollback: remove the entry we just added */
        ext4_dir_remove_entry(&new_parent_ref, name, (uint32_t)name_len);
        goto fail;
    }

    /* For directories: update ".." and adjust parent link counts */
    if (is_dir && old_dir->ino != new_dir->ino) {
        /* Update ".." entry to point to new parent */
        struct ext4_dir_search_result dotdot;
        int r2 = ext4_dir_find_entry(&dotdot, &child_ref, "..", 2);
        if (r2 == EOK) {
            ext4_dir_en_set_inode(dotdot.dentry, new_parent_ref.index);
            ext4_trans_set_block_dirty(dotdot.block.buf);
            ext4_dir_destroy_result(&child_ref, &dotdot);
        }
        /* Also handle htree-indexed directories */
        ext4_dir_dx_reset_parent_inode(&child_ref, new_parent_ref.index);

        /* Decrement old parent link count, increment new parent */
        ext4_fs_inode_links_count_dec(&old_parent_ref);
        old_parent_ref.dirty = true;
        old_dir->n_links--;

        ext4_fs_inode_links_count_inc(&new_parent_ref);
        new_parent_ref.dirty = true;
        new_dir->n_links++;
    }

    /* Stamp mtime/ctime on both parent directories */
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        ext4_inode_set_modif_time(old_parent_ref.inode, now);
        ext4_inode_set_change_inode_time(old_parent_ref.inode, now);
        old_parent_ref.dirty = true;
        old_dir->mtime = now;
        old_dir->ctime = now;

        ext4_inode_set_modif_time(new_parent_ref.inode, now);
        ext4_inode_set_change_inode_time(new_parent_ref.inode, now);
        new_parent_ref.dirty = true;
        new_dir->mtime = now;
        new_dir->ctime = now;

        /* Stamp ctime on the moved inode itself */
        ext4_inode_set_change_inode_time(child_ref.inode, now);
        child_ref.dirty = true;
    }

    ext4_fs_put_inode_ref(&child_ref);
    ext4_fs_put_inode_ref(&new_parent_ref);
    ext4_fs_put_inode_ref(&old_parent_ref);
    ext4fs_unlock(esb);
    return 0;

fail:
    ext4_fs_put_inode_ref(&child_ref);
    ext4_fs_put_inode_ref(&new_parent_ref);
    ext4_fs_put_inode_ref(&old_parent_ref);
    ext4fs_unlock(esb);
    return -r;
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
    stat->st_dev   = ext4fs_sb_dev(esb);
    stat->st_ino   = inode->ino;
    stat->st_mode  = inode->mode;
    stat->st_nlink = inode->n_links;
    stat->st_uid   = inode->uid;
    stat->st_gid   = inode->gid;
    stat->st_size  = inode->size;
    stat->st_blksize = inode->sb ? inode->sb->block_size : 1024;
    stat->st_blocks  = inode->n_blocks;
    stat->st_atime_sec = inode->atime;
    stat->st_mtime_sec = inode->mtime;
    stat->st_ctime_sec = inode->ctime;

    vfs_iunlock(inode);
    return 0;
}

static int ext4fs_setattr(struct vfs_inode *inode, const struct stat *stat)
{
    if (inode == NULL || stat == NULL)
        return -EINVAL;

    struct ext4fs_superblock *esb = ext4fs_get_esb(inode->sb);
    struct ext4_fs *fs = &esb->ext4fs;

    vfs_ilock(inode);
    ext4fs_lock(esb);

    struct ext4_inode_ref ref;
    int r = ext4_fs_get_inode_ref(fs, (uint32_t)inode->ino, &ref);
    if (r != EOK) {
        ext4fs_unlock(esb);
        vfs_iunlock(inode);
        return -r;
    }

    /* Update mode (preserve file type bits from current mode) */
    if (stat->st_mode != 0) {
        mode_t new_mode = (inode->mode & S_IFMT) | (stat->st_mode & ~S_IFMT);
        inode->mode = new_mode;
        ext4_inode_set_mode(&fs->sb, ref.inode,
                            vfs_mode_to_ext4_imode(new_mode));
    }

    /* Update uid/gid */
    if (stat->st_uid != (uint32)-1 && stat->st_uid != 0xFFFFFFFF) {
        inode->uid = stat->st_uid;
        ext4_inode_set_uid(ref.inode, stat->st_uid);
    }
    if (stat->st_gid != (uint32)-1 && stat->st_gid != 0xFFFFFFFF) {
        inode->gid = stat->st_gid;
        ext4_inode_set_gid(ref.inode, stat->st_gid);
    }

    /* Update size (truncate) */
    if (stat->st_size >= 0 && stat->st_size != inode->size &&
        S_ISREG(inode->mode)) {
        ext4fs_inode_map_cache_invalidate(inode);
        if (inode->i_data.active)
            pcache_teardown(&inode->i_data);
        int tr = ext4_fs_truncate_inode(&ref, (uint64_t)stat->st_size);
        if (tr == EOK) {
            ext4_inode_set_size(ref.inode, (uint64_t)stat->st_size);
            inode->size = stat->st_size;
            inode->n_blocks = ext4_inode_get_blocks_count(&fs->sb, ref.inode);
        }
    }

    /* Update timestamps if provided (non-zero) */
    if (stat->st_atime_sec != 0) {
        inode->atime = stat->st_atime_sec;
        ext4_inode_set_access_time(ref.inode, (uint32_t)stat->st_atime_sec);
    }
    if (stat->st_mtime_sec != 0) {
        inode->mtime = stat->st_mtime_sec;
        ext4_inode_set_modif_time(ref.inode, (uint32_t)stat->st_mtime_sec);
    }

    /* Always update ctime on any attribute change */
    {
        uint32_t now = (uint32_t)goldfish_rtc_read_sec();
        inode->ctime = now;
        ext4_inode_set_change_inode_time(ref.inode, now);
    }

    ref.dirty = true;
    ext4_fs_put_inode_ref(&ref);
    ext4fs_unlock(esb);
    vfs_iunlock(inode);
    return 0;
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
    .move          = ext4fs_move,
    .symlink       = ext4fs_symlink,
    .truncate      = ext4fs_truncate,
    .destroy_inode = ext4fs_destroy_inode,
    .free_inode    = ext4fs_free_inode,
    .dirty_inode   = ext4fs_dirty_inode,
    .sync_inode    = ext4fs_sync_inode,
    .open          = ext4fs_open,
};
