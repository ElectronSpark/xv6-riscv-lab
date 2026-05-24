// VFS File Descriptor Table Management
//
// This module manages per-thread file descriptor tables for the VFS layer.
// Each thread has an fdtable that maps integer file descriptors to vfs_file
// structures.
//
// KEY CONCEPTS:
//   - File descriptor: Integer handle (0 to NOFILE-1) used by userspace
//   - fdtable: Per-thread table mapping fd -> vfs_file pointer
//   - Reference counting: Both fdtable and vfs_file use refcounts
//   - RCU protection: Readers use RCU, writers use spinlock
//
// SYNCHRONIZATION STRATEGY:
//   The fdtable uses a hybrid RCU + spinlock approach:
//
//   READERS (vfs_fdtable_get_file):
//     - Use rcu_read_lock() to protect against concurrent deallocation
//     - Increment file refcount before releasing RCU lock
//     - No spinlock needed - wait-free read path
//
//   WRITERS (alloc_fd, dealloc_fd):
//     - Must hold fdtable->lock spinlock
//     - Use rcu_assign_pointer() for pointer updates
//     - Actual file release deferred via call_rcu() to ensure
//       no readers are still accessing the file
//
//   CLONING (vfs_fdtable_clone):
//     - Uses RCU read lock to safely iterate source fdtable
//     - Duplicates file references to new fdtable
//     - Supports CLONE_FILES flag for shared fdtable
//
// REFERENCE COUNTING:
//   - fdtable->ref_count: Number of threads sharing this fdtable
//   - vfs_file->ref_count: Number of references to the file
//   - vfs_fdtable_alloc_fd() increments file refcount
//   - vfs_fdtable_dealloc_fd() returns file for caller to release via RCU
//
// MEMORY MANAGEMENT:
//   - fdtables allocated from dedicated slab cache
//   - Files released via RCU callback after grace period
//

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include <smp/atomic.h>
#include "param.h"
#include "errno.h"
#include "bits.h"
#include "vfs/stat.h"
#include "vfs/fcntl.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "resource.h"
#include "lock/mutex_types.h"
#include "lock/rwsem.h"
#include "vfs/fs.h"
#include "vfs/file.h"
#include "vfs/file_lock.h"
#include "vfs_private.h"
#include <mm/page.h>

// Check if a pointer value represents a valid file descriptor entry.
// Valid file pointers have addresses > NOFILE (small integers are invalid).
#define IS_FD(fd) ((uint64)(fd) > NOFILE)

static int __vfs_fdtable_alloc_order(void) {
    size_t alloc_size = PAGE_SIZE;
    int order = 0;

    while (alloc_size < sizeof(struct vfs_fdtable)) {
        order++;
        if (order > PAGE_BUDDY_MAX_ORDER) {
            return -E2BIG;
        }
        alloc_size <<= 1;
    }

    return order;
}

static int vfs_file_note_fd_open(struct vfs_file *file)
{
    if (__atomic_fetch_add(&file->visible_fd_refs, 1,
                           __ATOMIC_ACQ_REL) != 0)
        return 0;
    __atomic_store_n(&file->last_fd_close_notified, 0, __ATOMIC_RELEASE);
    return 1;
}

static void vfs_file_maybe_first_fd_open(struct vfs_file *file, int first)
{
    if (first && file->ops != NULL && file->ops->first_fd_open != NULL)
        file->ops->first_fd_open(file);
}

/**
 * __vfs_fdtable_global_init - Validate fdtable allocator constraints
 *
 * Called once during early boot. The fdtable is page-allocated rather than
 * slab-allocated because it can exceed a single page when NOFILE is large.
 */
void __vfs_fdtable_global_init(void) {
    int order = __vfs_fdtable_alloc_order();

        assert(order >= 0,
            "vfs_fdtable too large for page allocator, size=%lu errno=%d",
            (unsigned long)sizeof(struct vfs_fdtable), order);
}

/**
 * __vfs_fdtable_alloc_init - Allocate and initialize a new fdtable
 *
 * Allocates an fdtable from the slab cache and initializes its fields.
 * The fdtable starts with ref_count = 1.
 *
 * Returns: Pointer to new fdtable, or NULL on allocation failure
 */
static struct vfs_fdtable *__vfs_fdtable_alloc_init(void) {
    int order = __vfs_fdtable_alloc_order();
    struct vfs_fdtable *fdtable;

    if (order < 0) {
        return NULL;
    }

    fdtable = page_alloc(order, PAGE_TYPE_ANON);
    if (fdtable == NULL) {
        return NULL;
    }

