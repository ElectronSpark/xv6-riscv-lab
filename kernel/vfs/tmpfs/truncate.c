#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"
#include "mutex_types.h"
#include "rwlock.h"
#include "completion.h"
#include "vfs/fs.h"
#include "../vfs_private.h"
#include "list.h"
#include "hlist.h"
#include "slab.h"
#include "tmpfs_private.h"

static const int __TMPFS_LEVEL_ITEM_BLOCKS[3] = {
    1,
    TMPFS_INODE_INDRECT_ITEMS,
    TMPFS_INODE_INDRECT_ITEMS * TMPFS_INODE_INDRECT_ITEMS
};

void __tmpfs_truncate_free_blocks(void *blocks, int start_block, int end_block, int level) {
    assert (blocks != NULL, "__tmpfs_truncate_free_blocks: blocks is NULL");
    assert (start_block >= 0 && end_block >= start_block,
            "__tmpfs_truncate_free_blocks: invalid block range");
    assert (level >= 0 && level <= 2,
            "__tmpfs_truncate_free_blocks: invalid level");
    if (level == 0) {
        void **block_array = (void **)blocks;
        for (int i = start_block; i < end_block; i++) {
            if (block_array[i] != NULL) {
                kfree(block_array[i]);
                block_array[i] = NULL;
            }
        }
        return;
    }

    int item_blocks = __TMPFS_LEVEL_ITEM_BLOCKS[level];
    int item_blocks_mask = item_blocks - 1;
    int item_index = 0;
    int local_end = 0;

    // Free partially occupied first item if start_block is not aligned
    if (start_block & item_blocks_mask) {
        item_index = start_block / item_blocks;
        local_end = (start_block + item_blocks) & ~item_blocks_mask;
        if (local_end > end_block) {
            local_end = end_block;
        }
        void **item_ptr = &((void **)blocks)[item_index];
        if (*item_ptr != NULL) {
            __tmpfs_truncate_free_blocks(*item_ptr,
                                          start_block & item_blocks_mask,
                                          local_end - item_index * item_blocks,
                                          level - 1);
        } else {
            // Because the blocks are allocated sequentially, if this item is NULL,
            // the following items must also be NULL
            return;
        }
        start_block = local_end;
    }
    while (start_block < end_block) {
        item_index = start_block / item_blocks;
        // Since we already handled the first partial item, here start_block must be aligned
        local_end = start_block + item_blocks;
        if (local_end > end_block) {
            local_end = end_block;
        }
        void **item_ptr = &((void **)blocks)[item_index];
        if (*item_ptr != NULL) {
            __tmpfs_truncate_free_blocks(*item_ptr,
                                          0,
                                          local_end - start_block,
                                          level - 1);
            kfree(*item_ptr);
            *item_ptr = NULL;
        } else {
            // Because the blocks are allocated sequentially, if this item is NULL,
            // the following items must also be NULL
            return;
        }
        start_block = local_end;
    }
}

void __tmpfs_do_shrink_blocks(struct tmpfs_inode *tmpfs_inode, int block_cnt, int new_block_cnt) {
    int start = 0;
    int end = 0;

    if (TMPFS_INODE_TINDRECT_START < block_cnt) {
        // Free triple indirect blocks
        
        if (new_block_cnt > TMPFS_INODE_TINDRECT_START) {
            start = new_block_cnt - TMPFS_INODE_TINDRECT_START;
            end = block_cnt - TMPFS_INODE_TINDRECT_START;
        } else {
            start = 0;
            end = block_cnt - TMPFS_INODE_TINDRECT_START;
        }
        __tmpfs_truncate_free_blocks(tmpfs_inode->file.triple_indirect, start, end, 2);
        block_cnt = TMPFS_INODE_TINDRECT_START;
    }
    // Free triple indirect pointer if layer is now empty
    if (new_block_cnt <= TMPFS_INODE_TINDRECT_START && tmpfs_inode->file.triple_indirect != NULL) {
        kfree(tmpfs_inode->file.triple_indirect);
        tmpfs_inode->file.triple_indirect = NULL;
    }
    if (new_block_cnt >= block_cnt) {
        return;
    }

    if (TMPFS_INODE_DINDRECT_START < block_cnt) {
        // Free double indirect blocks
        if (new_block_cnt > TMPFS_INODE_DINDRECT_START) {
            start = new_block_cnt - TMPFS_INODE_DINDRECT_START;
            end = block_cnt - TMPFS_INODE_DINDRECT_START;
        } else {
            start = 0;
            end = block_cnt - TMPFS_INODE_DINDRECT_START;
        }
        __tmpfs_truncate_free_blocks(tmpfs_inode->file.double_indirect, start, end, 1);
        block_cnt = TMPFS_INODE_DINDRECT_START;
    }
    // Free double indirect pointer if layer is now empty
    if (new_block_cnt <= TMPFS_INODE_DINDRECT_START && tmpfs_inode->file.double_indirect != NULL) {
        kfree(tmpfs_inode->file.double_indirect);
        tmpfs_inode->file.double_indirect = NULL;
    }
    if (new_block_cnt >= block_cnt) {
        return;
    }

    if (TMPFS_INODE_INDRECT_START < block_cnt) {
        // Free indirect blocks
        if (new_block_cnt > TMPFS_INODE_INDRECT_START) {
            start = new_block_cnt - TMPFS_INODE_INDRECT_START;
            end = block_cnt - TMPFS_INODE_INDRECT_START;
        } else {
            start = 0;
            end = block_cnt - TMPFS_INODE_INDRECT_START;
        }
        __tmpfs_truncate_free_blocks(tmpfs_inode->file.indirect, start, end, 0);
        block_cnt = TMPFS_INODE_INDRECT_START;
    }
    // Free indirect pointer if layer is now empty
    if (new_block_cnt <= TMPFS_INODE_INDRECT_START && tmpfs_inode->file.indirect != NULL) {
        kfree(tmpfs_inode->file.indirect);
        tmpfs_inode->file.indirect = NULL;
    }
    if (new_block_cnt >= block_cnt) {
        return;
    }

    if (new_block_cnt < block_cnt) {
        // Free direct blocks
        __tmpfs_truncate_free_blocks(&tmpfs_inode->file.direct, new_block_cnt, block_cnt, 0);
    }
}

