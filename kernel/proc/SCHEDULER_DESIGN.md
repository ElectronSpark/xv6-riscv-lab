# xv6 Scheduler Design

This document describes the scheduler design in xv6, modeled after the Linux kernel's
`try_to_wake_up()` pattern from `kernel/sched/core.c`.

## Overview

The xv6 scheduler uses a **direct process-to-process context switch** model (no dedicated
scheduler thread). When a process yields or sleeps, it directly switches to the next
runnable process. The scheduler maintains correctness on SMP systems through careful
ordering of flag updates and memory barriers.

## Key Data Structures

### Scheduling Entity (`sched_entity`)

Scheduling-related fields are stored in `struct sched_entity`, separate from `struct proc`:

```c
struct sched_entity {
    union {
        struct rb_node rb_entry;    // For red-black tree (CFS-like)
        list_node_t list_entry;     // For linked list (FIFO)
    };
    struct rq *rq;              // Pointer to the run queue
    int priority;               // Scheduling priority
    struct proc *proc;          // Back pointer to the process
    struct sched_class *sched_class;  // Scheduling class
    spinlock_t pi_lock;         // Priority inheritance lock
    int on_rq;                  // On a ready queue
    int on_cpu;                 // Running on a CPU
    int cpu_id;                 // CPU running this process
    cpumask_t affinity_mask;    // CPU affinity mask
    
    uint64 start_time;          // Time when the process started running
    uint64 exec_start;          // Last time the process started executing
    uint64 exec_end;            // Last time the process stopped executing
    
    struct context context;     // swtch() here to run process
};
```

The process structure references the scheduling entity:

```c
struct proc {
    struct sched_entity *sched_entity;  // Pointer to scheduling entity
    enum procstate state;               // RUNNING, SLEEPING, ZOMBIE, etc.
    int stopped;                        // SIGSTOP/SIGCONT flag
    // ... other fields
};
```

### Run Queue (`rq`) and Scheduling Classes

The scheduler uses per-CPU run queues with pluggable scheduling classes:

```c
struct rq {
    struct sched_class *sched_class;  // Scheduling class in use
    int class_id;           // Scheduling class ID (0-63)
    int task_count;         // Number of processes in the run queue
    int cpu_id;             // CPU ID this run queue belongs to
};

struct sched_class {
    void (*enqueue_task)(struct rq *rq, struct sched_entity *se);
    void (*dequeue_task)(struct rq *rq, struct sched_entity *se);
    struct rq *(*select_task_rq)(struct rq *rq, struct sched_entity *se, cpumask_t cpumask);
    struct sched_entity* (*pick_next_task)(struct rq *rq);
    void (*put_prev_task)(struct rq *rq, struct sched_entity *se);
    void (*set_next_task)(struct rq *rq, struct sched_entity *se);
    void (*task_tick)(struct rq *rq, struct sched_entity *se);
    void (*task_fork)(struct rq *rq, struct sched_entity *se);
    void (*task_dead)(struct rq *rq, struct sched_entity *se);
    void (*yield_task)(struct rq *rq);
};
```

### Priority System

The scheduler uses a two-level priority system with 64 major priority levels:

```c
// Priority structure: 6-bit major (0-63) + 2-bit minor (0-3)
#define MAJOR_PRIORITY(prio)  ((prio) >> 2)
#define MINOR_PRIORITY(prio)  ((prio) & 0x3)
#define MAKE_PRIORITY(major, minor)  (((major) << 2) | (minor))

// Special priority levels
#define EXIT_MAJOR_PRIORITY   0   // Exiting processes (highest)
#define FIFO_MAJOR_PRIORITY   1   // Regular FIFO tasks (1-62)
#define IDLE_MAJOR_PRIORITY  63   // Idle processes (lowest)
```

### Two-Layer Ready Mask for O(1) Lookup

The scheduler uses a two-layer bitmask for O(1) lookup of the highest priority ready queue:

