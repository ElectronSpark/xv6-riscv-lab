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
#include "proc/thread_group.h"
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
#include "errno.h"

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
bool pid_try_lock_upgrade(void) {
    return rwlock_try_update(&proc_table.pid_lock);
}
bool pid_wholding(void) { return RWLOCK_W_HOLDING(&proc_table.pid_lock); }
void pid_assert_wholding(void) { assert(pid_wholding(), "pid lock not held"); }

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

// Advance nextpid past the given allocated PID, with wraparound.
// PID 1 is reserved for init, so valid range is [2, MAXPID).
// Must be called with proc_table lock held.
static void __nextpid_inc(int pid) {
    proc_table.nextpid = pid + 1;
    if (proc_table.nextpid >= MAXPID) {
        proc_table.nextpid = 2;
    }
}

// Reserve a PID slot. Does not assign an actual PID number — that is
// deferred to proctab_proc_add(). Lock-free: uses atomic_inc_unless so
// callers need not hold pid_lock.
// If thread creation fails after this, the caller must call __free_pid()
// to release the reservation.
// Returns 0 on success, -EAGAIN if no slots available.
int __alloc_pid(void) {
    // Atomically increment allocated_cnt unless it already equals NR_THREAD
    if (!atomic_inc_unless(&proc_table.allocated_cnt, NR_THREAD)) {
        return -EAGAIN; // No available PID slots
    }
    return 0;
}

// Release a PID slot reservation. Lock-free.
void __free_pid(void) {
    assert(proc_table.allocated_cnt > 0, "__free_pid: allocated_cnt underflow");
    atomic_sub(&proc_table.allocated_cnt, 1);
}

// Add a thread to the proc table, assigning it a real PID.
// The thread must have a PID slot reserved via __alloc_pid() beforehand.
// Caller must hold pid_wlock.
void proctab_proc_add(struct thread *p) {
    pid_assert_wholding();
    assert(p != NULL, "NULL proc passed to proctab_proc_add");
    assert(LIST_ENTRY_IS_DETACHED(&p->dmp_list_entry),
           "Process is already in the dump list");

    // Find an unused PID number
    int start = proc_table.nextpid;
    while (__proctab_get_pid_tcb_locked(proc_table.nextpid) != NULL) {
        __nextpid_inc(proc_table.nextpid);
        assert(proc_table.nextpid != start,
               "proctab_proc_add: no free PID (should not happen)");
    }
    p->pid = proc_table.nextpid;
    __nextpid_inc(p->pid);

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
        return -EINVAL; // Invalid argument
    }
    struct thread dummy = {.pid = pid};
    struct thread *p = hlist_get_rcu(&proc_table.procs, &dummy);
    *pp = p;
    return 0;
}

void proctab_proc_remove(struct thread *p) {
    pid_assert_wholding();
    // Use RCU-safe removal for concurrent readers
    struct thread *existing = hlist_pop_rcu(&proc_table.procs, p);
    // Remove from the global list of threads for dumping (RCU-safe).
    list_entry_del_init_rcu(&p->dmp_list_entry);
    proc_table.registered_cnt--;

    assert(existing == NULL || existing == p,
           "thread_destroy called with a different proc");

    // Note: Caller must call synchronize_rcu() or use call_rcu() before freeing
    // the proc structure, to ensure all RCU readers have finished accessing it.
}

