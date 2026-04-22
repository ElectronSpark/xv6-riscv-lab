# Thread Queue Design (`thread_queue.c`)

## Overview

`kernel/proc/thread_queue.c` implements two scheduler-integrated waiter containers used by blocking paths in the kernel:

- **List queue (`tq_t`)**: FIFO-like wait queue
- **Tree queue (`ttree_t`)**: key-ordered wait queue (RB tree)

Both queues store `tnode_t` wait nodes that carry:

- owning `struct thread *`
- wakeup result code (`error_no`)
- wakeup payload (`data`)
- membership metadata (list/tree + owning queue pointer)

This document describes the current behavior in this repository (Feb 2026).

## Data Structures

Defined in `kernel/inc/proc/tq_type.h`:

- `tnode_t`: waiter node used by either queue flavor
- `tq_t`: intrusive linked-list queue + counter
- `ttree_t`: red-black-tree queue + counter

Membership is tracked by node type and queue pointer, and checked via `tq_enqueued(node)`.

## List Queue Operations (`tq_*`)

- `tq_init`, `tq_set_lock`
- `tq_push`, `tq_first`, `tq_pop`, `tq_remove`
- `tq_bulk_move` (move all waiters from one queue to another)
- `tq_wakeup`, `tq_wakeup_all`

### Key invariants

- `counter >= 0`
- node can be enqueued in at most one container at a time
- on enqueue: node type becomes LIST and `node->list.queue` is set
- on dequeue/remove: node type is reset to NONE

## Tree Queue Operations (`ttree_*`)

- `ttree_init`, `ttree_set_lock`
- `ttree_add`, `ttree_first`, `ttree_key_min`, `ttree_remove`
- `ttree_wakeup_one`, `ttree_wakeup_key`, `ttree_wakeup_all`

### Ordering model

Tree ordering compares:

1. `node->tree.key`
2. node address (tie-breaker)

This permits multiple waiters sharing the same key while preserving deterministic uniqueness for insertion/removal.

## Wait/Wakeup Protocol

List waits use `tq_wait_cb` / `tq_wait`; tree waits use `ttree_wait_cb` / `ttree_wait`.

Core sequence:

1. Disable interrupts
2. Initialize stack-local waiter node (`tnode_init`)
3. Set default wake error to `-EINTR`
4. Enqueue waiter into queue/tree
5. Run sleep callback (typically releases caller lock)
6. `scheduler_yield()`
7. Run wake callback (typically re-acquires lock)
8. If still enqueued, self-remove (asynchronous wake path)
9. Restore interrupts
10. Return `waiter.error_no` (+ optional `waiter.data`)

Wakers set `waiter.error_no` and `waiter.data` before invoking `scheduler_wakeup(thread)`.

## Locking Model

Queues expose an optional `spinlock_t *lock` field but do not implicitly take it in core operations.
Callers are responsible for external synchronization and lock ordering.

The wait API uses callback hooks (`sleep_callback_t`, `wakeup_callback_t`) to integrate queue waits with caller-owned locks.

## Error Semantics

Common return behavior:

- `-EINVAL`: invalid arguments or invalid membership
- `-ENOENT`: no matching tree node / empty tree wakeup path
- `-ENOTEMPTY`: bulk move destination queue not empty

Wait return values are carried through the node:

- default `-EINTR` when asynchronously interrupted
- queue-specific wake status set by wake side

## Memory Ordering Notes

Queue mutation paths use `__atomic_thread_fence(__ATOMIC_SEQ_CST)` after enqueue/dequeue operations to maintain conservative cross-CPU visibility.

## Related Files

- `kernel/proc/thread_queue.c` (implementation)
- `kernel/inc/proc/tq.h` (public API)
- `kernel/inc/proc/tq_type.h` (types)
- `kernel/lock/rwlock.c` (example integration)
- `kernel/proc/sched.c` / `kernel/proc/SCHEDULER_DESIGN.md` (wakeup behavior context)