```
ready_mask (8 bits): Each bit indicates if the corresponding group has any ready tasks.
    Bit i is set if any priority level in group i (cls_id 8*i to 8*i+7) has tasks.

ready_mask_secondary (64 bits): Each bit indicates if that priority level has ready tasks.
    Organized as 8 groups of 8 bits each, matching ready_mask groups.

Lookup algorithm:
  1. Find lowest set bit in ready_mask -> top_id (group 0-7)
  2. Extract 8-bit group from ready_mask_secondary at (top_id * 8)
  3. Find lowest set bit in that group -> cls_id = top_id*8 + bit_position
```

Currently implemented scheduling classes:

- **FIFO** (`fifo_rq`): FIFO scheduling for major priorities 1-62, with 4 minor priority subqueues per level
- **IDLE** (`idle_rq`): Idle process scheduling (major priority 63)

### FIFO Scheduling Class

The FIFO scheduling class manages tasks within each major priority level using minor priority subqueues:

```c
struct fifo_subqueue {
    list_node_t head;   // List of sched_entities at this minor priority
    int count;          // Number of tasks in this subqueue
};

struct fifo_rq {
    struct rq rq;                               // Base run queue
    struct fifo_subqueue subqueues[4];          // Subqueues for minor priorities 0-3
    uint8 ready_mask;                           // Bitmask: which subqueues have tasks
};
```

**Task Selection**: Within a major priority level, tasks are selected from the lowest-index non-empty subqueue (minor priority 0 has highest priority within the major level).

**Load Balancing**: The `select_task_rq` callback implements load balancing:
1. Prefer current CPU for cache locality
2. If current CPU's subqueue is empty, use it immediately
3. Otherwise, find the CPU with the lowest task count in the relevant subqueue

### Process States

| State | Meaning |
|-------|---------|
| `PSTATE_UNUSED` | PCB slot is free |
| `PSTATE_RUNNING` | Runnable (on queue or on CPU) |
| `PSTATE_INTERRUPTIBLE` | Sleeping, can be woken by signal |
| `PSTATE_UNINTERRUPTIBLE` | Sleeping, cannot be interrupted |
| `PSTATE_TIMER` | Sleeping with timeout |
| `PSTATE_TIMER_KILLABLE` | Sleeping with timeout, killable |
| `PSTATE_WAKENING` | Transient state during wakeup (race protection) |
| `PSTATE_ZOMBIE` | Exited, waiting for parent to reap |

### on_cpu and on_rq Semantics

A RUNNING process is either:
- **On the ready queue** (`se->on_rq = 1, se->on_cpu = 0`) — waiting to be scheduled
- **On a CPU** (`se->on_rq = 0, se->on_cpu = 1`) — currently executing
- **Transitioning** — briefly both or neither during context switch

A SLEEPING process has:
- `se->on_rq = 0` — not on any run queue
- `se->on_cpu = 0` — not executing (after context switch completes)

## Locking Hierarchy

Locks must be acquired in this order to prevent deadlock:

```
sleep_lock / proc_queue locks  (highest)
    ↓
proc_lock (p->lock)
    ↓
pi_lock (p->sched_entity->pi_lock)
    ↓
rq_lock                        (lowest)
```

### Lock Purposes

| Lock | Purpose |
|------|---------|
| `rq_lock` | Per-CPU run queue lock protecting enqueue/dequeue operations |
| `pi_lock` | Protects wakeup operations (must be held to call wakeup) |
| `proc_lock` | Protects process data fields |
| `sleep_lock` | Protects channel-based sleep queues |

### Critical Rule: No proc_lock During Spin Wait

The wakeup path may spin-wait on `on_cpu`. If the caller held `proc_lock` while waiting,
and the target process needed that lock before clearing `on_cpu`, we'd have deadlock.
Therefore, **wakeup functions require `pi_lock`, not `proc_lock`**.

## Context Switch Flow

### Yielding Process (Switching Out)

