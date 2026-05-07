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
#include <mm/pgtable.h>
#include "errno.h"
#include "signal.h"
#include "timer/goldfish_rtc.h"
#include "timer/timer.h"

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

#define FUTEX_WAITERS          0x80000000U
#define FUTEX_OWNER_DIED       0x40000000U
#define FUTEX_TID_MASK         0x3fffffffU
#define ROBUST_LIST_LIMIT      2048

#define FUTEX_OP_SET         0
#define FUTEX_OP_ADD         1
#define FUTEX_OP_OR          2
#define FUTEX_OP_ANDN        3
#define FUTEX_OP_XOR         4
#define FUTEX_OP_OPARG_SHIFT 8

#define FUTEX_OP_CMP_EQ      0
#define FUTEX_OP_CMP_NE      1
#define FUTEX_OP_CMP_LT      2
#define FUTEX_OP_CMP_LE      3
#define FUTEX_OP_CMP_GT      4
#define FUTEX_OP_CMP_GE      5

#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000ULL

struct futex_timespec {
    int64 tv_sec;
    int64 tv_nsec;
};

/*
 * Futex hash table: waiters are hashed by (vm, user-virtual-address).
 * Each bucket is a linked list of sleeping threads protected by a spinlock.
 */
#define FUTEX_HASH_BITS  6
#define FUTEX_HASH_SIZE  (1 << FUTEX_HASH_BITS)

struct futex_waiter {
    struct thread *thread;
    vm_t *key_vm;
    uint64 key_addr;
    uint32 bitset;
    struct futex_waiter *next;
};

struct futex_bucket {
    spinlock_t lock;
    struct futex_waiter *head;
};

static struct futex_bucket futex_table[FUTEX_HASH_SIZE];

struct futex_key {
    vm_t *vm;
    uint64 addr;
};

struct robust_list_user {
    uint64 next;
};

struct robust_list_head_user {
    struct robust_list_user list;
    int64 futex_offset;
    uint64 list_op_pending;
};

static uint64 futex_hash(const struct futex_key *key) {
    uint64 h = (uint64)key->vm ^ (key->addr >> 2);
    // Simple multiplicative hash
    h = h * 0x9e3779b97f4a7c15ULL;
    return (h >> (64 - FUTEX_HASH_BITS)) & (FUTEX_HASH_SIZE - 1);
}

void futex_init(void) {
    for (int i = 0; i < FUTEX_HASH_SIZE; i++) {
        spin_init(&futex_table[i].lock, "futex");
        futex_table[i].head = NULL;
    }
}

/*
 * futex_read_u32 — lockless read of a 32-bit word from user space.
 *
 * Uses walkaddr() to translate the virtual address to physical without
 * acquiring the VM rwsem, making it safe to call while holding a spinlock.
 * Returns 0 on success, -EFAULT if the page is not mapped.
 */
static int futex_read_u32(vm_t *vm, uint64 uaddr, uint32 *val)
{
    uint64 pa = walkaddr(vm->pagetable, uaddr);
    if (pa == 0)
        return -EFAULT;
    uint64 offset = uaddr - PGROUNDDOWN(uaddr);
    /* Ensure the 4-byte read doesn't cross a page boundary */
    if (offset + sizeof(uint32) > PGSIZE)
        return -EFAULT;
    *val = *(volatile uint32 *)((uint64)PA2VA(pa) + offset);
    return 0;
}

static int futex_write_u32(vm_t *vm, uint64 uaddr, uint32 val)
{
    uint64 pa = walkaddr(vm->pagetable, uaddr);
    if (pa == 0)
        return -EFAULT;
    uint64 offset = uaddr - PGROUNDDOWN(uaddr);
    if (offset + sizeof(uint32) > PGSIZE)
        return -EFAULT;
    *(volatile uint32 *)((uint64)PA2VA(pa) + offset) = val;
    return 0;
}