    memset(fdtable, 0, sizeof(*fdtable));
    spin_init(&fdtable->lock, "vfs_fdtable_lock");
    fdtable->ref_count = 1;
    return fdtable;
}

/**
 * __vfs_fdtable_free - Free an fdtable back to slab cache
 * @fdtable: The fdtable to free
 */
static void __vfs_fdtable_free(struct vfs_fdtable *fdtable) {
    if (fdtable) {
        int order = __vfs_fdtable_alloc_order();

        assert(order >= 0,
               "vfs_fdtable free failed: invalid alloc order errno=%d", order);
        page_free(fdtable, order);
    }
}

/**
 * vfs_fdtable_alloc_fd_from - Allocate a file descriptor starting from a
 * minimum fd
 * @fdtable: The file descriptor table
 * @file: The file to associate with the new fd
 * @start_fd: Lowest fd number to consider (0 to NOFILE-1)
 *
 * Searches for the lowest available file descriptor >= @start_fd and
 * associates it with @file.  Increments @file's reference count via
 * vfs_fdup() and updates the fdtable's bitmap and fd_count.
 *
 * This is the core allocation routine; vfs_fdtable_alloc_fd() is a
 * convenience wrapper that passes @start_fd = 0.
 *
 * LOCKING: Caller MUST hold fdtable->lock
 *
 * Returns: Non-negative fd on success, negative errno on failure
 *   -EINVAL: NULL fdtable or file, or @start_fd out of range
 *   -EMFILE: No free fd available (table full or none >= @start_fd)
 *   -ENOMEM: Failed to increment file reference count
 */
int vfs_fdtable_alloc_fd_from(struct vfs_fdtable *fdtable,
                              struct vfs_file *file, int start_fd) {
    if (fdtable == NULL || file == NULL) {
        return -EINVAL; // Invalid arguments
    }
    if (start_fd < 0 || start_fd >= NOFILE) {
        return -EINVAL; // Invalid starting fd
    }
    assert(spin_holding(&fdtable->lock),
           "vfs_fdtable_alloc_fd_from: fdtable lock not held");

    /* Determine effective limit: min(NOFILE, RLIMIT_NOFILE) */
    int max_fd = NOFILE;
    if (current && current->thread_group) {
        uint64 rl = current->thread_group->rlim[RLIMIT_NOFILE].rlim_cur;
        if (rl < (uint64)max_fd)
            max_fd = (int)rl;
    }

    if (fdtable->fd_count >= max_fd) {
        return -EMFILE; // Too many open files
    }
    int fd = bits_ctz_ptr_from_inv(fdtable->files_bitmap, start_fd, max_fd);
    if (fd < 0 || fd >= max_fd) {
        return -EMFILE; // No free file descriptor
    }
    if (vfs_fdup(file) == NULL) {
        return -ENOMEM; // Failed to duplicate file reference
    }
    int first_visible = vfs_file_note_fd_open(file);

    bits_test_and_set_bit64(&fdtable->files_bitmap[fd >> 6], fd & 63);
    bits_test_and_clear_bit64(&fdtable->cloexec_bitmap[fd >> 6], fd & 63);
    rcu_assign_pointer(fdtable->files[fd], file);
    atomic_inc(&fdtable->fd_count);
    vfs_file_maybe_first_fd_open(file, first_visible);
    return fd;
}

/**
 * vfs_fdtable_alloc_fd - Allocate a file descriptor for a file
 * @fdtable: The file descriptor table
 * @file: The file to associate with the new fd
 *
 * Finds the lowest available file descriptor and associates it with
 * the given file. Increments the file's reference count.
 *
 * LOCKING: Caller MUST hold fdtable->lock
 *
 * Returns: Non-negative fd on success, negative errno on failure
 *   -EINVAL: NULL fdtable or file
 *   -EMFILE: Too many open files (no free fd available)
 *   -ENOMEM: Failed to increment file reference count
 */
int vfs_fdtable_alloc_fd(struct vfs_fdtable *fdtable, struct vfs_file *file) {
    return vfs_fdtable_alloc_fd_from(fdtable, file, 0);
}

/**
 * vfs_fdtable_init - Create a new empty fdtable
 *
 * Allocates and initializes a new file descriptor table with no open files.
 * Used when creating a new thread that doesn't share its parent's fdtable.
 *
 * Returns: Pointer to new fdtable (panics on allocation failure)
 */