```
scheduler_yield():
    1. push_off()                          // Disable interrupts
    2. __do_timer_tick()                   // Check expired timers
    3. rq_lock_current()                   // Acquire current CPU's rq_lock
    4. cur_state = state                   // Cache state for abort check
    5. p = __sched_pick_next()             // Get next process from rq
    6. context_switch_prepare(prev, next)
    7. __switch_to(next)                   // Context switch happens here
    8. context_switch_finish(prev, next)   // On return, we are 'next'
    9. pop_off()
```

### context_switch_prepare()

```c
void context_switch_prepare(struct proc *prev, struct proc *next) {
    // Caller must hold rq_lock
    smp_store_release(&next->sched_entity->on_cpu, 1);  // Mark next as on CPU
    next->sched_entity->cpu_id = cpuid();
    if (PROC_ZOMBIE(prev)) {
        rq_task_dead(prev->sched_entity);  // Clean up exiting process
    }
}
```

### context_switch_finish()

```c
void context_switch_finish(struct proc *prev, struct proc *next) {
    pstate = __proc_get_pstate(prev);       // RE-READ state (critical!)
    struct sched_entity *se = prev->sched_entity;
    int did_enqueue = 0;
    
    if (pstate == PSTATE_RUNNING && !PROC_STOPPED(prev)) {
        rq_put_prev_task(se);               // Put prev back on queue
        smp_store_release(&se->on_rq, 1);   // Mark as on run queue
        did_enqueue = 1;
    } else if (PSTATE_IS_SLEEPING(pstate) || PROC_STOPPED(prev)) {
        if (se->rq != NULL) {
            rq_dequeue_task(se->rq, se);    // Properly dequeue
        }
    }
    rq_unlock_current();                    // Release rq_lock
    
    // Race fix: If state changed to RUNNING after our read, enqueue now.
    // CAS on on_rq serializes with wakeup path — no pi_lock needed here.
    if (!did_enqueue && !PROC_ZOMBIE(prev) && !PROC_STOPPED(prev)) {
        if (__proc_get_pstate(prev) == PSTATE_RUNNING) {
            int expected = 0;
            if (CAS(&se->on_rq, &expected, 1)) {
                rq_lock(...); rq_enqueue_task(rq, se); rq_unlock(...);
            }
            // If CAS failed, waker already enqueued
        }
    }
    
    smp_store_release(&se->on_cpu, 0);      // Now safe for wakers
    rcu_check_callbacks();
}
```

### Task Switch Flow (via sched_class)

```
   Run Queue (data structure)          CPU (current task)
   ┌─────────────────────────┐         ┌─────────────────┐
   │  [A] [B] [C] [D] ...    │         │     prev        │
   └─────────────────────────┘         └─────────────────┘
             │                                  │
             │ pick_next_task(rq)               │
             │ (select next, keep in queue)     │
             ▼                                  │
        next = [A]                              │
             │                                  │
             │ set_next_task(rq, next)          │
             │ (remove next from queue,         │
             │  set as current)                 │
             ▼                                  ▼
   ┌─────────────────────────┐         ┌─────────────────┐
   │  [B] [C] [D] ...        │         │     next        │
   └─────────────────────────┘         └─────────────────┘
             │                                  │
             │         ~~~ context switch ~~~   │
             │         (now running as next)    │
             │                                  │
             │                    put_prev_task(rq, prev)
             │                    (insert prev back to queue,
             │                     unset as current)
             ▼                                  │
   ┌─────────────────────────┐         ┌─────────────────┐
   │  [B] [C] [D] [prev] ... │         │     next        │
   └─────────────────────────┘         └─────────────────┘
```

### Pick Next Process