int __tmpfs_truncate_shrink(struct vfs_inode *inode, loff_t new_size) {
    struct tmpfs_inode *tmpfs_inode = container_of(inode, struct tmpfs_inode, vfs_inode);
    if (tmpfs_inode->embedded) {
        // embedded data, nothing to free
        return 0;
    }
    int new_block_cnt = TMPFS_IBLOCK(new_size + PAGE_SIZE - 1);
    int block_cnt = inode->n_blocks;
    __tmpfs_do_shrink_blocks(tmpfs_inode, block_cnt, new_block_cnt);
    inode->n_blocks = new_block_cnt;
    return 0;
}

int __tmpfs_truncate_allocate_blocks(void *blocks, int start_block, int end_block, int level) {
    assert (blocks != NULL, "__tmpfs_truncate_allocate_blocks: blocks is NULL");
    assert (start_block >= 0 && end_block >= start_block,
            "__tmpfs_truncate_allocate_blocks: invalid block range");
    assert (level >= 0 && level <= 2,
            "__tmpfs_truncate_allocate_blocks: invalid level");
    void **block_array = (void **)blocks;
    if (level == 0) {
        for (int i = start_block; i < end_block; i++) {
            assert (block_array[i] == NULL,
                    "__tmpfs_truncate_allocate_blocks: block already allocated");
            void *new_block = kalloc();
            if (new_block == NULL) {
                // Free previously allocated blocks in this call
                for (int j = start_block; j < i; j++) {
                    kfree(block_array[j]);
                    block_array[j] = NULL;
                }
                return -ENOMEM; // Memory allocation failed
            }
            memset(new_block, 0, PAGE_SIZE);
            block_array[i] = new_block;
        }
        return 0;
    }
    
    int item_blocks = __TMPFS_LEVEL_ITEM_BLOCKS[level];
    int item_blocks_mask = item_blocks - 1;
    int item_index = 0;
    int local_end = 0;
    int ret = 0;
    int first_new_item_index = -1;  // Track first item we allocate

    // Allocate partially occupied first item if start_block is not aligned
    if (start_block & item_blocks_mask) {
        item_index = start_block / item_blocks;
        local_end = (start_block + item_blocks) & ~item_blocks_mask;
        if (local_end > end_block) {
            local_end = end_block;
        }
        void **item_ptr = &((void **)blocks)[item_index];
        assert (*item_ptr == NULL, 
                "__tmpfs_truncate_allocate_blocks: item already allocated");
        ret = __tmpfs_truncate_allocate_blocks(*item_ptr,
                                                start_block & item_blocks_mask,
                                                local_end - item_index * item_blocks,
                                                level - 1);
        if (ret != 0) {
            return ret;
        }
        start_block = local_end;
    }
    while (start_block < end_block) {
        item_index = start_block / item_blocks;
        // Since we already handled the first partial item, here start_block must be aligned
        local_end = start_block + item_blocks;
        if (local_end > end_block) {
            local_end = end_block;
        }
        void **item_ptr = &((void **)blocks)[item_index];
        assert (*item_ptr == NULL, 
                "__tmpfs_truncate_allocate_blocks: item already allocated");
        void *new_item = kalloc();
        if (new_item == NULL) {
            // Free all items we allocated in this loop
            if (first_new_item_index >= 0) {
                __tmpfs_truncate_free_blocks(blocks, 
                                             first_new_item_index * item_blocks,
                                             start_block,
                                             level);
            }
            return -ENOMEM; // Memory allocation failed
        }
        memset(new_item, 0, PAGE_SIZE);
        *item_ptr = new_item;
        if (first_new_item_index < 0) {
            first_new_item_index = item_index;
        }
        ret = __tmpfs_truncate_allocate_blocks(*item_ptr,
                                                start_block & item_blocks_mask,
                                                local_end - item_index * item_blocks,
                                                level - 1);
        if (ret != 0) {
            // Recursive call failed and already cleaned up its own allocations.
            // Free the empty item block we just allocated.
            kfree(new_item);
            *item_ptr = NULL;
            // Free all items we previously allocated in this loop
            if (first_new_item_index >= 0 && first_new_item_index < item_index) {
                __tmpfs_truncate_free_blocks(blocks, 
                                             first_new_item_index * item_blocks,
                                             item_index * item_blocks,
                                             level);
            }
            return ret;
        }
        start_block = local_end;
    }

    return 0;
}

