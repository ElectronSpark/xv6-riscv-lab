/*
 * VFS dentry cache
 *
 * Positive lookup cache keyed by (parent sb, parent ino, name).
 * Entries are invalidated lazily by a per-directory generation counter.
 */

#include "types.h"
#include "string.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "vfs/fs.h"
#include "vfs_private.h"
#include "list.h"
#include "hlist.h"
#include <mm/slab.h>
#include <mm/vm.h>
#include "kstats.h"

#define VFS_DCACHE_BUCKETS 4096U

struct vfs_dcache_entry {
    list_node_t link;
    struct vfs_superblock *parent_sb;
    uint64 parent_ino;
    uint64 parent_seq;
    struct vfs_superblock *child_sb;
    uint64 child_ino;
    uint64 hash;
    uint16 name_len;
    uint8 negative;
    char *name;
};

struct vfs_dcache_bucket {
    spinlock_t lock;
    list_node_t head;
};

static slab_cache_t __vfs_dcache_entry_cache;
static int __vfs_dcache_initialized = 0;
static struct vfs_dcache_bucket __vfs_dcache_buckets[VFS_DCACHE_BUCKETS];

/*
 * Global monotonic generation source for vfs_inode.lookup_seq.
 *
 * Every seed (inode init) and every bump draws a fresh, never-reused value so
 * no dcache entry's parent_seq can ever alias a live inode's lookup_seq across
 * incarnations.  This closes two aliasing gaps:
 *   - an inode evicted from the cache and later reloaded is a new incarnation;
 *     under the old per-inode reset to 0 it aliased stale entries cached
 *     against the previous incarnation;
 *   - an sb address returned to its slab and re-handed by an immediate remount
 *     produced a colliding (parent_sb, parent_ino) whose tiny seq could match.
 * First value handed out is 1, so 0 belongs to no live inode (0 was the old
 * poisoned "reset" value).  RELAXED is sufficient: only atomicity/uniqueness of
 * the allocation matters; the RELEASE store into dir->lookup_seq and the
 * ACQUIRE honor-read (below) provide the ordering against publication.
 * A uint64 counter wraps in centuries at any realistic lookup rate.
 */
static uint64 __vfs_dcache_seq_counter = 0;

uint64 __vfs_dcache_alloc_seq(void) {
    return __atomic_add_fetch(&__vfs_dcache_seq_counter, 1, __ATOMIC_RELAXED);
}

static inline uint64 __vfs_dcache_dir_seq(const struct vfs_inode *dir) {
    return __atomic_load_n(&dir->lookup_seq, __ATOMIC_ACQUIRE);
}

void __vfs_dcache_bump_dir_seq(struct vfs_inode *dir) {
    if (dir == NULL || !S_ISDIR(dir->mode)) {
        return;
    }
    __atomic_store_n(&dir->lookup_seq, __vfs_dcache_alloc_seq(),
                     __ATOMIC_RELEASE);
}

static inline uint64 __vfs_dcache_hash(struct vfs_superblock *sb,
                                       uint64 parent_ino, const char *name,
                                       size_t name_len) {
    uint64 hash = hlist_hash_uint64((uint64)sb);
    hash ^= hlist_hash_uint64(parent_ino);
    hash ^= hlist_hash_str((char *)name, name_len);
    if (hash == 0) {
        hash = GOLDEN_RATIO_PRIME;
    }
    return hash;
}

static inline uint32 __vfs_dcache_bucket_index(uint64 hash) {
    return (uint32)(hash & (VFS_DCACHE_BUCKETS - 1));
}

static inline bool __vfs_dcache_entry_matches(struct vfs_dcache_entry *entry,
                                              struct vfs_superblock *sb,
                                              uint64 parent_ino,
                                              const char *name,
                                              size_t name_len,
                                              uint64 hash) {
    return entry->hash == hash && entry->parent_sb == sb &&
           entry->parent_ino == parent_ino && entry->name_len == name_len &&
           entry->name != NULL && memcmp(entry->name, name, name_len) == 0;
}

