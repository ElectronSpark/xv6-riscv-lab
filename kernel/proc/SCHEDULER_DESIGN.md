# xv6 Scheduler Design

This document describes the scheduler design in xv6, modeled after the Linux kernel's
`try_to_wake_up()` pattern from `kernel/sched/core.c`.

## Overview

The xv6 scheduler uses a **direct process-to-process context switch** model (no dedicated
scheduler thread). When a process yields or sleeps, it directly switches to the next
runnable process. The scheduler maintains correctness on SMP systems through careful
ordering of flag updates and memory barriers.

## Key Data Structures

### Per-Process Scheduling Flags

```c
struct proc {
    int on_cpu;              // 1 if currently executing on a CPU
    int on_rq;               // 1 if on the ready queue
    enum procstate state;    // RUNNING, SLEEPING, ZOMBIE, etc.
    int stopped;             // SIGSTOP/SIGCONT flag (orthogonal to state)
    list_node_t sched_entry; // Link in ready queue
    ...
};
```

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
- **On the ready queue** (`on_rq = 1, on_cpu = 0`) — waiting to be scheduled
- **On a CPU** (`on_rq = 0, on_cpu = 1`) — currently executing
- **Transitioning** — briefly both or neither during context switch

A SLEEPING process has:
- `on_rq = 0` — not on any run queue
- `on_cpu = 0` — not executing (after context switch completes)

## Locking Hierarchy

Locks must be acquired in this order to prevent deadlock:

```
sleep_lock / proc_queue locks  (highest)
    ↓
proc_lock (p->lock)
    ↓
pi_lock (p->pi_lock)
    ↓
sched_lock                     (lowest)
```

### Lock Purposes