static int futex_cmpxchg_u32(vm_t *vm, uint64 uaddr, uint32 *old, uint32 newval)
{
    uint64 pa = walkaddr(vm->pagetable, uaddr);
    if (pa == 0)
        return -EFAULT;
    uint64 offset = uaddr - PGROUNDDOWN(uaddr);
    if (offset + sizeof(uint32) > PGSIZE)
        return -EFAULT;

    volatile uint32 *ptr = (volatile uint32 *)((uint64)PA2VA(pa) + offset);
    uint32 expected = *old;
    if (__atomic_compare_exchange_n(ptr, &expected, newval, false,
                                    __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
        return 0;
    *old = expected;
    return -EAGAIN;
}

/*
 * Build the key Linux exposes for futex matching.
 *
 * FUTEX_PRIVATE_FLAG waiters are process-private, so the key is the owning
 * address space plus the userspace word address.  Shared futexes must match
 * across processes that map the same physical page (for example WebKit's
 * shared-memory synchronization), so key them by physical word address.
 */
static int futex_make_key(vm_t *vm, uint64 uaddr, bool private,
                          struct futex_key *key)
{
    if (private) {
        key->vm = vm;
        key->addr = uaddr;
        return 0;
    }

    uint64 pa = walkaddr(vm->pagetable, uaddr);
    if (pa == 0)
        return -EFAULT;

    key->vm = NULL;
    key->addr = pa + (uaddr - PGROUNDDOWN(uaddr));
    return 0;
}

static int futex_keys_equal(const struct futex_waiter *w,
                            const struct futex_key *key)
{
    return w->key_vm == key->vm && w->key_addr == key->addr;
}

static int futex_wake_locked(struct futex_bucket *bucket,
                             const struct futex_key *key,
                             int val, uint32 bitset);

static uint64 futex_ns_to_ms_ceil(uint64 ns)
{
    if (ns == 0)
        return 0;
    return (ns + NSEC_PER_MSEC - 1) / NSEC_PER_MSEC;
}

static int futex_timespec_to_ns(const struct futex_timespec *ts, uint64 *ns)
{
    if (ts->tv_sec < 0 || ts->tv_nsec < 0 ||
        ts->tv_nsec >= (int64)NSEC_PER_SEC)
        return -EINVAL;

    uint64 sec = (uint64)ts->tv_sec;
    if (sec > UINT64_MAX / NSEC_PER_SEC)
        return -EINVAL;
    uint64 base = sec * NSEC_PER_SEC;
    uint64 nsec = (uint64)ts->tv_nsec;
    if (base > UINT64_MAX - nsec)
        return -EINVAL;
    *ns = base + nsec;
    return 0;
}

static uint64 futex_now_ns(bool realtime)
{
    if (realtime)
        return goldfish_rtc_read_ns();

    uint64 freq = __timebase_frequency;
    if (freq == 0)
        return sched_timer_now_ms() * NSEC_PER_MSEC;

    uint64 ticks = r_time();
    return (ticks / freq) * NSEC_PER_SEC +
        ((ticks % freq) * NSEC_PER_SEC) / freq;
}

static int futex_parse_timeout(uint64 timeout_addr, bool absolute,
                               bool realtime, bool *has_timeout,
                               uint64 *timeout_ms)
{
    *has_timeout = false;
    *timeout_ms = 0;
    if (timeout_addr == 0)
        return 0;

    struct futex_timespec ts;
    if (either_copyin(&ts, 1, timeout_addr, sizeof(ts)) < 0)
        return -EFAULT;

    uint64 timeout_ns;
    int ret = futex_timespec_to_ns(&ts, &timeout_ns);
    if (ret < 0)
        return ret;

    *has_timeout = true;
    if (!absolute) {
        *timeout_ms = futex_ns_to_ms_ceil(timeout_ns);
        return 0;
    }

    uint64 now_ns = futex_now_ns(realtime);
    if (timeout_ns <= now_ns) {
        *timeout_ms = 0;
        return 0;
    }
    *timeout_ms = futex_ns_to_ms_ceil(timeout_ns - now_ns);
    return 0;
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
static int futex_wait(uint64 uaddr, uint32 val, uint32 bitset, bool private,
                      bool has_timeout, uint64 timeout_ms) {
    if (bitset == 0)
        return -EINVAL;

    vm_t *vm = current->vm;
    struct futex_key key;
    int key_ret = futex_make_key(vm, uaddr, private, &key);
    if (key_ret < 0)
        return key_ret;

    uint64 idx = futex_hash(&key);
    struct futex_bucket *bucket = &futex_table[idx];

    struct futex_waiter waiter;
    waiter.thread = current;
    waiter.key_vm = key.vm;
    waiter.key_addr = key.addr;
    waiter.bitset = bitset;
    waiter.next = NULL;

    spin_lock(&bucket->lock);

    // Read the futex word locklessly (walkaddr does not acquire VM rwsem)
    uint32 curval;
    if (futex_read_u32(vm, uaddr, &curval) < 0) {
        spin_unlock(&bucket->lock);
        return -EFAULT;
    }

    if (curval != val) {
        spin_unlock(&bucket->lock);
        return -EAGAIN;
    }

    if (has_timeout && timeout_ms == 0) {
        spin_unlock(&bucket->lock);
        return -ETIMEDOUT;
    }

    // Enqueue waiter
    waiter.next = bucket->head;
    bucket->head = &waiter;

    struct timer_node tn = {0};
    uint64 deadline_ms = 0;
    bool timer_armed = false;
    bool timeout_arm_failed = false;

    if (!has_timeout) {
        scheduler_sleep(&bucket->lock, THREAD_INTERRUPTIBLE);
    } else {
        /*
         * Set the sleep state before arming the timer and before releasing the
         * futex bucket lock. This mirrors sleep_ms() and avoids the
         * lost-timeout race where a very short timer fires while the caller is
         * still running.
         */
        int intr = intr_off_save();
        __thread_state_set(current, THREAD_INTERRUPTIBLE);
        deadline_ms = sched_timer_now_ms() + timeout_ms;
        if (sched_timer_set(&tn, timeout_ms) == 0) {
            timer_armed = true;
            spin_unlock(&bucket->lock);
            scheduler_yield();
            spin_lock(&bucket->lock);
        } else {
            /*
             * Do not yield in an interruptible state if no timer was armed:
             * no callback exists to wake us.  Treat the wait as an immediate
             * timeout after cleaning the futex queue below.
             */
            __thread_state_set(current, THREAD_RUNNING);
            timeout_arm_failed = true;
        }
        intr_restore(intr);
    }

    // We've been woken up. bucket->lock is already held (re-acquired by
    // us above). Remove ourselves if still linked (timeout, spurious wakeup,
    // or signal).
    // Remove ourselves from the list if still present (spurious wakeup / signal)
    bool still_linked = false;
    struct futex_waiter **pp = &bucket->head;
    while (*pp) {
        if (*pp == &waiter) {
            *pp = waiter.next;
            still_linked = true;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&bucket->lock);

    if (timer_armed)
        sched_timer_done(&tn);

    // Check if we were woken by a signal
    if (signal_pending(current))
        return -EINTR;

    if (timeout_arm_failed)
        return -ETIMEDOUT;

    if (has_timeout && still_linked &&
        sched_timer_now_ms() >= deadline_ms)
        return -ETIMEDOUT;

    return 0;
}

/*
 * futex_wake — wake up to 'val' waiters sleeping on uaddr
 *
 * Returns the number of waiters actually woken.
 */
static int futex_wake(uint64 uaddr, int val, uint32 bitset, bool private) {
    if (bitset == 0)
        return -EINVAL;

    vm_t *vm = current->vm;
    struct futex_key key;
    int key_ret = futex_make_key(vm, uaddr, private, &key);
    if (key_ret < 0)
        return key_ret;

    uint64 idx = futex_hash(&key);
    struct futex_bucket *bucket = &futex_table[idx];

    int woken = 0;

    spin_lock(&bucket->lock);

    struct futex_waiter **pp = &bucket->head;
    while (*pp && woken < val) {
        struct futex_waiter *w = *pp;
        if (futex_keys_equal(w, &key) && (w->bitset & bitset)) {
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
    struct futex_key private_key = {
        .vm = vm,
        .addr = uaddr,
    };
    uint64 idx = futex_hash(&private_key);
    struct futex_bucket *bucket = &futex_table[idx];

    spin_lock(&bucket->lock);
    int woken = futex_wake_locked(bucket, &private_key, val,
                                  FUTEX_BITSET_MATCH_ANY);
    spin_unlock(&bucket->lock);

    /*
     * CLONE_CHILD_CLEARTID is a kernel-originated wake and does not carry the
     * userspace futex operation flags.  musl/WebKit may wait on the clear-tid
     * word without FUTEX_PRIVATE_FLAG, which keys the waiter by the physical
     * word address.  Wake that shared key as well so pthread_join-style waits
     * do not miss the thread-exit notification.
     */
    if (woken < val) {
        struct futex_key shared_key;
        if (futex_make_key(vm, uaddr, false, &shared_key) == 0 &&
            !((shared_key.vm == private_key.vm) &&
              (shared_key.addr == private_key.addr))) {
            idx = futex_hash(&shared_key);
            bucket = &futex_table[idx];
            spin_lock(&bucket->lock);
            woken += futex_wake_locked(bucket, &shared_key, val - woken,
                                       FUTEX_BITSET_MATCH_ANY);
            spin_unlock(&bucket->lock);
        }
    }
    return woken;
}

static void futex_handle_robust_entry(struct thread *p, uint64 entry,
                                      int64 futex_offset)
{
    uint64 uaddr = entry + (uint64)futex_offset;

    for (;;) {
        uint32 oldval;
        if (futex_read_u32(p->vm, uaddr, &oldval) < 0)
            return;

        if ((oldval & FUTEX_TID_MASK) != (uint32)p->pid)
            return;

        uint32 newval = (oldval & FUTEX_WAITERS) | FUTEX_OWNER_DIED;
        uint32 expected = oldval;
        int ret = futex_cmpxchg_u32(p->vm, uaddr, &expected, newval);
        if (ret == -EAGAIN)
            continue;
        if (ret < 0)
            return;

        if (oldval & FUTEX_WAITERS)
            futex_wake_addr(p->vm, uaddr, 1);
        return;
    }
}

void futex_exit_robust_list(struct thread *p)
{
    if (p == NULL || p->vm == NULL || p->robust_list_head == 0)
        return;

    struct robust_list_head_user head;
    if (vm_copyin(p->vm, &head, p->robust_list_head, sizeof(head)) < 0)
        return;

    uint64 list_head = p->robust_list_head;
    uint64 entry = head.list.next;
    uint64 pending = head.list_op_pending;
    uint64 handled_pending = 0;

    for (int i = 0; i < ROBUST_LIST_LIMIT; i++) {
        if (entry == 0 || entry == list_head)
            break;

        if (entry == pending)
            handled_pending = 1;

        struct robust_list_user node;
        if (vm_copyin(p->vm, &node, entry, sizeof(node)) < 0)
            break;

        futex_handle_robust_entry(p, entry, head.futex_offset);
        entry = node.next;
    }

    if (pending != 0 && !handled_pending)
        futex_handle_robust_entry(p, pending, head.futex_offset);

    p->robust_list_head = 0;
    p->robust_list_len = 0;
}

static int futex_wake_locked(struct futex_bucket *bucket,
                             const struct futex_key *key,
                             int val, uint32 bitset)
{
    int woken = 0;
    struct futex_waiter **pp = &bucket->head;

    while (*pp && woken < val) {
        struct futex_waiter *w = *pp;
        if (futex_keys_equal(w, key) && (w->bitset & bitset)) {
            *pp = w->next;
            w->next = NULL;
            scheduler_wakeup_interruptible(w->thread);
            woken++;
        } else {
            pp = &(*pp)->next;
        }
    }
    return woken;
}

static int futex_op_sign_extend12(uint32 value)
{
    value &= 0xfff;
    if (value & 0x800)
        value |= 0xfffff000;
    return (int)(int32)value;
}

static int futex_cmp_result(int oldval, int cmparg, int cmp)
{
    switch (cmp) {
    case FUTEX_OP_CMP_EQ: return oldval == cmparg;
    case FUTEX_OP_CMP_NE: return oldval != cmparg;
    case FUTEX_OP_CMP_LT: return oldval < cmparg;
    case FUTEX_OP_CMP_LE: return oldval <= cmparg;
    case FUTEX_OP_CMP_GT: return oldval > cmparg;
    case FUTEX_OP_CMP_GE: return oldval >= cmparg;
    default:              return -EINVAL;
    }
}

static int futex_apply_op(int op, uint32 oldval, int oparg, uint32 *newval)
{
    switch (op & ~FUTEX_OP_OPARG_SHIFT) {
    case FUTEX_OP_SET:  *newval = (uint32)oparg; break;
    case FUTEX_OP_ADD:  *newval = oldval + (uint32)oparg; break;
    case FUTEX_OP_OR:   *newval = oldval | (uint32)oparg; break;
    case FUTEX_OP_ANDN: *newval = oldval & ~(uint32)oparg; break;
    case FUTEX_OP_XOR:  *newval = oldval ^ (uint32)oparg; break;
    default:            return -EINVAL;
    }
    return 0;
}

static int futex_wake_op(uint64 uaddr1, int nr_wake1, int nr_wake2,
                         uint64 uaddr2, uint32 encoded_op, bool private)
{
    vm_t *vm = current->vm;
    struct futex_key key1, key2;
    int ret = futex_make_key(vm, uaddr1, private, &key1);
    if (ret < 0)
        return ret;
    ret = futex_make_key(vm, uaddr2, private, &key2);
    if (ret < 0)
        return ret;

    uint64 idx1 = futex_hash(&key1);
    uint64 idx2 = futex_hash(&key2);
    struct futex_bucket *b1 = &futex_table[idx1];
    struct futex_bucket *b2 = &futex_table[idx2];
    uint32 oldval, newval;

    int op = (encoded_op >> 28) & 0xf;
    int cmp = (encoded_op >> 24) & 0xf;
    int oparg = futex_op_sign_extend12((encoded_op >> 12) & 0xfff);
    int cmparg = futex_op_sign_extend12(encoded_op & 0xfff);

    if (op & FUTEX_OP_OPARG_SHIFT) {
        int shift = oparg;
        if (shift < 0 || shift >= 32)
            return -EINVAL;
        oparg = 1 << shift;
    }

    if (idx1 < idx2) {
        spin_lock(&b1->lock);
        spin_lock(&b2->lock);
    } else if (idx1 > idx2) {
        spin_lock(&b2->lock);
        spin_lock(&b1->lock);
    } else {
        spin_lock(&b1->lock);
    }

    ret = futex_read_u32(vm, uaddr2, &oldval);
    if (ret != 0)
        goto out_unlock;
    ret = futex_apply_op(op, oldval, oparg, &newval);
    if (ret != 0)
        goto out_unlock;
    ret = futex_write_u32(vm, uaddr2, newval);
    if (ret != 0)
        goto out_unlock;

    int cmp_ret = futex_cmp_result((int)(int32)oldval, cmparg, cmp);
    if (cmp_ret < 0) {
        ret = cmp_ret;
        goto out_unlock;
    }

    ret = futex_wake_locked(b1, &key1, nr_wake1, FUTEX_BITSET_MATCH_ANY);
    if (cmp_ret)
        ret += futex_wake_locked(b2, &key2, nr_wake2,
                                 FUTEX_BITSET_MATCH_ANY);

out_unlock:
    if (idx1 != idx2)
        spin_unlock(&b2->lock);
    spin_unlock(&b1->lock);
    return ret;
}

/*
 * futex_requeue — wake up to 'nr_wake' waiters on uaddr1, then move
 * up to 'nr_requeue' remaining waiters from uaddr1's queue to uaddr2's queue.
 *
 * For FUTEX_CMP_REQUEUE, the caller passes cmpval and we verify *uaddr1 ==
 * cmpval before proceeding. For plain FUTEX_REQUEUE, cmpval is ignored
 * (pass -1 or any value with do_cmp=false).
 *
 * Returns the total number of waiters woken or requeued on success, matching
 * the Linux futex ABI used by pthread condition-variable implementations.
 */
static int futex_requeue(uint64 uaddr1, int nr_wake, uint64 uaddr2,
                         int nr_requeue, uint32 cmpval, int do_cmp,
                         bool private) {
    vm_t *vm = current->vm;
    struct futex_key key1, key2;
    int ret = futex_make_key(vm, uaddr1, private, &key1);
    if (ret < 0)
        return ret;
    ret = futex_make_key(vm, uaddr2, private, &key2);
    if (ret < 0)
        return ret;

    uint64 idx1 = futex_hash(&key1);
    uint64 idx2 = futex_hash(&key2);
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
        if (futex_read_u32(vm, uaddr1, &curval) < 0) {
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
        if (futex_keys_equal(w, &key1)) {
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
        if (futex_keys_equal(w, &key1)) {
            /* Remove from b1 */
            *pp = w->next;
            /* Update the waiter's key to uaddr2 */
            w->key_vm = key2.vm;
            w->key_addr = key2.addr;
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

    return woken + requeued;
}

/*
 * sys_futex — system call entry point
 *
 * Arguments:
 *   a0: uint64  uaddr   — user-space futex word address
 *   a1: int     futex_op — operation (FUTEX_WAIT, FUTEX_WAKE, etc.)
 *   a2: uint32  val     — expected value (WAIT) or count (WAKE)
 *   a3: uint64  timeout — pointer to relative/absolute timespec
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
    bool realtime = (futex_op & FUTEX_CLOCK_REALTIME) != 0;
    bool private = (futex_op & FUTEX_PRIVATE_FLAG) != 0;

    switch (cmd) {
    case FUTEX_WAIT: {
        bool has_timeout;
        uint64 timeout_ms;
        int ret = futex_parse_timeout(timeout_or_val2, false, realtime,
                                      &has_timeout, &timeout_ms);
        if (ret < 0)
            return (uint64)ret;
        return (uint64)futex_wait(uaddr, val, FUTEX_BITSET_MATCH_ANY, private,
                                  has_timeout, timeout_ms);
    }

    case FUTEX_WAKE:
        return (uint64)futex_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY,
                                  private);

    case FUTEX_WAIT_BITSET: {
        bool has_timeout;
        uint64 timeout_ms;
        int ret = futex_parse_timeout(timeout_or_val2, true, realtime,
                                      &has_timeout, &timeout_ms);
        if (ret < 0)
            return (uint64)ret;
        return (uint64)futex_wait(uaddr, val, val3, private,
                                  has_timeout, timeout_ms);
    }

    case FUTEX_WAKE_BITSET:
        return (uint64)futex_wake(uaddr, val, val3, private);

    case FUTEX_REQUEUE:
        if (uaddr2 & 3)
            return (uint64)-EINVAL;
        /* val=nr_wake, timeout_or_val2=nr_requeue, uaddr2=target addr */
        return (uint64)futex_requeue(uaddr, (int)val, uaddr2,
                                     (int)timeout_or_val2, 0, 0, private);

    case FUTEX_CMP_REQUEUE:
        if (uaddr2 & 3)
            return (uint64)-EINVAL;
        /* val=nr_wake, timeout_or_val2=nr_requeue, uaddr2=target, val3=cmpval */
        return (uint64)futex_requeue(uaddr, (int)val, uaddr2,
                                     (int)timeout_or_val2, val3, 1, private);

    case FUTEX_WAKE_OP:
        if (uaddr2 & 3)
            return (uint64)-EINVAL;
        /* val=nr_wake on uaddr, timeout_or_val2=nr_wake on uaddr2,
         * uaddr2=second futex, val3=encoded FUTEX_OP operation. */
        return (uint64)futex_wake_op(uaddr, (int)val,
                                     (int)timeout_or_val2, uaddr2, val3,
                                     private);

    default:
        return (uint64)-ENOSYS;
    }
}
