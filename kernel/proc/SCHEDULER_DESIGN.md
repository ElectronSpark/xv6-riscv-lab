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

### on_cpu and on_rq Semantics (Matching Linux Kernel)

`on_rq` indicates whether the task is logically part of a run queue:
- `on_rq = 1`: Task is runnable (either waiting in queue OR currently executing)
- `on_rq = 0`: Task is sleeping or dead

`on_cpu` indicates whether the task is physically running on a CPU:
- `on_cpu = 1`: Task is currently executing on a CPU
- `on_cpu = 0`: Task is not executing

**State combinations:**
- **Waiting in queue** (`on_rq = 1, on_cpu = 0`) — runnable, waiting to be picked
- **Running on CPU** (`on_rq = 1, on_cpu = 1`) — currently executing
- **Sleeping** (`on_rq = 0, on_cpu = 0`) — dequeued, waiting for wakeup
- **Transitioning** (`on_rq = 0, on_cpu = 1`) — brief window during context_switch_finish
  when task is being dequeued but hasn't cleared on_cpu yet

This matches Linux kernel behavior where tasks stay "on rq" while running.

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
| `pi_lock` | Serializes concurrent wakeup attempts (acquired internally by wakeup) |
| `proc_lock` | Protects process data fields |
| `sleep_lock` | Protects channel-based sleep queues |

### Critical Rule: No proc_lock During Spin Wait

The wakeup path may spin-wait on `on_cpu`. If the caller held `proc_lock` while waiting,
and the target process needed that lock before clearing `on_cpu`, we'd have deadlock.
Therefore, **wakeup functions must NOT be called with proc_lock held**.

### trylock Functions for Lock Convoy Prevention

To avoid lock convoy when many processes wake the same target (e.g., many children
exiting and waking the same parent), wakeup uses trylock with backoff:

| Function | Purpose |
|----------|---------|
| `rq_trylock(cpu_id)` | Try to acquire rq_lock without spinning; returns 1 if acquired |
| `rq_trylock_two(cpu1, cpu2)` | Try to acquire two rq_locks atomically (ordered to prevent deadlock) |

**Pattern:** `__do_scheduler_wakeup` acquires `pi_lock`, then uses `rq_trylock_two()`.
If trylock fails, it releases `pi_lock`, backs off with `cpu_relax()`, then retries.

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
void context_switch_finish(struct proc *prev, struct proc *next, int intr) {
    pstate = __proc_get_pstate(prev);       // RE-READ state (critical!)
    struct sched_entity *se = prev->sched_entity;
    
    if (prev != mycpu()->idle_proc) {
        if (pstate == PSTATE_RUNNING) {
            rq_put_prev_task(se);           // Put prev back in scheduler list
        } else if (PSTATE_IS_SLEEPING(pstate) || pstate == PSTATE_STOPPED) {
            if (se->rq != NULL) {
                rq_dequeue_task(se->rq, se); // Dequeue: sets on_rq=0
            }
        }
    }
    
    smp_store_release(&se->on_cpu, 0);      // Now safe for wakers
    rq_unlock_current_irqrestore(intr);     // Release rq_lock
    rcu_check_callbacks();
}
```

**Key insight:** The rq_lock is held throughout state checking and on_cpu clearing.
This serializes with wakeup, which also acquires the rq_lock before checking on_rq/on_cpu.

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
    rq_set_next_task(se);                     // Detach from list (on_rq stays 1!)
    smp_store_release(&se->on_cpu, 1);        // Mark as on CPU
    return p;
}
```