| Lock | Purpose |
|------|---------|
| `sched_lock` | Protects ready queue operations (misnomer: it's really a run-queue lock, not a general scheduler lock) |
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
    3. cur_state = state                   // Cache state for abort check
    4. p = __sched_pick_next()             // Get next process from queue
    5. context_switch_prepare(prev, next)
    6. __switch_to(next)                   // Context switch happens here
    7. context_switch_finish(prev, next)   // On return, we are 'next'
    8. pop_off()
```

### context_switch_prepare()

```c
void context_switch_prepare(struct proc *prev, struct proc *next) {
    smp_store_release(&next->on_cpu, 1);   // Mark next as on CPU
    sched_lock();                           // Acquire scheduler lock
    next->cpu_id = cpuid();
}
```

### context_switch_finish()

```c
void context_switch_finish(struct proc *prev, struct proc *next) {
    pstate = __proc_get_pstate(prev);       // RE-READ state (critical!)
    
    if (pstate == PSTATE_RUNNING && !PROC_STOPPED(prev)) {
        __scheduler_add_ready(prev);        // on_rq = 1 (inside sched_lock)
    }
    sched_unlock();
    
    smp_store_release(&prev->on_cpu, 0);    // Now safe for wakers
    
    // Handle zombie parent wakeup, RCU callbacks, etc.
}
```

### Pick Next Process

```c
static struct proc *__sched_pick_next(void) {
    sched_lock();
    p = list_node_pop_back(&ready_queue, ...);
    if (p) {
        smp_store_release(&p->on_rq, 0);    // Dequeue (inside lock)
    }
    sched_unlock();
    
    if (p) {
        smp_store_release(&p->on_cpu, 1);   // Mark as running
    }
    return p;
}
```

## Wakeup Flow

### Public Wakeup Functions

```c
void wakeup_proc(struct proc *p) {
    spin_acquire(&p->pi_lock);
    scheduler_wakeup(p);
    spin_release(&p->pi_lock);
}
```

### Internal Wakeup Logic

`__do_scheduler_wakeup()` follows the Linux `try_to_wake_up()` pattern:

```c
static void __do_scheduler_wakeup(struct proc *p) {
    // Special case: self-wakeup from interrupt
    if (p == myproc()) {
        atomic_cas(&p->state, old_state, PSTATE_RUNNING);
        return;
    }
    
    // 1. Check if already queued (Linux: ttwu_runnable path)
    smp_rmb();
    if (smp_load_acquire(&p->on_rq)) {
        return;  // Already on run queue
    }
    
    // 2. Wait for context switch out (Linux: smp_cond_load_acquire)
    smp_rmb();
    smp_cond_load_acquire(&p->on_cpu, !VAL);
    
    // 3. Atomically claim wakeup (prevent double-enqueue)
    if (!atomic_cas(&p->state, SLEEPING, PSTATE_WAKENING)) {
        return;  // Someone else woke it, or state changed
    }
    
    // 4. Set to RUNNING before enqueue
    smp_store_release(&p->state, PSTATE_RUNNING);
    
    // 5. Enqueue
    if (!PROC_STOPPED(p)) {
        sched_lock();
        __scheduler_add_ready(p);
        sched_unlock();
    }
}
```

## Memory Ordering

### The Critical Invariant

**Writer (context_switch_finish):**
```
on_rq = 1          (1) First: enqueue
sched_unlock()     (2) Release barrier
on_cpu = 0         (3) Last: signal completion
```

**Reader (waker):**
```
read on_rq         (1) First: check if queued
smp_rmb()          (2) Read barrier
read/wait on_cpu   (3) Last: wait for completion
```

This ensures: **If waker sees `on_cpu = 0`, it must also see `on_rq = 1` (if it was set).**

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

**Solution:** Waker spin-waits on `on_cpu`
```
CPU 0: P switching out          CPU 1: Waker
────────────────────            ────────────────
on_rq = 1
sched_unlock()
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
4. Sees `RUNNING`, enqueues itself

```c
void context_switch_finish(...) {
    pstate = __proc_get_pstate(prev);  // RE-READ, not cached!
    if (pstate == PSTATE_RUNNING) {
        __scheduler_add_ready(prev);   // Self-enqueue
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
    // Caller must hold p->pi_lock, must NOT hold proc_lock or sched_lock.
    assert(spin_holding(&p->pi_lock));
    assert(!spin_holding(&p->lock));
    assert(!sched_holding());
    
    if (!PROC_STOPPED(p)) {
        return;  // Not stopped, nothing to do
    }
    
    // Wait for process to be off CPU before modifying state.
    // Like Linux's smp_cond_load_acquire in try_to_wake_up().
    smp_cond_load_acquire(&p->on_cpu, !VAL);
    
    PROC_CLEAR_STOPPED(p);
    
    // If process is RUNNING, add it back to the ready queue
    if (__proc_get_pstate(p) == PSTATE_RUNNING) {
        sched_lock();
        __scheduler_add_ready(p);
        sched_unlock();
    }
}
```

### Caller Pattern (from signal.c)

```c
// In signal_send() when handling SIGCONT:
if (is_cont) {
    // scheduler_continue uses pi_lock protocol.
    proc_unlock(p);
    spin_acquire(&p->pi_lock);
    scheduler_continue(p);
    spin_release(&p->pi_lock);
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
- Both acquire `sched_lock` internally

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

## Summary: The Complete Picture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              SLEEP PATH                                      │
├─────────────────────────────────────────────────────────────────────────────┤
│  1. state = SLEEPING                                                         │
│  2. yield() → pick_next()                                                    │
│  3. context_switch_prepare: next->on_cpu = 1, sched_lock()                  │
│  4. __switch_to()                                                            │
│  5. context_switch_finish:                                                   │
│       - RE-READ state                                                        │
│       - if RUNNING: on_rq = 1 (enqueue)                                     │
│       - sched_unlock()                                                       │
│       - on_cpu = 0                                                           │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                              WAKEUP PATH                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  1. Acquire pi_lock                                                          │
│  2. Check on_rq → if set, return (already queued)                           │
│  3. smp_rmb()                                                                │
│  4. Wait for on_cpu == 0                                                     │
│  5. CAS state: SLEEPING → WAKENING                                          │
│  6. state = RUNNING                                                          │
│  7. sched_lock(); on_rq = 1; sched_unlock()                                 │
│  8. Release pi_lock                                                          │
└─────────────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────────────┐
│                           KEY INVARIANTS                                     │
├─────────────────────────────────────────────────────────────────────────────┤
│  • on_rq = 1 always happens BEFORE on_cpu = 0 (write order)                 │
│  • Waker reads on_rq BEFORE on_cpu (read order matches write order)         │
│  • State CAS prevents double-enqueue from multiple wakers                    │
│  • context_switch_finish RE-READS state for interrupt wakeups               │
│  • on_rq changes only under sched_lock                                       │
│  • Wakeup AND continue require pi_lock (not proc_lock) to avoid deadlock    │
│  • Both wakeup and continue wait for on_cpu == 0 before enqueuing           │
└─────────────────────────────────────────────────────────────────────────────┘
```
