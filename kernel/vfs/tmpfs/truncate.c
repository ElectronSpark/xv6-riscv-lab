#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "lock/spinlock.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include <mm/vm.h>
#include <mm/pcache.h>
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include "tmpfs_private.h"

/* Convert block size to 512-byte units for pcache */
#define PCACHE_BLKS_PER_PAGE (PGSIZE / 512)

/**
 * Shrink a tmpfs file to new_size by discarding pcache pages beyond the
 * new boundary.  Embedded files have no pages to free.
 */
static int __tmpfs_truncate_shrink(struct vfs_inode *inode, loff_t new_size) {
    struct tmpfs_inode *tmpfs_inode = container_of(inode, struct tmpfs_inode, vfs_inode);
    if (tmpfs_inode->embedded) {
        /* Embedded data lives inside the inode struct; nothing to free. */
        return 0;
    }

    struct pcache *pc = &inode->i_data;
    if (!pc->active) {
        return 0;
    }

    /* Discard every pcache page whose base offset >= new_size. */
    loff_t first_discard = TMPFS_IBLOCK(new_size + PAGE_SIZE - 1);
    loff_t old_block_cnt = TMPFS_IBLOCK(inode->size + PAGE_SIZE - 1);

    for (loff_t blk = first_discard; blk < old_block_cnt; blk++) {
        uint64 blkno_512 = (uint64)blk * PCACHE_BLKS_PER_PAGE;
        pcache_discard_blk(pc, blkno_512);
    }
    return 0;
}

/**
 * Migrate from embedded storage to pcache-backed storage.
 * Copies up to inode->size bytes of embedded data into the first pcache page.
 */
int __tmpfs_migrate_to_allocated_blocks(struct tmpfs_inode *tmpfs_inode) {
    struct vfs_inode *inode = &tmpfs_inode->vfs_inode;
    loff_t size = inode->size;
    char embedded_copy[TMPFS_INODE_EMBEDDED_DATA_LEN];

    /* Snapshot embedded data before clearing the union. */
    if (size > 0) {
        memcpy(embedded_copy, tmpfs_inode->file.data, size);
    }

    /* Clear the union (embedded data and pcache overlap in the same memory). */
    memset(&tmpfs_inode->file, 0, sizeof(tmpfs_inode->file));
    tmpfs_inode->embedded = false;

    /* Bring up the per-inode pcache. */
    tmpfs_inode_pcache_init(inode);

    struct pcache *pc = &inode->i_data;
    if (!pc->active) {
        return -ENOMEM;
    }

    /* Copy the old embedded data into the first page. */
    if (size > 0) {
        page_t *page = pcache_get_page(pc, 0);
        if (page == NULL) {
            pcache_teardown(pc);
            return -ENOMEM;
        }
        int ret = pcache_read_page(pc, page);
        if (ret != 0) {
            pcache_put_page(pc, page);
            pcache_teardown(pc);
            return ret;
        }
        struct pcache_node *pcn = page->pcache.pcache_node;
        memcpy(pcn->data, embedded_copy, size);
        pcache_mark_page_dirty(pc, page);
        pcache_put_page(pc, page);
    }

    return 0;
}

/**
 * Grow a tmpfs file to new_size.
 * For embedded files that still fit, just zero-fill the gap.
 * For pcache files, nothing to pre-allocate (pages are demand-allocated).
 */
static int __tmpfs_truncate_grow(struct vfs_inode *inode, loff_t new_size) {
    struct tmpfs_inode *tmpfs_inode = container_of(inode, struct tmpfs_inode, vfs_inode);
    int ret = 0;

    if (tmpfs_inode->embedded) {
        if (new_size <= TMPFS_INODE_EMBEDDED_DATA_LEN) {
            /* Still fits in embedded storage — zero the gap. */
            memset(&tmpfs_inode->file.data[inode->size], 0, new_size - inode->size);
            return 0;
        }
        /* Outgrew embedded; migrate to pcache. */
        ret = __tmpfs_migrate_to_allocated_blocks(tmpfs_inode);
        if (ret != 0) {
            return ret;
        }
    }

    /* pcache pages are allocated on demand — nothing more to do. */
    return 0;
}

/**
 * Top-level truncate: grow or shrink a tmpfs regular file.
 */
int __tmpfs_truncate(struct vfs_inode *inode, loff_t new_size) {
    if (new_size > TMPFS_MAX_FILE_SIZE) {
        return -EFBIG;
    }
    int ret = 0;
    loff_t old_size = inode->size;
    if (inode->size < new_size) {
        ret = __tmpfs_truncate_grow(inode, new_size);
        if (ret != 0) {
            /* Grow failed — undo any partial work. */
            __tmpfs_truncate_shrink(inode, old_size);
        }
    } else if (inode->size > new_size) {
        ret = __tmpfs_truncate_shrink(inode, new_size);
    }
    if (ret == 0) {
        inode->size = new_size;
    }
    return ret;
}