**Note:** Unlike older designs, `on_rq` stays 1 while running (matching Linux).
The task is removed from the scheduler's internal list but is still logically
"on the run queue". This simplifies wakeup - if on_rq=1, no enqueue needed.
```

## Wakeup Flow

### Public Wakeup Functions

```c
void wakeup_proc(struct proc *p) {
    // Note: pi_lock is acquired internally by __do_scheduler_wakeup
    scheduler_wakeup(p);
}
```

### Internal Wakeup Logic

`__do_scheduler_wakeup()` follows the Linux `try_to_wake_up()` pattern with critical
improvements: **internal pi_lock acquisition** and **trylock with backoff** to prevent
lock convoy when many processes wake the same target (e.g., many children exiting
simultaneously, all waking the same parent).

**Key change:** `pi_lock` is now acquired **inside** `__do_scheduler_wakeup()`, not
by external callers. This, combined with `rq_trylock_two()`, avoids the convoy scenario
where processes serialize on pi_lock while spinning on rq_lock.

```c
static void __do_scheduler_wakeup(struct proc *p, bool from_stopped) {
    struct sched_entity *se = p->sched_entity;
    
    // Acquire pi_lock to serialize wakeup attempts on this process
    spin_lock(&se->pi_lock);
    
    // Special case: self-wakeup from interrupt
    // Current process set SLEEPING but hasn't switched out yet.
    // Just change state back - it will see this before context switching.
    if (p == myproc()) {
        smp_store_release(&p->state, PSTATE_RUNNING);
        spin_unlock(&se->pi_lock);
        return;
    }
    
    // Validate state is sleepable
    enum procstate old_state = smp_load_acquire(&p->state);
    if (!PSTATE_IS_SLEEPING(old_state)) {
        spin_unlock(&se->pi_lock);
        return;
    }
    
retry:
    // Read origin CPU and select target CPU
    push_off();
    struct rq *rq = rq_select_task_rq(se, se->affinity_mask);
    int origin_cpuid = smp_load_acquire(&se->cpu_id);
    int target_cpu = rq->cpu_id;
    if (origin_cpuid < 0) {
        origin_cpuid = target_cpu;  // New task, no origin to serialize with
    }
    
    // Use trylock to avoid spinning while holding pi_lock (prevents lock convoy)
    if (!rq_trylock_two(origin_cpuid, target_cpu)) {
        pop_off();
        // Couldn't get rq locks - release pi_lock, backoff, re-check state
        spin_unlock(&se->pi_lock);
        for (volatile int i = 0; i < 10; i++) cpu_relax();
        spin_lock(&se->pi_lock);
        // Re-check state after reacquiring pi_lock
        if (!PSTATE_IS_SLEEPING(smp_load_acquire(&p->state))) {
            spin_unlock(&se->pi_lock);
            return;
        }
        goto retry;
    }
    pop_off();
    
    // **Critical:** Re-check cpu_id after acquiring locks.
    // If task migrated between read and lock, we have the wrong lock!
    int current_cpuid = smp_load_acquire(&se->cpu_id);
    if (current_cpuid >= 0 && current_cpuid != origin_cpuid) {
        rq_unlock_two(origin_cpuid, target_cpu);
        // Release pi_lock, re-acquire and re-check state
        spin_unlock(&se->pi_lock);
        spin_lock(&se->pi_lock);
        if (!PSTATE_IS_SLEEPING(smp_load_acquire(&p->state))) {
            spin_unlock(&se->pi_lock);
            return;
        }
        goto retry;  // Retry with correct locks
    }
    
    smp_store_release(&p->state, PSTATE_WAKENING);
    
    // Linux ttwu pattern - check on_rq FIRST (under lock):
    if (smp_load_acquire(&se->on_rq)) {
        // Task logically on rq (on_rq=1 while running).
        // Just set RUNNING - put_prev_task will add back to list.
        smp_store_release(&p->state, PSTATE_RUNNING);
        spin_unlock(&se->pi_lock);
        rq_unlock_two(origin_cpuid, target_cpu);
        return;
    }
    
    // on_rq=0: check if still on CPU (brief dequeue→on_cpu=0 window)
    if (smp_load_acquire(&se->on_cpu)) {
        // Task switching out, use wake_list for deferred enqueue
        rq_add_wake_list(origin_cpuid, se);
        spin_unlock(&se->pi_lock);
        rq_unlock_two(origin_cpuid, target_cpu);
        ipi_send_single(origin_cpuid, IPI_REASON_RESCHEDULE);
        return;
    }
    
    // on_rq=0 and on_cpu=0: task fully off CPU, enqueue directly
    rq_enqueue_task(rq, se);
    spin_unlock(&se->pi_lock);
    rq_unlock_two(origin_cpuid, target_cpu);
}
```

### The CPU Migration Race

The retry loop fixes a subtle race condition:

```
CPU 0 (task P running)              CPU 1 (waker)
─────────────────────               ─────────────────
                                    origin = se->cpu_id (= 0)
