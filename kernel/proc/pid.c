// Process table

#include "types.h"
#include "string.h"
#include "param.h"
#include <mm/memlayout.h>
#include "riscv.h"
#include "lock/spinlock.h"
#include "lock/rcu.h"
#include "lock/rwlock.h"
#include "proc/thread.h"
#include "proc_private.h"
#include "defs.h"
#include "printf.h"
#include "list.h"
#include "hlist.h"
#include "proc/tq.h"
#include "proc/sched.h"
#include <mm/slab.h>
#include <mm/page.h>
#include <mm/vm.h>
#include "vfs/fs.h"
#include "vfs/file.h"

static struct {
    struct {
        hlist_t procs;
        hlist_bucket_t buckets[NR_THREAD_HASH_BUCKETS];
    };
    int64 registered_cnt;
    int64 allocated_cnt;
    list_node_t procs_list; // List of all threads, for dumping
    struct thread *initproc;
    int nextpid;
    struct rwlock pid_lock;
} proc_table;

/* Hash table callback functions for proc table */

static ht_hash_t __proctab_hash(void *node) {
    struct thread *p = (struct thread *)node;
    return hlist_hash_int(p->pid);
}

static int __proctab_hash_cmp(hlist_t *ht, void *node1, void *node2) {
    struct thread *p1 = (struct thread *)node1;
    struct thread *p2 = (struct thread *)node2;
    return p1->pid - p2->pid;
}

static hlist_entry_t *__proctab_hash_get_entry(void *node) {
    struct thread *p = (struct thread *)node;
    return &p->proctab_entry;
}

static void *__proctab_hash_get_node(hlist_entry_t *entry) {
    return (void *)container_of(entry, struct thread, proctab_entry);
}

// initialize the proc table and pid_lock.
void __proctab_init(void) {
    hlist_func_t funcs = {
        .hash = __proctab_hash,
        .get_node = __proctab_hash_get_node,
        .get_entry = __proctab_hash_get_entry,
        .cmp_node = __proctab_hash_cmp,
    };
    hlist_init(&proc_table.procs, NR_THREAD_HASH_BUCKETS, &funcs);
    rwlock_init(&proc_table.pid_lock, "pid_lock");
    list_entry_init(&proc_table.procs_list);
    proc_table.initproc = NULL;
    proc_table.nextpid = 1;
}

/* Lock and unlock proc table
   Required to hold when modifying proc table */

void pid_wlock(void) { rwlock_wlock(&proc_table.pid_lock); }
void pid_wunlock(void) { rwlock_wunlock(&proc_table.pid_lock); }
void pid_rlock(void) { rwlock_rlock(&proc_table.pid_lock); }
void pid_runlock(void) { rwlock_runlock(&proc_table.pid_lock); }

/* The following will assert that the process table is locked */
void __proctab_set_initproc(struct thread *p) {
    pid_wlock();
    assert(p != NULL, "NULL initproc");
    assert(proc_table.initproc == NULL, "initproc already set");
    // Use atomic store with release semantics
    rcu_assign_pointer(proc_table.initproc, p);
    pid_wunlock();
}

// get the init process.
// This function won't check locking state
struct thread *__proctab_get_initproc(void) {
    assert(proc_table.initproc != NULL, "initproc not set");
    return rcu_dereference(proc_table.initproc);
}

// get a PCB by pid.
static struct thread *__proctab_get_pid_tcb_locked(int pid) {
    struct thread dummy = {.pid = pid};
    struct thread *p = hlist_get(&proc_table.procs, &dummy);
    return p;
}

// Increment nextpid with wraparound.
// PID 1 is reserved for init, so valid range is [2, MAXPID).
// Must be called with proc_table lock held.
static void __nextpid_inc(void) {
    proc_table.nextpid++;
    if (proc_table.nextpid >= MAXPID) {
        proc_table.nextpid = 2;
    }
}

// allocate a new pid.
// If thread creation fails after this, the caller must call __free_pid to
// release it.
int __alloc_pid(void) {
    pid_wlock();

    // Limit the number of allocated PIDs to NR_THREAD
    if (proc_table.allocated_cnt >= NR_THREAD) {
        pid_wunlock();
        return -1; // No available PIDs
    }

    int start = proc_table.nextpid;

    // Search for an unused PID, wrapping around if necessary
    while (__proctab_get_pid_tcb_locked(proc_table.nextpid) != NULL) {
        __nextpid_inc();
        if (proc_table.nextpid == start) {
            // We've searched the entire range, no free PID
            pid_wunlock();
            return -1;
        }
    }

    int pid = proc_table.nextpid;
    __nextpid_inc();
    proc_table.allocated_cnt++;
    pid_wunlock();
    return pid;
}