// Format an integer into buf at offset pos. Returns new pos.
static int __fmt_int(char *buf, int pos, int bufsz, int val) {
    char tmp[12];
    int i = 0;
    unsigned int uv;
    if (val < 0) {
        if (pos < bufsz)
            buf[pos++] = '-';
        uv = (unsigned int)(-val);
    } else {
        uv = (unsigned int)val;
    }
    do {
        tmp[i++] = '0' + uv % 10;
        uv /= 10;
    } while (uv);
    while (i > 0 && pos < bufsz)
        buf[pos++] = tmp[--i];
    return pos;
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// Uses RCU for lock-free iteration to avoid wedging a stuck machine.
void procdump(void) {
    struct thread *p;

    printf("%-20s %-5s %-2s %-3s %s\n", "SID:PGID:TGID:TID", "CPU", "ST", "U/K",
           "COMMAND");
    rcu_read_lock();

    // Use RCU-safe iteration for concurrent access
    hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
        tcb_lock(p);
        enum thread_state pstate = __thread_state_get(p);
        int tid = p->pid;
        int tgid = p->tgid;
        int pgid = p->pgid;
        int sid = p->sid;
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

        // Build "sid:pgid:tgid:tid" id string
        char idbuf[40];
        int pos = 0;
        pos = __fmt_int(idbuf, pos, sizeof(idbuf), sid);
        idbuf[pos++] = ':';
        pos = __fmt_int(idbuf, pos, sizeof(idbuf), pgid);
        idbuf[pos++] = ':';
        pos = __fmt_int(idbuf, pos, sizeof(idbuf), tgid);
        idbuf[pos++] = ':';
        pos = __fmt_int(idbuf, pos, sizeof(idbuf), tid);
        idbuf[pos] = '\0';
        // Build "cpu" string with optional '*' prefix
        char cpubuf[8];
        int ci = 0;
        if (smp_load_acquire(&p->sched_entity->on_cpu))
            cpubuf[ci++] = '*';
        int cpu_id = p->sched_entity->cpu_id;
        if (cpu_id >= 10)
            cpubuf[ci++] = '0' + cpu_id / 10;
        cpubuf[ci++] = '0' + cpu_id % 10;
        cpubuf[ci] = '\0';

        printf("%-20s %-5s %-2s [%s] %s/%s\n", idbuf, cpubuf,
               thread_state_short(pstate), THREAD_USER_SPACE(p) ? "U" : "K",
               pname, name);
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

        int tgid = p->tgid;
        int pgid = p->pgid;
        int sid_val = p->sid;

        // Only backtrace blocked threads (sleeping/uninterruptible)
        if (pstate == THREAD_INTERRUPTIBLE ||
            pstate == THREAD_UNINTERRUPTIBLE) {
            const char *stype = pstate == THREAD_INTERRUPTIBLE
                                    ? "interruptible"
                                    : "uninterruptible";
            // Skip if thread is currently on a CPU (context not saved)
            if (smp_load_acquire(&p->sched_entity->on_cpu)) {
                printf("\n--- %d:%d:%d:%d [%s] %s ---"
                       " (on CPU, cannot backtrace)\n",
                       sid_val, pgid, tgid, pid, stype, name);
            } else {
                printf("\n--- %d:%d:%d:%d [%s] %s ---\n", sid_val, pgid, tgid,
                       pid, stype, name);
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
    int tgid = p->tgid;
    int pgid = p->pgid;
    int sid_val = p->sid;

    printf("\n--- %d:%d:%d:%d [%s] %s ---\n", sid_val, pgid, tgid, pid,
           thread_state_short(pstate), name);

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

// Helper function to recursively print thread tree.
// Caller must hold pid_rlock to protect the children list traversal.
// Individual tcb_lock is taken only to read thread state/name atomically.
// protected by pid_rlock
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
    pstate = __thread_state_get(p);
    pid = p->pid;
    safestrcpy(name, p->name, sizeof(name));

    int tgid = p->tgid;
    int pgid_val = p->pgid;
    int sid_val = p->sid;

    state = thread_state_short(pstate);
    printf("%d:%d:%d:%d %s [%s] %s", sid_val, pgid_val, tgid, pid, state,
           THREAD_USER_SPACE(p) ? "U" : "K", name);
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

// Print process tree starting from a specific PID.
void procdump_tree_pid(int target_pid) {
    printf("Process Tree (from pid %d):\n", target_pid);

    pid_rlock();

    struct thread *p = __proctab_get_pid_tcb_locked(target_pid);
    if (p == NULL) {
        printf("Process %d not found\n", target_pid);
        pid_runlock();
        return;
    }

    __procdump_tree_recursive(p, 0);

    pid_runlock();
}

// =====================================================================
// Session / process-group / process / thread hierarchy dump
// =====================================================================

// Collect unique session IDs from the proc table.
// Returns number of unique SIDs found.
static int __collect_sids(pid_t *sids, int max) {
    int n = 0;
    struct thread *p;
    hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
        pid_t sid = p->sid;
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (sids[i] == sid) {
                found = 1;
                break;
            }
        }
        if (!found && n < max)
            sids[n++] = sid;
    }
    return n;
}

// Collect unique PGIDs within a given session.
static int __collect_pgids(pid_t sid, pid_t *pgids, int max) {
    int n = 0;
    struct thread *p;
    hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
        if (p->sid != sid)
            continue;
        pid_t pgid = p->pgid;
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (pgids[i] == pgid) {
                found = 1;
                break;
            }
        }
        if (!found && n < max)
            pgids[n++] = pgid;
    }
    return n;
}

// Collect unique TGIDs within a given process group.
static int __collect_tgids(pid_t pgid, pid_t *tgids, int max) {
    int n = 0;
    struct thread *p;
    hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
        if (p->pgid != pgid)
            continue;
        pid_t tgid = p->tgid;
        int found = 0;
        for (int i = 0; i < n; i++) {
            if (tgids[i] == tgid) {
                found = 1;
                break;
            }
        }
        if (!found && n < max)
            tgids[n++] = tgid;
    }
    return n;
}

// Simple insertion sort for a small pid array.
static void __sort_pids(pid_t *arr, int n) {
    for (int i = 1; i < n; i++) {
        pid_t key = arr[i];
        int j = i - 1;
        while (j >= 0 && arr[j] > key) {
            arr[j + 1] = arr[j];
            j--;
        }
        arr[j + 1] = key;
    }
}