```c
static struct proc *__sched_pick_next(void) {
    // Caller must hold rq_lock
    
    // Two-layer O(1) lookup for highest priority ready queue
    uint64 top_mask = rq_global.ready_mask[cpu];
    uint64 secondary_mask = rq_global.ready_mask_secondary[cpu];
    
    // Find top-level group (0-7)
    int top_id = bits_ctz8(top_mask);
    // Extract 8-bit group from secondary mask
    uint8 group_bits = (secondary_mask >> (top_id << 3)) & 0xff;
    // Find class within group
    int cls_id = (top_id << 3) + bits_ctz8(group_bits);
    
    struct rq *rq = get_rq_for_cpu(cls_id, cpu);
    struct sched_entity *se = rq_pick_next_task(rq);
    if (se == NULL) {
        return NULL;
    }
    
    struct proc *p = se->proc;
    rq_set_next_task(se);                     // Remove from queue, set as current
    smp_store_release(&se->on_rq, 0);         // Mark as off run queue
    smp_store_release(&se->on_cpu, 1);        // Mark as on CPU
    return p;
}
```

## Wakeup Flow

### Public Wakeup Functions

```c
void wakeup_proc(struct proc *p) {
    spin_lock(&p->sched_entity->pi_lock);
    scheduler_wakeup(p);
    spin_unlock(&p->sched_entity->pi_lock);
}
```

### Internal Wakeup Logic

`__do_scheduler_wakeup()` follows the Linux `try_to_wake_up()` pattern:

```c
static void __do_scheduler_wakeup(struct proc *p) {
    struct sched_entity *se = p->sched_entity;
    
    // Special case: self-wakeup from interrupt
    if (p == myproc()) {
        atomic_cas(&p->state, old_state, PSTATE_RUNNING);
        return;
    }
    
    // 1. Check if already queued (Linux: ttwu_runnable path)
    smp_rmb();
    if (smp_load_acquire(&se->on_rq)) {
        return;  // Already on run queue
    }
    
    // 2. Wait for context switch out (Linux: smp_cond_load_acquire)
    smp_rmb();
    smp_cond_load_acquire(&se->on_cpu, !VAL);
    
    // 3. Atomically claim wakeup (prevent double-enqueue)
    if (!atomic_cas(&p->state, SLEEPING, PSTATE_WAKENING)) {
        return;  // Someone else woke it, or state changed
    }
    
    // 4. Set to RUNNING before enqueue
    smp_store_release(&p->state, PSTATE_RUNNING);
    
    // 5. Select run queue and enqueue (CAS serializes with context_switch_finish)
    if (!PROC_STOPPED(p)) {
        push_off();
        struct rq *rq = rq_select_task_rq(se, se->affinity_mask);
        rq_lock(rq->cpu_id);
        pop_off();
        
        // CAS on on_rq to serialize with context_switch_finish race fix
        int expected = 0;
        if (__atomic_compare_exchange_n(&se->on_rq, &expected, 1, ...)) {
            rq_enqueue_task(rq, se);
        }
        // If CAS failed, context_switch_finish already enqueued
        
        rq_unlock(rq->cpu_id);
    }
}
```

## Memory Ordering

### The Critical Invariant

**Writer (context_switch_finish):**
```
on_rq = 1          (1) First: enqueue (via rq_put_prev_task)
rq_unlock()        (2) Release barrier
on_cpu = 0         (3) Last: signal completion
```

**Reader (waker):**
```
read on_rq         (1) First: check if queued
smp_rmb()          (2) Read barrier
read/wait on_cpu   (3) Last: wait for completion
```

This ensures: **If waker sees `se->on_cpu = 0`, it must also see `se->on_rq = 1` (if it was set).**

### Why This Order?

Without proper ordering, a waker could:
1. See stale `on_cpu = 0` (from cache)
2. See stale `on_rq = 0` (not propagated yet)
3. Conclude: "process is not queued and not on CPU"
4. Attempt to enqueue → **double enqueue bug!**

The `smp_rmb()` between reads, combined with the write ordering (on_rq before on_cpu),
prevents this race.

## Race Condition Handling

### Double Wakeup Race

**Scenario:** Two CPUs try to wake the same process simultaneously.