P yields, context_switch
  to different CPU (migrated)       
se->cpu_id = 2                      rq_lock_two(0, target)
                                    // Locked CPU 0's rq, but P is on CPU 2!
context_switch_finish on CPU 2
  reads state = INTERRUPTIBLE
  dequeues P (on_rq = 0)
  releases CPU 2's rq_lock
                                    // Stale: sees on_rq=1 (cached)
                                    // Sets RUNNING and returns
                                    // BUG: P not in any list!
```

**Fix:** After locking, re-check `cpu_id`. If it changed, release and retry:
```c
if (current_cpuid != origin_cpuid) {
    rq_unlock_two(...);
    goto retry;
}
```

## Memory Ordering

### The Critical Invariant

With rq_lock-based serialization, memory ordering is simpler:
- All on_rq/on_cpu reads in wakeup happen **under rq_lock**
- All on_rq/on_cpu writes in context_switch_finish happen **under rq_lock**
- The lock provides the necessary memory barriers

**Writer (context_switch_finish) under rq_lock:**
```
if SLEEPING: rq_dequeue_task() → on_rq = 0
smp_store_release(&on_cpu, 0)
rq_unlock()                     // Release barrier
```

**Reader (wakeup) under rq_lock:**
```
rq_lock_two(origin, target)     // Acquire barrier, waits for unlock
read on_rq                      // Sees consistent state
read on_cpu
... decide path ...
```

This ensures: **If wakeup holds the origin CPU's rq_lock, it sees the final
state written by context_switch_finish (which held the same lock).**

## Race Condition Handling

### Double Wakeup Race

**Scenario:** Two CPUs try to wake the same process simultaneously.

**Solution:** Both acquire the same rq_lock (origin CPU's lock), serializing access.
The first waker to acquire the lock sets state to WAKENING under the lock.
The second waker sees non-sleeping state and returns early.

### Wakeup During Context Switch (on_rq=1 path)

**Scenario:** Process P is running, sets state=SLEEPING, yields. Waker runs.

```
CPU 0: P yielding               CPU 1: Waker
────────────────────            ────────────────
state = SLEEPING
rq_lock_current()
pick_next(), switch             rq_lock_two(origin=0, target)
                                // blocked on CPU 0's lock
context_switch_finish:
  reads state = SLEEPING
  dequeues (on_rq = 0)
  on_cpu = 0
rq_unlock()
                                // lock acquired
                                state = WAKENING
                                sees on_rq = 0, on_cpu = 0
                                enqueues directly
```

### Wakeup During Context Switch (on_rq=1, waker first)

**Scenario:** Waker acquires lock before context_switch_finish dequeues.

```
CPU 0: P yielding               CPU 1: Waker
────────────────────            ────────────────
state = SLEEPING
(P still has on_rq=1, on_cpu=1)
context_switch to next
                                rq_lock_two(origin=0, target)
                                state = WAKENING
                                sees on_rq = 1
                                → sets RUNNING, returns
context_switch_finish on next:
  re-reads state = RUNNING
  calls rq_put_prev_task()
  P is back in scheduler list ✓
```

### CPU Migration Race

**Scenario:** Task migrated between reading cpu_id and locking.

**Solution:** Retry loop with re-check after lock acquisition.
```
Waker                           context_switch_finish
─────                           ─────────────────────
origin = cpu_id (= 0)           
                                task migrates to CPU 2
rq_lock_two(0, target)          context_switch_finish on CPU 2
  // has CPU 0's lock             holds CPU 2's lock
                                  dequeues, clears on_cpu
current_cpuid = 2
2 != 0 → unlock, retry
origin = cpu_id (= 2)
rq_lock_two(2, target)
  // now correct lock
sees on_rq = 0, on_cpu = 0
enqueues correctly ✓
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
- `scheduler_wakeup_stopped(p)`: Clears flag, re-enqueues if RUNNING (pi_lock acquired internally)

A stopped process is not enqueued even if RUNNING. When continued, it resumes
from where it was.

### Caller Pattern (from signal.c)