// Print all threads grouped by session → process group → process.
// Uses pid_rlock to serialize access to the proc table hierarchy.
void procdump_sessions(void) {
// Enough for typical workloads; larger systems would need dynamic alloc
#define DUMP_MAX_IDS 128
    pid_t sids[DUMP_MAX_IDS];
    pid_t pgids[DUMP_MAX_IDS];
    pid_t tgids[DUMP_MAX_IDS];

    printf("\n=== Process Hierarchy (Session / PGroup / Process / Thread) "
           "===\n");

    pid_rlock();

    int nsid = __collect_sids(sids, DUMP_MAX_IDS);
    __sort_pids(sids, nsid);

    for (int si = 0; si < nsid; si++) {
        printf("\nSession %d\n", sids[si]);

        int npg = __collect_pgids(sids[si], pgids, DUMP_MAX_IDS);
        __sort_pids(pgids, npg);

        for (int gi = 0; gi < npg; gi++) {
            printf("  PGroup %d\n", pgids[gi]);

            int ntg = __collect_tgids(pgids[gi], tgids, DUMP_MAX_IDS);
            __sort_pids(tgids, ntg);

            for (int ti = 0; ti < ntg; ti++) {
                // Print all threads in this tgid
                struct thread *p;
                int first = 1;
                hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
                    if (p->tgid != tgids[ti] || p->pgid != pgids[gi])
                        continue;

                    const char *state =
                        thread_state_to_str(__thread_state_get(p));
                    int on_cpu = smp_load_acquire(&p->sched_entity->on_cpu);

                    if (first) {
                        printf("    Process %d [%s] %s", p->tgid,
                               THREAD_USER_SPACE(p) ? "U" : "K", p->name);
                        if (thread_is_group_leader(p)) {
                            printf(" (leader, tid %d, %s%s)\n", p->pid, state,
                                   on_cpu ? ", on CPU" : "");
                        } else {
                            printf("\n");
                            printf("      tid %d %s%s\n", p->pid, state,
                                   on_cpu ? " (on CPU)" : "");
                        }
                        first = 0;
                    } else {
                        printf("      tid %d %s %s%s\n", p->pid, state, p->name,
                               on_cpu ? " (on CPU)" : "");
                    }
                }
            }
        }
    }

    pid_runlock();

    printf("\n=== End Hierarchy ===\n");
#undef DUMP_MAX_IDS
}

// Print hierarchy for a single session.
// Uses pid_rlock to serialize access.
void procdump_sessions_sid(pid_t target_sid) {
#define DUMP_MAX_IDS 128
    pid_t pgids[DUMP_MAX_IDS];
    pid_t tgids[DUMP_MAX_IDS];

    printf("\n=== Session %d Hierarchy ===\n", target_sid);

    pid_rlock();

    int npg = __collect_pgids(target_sid, pgids, DUMP_MAX_IDS);
    if (npg == 0) {
        printf("Session %d not found\n", target_sid);
        pid_runlock();
        printf("\n=== End Hierarchy ===\n");
        return;
    }
    __sort_pids(pgids, npg);

    printf("\nSession %d\n", target_sid);
    for (int gi = 0; gi < npg; gi++) {
        printf("  PGroup %d\n", pgids[gi]);

        int ntg = __collect_tgids(pgids[gi], tgids, DUMP_MAX_IDS);
        __sort_pids(tgids, ntg);

        for (int ti = 0; ti < ntg; ti++) {
            struct thread *p;
            int first = 1;
            hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) {
                if (p->tgid != tgids[ti] || p->pgid != pgids[gi])
                    continue;

                const char *state = thread_state_to_str(__thread_state_get(p));
                int on_cpu = smp_load_acquire(&p->sched_entity->on_cpu);

                if (first) {
                    printf("    Process %d [%s] %s", p->tgid,
                           THREAD_USER_SPACE(p) ? "U" : "K", p->name);
                    if (thread_is_group_leader(p)) {
                        printf(" (leader, tid %d, %s%s)\n", p->pid, state,
                               on_cpu ? ", on CPU" : "");
                    } else {
                        printf("\n");
                        printf("      tid %d %s%s\n", p->pid, state,
                               on_cpu ? " (on CPU)" : "");
                    }
                    first = 0;
                } else {
                    printf("      tid %d %s %s%s\n", p->pid, state, p->name,
                           on_cpu ? " (on CPU)" : "");
                }
            }
        }
    }

    pid_runlock();

    printf("\n=== End Hierarchy ===\n");
#undef DUMP_MAX_IDS
}

uint64 sys_dumpproc(void) {
    int mode, id;
    argint(0, &mode);
    argint(1, &id);
    switch (mode) {
    case 0:
        procdump();
        break;
    case 1:
        if (id >= 0)
            procdump_tree_pid(id);
        else
            procdump_tree();
        break;
    case 2:
        if (id >= 0)
            procdump_sessions_sid(id);
        else
            procdump_sessions();
        break;
    case 3:
        if (id >= 0)
            procdump_bt_pid(id);
        else
            procdump_bt();
        break;
    default:
        procdump();
        break;
    }
    return 0;
}

// Iterate all threads under RCU protection.
// The callback receives each thread and an opaque argument.
// Acquires and releases rcu_read_lock internally.
void proctab_for_each_rcu(void (*fn)(struct thread *, void *), void *arg) {
    struct thread *p;
    rcu_read_lock();
    hlist_foreach_node_rcu(&proc_table.procs, p, proctab_entry) { fn(p, arg); }
    rcu_read_unlock();
}