struct vfs_fdtable *vfs_fdtable_init(void) {
    struct vfs_fdtable *fdtable = __vfs_fdtable_alloc_init();
    assert(fdtable != NULL, "vfs_fdtable_init: fdtable is NULL\n");
    memset(fdtable->files, 0, sizeof(fdtable->files));
    memset(fdtable->files_bitmap, 0, sizeof(fdtable->files_bitmap));
    memset(fdtable->cloexec_bitmap, 0, sizeof(fdtable->cloexec_bitmap));
    return fdtable;
}

/**
 * vfs_fdtable_clone - Clone or share an fdtable for fork/clone
 * @src: Source fdtable to clone from
 * @clone_flags: Clone flags (CLONE_FILES means share, not copy)
 *
 * If CLONE_FILES is set, increments src's ref_count and returns src.
 * Otherwise, creates a deep copy with duplicated file references.
 *
 * SYNCHRONIZATION: Uses RCU read lock to safely iterate src during copy.
 * This protects against concurrent close() operations.
 *
 * Returns: Pointer to fdtable (shared or new), or ERR_PTR on failure
 */
struct vfs_fdtable *vfs_fdtable_clone(struct vfs_fdtable *src,
                                      int clone_flags) {
    if (src == NULL) {
        return ERR_PTR(-EINVAL); // Invalid arguments
    }

    rcu_read_lock();
    if (clone_flags & CLONE_FILES) {
        // share the fdtable
        atomic_inc(&src->ref_count);
        rcu_read_unlock();
        return src;
    }

    struct vfs_fdtable *dest = __vfs_fdtable_alloc_init();
    if (dest == NULL) {
        rcu_read_unlock();
        return ERR_PTR(-ENOMEM); // Allocation failed
    }
    dest->fd_count = 0;
    memset(dest->files, 0, sizeof(dest->files));
    memset(dest->files_bitmap, 0, sizeof(dest->files_bitmap));
    memset(dest->cloexec_bitmap, 0, sizeof(dest->cloexec_bitmap));

    // Duplicate file references while holding RCU read lock
    // This protects against concurrent close() deallocating files
    for (int i = 0; i < NOFILE; i++) {
        struct vfs_file *src_file = rcu_dereference(src->files[i]);
        if (IS_FD(src_file)) {
            struct vfs_file *dst_file = vfs_fdup(src_file);
            if (!IS_ERR_OR_NULL(dst_file)) {
                int first_visible = vfs_file_note_fd_open(dst_file);
                dest->files[i] = dst_file;
                dest->fd_count++;
                bits_test_and_set_bit64(&dest->files_bitmap[i >> 6], i & 63);
                if (bits_test_bit64(&src->cloexec_bitmap[i >> 6], i & 63)) {
                    bits_test_and_set_bit64(&dest->cloexec_bitmap[i >> 6],
                                            i & 63);
                }
                vfs_file_maybe_first_fd_open(dst_file, first_visible);
            } else {
                dest->files[i] = NULL;
            }
        }
    }
    rcu_read_unlock();
    smp_mb(); // Ensure all writes are visible before returning

    return dest;
}

/**
 * vfs_fdtable_put - Release a reference to an fdtable
 * @fdtable: The fdtable to release
 *
 * Decrements the fdtable's reference count. When the last reference
 * is released, closes all open files and frees the fdtable.
 *
 * Called when a thread exits or when unsharing an fdtable.
 */
void vfs_fdtable_put(struct vfs_fdtable *fdtable) {
    if (fdtable == NULL) {
        return;
    }

    if (atomic_dec_unless(&fdtable->ref_count, 1)) {
        // Still has references
        return;
    }

    // No need to hold the lock here since no other references exist
    // First, close all open files in the range [0, NOFILE)
    for (int i = 0; i < NOFILE; i++) {
        struct vfs_file *file = fdtable->files[i];
        if (IS_FD(file)) {
            if (current != NULL) {
                vfs_file_lock_release(file, current->tgid);
            }
            __atomic_fetch_sub(&file->visible_fd_refs, 1,
                               __ATOMIC_ACQ_REL);
            vfs_file_maybe_last_fd_close(file);
            vfs_fput(file);
            fdtable->files[i] = NULL;
            fdtable->fd_count--;
        }
    }

    assert(fdtable->fd_count >= 0, "vfs_fdtable_destroy: fd_count negative");
    __vfs_fdtable_free(fdtable);
}

/**
 * vfs_fdtable_get_file - Get file from fd with increased refcount
 * @fdtable: The file descriptor table
 * @fd: The file descriptor to look up
 *
 * Looks up the file associated with fd and increments its reference count.
 * The caller must call vfs_fput() when done with the file.
 *
 * SYNCHRONIZATION: Uses RCU read lock for wait-free lookup.
 * Safe to call concurrently with close() on the same fd.
 *
 * Returns: File pointer with incremented refcount, or NULL if fd invalid
 */