void __free_pid(int pid) {
    (void)pid;
    pid_wlock();
    proc_table.allocated_cnt--;
    pid_wunlock();
}

void __proctab_proc_add_locked(struct thread *p) {
    assert(p != NULL, "NULL proc passed to __proctab_proc_add_locked");
    assert(LIST_ENTRY_IS_DETACHED(&p->dmp_list_entry),
           "Process %d is already in the dump list", p->pid);

    // Use RCU-safe insertion for concurrent readers
    struct thread *existing = hlist_put_rcu(&proc_table.procs, p, false);

    assert(existing != p, "Failed to add process with pid %d", p->pid);
    assert(existing == NULL, "Process with pid %d already exists", p->pid);
    // Add to the global list of threads for dumping (RCU-safe).
    list_entry_add_tail_rcu(&proc_table.procs_list, &p->dmp_list_entry);
    proc_table.registered_cnt++;
}

// RCU-safe version: get a PCB by pid without holding locks.
// Caller MUST be within rcu_read_lock()/rcu_read_unlock() critical section.
// The returned pointer is only valid within the RCU critical section.
int get_pid_thread(int pid, struct thread **pp) {
    if (!pp) {
        return -1; // Invalid argument
    }
    struct thread dummy = {.pid = pid};
    struct thread *p = hlist_get_rcu(&proc_table.procs, &dummy);
    *pp = p;
    return 0;
}

void proctab_proc_add(struct thread *p) {
    pid_wlock();
    __proctab_proc_add_locked(p);
    pid_wunlock();
}

void proctab_proc_remove(struct thread *p) {
    // Caller must hold p->lock
    proc_assert_holding(p);

    pid_wlock();
    // Use RCU-safe removal for concurrent readers
    struct thread *existing = hlist_pop_rcu(&proc_table.procs, p);
    // Remove from the global list of threads for dumping (RCU-safe).
    list_entry_del_init_rcu(&p->dmp_list_entry);
    proc_table.registered_cnt--;
    pid_wunlock();

    assert(existing == NULL || existing == p,
           "thread_destroy called with a different proc");

    // Note: Caller must call synchronize_rcu() or use call_rcu() before freeing
    // the proc structure, to ensure all RCU readers have finished accessing it.
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// Uses RCU for lock-free iteration to avoid wedging a stuck machine.
void procdump(void) {
    struct thread *p;
    const char *state;

    printf("Thread list(* means on_cpu is set):\n");
    rcu_read_lock();

    // Use RCU-safe iteration for concurrent access
    hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
        tcb_lock(p);
        enum thread_state pstate = __thread_state_get(p);
        int pid = p->pid;
        char name[sizeof(p->name)];
        safestrcpy(name, p->name, sizeof(name));
        char pname[sizeof(p->parent->name)];
        if (p->parent) {
            safestrcpy(pname, p->parent->name, sizeof(pname));
        } else {
            safestrcpy(pname, "N/A", sizeof(pname));
        }
        tcb_unlock(p);

        if (pstate == THREAD_UNUSED)
            continue;

        state = thread_state_to_str(pstate);
        printf("(CPU: %s%d) %d %s [%s] %s : %s\n",
               smp_load_acquire(&p->sched_entity->on_cpu) ? "*" : "",
               p->sched_entity->cpu_id, pid, state,
               THREAD_USER_SPACE(p) ? "U" : "K", pname, name);
    }

    rcu_read_unlock();
}