**Solution:** Atomic CAS on state: `SLEEPING → WAKENING`
- Only one CAS succeeds
- Loser sees `WAKENING` and returns
- Winner proceeds to enqueue

### Wakeup During Context Switch

**Scenario:** Process P is context-switching out. CPU 1 tries to wake P.

**Solution:** Waker spin-waits on `se->on_cpu`
```
CPU 0: P switching out          CPU 1: Waker
────────────────────            ────────────────
on_rq = 1
rq_unlock()
                                spin on on_cpu...
on_cpu = 0 ─────────────────────►spin ends
                                CAS state
                                sees on_rq = 1, returns
```

### Self-Wakeup (Interrupt Wakeup)

**Scenario:** Process P sets `state = SLEEPING` but hasn't switched out yet.
An interrupt completes I/O for P and tries to wake it.

**Solution:** 
1. Interrupt handler sees `p == myproc()`
2. Just does `atomic_cas(&p->state, SLEEPING, RUNNING)`
3. When P reaches `context_switch_finish()`, it re-reads state
4. Sees `RUNNING`, enqueues itself via `rq_put_prev_task()`

```c
void context_switch_finish(...) {
    pstate = __proc_get_pstate(prev);  // RE-READ, not cached!
    if (pstate == PSTATE_RUNNING) {
        rq_put_prev_task(prev->sched_entity);  // Self-enqueue
    }
}
```

### Wakeup vs Sleep Race

**Scenario:** Waker reads state before sleeper sets it to SLEEPING.

**Solution:** State CAS fails
```
Sleeper                         Waker
───────                         ─────
                                read state → RUNNING
state = SLEEPING
yield()                         CAS(RUNNING, WAKENING) → FAIL
                                return (no-op)
```

The sleeper will be on a wait queue; the proper event will wake it later.

## STOPPED Flag

The `stopped` flag is orthogonal to process state. It handles SIGSTOP/SIGCONT:

- `scheduler_stop(p)`: Sets `PROC_STOPPED` flag, requires `proc_lock`
- `scheduler_continue(p)`: Clears flag, re-enqueues if RUNNING, requires `pi_lock`

A stopped process is not enqueued even if RUNNING. When continued, it resumes
from where it was.

### scheduler_continue() Implementation

`scheduler_continue()` uses the same protocol as `__do_scheduler_wakeup()` to safely
re-enqueue a stopped process:

```c
void scheduler_continue(struct proc *p) {
    // Uses pi_lock protocol like __do_scheduler_wakeup.
    // Caller must hold p->sched_entity->pi_lock, must NOT hold proc_lock or rq_lock.
    struct sched_entity *se = p->sched_entity;
    assert(spin_holding(&se->pi_lock));
    assert(!spin_holding(&p->lock));
    assert(!sched_holding());
    
    if (!PROC_STOPPED(p)) {
        return;  // Not stopped, nothing to do
    }
    
    // Wait for process to be off CPU before modifying state.
    // Like Linux's smp_cond_load_acquire in try_to_wake_up().
    smp_cond_load_acquire(&se->on_cpu, !VAL);
    
    PROC_CLEAR_STOPPED(p);
    
    // If process is RUNNING, add it back to the ready queue
    if (__proc_get_pstate(p) == PSTATE_RUNNING) {
        push_off();
        struct rq *rq = rq_select_task_rq(se, se->affinity_mask);
        rq_lock(rq->cpu_id);
        pop_off();
        
        smp_store_release(&se->on_rq, 1);
        rq_enqueue_task(rq, se);
        
        rq_unlock(rq->cpu_id);
    }
}
```

### Caller Pattern (from signal.c)

```c
// In signal_send() when handling SIGCONT:
if (is_cont) {
    // scheduler_continue uses pi_lock protocol.
    proc_unlock(p);
    spin_lock(&p->sched_entity->pi_lock);
    scheduler_continue(p);
    spin_unlock(&p->sched_entity->pi_lock);
    proc_lock(p);
}
```