```c
// In signal_send() when handling SIGCONT:
if (is_cont) {
    // Note: pi_lock no longer needed - rq_lock serializes wakeups
    proc_unlock(p);
    scheduler_wakeup_stopped(p);
    proc_lock(p);
}
```

This pattern mirrors how `signal_notify()` calls `scheduler_wakeup_interruptible()`.
Note that pi_lock is no longer required externally - it's acquired internally by the
wakeup functions.

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
│  2. yield() → rq_lock_current() → pick_next() → set_next_task()             │
│  3. context_switch_prepare: next->on_cpu = 1                                │
│  4. __switch_to()                                                            │
│  5. context_switch_finish (on new process's stack):                          │
│       - RE-READ prev state (under rq_lock)                                   │
│       - if RUNNING: rq_put_prev_task() → add back to list                   │
│       - if SLEEPING: rq_dequeue_task() → on_rq = 0                          │
│       - smp_store_release(&on_cpu, 0)                                       │
│       - rq_unlock_current()                                                  │
│       - rcu_check_callbacks()                                                │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                              WAKEUP PATH                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  1. [Internal] Acquire se->pi_lock                                           │
│  2. Self-wakeup check: if p == myproc(), set RUNNING, release pi_lock       │
│  3. Validate state is sleepable                                              │
│  4. retry: Read origin_cpu = se->cpu_id, select target_cpu                  │
│  5. rq_trylock_two(origin_cpu, target_cpu) - if fail, release pi_lock,      │
│     backoff, re-acquire pi_lock, re-check state, goto retry                 │
│  6. Re-check cpu_id: if changed, unlock, release/re-acquire pi_lock,        │
│     re-check state, goto retry                                               │
│  7. state = WAKENING                                                         │
│  8. if on_rq=1: state = RUNNING, release pi_lock, rq_unlock_two, return     │
│  9. if on_cpu=1: add to wake_list, release pi_lock, rq_unlock_two,          │
│     send IPI, return                                                         │
│ 10. else: rq_enqueue_task(), release pi_lock, rq_unlock_two                 │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                           KEY INVARIANTS                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  • on_rq = 1 while running (task logically on rq, detached from list)       │
│  • on_rq = 0 only when sleeping (dequeued via rq_dequeue_task)              │
│  • rq_lock serializes wakeup with context_switch_finish                     │
│  • Retry loop handles cpu_id changing between read and lock                  │
│  • pi_lock acquired INTERNALLY by wakeup, not by callers                    │
│  • trylock + backoff avoids lock convoy when many wake same target          │
│  • CPU affinity respected via rq_select_task_rq()                            │
│  • wake_list handles the brief on_rq=0, on_cpu=1 window during dequeue      │
└─────────────────────────────────────────────────────────────────────────────┘
```

## Historical Notes: Evolution of Wakeup Synchronization

This section documents key design decisions and bugs encountered during development.

### The on_cpu Spin Deadlock Problem (Resolved)

An earlier design used `smp_cond_load_acquire(&se->on_cpu, !VAL)` to spin-wait for
the target to finish context switching. This caused deadlocks when combined with
lock requirements.

**Root cause:** Waker held `pi_lock` while spinning; target needed `pi_lock` to proceed.

**Solution:** Replace spin-wait with lock-based serialization. The wakeup path now:
1. Locks the origin CPU's rq (where task is/was)
2. Checks on_rq/on_cpu under the lock
3. Serializes with context_switch_finish (which holds the same lock)

### The CPU Migration Race (Resolved January 2026)

**Symptom:** Parent process stuck in RUNNING state but never scheduled.

**Root cause:** Wakeup read `cpu_id` before locking, but task migrated before lock acquired:
- Wakeup locked CPU 0's rq (stale cpu_id)
- context_switch_finish ran on CPU 2 (actual location), dequeued task
- Wakeup saw stale `on_rq=1`, set RUNNING and returned
- Task was dequeued but not in any scheduler list

**Solution:** Retry loop with re-check after lock acquisition:
```c
retry:
    origin_cpuid = se->cpu_id;
    rq_lock_two(origin_cpuid, target_cpu);
    
    current_cpuid = se->cpu_id;  // Re-read under lock
    if (current_cpuid != origin_cpuid) {
        rq_unlock_two(...);
        goto retry;  // Locked wrong rq, retry
    }
```

### Why on_rq Stays 1 While Running (Linux Pattern)

Earlier xv6 designs set `on_rq=0` when picking a task to run. This complicated wakeup:
- Wakeup couldn't tell if task was "running" vs "sleeping and dequeued"
- Required complex state machine with CAS operations

Linux keeps `on_rq=1` while running:
- Task is removed from scheduler's internal list (via `set_next_task`)
- But logically still "on the run queue" (counts, flags remain)
- Wakeup sees `on_rq=1` → knows task will be re-added by `put_prev_task`
- Simplifies wakeup: just set state to RUNNING and return

### The Current Solution: Lock-Based Serialization with Retry

The final design avoids both spinning and CAS complexity by using proper locking:

1. **Lock the origin CPU's rq**: Wakeup locks the rq where the task currently is
   (read from `cpu_id`), which is the same lock held by `context_switch_finish`.

2. **Retry if cpu_id changed**: After locking, re-check `cpu_id`. If it changed
   (task migrated), release locks and retry with the correct rq.

3. **Read on_rq/on_cpu under lock**: All checks happen with the origin rq locked,
   ensuring visibility of `context_switch_finish`'s writes.

This provides the correctness of spin-waiting (wakeup sees final state) without
the deadlock risk (no spinning while holding locks).

### Key Lessons Learned

1. **Lock both source and destination**: For task migration/wakeup, lock the CPU
   where the task currently is (to serialize with context_switch_finish), not just
   the target CPU.

2. **Re-validate after locking**: Any value read before acquiring a lock may be
   stale. Re-check critical values after locking.

3. **Prefer lock retry over spin-wait**: Spin-waiting while holding locks is
   dangerous. A retry loop that releases and re-acquires locks is safer.

4. **on_rq=1 while running simplifies logic**: Following Linux's pattern reduces
   the number of state transitions and race conditions to handle.

5. **Avoid lock convoy with trylock+backoff**: When many processes wake the same
   target, blocking on locks creates a convoy that serializes all wakers.

### The Lock Convoy Problem (Resolved January 2026)

**Symptom:** `cowtest forkfork` hung on 8-core hardware. Spinlock panic showed
many cores blocked on `pi_lock`, `rq_lock`, and `sleep_lock`.

**Multiple contributing factors:**

1. **Wakeup convoy (pi_lock + rq_lock):** When many children exit simultaneously
   and wake the same parent, all compete for `pi_lock` then `rq_lock`. Each process
   must wait for all previous ones to complete, causing severe serialization.

2. **Global sleep_lock contention:** Filesystem operations (checking file existence
   to decide whether to continue forking) used `sleep_on_chan()` which acquires the
   global `sleep_lock`. With many processes doing concurrent FS operations, this
   single lock became a severe bottleneck, serializing all waiters.

**The convoy effect (wakeup path):**
```
Child 1: [holds pi_lock] [spins on rq_lock.......] [enqueues] [releases]
Child 2:                 [waits for pi_lock.......] [spins on rq_lock...] ...
Child 3:                                            [waits for pi_lock...] ...
```

**The sleep_lock bottleneck (filesystem path):**
```
Process A: [holds sleep_lock] [FS operation...] [releases]
Process B:                    [spins on sleep_lock.............] ...
Process C:                    [spins on sleep_lock.............] ...
...all processes serialize on global lock...
```

**Solutions applied:**

1. **Wakeup path:** Move `pi_lock` inside `__do_scheduler_wakeup()`, use
   `rq_trylock_two()` with backoff. Concurrent wakers don't block each other.

2. **Filesystem/driver paths:** Replace `sleep_on_chan()` with per-subsystem
   `proc_queue_t` wait queues:
   - `log.c`: Per-log `wait_queue` instead of `sleep_on_chan(log, ...)`
   - `virtio_disk.c`: Per-disk `desc_wait_queue` for descriptor allocation
   - Wakeup uses temp queue pattern: `proc_queue_bulk_move()` then wake outside lock

**Key insight:** Global locks that seem innocuous become critical bottlenecks
at higher core counts. Per-subsystem queues eliminate false sharing and allow
independent subsystems to operate in parallel.