void __vfs_dcache_global_init(void) {
    if (__vfs_dcache_initialized) {
        return;
    }

    int ret = slab_cache_init(&__vfs_dcache_entry_cache, "vfs_dcache_entry",
                              sizeof(struct vfs_dcache_entry),
                              SLAB_FLAG_STATIC | SLAB_FLAG_DEBUG_BITMAP);
    assert(ret == 0,
           "Failed to initialize vfs_dcache_entry_cache slab cache, errno=%d",
           ret);

    for (int i = 0; i < VFS_DCACHE_BUCKETS; i++) {
        spin_init(&__vfs_dcache_buckets[i].lock, "vfs_dcache_bucket");
        list_entry_init(&__vfs_dcache_buckets[i].head);
    }
    __vfs_dcache_initialized = 1;
}

int __vfs_dcache_lookup(struct vfs_inode *dir, struct vfs_dentry *dentry,
                        const char *name, size_t name_len) {
    /*
     * -EAGAIN means "cache says nothing, resolve via the driver" — used for
     * every fall-through / sentinel case below.  -ENOENT is reserved to mean
     * UNIQUELY "genuine cached negative hit" (see the negative-hit return
     * further down), because vfs_ilookup treats -ENOENT as load-bearing when
     * the neg-dcache gate is on.  Never return -ENOENT for bad-args or the
     * "."/".." sentinels, or "." / ".." resolution would break.
     */
    if (!__vfs_dcache_initialized || dir == NULL || dentry == NULL ||
        name == NULL || name_len == 0 || dir->sb == NULL) {
        return -EAGAIN;
    }
    if (name_len == 1 && name[0] == '.') {
        return -EAGAIN;
    }
    if (name_len == 2 && name[0] == '.' && name[1] == '.') {
        return -EAGAIN;
    }

    uint64 hash = __vfs_dcache_hash(dir->sb, dir->ino, name, name_len);
    uint32 bucket = __vfs_dcache_bucket_index(hash);
    uint64 seq = __vfs_dcache_dir_seq(dir);
    struct vfs_superblock *child_sb = NULL;
    uint64 child_ino = 0;
    bool hit = false;
    bool negative = false;
    struct vfs_dcache_entry *entry;
    struct vfs_dcache_entry *tmp;

    spin_lock(&__vfs_dcache_buckets[bucket].lock);
    list_foreach_node_safe(&__vfs_dcache_buckets[bucket].head, entry, tmp,
                           link) {
        if (!__vfs_dcache_entry_matches(entry, dir->sb, dir->ino, name,
                                        name_len, hash)) {
            continue;
        }
        if (entry->parent_seq != seq) {
            continue;
        }
        if (entry->negative) {
            /*
             * Only honor a negative hit against a still-valid superblock.
             * The parent_seq guard above already covers in-dir mutations;
             * this additionally rejects entries whose mount was torn down
             * (dangling parent_sb), mirroring the positive branch's
             * child_sb->valid check below.
             */
            if (entry->parent_sb == NULL || !entry->parent_sb->valid) {
                continue;
            }
            negative = true;
            hit = true;
            break;
        }
        if (entry->child_sb == NULL || !entry->child_sb->valid) {
            continue;
        }
        child_sb = entry->child_sb;
        child_ino = entry->child_ino;
        hit = true;
        break;
    }
    spin_unlock(&__vfs_dcache_buckets[bucket].lock);

    if (!hit) {
        KSTATS_PROFILE_INC(g_vfs_lookup_cache_misses);
        return -EAGAIN;
    }
    if (negative) {
        KSTATS_PROFILE_INC(g_vfs_lookup_negative_hits);
        /* -ENOENT UNIQUELY means "genuine cached negative hit" — all
         * fall-through/sentinel cases above return -EAGAIN instead. */
        return -ENOENT;
    }

    KSTATS_PROFILE_INC(g_vfs_lookup_dcache_hits);

    dentry->name = strndup(name, name_len);
    if (dentry->name == NULL) {
        return -ENOMEM;
    }
    dentry->name_len = name_len;
    dentry->sb = child_sb;
    dentry->ino = child_ino;
    dentry->parent = dir;
    dentry->cookies = 0;
    return 0;
}