This pattern mirrors how `signal_notify()` calls `scheduler_wakeup_interruptible()`.

### Why scheduler_continue Needs on_cpu Wait

Previously, `scheduler_continue()` used a different protocol (`proc_lock` + `sched_lock`)
and did NOT wait for `on_cpu == 0`. This was unsafe because:

1. **Race with context switch**: If process P is still running on CPU 0 and CPU 1 sends
   SIGCONT, the old code would add P to the ready queue while P was still on CPU 0.
   
2. **Double scheduling**: CPU 2 could pick P from the ready queue and try to run it,
   while CPU 0 still had P on its CPU.

The fix aligns `scheduler_continue()` with `__do_scheduler_wakeup()`:
- Both use `pi_lock` (avoiding deadlock with `proc_lock`)
- Both wait for `on_cpu == 0` before enqueuing
- Both acquire `rq_lock` internally via `rq_lock()`/`rq_unlock()`

## CPU Affinity

The scheduler supports CPU affinity via `sched_entity.affinity_mask`. When enqueuing a task:

1. `rq_select_task_rq()` selects a run queue on an allowed CPU
2. The task is enqueued to that CPU's run queue
3. If affinity changes while running, the task stays on the current CPU until it sleeps
4. On wakeup, `rq_select_task_rq()` places it on an allowed CPU

### Active CPU Tracking

The scheduler tracks which CPUs are active at runtime via `rq_cpu_activate()`:

```c
// Called from idle_proc_init() on each CPU
void rq_cpu_activate(int cpu);

// Get bitmask of active CPUs
uint64 rq_get_active_cpu_mask(void);
```

When selecting a run queue, `rq_select_task_rq()` masks the requested cpumask with
the active CPU mask to ensure tasks are never placed on inactive CPUs:

```c
cpumask_t effective_mask = cpumask & rq_global.active_cpu_mask;
if (effective_mask == 0) {
    effective_mask = rq_global.active_cpu_mask;  // Fallback to all active CPUs
}
```

This is essential when `NCPU` (compile-time constant) exceeds the actual number of
CPUs available at runtime (e.g., NCPU=8 but QEMU runs with 3 CPUs).

## Sleep Path

### scheduler_sleep()

```c
void scheduler_sleep(struct spinlock *lk, enum procstate sleep_state) {
    __proc_set_pstate(myproc(), sleep_state);  // Set to SLEEPING
    
    if (lk_holding) {
        spin_unlock(lk);
    }
    
    scheduler_yield();  // Context switch out
    
    if (lk_holding) {
        spin_lock(lk);
    }
}
```

### Channel-Based Sleep (sleep_on_chan)

```c
void sleep_on_chan(void *chan, struct spinlock *lk) {
    sleep_lock();
    myproc()->chan = chan;
    PROC_SET_ONCHAN(myproc());
    
    // Add to wait tree, releases sleep_lock during yield
    proc_tree_wait(&__chan_queue_root, chan, &__sleep_lock, NULL);
    
    PROC_CLEAR_ONCHAN(myproc());
    myproc()->chan = NULL;
    sleep_unlock();
}
```

## RCU Integration

The scheduler integrates with RCU (Read-Copy-Update) for grace period tracking:

- **Quiescent States**: `rcu_check_callbacks()` is called from `context_switch_finish()`
  to record that the CPU passed through a quiescent state (context switch).
  
- **Per-CPU RCU Kthreads**: RCU callback processing is handled by per-CPU kthreads,
  separate from the scheduler path. This avoids latency issues from processing
  callbacks during context switches.

- **RCU Read Sections**: Processes track their RCU read-side critical section nesting
  via `p->rcu_read_lock_nesting`. A process with `rcu_read_lock_nesting > 0` can safely
  migrate CPUs.