int __tmpfs_migrate_to_allocated_blocks(struct tmpfs_inode *tmpfs_inode) {
    void *first_block = kalloc();
    if (first_block == NULL) {
        return -ENOMEM; // Memory allocation failed
    }
    loff_t size = tmpfs_inode->vfs_inode.size;
    memcpy(first_block, tmpfs_inode->file.data, size);
    memset(first_block + size, 0, PAGE_SIZE - size);
    tmpfs_inode->file.direct[0] = first_block;
    tmpfs_inode->embedded = false;
    tmpfs_inode->vfs_inode.n_blocks = 1;
    return 0;
}

// This function will be called when truncating to a larger size while
// no new blocks are allocated (e.g., when growing within the same block)
void __tmpfs_zero_tail(struct vfs_inode *inode, loff_t new_size) {
    int last_block_index = TMPFS_IBLOCK(new_size - 1);
    int start_offset = TMPFS_IBLOCK_OFFSET(new_size);
    int end_offset = TMPFS_IBLOCK_OFFSET(inode->size);
    if (start_offset >= end_offset) {
        // No tail to zero
        return;
    }
    struct tmpfs_inode *tmpfs_inode = container_of(inode, struct tmpfs_inode, vfs_inode);
    void *block_ptr = NULL;
    if (last_block_index > TMPFS_INODE_TINDRECT_START) {
        int tind_index_offset = last_block_index - TMPFS_INODE_TINDRECT_START;
        int tind_index = tind_index_offset / TMPFS_INODE_DINDRECT_ITEMS;
        int dind_index_offset = tind_index_offset % TMPFS_INODE_DINDRECT_ITEMS;
        int dind_index = dind_index_offset / TMPFS_INODE_INDRECT_ITEMS;
        int ind_index = dind_index_offset % TMPFS_INODE_INDRECT_ITEMS;
        block_ptr = &tmpfs_inode->file.triple_indirect[tind_index][dind_index][ind_index];
    } else if (last_block_index > TMPFS_INODE_DINDRECT_START) {
        int dind_index_offset = last_block_index % TMPFS_INODE_DINDRECT_ITEMS;
        int dind_index = dind_index_offset / TMPFS_INODE_INDRECT_ITEMS;
        int ind_index = dind_index_offset % TMPFS_INODE_INDRECT_ITEMS;
        block_ptr = &tmpfs_inode->file.double_indirect[dind_index][ind_index];
    } else if (last_block_index > TMPFS_INODE_INDRECT_START) {
        int ind_index = last_block_index % TMPFS_INODE_INDRECT_ITEMS;
        block_ptr = &tmpfs_inode->file.indirect[ind_index];
    } else {
        block_ptr = tmpfs_inode->file.direct[last_block_index];
    }
    memset(block_ptr + start_offset, 0, end_offset - start_offset);
}