struct vfs_file *vfs_fdtable_get_file(struct vfs_fdtable *fdtable, int fd) {
    if (fdtable == NULL || fd < 0 || fd >= NOFILE) {
        return NULL; // Invalid arguments
    }
    rcu_read_lock();
    struct vfs_file *file = rcu_dereference(fdtable->files[fd]);
    if (IS_FD(file)) {
        file = vfs_fdup(file); // Increment reference count
        rcu_read_unlock();
        return file;
    }
    rcu_read_unlock();
    return NULL; // File descriptor not allocated
}

/**
 * vfs_fdtable_dealloc_fd - Remove a file descriptor from the table
 * @fdtable: The file descriptor table
 * @fd: The file descriptor to deallocate
 *
 * Removes the fd from the table and returns the associated file.
 * Does NOT decrement the file's refcount - caller must handle this,
 * typically via call_rcu() to defer the vfs_fput() until after
 * any concurrent RCU readers have completed.
 *
 * LOCKING: Caller MUST hold fdtable->lock
 *
 * Returns: The file that was associated with fd, or NULL if fd invalid
 */
struct vfs_file *vfs_fdtable_dealloc_fd(struct vfs_fdtable *fdtable, int fd) {
    if (fdtable == NULL || fd < 0 || fd >= NOFILE) {
        return NULL; // Invalid arguments
    }
    assert(spin_holding(&fdtable->lock),
           "vfs_fdtable_dealloc_fd: fdtable lock not held");
    struct vfs_file *file = fdtable->files[fd];
    if (!IS_FD(file)) {
        return NULL; // File descriptor not allocated
    }

    rcu_assign_pointer(fdtable->files[fd], NULL);
    bits_test_and_clear_bit64(&fdtable->files_bitmap[fd >> 6], fd & 63);
    bits_test_and_clear_bit64(&fdtable->cloexec_bitmap[fd >> 6], fd & 63);

    atomic_dec(&fdtable->fd_count);
    __atomic_fetch_sub(&file->visible_fd_refs, 1, __ATOMIC_ACQ_REL);
    return file;
}

int vfs_fdtable_get_fdflags(struct vfs_fdtable *fdtable, int fd) {
    if (fdtable == NULL || fd < 0 || fd >= NOFILE) {
        return -EBADF;
    }

    assert(spin_holding(&fdtable->lock),
           "vfs_fdtable_get_fdflags: fdtable lock not held");

    struct vfs_file *file = fdtable->files[fd];
    if (!IS_FD(file)) {
        return -EBADF;
    }

    int flags = 0;
    if (bits_test_bit64(&fdtable->cloexec_bitmap[fd >> 6], fd & 63)) {
        flags |= FD_CLOEXEC;
    }
    return flags;
}

int vfs_fdtable_set_fdflags(struct vfs_fdtable *fdtable, int fd, int flags) {
    if (fdtable == NULL || fd < 0 || fd >= NOFILE) {
        return -EBADF;
    }

    assert(spin_holding(&fdtable->lock),
           "vfs_fdtable_set_fdflags: fdtable lock not held");

    struct vfs_file *file = fdtable->files[fd];
    if (!IS_FD(file)) {
        return -EBADF;
    }

    if (flags & FD_CLOEXEC) {
        bits_test_and_set_bit64(&fdtable->cloexec_bitmap[fd >> 6], fd & 63);
    } else {
        bits_test_and_clear_bit64(&fdtable->cloexec_bitmap[fd >> 6], fd & 63);
    }
    return 0;
}

void vfs_fdtable_close_on_exec(struct vfs_fdtable *fdtable) {
    if (fdtable == NULL) {
        return;
    }

    struct vfs_file *to_close[64];

    for (;;) {
        int close_count = 0;

        spin_lock(&fdtable->lock);
        for (int fd = 0; fd < NOFILE && close_count < NELEM(to_close); fd++) {
            if (!bits_test_and_clear_bit64(&fdtable->cloexec_bitmap[fd >> 6], fd & 63)) {
                continue;
            }

            struct vfs_file *file = vfs_fdtable_dealloc_fd(fdtable, fd);
            if (IS_FD(file)) {
                to_close[close_count++] = file;
            }
        }
        spin_unlock(&fdtable->lock);

        if (close_count == 0) {
            break;
        }

        for (int i = 0; i < close_count; i++) {
            if (current != NULL) {
                vfs_file_lock_release(to_close[i], current->tgid);
            }
            vfs_file_maybe_last_fd_close(to_close[i]);
            vfs_fput(to_close[i]);
        }
    }
}