## Summary: The Complete Picture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              SLEEP PATH                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│  1. state = SLEEPING                                                         │
│  2. yield() → rq_lock_current() → pick_next()                               │
│  3. context_switch_prepare: se->on_cpu = 1                                  │
│  4. __switch_to()                                                            │
│  5. context_switch_finish:                                                   │
│       - RE-READ state                                                        │
│       - if RUNNING: rq_put_prev_task() → se->on_rq = 1                      │
│       - rq_unlock_current()                                                  │
│       - Race fix: if state now RUNNING, CAS(on_rq, 0→1) and enqueue         │
│       - se->on_cpu = 0                                                       │
│       - rcu_check_callbacks()                                                │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                              WAKEUP PATH                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  1. Acquire se->pi_lock                                                      │
│  2. Check se->on_rq → if set, return (already queued)                       │
│  3. smp_rmb()                                                                │
│  4. Wait for se->on_cpu == 0                                                │
│  5. CAS state: SLEEPING → WAKENING                                          │
│  6. state = RUNNING                                                          │
│  7. rq_select_task_rq() → rq_lock(); CAS(on_rq, 0→1); rq_enqueue_task()    │
│  8. rq_unlock(); release se->pi_lock                                        │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                           KEY INVARIANTS                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  • se->on_rq = 1 always happens BEFORE se->on_cpu = 0 (write order)         │
│  • Waker reads on_rq BEFORE on_cpu (read order matches write order)         │
│  • State CAS prevents double-enqueue from multiple wakers                    │
│  • context_switch_finish RE-READS state for interrupt wakeups               │
│  • on_rq changes via CAS to serialize waker vs context_switch_finish        │
│  • Wakeup AND continue require pi_lock (not proc_lock) to avoid deadlock    │
│  • Both wakeup and continue wait for on_cpu == 0 before enqueuing           │
│  • CPU affinity is respected via rq_select_task_rq()                         │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Wakeup-Context Switch Deadlock Analysis (January 2026)

This section documents a subtle deadlock discovered during testing and its resolution.

### The Problem: on_cpu Spin Deadlock

During stress testing (e.g., `usertests`), a deadlock was observed where multiple CPUs
would hang in the wakeup path:

```
Thread 3: __do_scheduler_wakeup spinning on smp_cond_load_acquire(&se->on_cpu, !VAL)
          - Holding: pi_lock for process P
          - Waiting: P->on_cpu to become 0

Thread 2: context_switch_finish trying to acquire pi_lock for process P
          - Stuck: waiting for pi_lock
          - Hasn't cleared: P->on_cpu yet
```

#### Root Cause Analysis

The deadlock occurred because:

1. **Waker** (in `__do_scheduler_wakeup`): Holds `pi_lock`, spins indefinitely on `on_cpu`
2. **Target** (in `context_switch_finish`): Needs `pi_lock` to handle race fix path, 
   but can't acquire it, so can't proceed to clear `on_cpu`

This created a circular dependency:
- Waker needs `on_cpu = 0` to proceed → Target must clear it
- Target needs `pi_lock` to complete → Waker is holding it

#### Why Did context_switch_finish Need pi_lock?

A race fix was added to handle the following scenario:

```
Waker (CPU 1)                           Target (CPU 0, in context_switch_finish)
============                            ========================================
CAS state = RUNNING                     read state → sees SLEEPING (stale)
spin on on_cpu...                       dequeue from rq (or skip enqueue)
                                        release rq_lock
                                        ← waker's CAS happened before here
                                        on_cpu = 0
spin ends
check on_rq → 0!
enqueue task ← CORRECT
```

But if the waker gave up (bounded spin) or wasn't fast enough:

```
Waker (CPU 1)                           Target (CPU 0)
============                            ==============
                                        read state → SLEEPING
                                        skip enqueue
                                        release rq_lock
CAS state = RUNNING                     
spin on on_cpu...                       on_cpu = 0
spin ends                               ← TARGET IS GONE FROM CPU
check on_rq → 0
return (rely on target)                 ← OOPS! Target never enqueues!
```