int __tmpfs_truncate_grow(struct vfs_inode *inode, loff_t new_size) {
    struct tmpfs_inode *tmpfs_inode = container_of(inode, struct tmpfs_inode, vfs_inode);
    int ret = 0;

    if (tmpfs_inode->embedded) {
        // Make sure it's no longer embedded if the new size exceeds embedded data length
        if (new_size <= TMPFS_INODE_EMBEDDED_DATA_LEN) {
            // Still fits in embedded data
            memset(&tmpfs_inode->file.data[inode->size], 0, new_size - inode->size);
            return 0;
        }
        // Need to migrate to allocated blocks
        ret = __tmpfs_migrate_to_allocated_blocks(tmpfs_inode);
        if (ret != 0) {
            return ret;
        }
    }
    int old_block_cnt = inode->n_blocks;
    int new_block_cnt = TMPFS_IBLOCK(new_size + PAGE_SIZE - 1);
    if (new_block_cnt == old_block_cnt) {
        // No new blocks needed, zero the tail if file has blocks
        if (inode->n_blocks > 0) {
            __tmpfs_zero_tail(inode, new_size);
        }
        return 0;
    }

    // Allocate direct blocks first
    int this_start_block = old_block_cnt;
    int this_end_block = 0;
    
    if (this_start_block < TMPFS_INODE_INDRECT_START) {
        this_end_block = TMPFS_INODE_INDRECT_START;
        if (new_block_cnt < this_end_block) {
            this_end_block = new_block_cnt;
        }
        ret = __tmpfs_truncate_allocate_blocks(tmpfs_inode->file.direct, this_start_block, this_end_block, 0);
        if (ret != 0) {
            return ret;
        }
        inode->n_blocks = this_end_block;
        this_start_block = this_end_block;
        if (new_block_cnt == this_start_block) {
            return 0;
        }
    }
    if (new_block_cnt > TMPFS_INODE_INDRECT_START && this_start_block < TMPFS_INODE_DINDRECT_START) {
       this_end_block = TMPFS_INODE_DINDRECT_START;
        if (new_block_cnt < this_end_block) {
            this_end_block = new_block_cnt;
        }
        if (tmpfs_inode->file.indirect == NULL) {
            tmpfs_inode->file.indirect = kalloc();
            if (tmpfs_inode->file.indirect == NULL) {
                return -ENOMEM;
            }
            memset(tmpfs_inode->file.indirect, 0, PAGE_SIZE);
        }
        ret = __tmpfs_truncate_allocate_blocks(tmpfs_inode->file.indirect, this_start_block - TMPFS_INODE_INDRECT_START, this_end_block - TMPFS_INODE_INDRECT_START, 0);
        if (ret != 0) {
            return ret;
        }
        inode->n_blocks = this_end_block;
        this_start_block = this_end_block;
        if (new_block_cnt == this_start_block) {
            return 0;
        }
    }
    if (new_block_cnt > TMPFS_INODE_DINDRECT_START && this_start_block < TMPFS_INODE_TINDRECT_START) {
       this_end_block = TMPFS_INODE_TINDRECT_START;
        if (new_block_cnt < this_end_block) {
            this_end_block = new_block_cnt;
        }
        if (tmpfs_inode->file.double_indirect == NULL) {
            tmpfs_inode->file.double_indirect = kalloc();
            if (tmpfs_inode->file.double_indirect == NULL) {
                return -ENOMEM;
            }
            memset(tmpfs_inode->file.double_indirect, 0, PAGE_SIZE);
        }
        ret = __tmpfs_truncate_allocate_blocks(tmpfs_inode->file.double_indirect, this_start_block - TMPFS_INODE_DINDRECT_START, this_end_block - TMPFS_INODE_DINDRECT_START, 1);
        if (ret != 0) {
            return ret;
        }
        inode->n_blocks = this_end_block;
        this_start_block = this_end_block;
        if (new_block_cnt == this_start_block) {
            return 0;
        }
    }
    if (new_block_cnt > TMPFS_INODE_TINDRECT_START) {
        if (tmpfs_inode->file.triple_indirect == NULL) {
            tmpfs_inode->file.triple_indirect = kalloc();
            if (tmpfs_inode->file.triple_indirect == NULL) {
                return -ENOMEM;
            }
            memset(tmpfs_inode->file.triple_indirect, 0, PAGE_SIZE);
        }
        ret = __tmpfs_truncate_allocate_blocks(tmpfs_inode->file.triple_indirect, this_start_block - TMPFS_INODE_TINDRECT_START, new_block_cnt - TMPFS_INODE_TINDRECT_START, 2);
        if (ret != 0) {
            return ret;
        }
        inode->n_blocks = new_block_cnt;
    }
    return 0;
}

int __tmpfs_truncate(struct vfs_inode *inode, loff_t new_size) {
    if (new_size > TMPFS_MAX_FILE_SIZE) {
        return -EFBIG; // File too large
    }
    int ret = 0;
    loff_t old_size = inode->size;
    if (inode->size < new_size) {
        ret = __tmpfs_truncate_grow(inode, new_size);
        if (ret != 0) {
            // Grow failed, use shrink logic to clean up any partial allocations
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
