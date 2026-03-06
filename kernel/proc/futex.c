/*
 * futex.c — Fast Userspace Mutex support
 *
 * Provides a minimal futex implementation sufficient for musl libc's
 * threading library (pthread_mutex_lock, pthread_cond_wait, thread joining).
 *
 * Supported operations:
 *   FUTEX_WAIT  — atomically check *uaddr == val and sleep
 *   FUTEX_WAKE  — wake up to val waiters on uaddr
 *
 * The FUTEX_PRIVATE_FLAG is accepted but treated the same as the non-private
 * variant since xv6 does not support cross-process shared memory mappings.
 */

#include "types.h"
#include "param.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "defs.h"
#include "printf.h"
#include <mm/vm.h>
#include "errno.h"
#include "signal.h"

// Futex operations (match Linux values)
#define FUTEX_WAIT          0
#define FUTEX_WAKE          1
#define FUTEX_FD            2
#define FUTEX_REQUEUE       3
#define FUTEX_CMP_REQUEUE   4
#define FUTEX_WAKE_OP       5
#define FUTEX_LOCK_PI       6
#define FUTEX_UNLOCK_PI     7
#define FUTEX_TRYLOCK_PI    8
#define FUTEX_WAIT_BITSET   9
#define FUTEX_WAKE_BITSET   10

#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256
#define FUTEX_CMD_MASK       ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME)

#define FUTEX_BITSET_MATCH_ANY 0xffffffff

/*
 * Futex hash table: waiters are hashed by (vm, user-virtual-address).
 * Each bucket is a linked list of sleeping threads protected by a spinlock.
 */
#define FUTEX_HASH_BITS  6
#define FUTEX_HASH_SIZE  (1 << FUTEX_HASH_BITS)

struct futex_waiter {
    struct thread *thread;
    vm_t *vm;
    uint64 uaddr;
    uint32 bitset;
    struct futex_waiter *next;
};

struct futex_bucket {
    spinlock_t lock;
    struct futex_waiter *head;
};

static struct futex_bucket futex_table[FUTEX_HASH_SIZE];

static uint64 futex_hash(vm_t *vm, uint64 uaddr) {
    uint64 key = (uint64)vm ^ (uaddr >> 2);
    // Simple multiplicative hash
    key = key * 0x9e3779b97f4a7c15ULL;
    return (key >> (64 - FUTEX_HASH_BITS)) & (FUTEX_HASH_SIZE - 1);
}

void futex_init(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        spin_init(&futex_table[i].lock, "futex");
        futex_table[i].head = NULL;
    }
}

/*
 * futex_wait — sleep if *uaddr == val
 *
 * The caller holds no locks. We:
 *   1. Hash to find the bucket
 *   2. Lock the bucket
 *   3. Read *uaddr from user space (under bucket lock to avoid lost wakeups)
 *   4. If *uaddr != val, return -EAGAIN
 *   5. Otherwise, enqueue ourselves and sleep
 */