void __vfs_dcache_store(struct vfs_inode *dir, struct vfs_dentry *dentry,
                        const char *name, size_t name_len) {
    if (!__vfs_dcache_initialized || dir == NULL || dentry == NULL ||
        name == NULL || name_len == 0 || dir->sb == NULL ||
        !S_ISDIR(dir->mode)) {
        return;
    }
    if (name_len == 1 && name[0] == '.') {
        return;
    }
    if (name_len == 2 && name[0] == '.' && name[1] == '.') {
        return;
    }

    struct vfs_dcache_entry *entry = slab_alloc(&__vfs_dcache_entry_cache);
    if (entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    list_entry_init(&entry->link);
    entry->name = strndup(name, name_len);
    if (entry->name == NULL) {
        slab_free(entry);
        return;
    }

    entry->parent_sb = dir->sb;
    entry->parent_ino = dir->ino;
    entry->parent_seq = __vfs_dcache_dir_seq(dir);
    /*
     * In the current tree every fs driver leaves dentry->sb unset on lookup,
     * so child_sb always ends up == dir->sb (same superblock as the parent);
     * negatives store child_sb = NULL.  __vfs_dcache_invalidate_sb does NOT
     * rely on this: it matches entries on parent_sb OR child_sb, so a future
     * driver returning a cross-sb dentry here stays UAF-safe at unmount.
     */
    entry->child_sb = dentry->sb ? dentry->sb : dir->sb;
    entry->child_ino = dentry->ino;
    entry->negative = 0;
    entry->hash = __vfs_dcache_hash(dir->sb, dir->ino, name, name_len);
    entry->name_len = (uint16)name_len;

    uint32 bucket = __vfs_dcache_bucket_index(entry->hash);
    struct vfs_dcache_entry *pos;
    struct vfs_dcache_entry *tmp;
    struct vfs_dcache_entry *replaced = NULL;

    spin_lock(&__vfs_dcache_buckets[bucket].lock);
    list_foreach_node_safe(&__vfs_dcache_buckets[bucket].head, pos, tmp,
                           link) {
        if (!__vfs_dcache_entry_matches(pos, dir->sb, dir->ino, name,
                                        name_len, entry->hash)) {
            continue;
        }
        list_entry_del_init_rcu(&pos->link);
        replaced = pos;
        break;
    }
    list_node_add_rcu(&__vfs_dcache_buckets[bucket].head, entry, link);
    spin_unlock(&__vfs_dcache_buckets[bucket].lock);

    if (replaced != NULL) {
        kvfree(replaced->name);
        slab_free(replaced);
    }
}

/*
 * Remove every cached entry that references @sb as parent OR child.
 *
 * Called under the sb write lock immediately before the sb is freed.  Two
 * concurrency cases:
 *  - Stores: excluded entirely.  A store into @sb runs under the sb read lock
 *    (vfs_ilookup, inode.c:681-707), which the caller's exclusive write lock
 *    blocks, so no new entry referencing @sb can appear during or after this
 *    scan.
 *  - Lookups: NOT excluded — __vfs_dcache_lookup runs without any sb lock
 *    (inode.c:664, before the rlock is taken).  It is safe purely via the
 *    bucket spinlock: a concurrent lookup either observes an entry before we
 *    unlink it here — at which point @sb memory is still live, since the
 *    caller frees @sb only after this function returns — or it does not see
 *    the entry at all.
 * After this returns and @sb is freed, no surviving dcache entry references
 * that sb address in either parent_sb or child_sb, so the honor-read guards
 * entry->parent_sb->valid / entry->child_sb->valid can never dereference freed
 * sb memory.
 *
 * Frees entries AFTER dropping each bucket lock (kvfree/slab_free must not run
 * under the bucket spinlock), mirroring __vfs_dcache_store.  The bucket
 * spinlock is the innermost (leaf) lock and is taken here strictly under the sb
 * rwsem, so no lock-order inversion is possible.  The lock is released between
 * buckets to bound the preempt-/irq-off window to one bucket.
 */
void __vfs_dcache_invalidate_sb(struct vfs_superblock *sb) {
    if (!__vfs_dcache_initialized || sb == NULL) {
        return;
    }

    for (uint32 b = 0; b < VFS_DCACHE_BUCKETS; b++) {
        list_node_t dead;
        list_entry_init(&dead);
        struct vfs_dcache_entry *entry;
        struct vfs_dcache_entry *tmp;

        spin_lock(&__vfs_dcache_buckets[b].lock);
        list_foreach_node_safe(&__vfs_dcache_buckets[b].head, entry, tmp,
                               link) {
            if (entry->parent_sb == sb || entry->child_sb == sb) {
                list_entry_del_init_rcu(&entry->link);
                list_node_push(&dead, entry, link);
            }
        }
        spin_unlock(&__vfs_dcache_buckets[b].lock);

        /*
         * list_foreach_node_safe latches the next node before the body runs,
         * so freeing the current entry (and its now-detached link) is safe;
         * no separate delete from @dead is needed since @dead is discarded.
         */
        list_foreach_node_safe(&dead, entry, tmp, link) {
            kvfree(entry->name);
            slab_free(entry);
        }
    }
}

void __vfs_dcache_store_negative(struct vfs_inode *dir, const char *name,
                                 size_t name_len) {
    if (!__vfs_dcache_initialized || dir == NULL || name == NULL ||
        name_len == 0 || dir->sb == NULL || !S_ISDIR(dir->mode)) {
        return;
    }
    /*
     * Synthetic filesystems (procfs/sysfs/devtmpfs) create/expose names
     * dynamically without going through vfs_create -> bump_dir_seq, so a
     * cached negative would never be invalidated and would shadow a
     * later-appearing node (e.g. /proc/<pid>).  Never store negatives for
     * such superblocks (honor site also refuses them, belt-and-suspenders).
     */
    if (dir->sb->no_neg_dcache) {
        return;
    }
    if (name_len == 1 && name[0] == '.') {
        return;
    }
    if (name_len == 2 && name[0] == '.' && name[1] == '.') {
        return;
    }

    struct vfs_dcache_entry *entry = slab_alloc(&__vfs_dcache_entry_cache);
    if (entry == NULL) {
        return;
    }
    memset(entry, 0, sizeof(*entry));
    list_entry_init(&entry->link);
    entry->name = strndup(name, name_len);
    if (entry->name == NULL) {
        slab_free(entry);
        return;
    }

    entry->parent_sb = dir->sb;
    entry->parent_ino = dir->ino;
    entry->parent_seq = __vfs_dcache_dir_seq(dir);
    entry->child_sb = NULL;
    entry->child_ino = 0;
    entry->negative = 1;
    entry->hash = __vfs_dcache_hash(dir->sb, dir->ino, name, name_len);
    entry->name_len = (uint16)name_len;

    uint32 bucket = __vfs_dcache_bucket_index(entry->hash);
    struct vfs_dcache_entry *pos;
    struct vfs_dcache_entry *tmp;
    struct vfs_dcache_entry *replaced = NULL;

    spin_lock(&__vfs_dcache_buckets[bucket].lock);
    list_foreach_node_safe(&__vfs_dcache_buckets[bucket].head, pos, tmp,
                           link) {
        if (!__vfs_dcache_entry_matches(pos, dir->sb, dir->ino, name,
                                        name_len, entry->hash)) {
            continue;
        }
        list_entry_del_init_rcu(&pos->link);
        replaced = pos;
        break;
    }
    list_node_add_rcu(&__vfs_dcache_buckets[bucket].head, entry, link);
    spin_unlock(&__vfs_dcache_buckets[bucket].lock);

    if (replaced != NULL) {
        kvfree(replaced->name);
        slab_free(replaced);
    }
}
