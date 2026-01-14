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
    int class_id;           // Scheduling class ID
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

Currently implemented scheduling classes:
- **FIFO** (`fifo_rq`): Simple first-in-first-out scheduling for regular tasks
- **IDLE** (`idle_rq`): Idle process scheduling (lowest priority)

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
    
    if (pstate == PSTATE_RUNNING && !PROC_STOPPED(prev)) {
        rq_put_prev_task(se);               // Put prev back on queue
        smp_store_release(&se->on_rq, 1);   // Mark as on run queue
    } else if (PSTATE_IS_SLEEPING(pstate) || PROC_STOPPED(prev)) {
        if (se->rq != NULL) {
            rq_dequeue_task(se->rq, se);    // Properly dequeue
        }
    }
    rq_unlock_current();                    // Release rq_lock
    
    smp_store_release(&se->on_cpu, 0);      // Now safe for wakers
    
    // Handle zombie parent wakeup
    // Note quiescent state for RCU - context switch is a quiescent state
    // Callback processing is handled by per-CPU RCU kthreads
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
    struct rq *rq = pick_next_rq();  // Find rq with ready tasks
    if (IS_ERR_OR_NULL(rq)) {
        return NULL;
    }
    
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
    spin_acquire(&p->sched_entity->pi_lock);
    scheduler_wakeup(p);
    spin_release(&p->sched_entity->pi_lock);
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
    
    // 5. Select run queue and enqueue
    if (!PROC_STOPPED(p)) {
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
    spin_acquire(&p->sched_entity->pi_lock);
    scheduler_continue(p);
    spin_release(&p->sched_entity->pi_lock);
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

## Sleep Path

### scheduler_sleep()

```c
void scheduler_sleep(struct spinlock *lk, enum procstate sleep_state) {
    __proc_set_pstate(myproc(), sleep_state);  // Set to SLEEPING
    
    if (lk_holding) {
        spin_release(lk);
    }
    
    scheduler_yield();  // Context switch out
    
    if (lk_holding) {
        spin_acquire(lk);
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
│  7. rq_select_task_rq() → rq_lock(); se->on_rq = 1; rq_enqueue_task()       │
│  8. rq_unlock(); release se->pi_lock                                        │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                           KEY INVARIANTS                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  • se->on_rq = 1 always happens BEFORE se->on_cpu = 0 (write order)         │
│  • Waker reads on_rq BEFORE on_cpu (read order matches write order)         │
│  • State CAS prevents double-enqueue from multiple wakers                    │
│  • context_switch_finish RE-READS state for interrupt wakeups               │
│  • on_rq changes only under rq_lock                                          │
│  • Wakeup AND continue require pi_lock (not proc_lock) to avoid deadlock    │
│  • Both wakeup and continue wait for on_cpu == 0 before enqueuing           │
│  • CPU affinity is respected via rq_select_task_rq()                         │
└─────────────────────────────────────────────────────────────────────────────┘
```