static int futex_wait(uint64 uaddr, uint32 val, uint32 bitset) {
    if (bitset == 0)
        return -EINVAL;

    vm_t *vm = current->vm;
    uint64 idx = futex_hash(vm, uaddr);
    struct futex_bucket *bucket = &futex_table[idx];

    struct futex_waiter waiter;
    waiter.thread = current;
    waiter.vm = vm;
    waiter.uaddr = uaddr;
    waiter.bitset = bitset;
    waiter.next = NULL;

    spin_lock(&bucket->lock);

    // Atomically read the futex word from user space
    uint32 curval;
    if (vm_copyin(vm, (char *)&curval, uaddr, sizeof(curval)) < 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }

    if (curval != val) {
        spin_unlock(&bucket->lock);
        return -EAGAIN;
    }

    // Enqueue waiter
    waiter.next = bucket->head;
    bucket->head = &waiter;

    // Sleep — releases bucket->lock and puts thread to sleep.
    // On wakeup, we need to re-acquire bucket->lock briefly to
    // ensure we're properly dequeued.
    scheduler_sleep(&bucket->lock, THREAD_INTERRUPTIBLE);

    // We've been woken up. The waker may have already removed us from the
    // list. Ensure we're not still linked (in case of spurious wakeup).
    spin_lock(&bucket->lock);
    // Remove ourselves from the list if still present (spurious wakeup / signal)
    struct futex_waiter **pp = &bucket->head;
    while (*pp) {
        if (*pp == &waiter) {
            *pp = waiter.next;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&bucket->lock);

    // Check if we were woken by a signal
    if (signal_pending(current))
        return -EINTR;

    return 0;
}

/*
 * futex_wake — wake up to 'val' waiters sleeping on uaddr
 *
 * Returns the number of waiters actually woken.
 */
static int futex_wake(uint64 uaddr, int val, uint32 bitset) {
    if (bitset == 0)
        return -EINVAL;

    vm_t *vm = current->vm;
    uint64 idx = futex_hash(vm, uaddr);
    struct futex_bucket *bucket = &futex_table[idx];

    int woken = 0;

    spin_lock(&bucket->lock);

    struct futex_waiter **pp = &bucket->head;
    while (*pp && woken < val) {
        struct futex_waiter *w = *pp;
        if (w->vm == vm && w->uaddr == uaddr && (w->bitset & bitset)) {
            // Remove from list
            *pp = w->next;
            w->next = NULL;
            // Wake the thread
            scheduler_wakeup_interruptible(w->thread);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }

    spin_unlock(&bucket->lock);
    return woken;
}

/*
 * futex_wake_addr — wake waiters on a specific address.
 * Called from kernel code (e.g., exit.c for CLONE_CHILD_CLEARTID).
 */
int futex_wake_addr(vm_t *vm, uint64 uaddr, int val) {
    uint64 idx = futex_hash(vm, uaddr);
    struct futex_bucket *bucket = &futex_table[idx];
    int woken = 0;

    spin_lock(&bucket->lock);

    struct futex_waiter **pp = &bucket->head;
    while (*pp && woken < val) {
        struct futex_waiter *w = *pp;
        if (w->vm == vm && w->uaddr == uaddr) {
            *pp = w->next;
            w->next = NULL;
            scheduler_wakeup_interruptible(w->thread);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }

    spin_unlock(&bucket->lock);
    return woken;
}

/*
 * futex_requeue — wake up to 'nr_wake' waiters on uaddr1, then move
 * up to 'nr_requeue' remaining waiters from uaddr1's queue to uaddr2's queue.
 *
 * For FUTEX_CMP_REQUEUE, the caller passes cmpval and we verify *uaddr1 ==
 * cmpval before proceeding. For plain FUTEX_REQUEUE, cmpval is ignored
 * (pass -1 or any value with do_cmp=false).
 *
 * Returns total number of woken waiters on success, or negative errno.
 */
static int futex_requeue(uint64 uaddr1, int nr_wake, uint64 uaddr2,
                         int nr_requeue, uint32 cmpval, int do_cmp) {
    vm_t *vm = current->vm;
    uint64 idx1 = futex_hash(vm, uaddr1);
    uint64 idx2 = futex_hash(vm, uaddr2);
    struct futex_bucket *b1 = &futex_table[idx1];
    struct futex_bucket *b2 = &futex_table[idx2];

    /* Lock both buckets in address order to avoid deadlock */
    if (idx1 < idx2) {
        spin_lock(&b1->lock);
        spin_lock(&b2->lock);
    } else if (idx1 > idx2) {
        spin_lock(&b2->lock);
        spin_lock(&b1->lock);
    } else {
        /* Same bucket */
        spin_lock(&b1->lock);
    }

    /* Optionally compare *uaddr1 with cmpval */
    if (do_cmp) {
        uint32 curval;
        if (vm_copyin(vm, (char *)&curval, uaddr1, sizeof(curval)) < 0) {
            if (idx1 != idx2) spin_unlock(&b2->lock);
            spin_unlock(&b1->lock);
            return -EFAULT;
        }
        if (curval != cmpval) {
            if (idx1 != idx2) spin_unlock(&b2->lock);
            spin_unlock(&b1->lock);
            return -EAGAIN;
        }
    }

    int woken = 0;
    int requeued = 0;

    /* Phase 1: Wake up to nr_wake waiters */
    struct futex_waiter **pp = &b1->head;
    while (*pp && woken < nr_wake) {
        struct futex_waiter *w = *pp;
        if (w->vm == vm && w->uaddr == uaddr1) {
            *pp = w->next;
            w->next = NULL;
            scheduler_wakeup_interruptible(w->thread);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }

    /* Phase 2: Requeue up to nr_requeue waiters from uaddr1 to uaddr2 */
    pp = &b1->head;
    while (*pp && requeued < nr_requeue) {
        struct futex_waiter *w = *pp;
        if (w->vm == vm && w->uaddr == uaddr1) {
            /* Remove from b1 */
            *pp = w->next;
            /* Update the waiter's address to uaddr2 */
            w->uaddr = uaddr2;
            /* Add to b2's list */
            w->next = b2->head;
            b2->head = w;
            requeued++;
        } else {
            pp = &(*pp)->next;
        }
    }

    if (idx1 != idx2)
        spin_unlock(&b2->lock);
    spin_unlock(&b1->lock);

    return woken;
}

/*
 * sys_futex — system call entry point
 *
 * Arguments:
 *   a0: uint64  uaddr   — user-space futex word address
 *   a1: int     futex_op — operation (FUTEX_WAIT, FUTEX_WAKE, etc.)
 *   a2: uint32  val     — expected value (WAIT) or count (WAKE)
 *   a3: uint64  timeout — pointer to timespec (unused, ignored for now)
 *   a4: uint64  uaddr2  — second futex address (unused)
 *   a5: uint32  val3    — for FUTEX_CMP_REQUEUE, or bitset
 */
uint64 sys_futex(void) {
    uint64 uaddr;
    int futex_op;
    uint32 val;
    uint64 timeout_or_val2; /* a3: timeout for WAIT, nr_requeue for REQUEUE */
    uint64 uaddr2;          /* a4 */
    uint32 val3;             /* a5 */

    argaddr(0, &uaddr);
    argint(1, &futex_op);
    argint(2, (int *)&val);
    argaddr(3, &timeout_or_val2);
    argaddr(4, &uaddr2);
    argint(5, (int *)&val3);

    // Alignment check: futex word must be 4-byte aligned
    if (uaddr & 3)
        return (uint64)-EINVAL;

    int cmd = futex_op & FUTEX_CMD_MASK;

    switch (cmd) {
    case FUTEX_WAIT:
        return (uint64)futex_wait(uaddr, val, FUTEX_BITSET_MATCH_ANY);

    case FUTEX_WAKE:
        return (uint64)futex_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY);

    case FUTEX_WAIT_BITSET:
        return (uint64)futex_wait(uaddr, val, val3);

    case FUTEX_WAKE_BITSET:
        return (uint64)futex_wake(uaddr, val, val3);

    case FUTEX_REQUEUE:
        if (uaddr2 & 3)
            return (uint64)-EINVAL;
        /* val=nr_wake, timeout_or_val2=nr_requeue, uaddr2=target addr */
        return (uint64)futex_requeue(uaddr, (int)val, uaddr2,
                                     (int)timeout_or_val2, 0, 0);

    case FUTEX_CMP_REQUEUE:
        if (uaddr2 & 3)
            return (uint64)-EINVAL;
        /* val=nr_wake, timeout_or_val2=nr_requeue, uaddr2=target, val3=cmpval */
        return (uint64)futex_requeue(uaddr, (int)val, uaddr2,
                                     (int)timeout_or_val2, val3, 1);

    default:
        return (uint64)-ENOSYS;
    }
}