The race fix had `context_switch_finish` re-read state after releasing `rq_lock` and
enqueue if state was now RUNNING. But this required serialization with wakeup paths,
which was implemented using `pi_lock` — causing the deadlock.

### The Solution: CAS on on_rq

The key insight: **We don't need a lock to serialize; we need an atomic operation.**

Instead of using `pi_lock` in `context_switch_finish`, we use **CAS on on_rq** to
ensure exactly one path (waker OR context_switch_finish) succeeds in enqueuing:

#### context_switch_finish (race fix path)

```c
// Race fix: If state changed to RUNNING after our initial read, enqueue now.
// CAS on on_rq serializes with wakeup path — no pi_lock needed here.
if (!did_enqueue && !PROC_ZOMBIE(prev) && !PROC_STOPPED(prev)) {
    enum procstate new_pstate = __proc_get_pstate(prev);
    if (new_pstate == PSTATE_RUNNING) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&se->on_rq, &expected, 1,
                                        false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            // We won the CAS — enqueue the task
            rq_lock(...);
            rq_enqueue_task(rq, se);
            rq_unlock(...);
        }
        // If CAS failed, waker already set on_rq = 1 and will/did enqueue
    }
}

// Now safe to clear on_cpu
smp_store_release(&se->on_cpu, 0);
```

#### __do_scheduler_wakeup (waker path)

```c
// Wait for on_cpu to clear — no deadlock since target doesn't need pi_lock
smp_cond_load_acquire(&se->on_cpu, !VAL);

// Claim wakeup via state CAS
CAS state: SLEEPING → WAKENING → RUNNING

// CAS on on_rq to serialize with context_switch_finish race fix
rq_lock(...);
int expected = 0;
if (__atomic_compare_exchange_n(&se->on_rq, &expected, 1,
                                false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
    rq_enqueue_task(rq, se);
}
// If CAS failed, context_switch_finish already enqueued
rq_unlock(...);
```

### Why This Works

1. **No pi_lock in context_switch_finish**: Target doesn't need any lock that waker holds,
   so waker can spin on `on_cpu` freely without deadlock.

2. **CAS provides mutual exclusion**: Exactly one of {waker, context_switch_finish}
   wins the `CAS(on_rq, 0, 1)` and performs the enqueue.

3. **Both paths are correct**:
   - If waker wins: Target's CAS fails, target knows waker handled it
   - If target wins: Waker's CAS fails, waker knows target handled it
   - Either way, the process is enqueued exactly once

4. **Memory ordering preserved**: The CAS uses `__ATOMIC_SEQ_CST` for full ordering.

### Key Lessons Learned

1. **Avoid holding locks while spinning**: If A holds lock L and spins waiting for B,
   and B needs L to make progress, deadlock is inevitable.

2. **CAS is sufficient for mutual exclusion**: When serializing two paths that each
   should execute at most once, CAS on a flag is simpler and deadlock-free.

3. **Separate serialization concerns**: 
   - State CAS (`SLEEPING → WAKENING`) serializes multiple wakers
   - on_rq CAS serializes waker vs context_switch_finish race fix
   - pi_lock serializes wakeup path internals (but not context_switch_finish)

4. **The pi_lock purpose**: pi_lock protects against races during the wakeup path
   (e.g., multiple CPUs trying to wake the same process). It should NOT be used
   in context_switch_finish because that creates the deadlock.

### Updated Invariants

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                    UPDATED KEY INVARIANTS                                    │
├─────────────────────────────────────────────────────────────────────────────┤
│  • on_rq changes via CAS(__ATOMIC_SEQ_CST) to serialize waker vs switch     │
│  • Wakeup holds pi_lock but context_switch_finish does NOT acquire it       │
│  • Waker can safely spin on on_cpu — no deadlock possible                   │
│  • Exactly one of {waker, context_switch_finish} enqueues via on_rq CAS     │
│  • State CAS (SLEEPING→WAKENING) prevents multiple wakers                   │
└─────────────────────────────────────────────────────────────────────────────┘
```