// Dump backtraces of all blocked (sleeping) threads.
// This is useful for debugging deadlocks.
// Uses RCU for lock-free iteration.
void procdump_bt(void) {
    struct thread *p;

    printf("\n=== Blocked Process Backtraces ===\n");
    rcu_read_lock();
    // Use RCU-safe iteration for concurrent access
    hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
        tcb_lock(p);
        enum thread_state pstate = __thread_state_get(p);
        int pid = p->pid;
        char name[sizeof(p->name)];
        safestrcpy(name, p->name, sizeof(name));

        // Only backtrace blocked threads (sleeping/uninterruptible)
        if (pstate == THREAD_INTERRUPTIBLE ||
            pstate == THREAD_UNINTERRUPTIBLE) {
            // Skip if thread is currently on a CPU (context not saved)
            if (smp_load_acquire(&p->sched_entity->on_cpu)) {
                printf(
                    "\n--- Process %d [%s] %s --- (on CPU, cannot backtrace)\n",
                    pid,
                    pstate == THREAD_INTERRUPTIBLE ? "interruptible"
                                                   : "uninterruptible",
                    name);
            } else {
                printf("\n--- Process %d [%s] %s ---\n", pid,
                       pstate == THREAD_INTERRUPTIBLE ? "interruptible"
                                                      : "uninterruptible",
                       name);
                print_thread_backtrace(&p->sched_entity->context, p->kstack,
                                       p->kstack_order);
            }
        }
        tcb_unlock(p);
    }

    printf("\n=== End Backtraces ===\n");

    rcu_read_unlock();
}

// Backtrace a specific thread by PID
// Uses RCU for lock-free lookup.
void procdump_bt_pid(int pid) {
    struct thread *p = NULL;
    rcu_read_lock();

    // Use RCU-safe lookup
    struct thread dummy = {.pid = pid};
    p = hlist_get_rcu(&proc_table.procs, &dummy);
    if (p == NULL) {
        printf("Process %d not found\n", pid);
        rcu_read_unlock();
        return;
    }

    tcb_lock(p);
    enum thread_state pstate = __thread_state_get(p);
    char name[sizeof(p->name)];
    safestrcpy(name, p->name, sizeof(name));

    printf("\n--- Process %d [%s] %s ---\n", pid, thread_state_to_str(pstate),
           name);

    if (smp_load_acquire(&p->sched_entity->on_cpu)) {
        printf("Process is currently on a CPU, context not saved\n");
    } else if (pstate == THREAD_UNUSED) {
        // ZOMBIE threads have a valid stack and context for backtracing, but
        // UNUSED threads do not.
        printf("Process is %s, no valid context\n",
               thread_state_to_str(pstate));
    } else {
        print_thread_backtrace(&p->sched_entity->context, p->kstack,
                               p->kstack_order);
    }

    tcb_unlock(p);

    rcu_read_unlock();
}

// Helper function to recursively print thread tree
// Locks the parent thread while traversing its children list, following
// the lock order: parent lock before child lock (see lock order comment at top
// of file).
static void __procdump_tree_recursive(struct thread *p, int depth) {
    const char *state;
    struct thread *child, *tmp;
    enum thread_state pstate;
    int pid;
    char name[16];

    // Print indentation
    for (int i = 0; i < depth; i++) {
        printf("  ");
    }

    // Print tree connector
    if (depth > 0) {
        printf("└─ ");
    }

    // Lock parent thread and get its info
    tcb_lock(p);
    pstate = __thread_state_get(p);
    pid = p->pid;
    safestrcpy(name, p->name, sizeof(name));

    state = thread_state_to_str(pstate);
    printf("%d %s [%s] %s", pid, state, THREAD_USER_SPACE(p) ? "U" : "K", name);
    if (smp_load_acquire(&p->sched_entity->on_cpu)) {
        printf(" (CPU: %d)\n", p->sched_entity->cpu_id);
    } else {
        printf("\n");
    }

    // Keep parent locked while traversing children (safe per lock order rules)
    // Each recursive call will lock the child while parent remains locked
    list_foreach_node_safe(&p->children, child, tmp, siblings) {
        __procdump_tree_recursive(child, depth + 1);
    }

    tcb_unlock(p);
}

// Print process tree based on parent-child relationships.
// Shows the hierarchical structure starting from init process.
// Because tree traversal requires locking parent and child threads, this
// function is not fully lock-free.
void procdump_tree(void) {
    struct thread *initproc;
    printf("Process Tree:\n");

    pid_rlock();

    initproc = __proctab_get_initproc();
    if (initproc == NULL) {
        printf("No init process\n");
        pid_runlock();
        return;
    }

    __procdump_tree_recursive(initproc, 0);

    pid_runlock();
}

uint64 sys_dumpproc(void) {
    // This function is called from the dumpproc user program.
    // It dumps the process table to the console.
    procdump();
    return 0;
}
