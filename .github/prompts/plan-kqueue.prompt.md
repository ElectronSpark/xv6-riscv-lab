## Plan: Complete kqueue Implementation for xv6

**TL;DR** — Add a BSD-inspired kqueue event notification subsystem with 3 syscalls (`kqueue`, `kevent_register`, `kevent_wait`), supporting 6 event filters (`EVFILT_READ`, `EVFILT_WRITE`, `EVFILT_TIMER`, `EVFILT_SIGNAL`, `EVFILT_PROC`, `EVFILT_VNODE`). The kqueue fd is created via `vfs_custom_fd_alloc()` following existing patterns (pipes, lwIP sockets). Event delivery is callback-driven: each subsystem (pipe, socket, signal, process lifecycle, VFS inode ops, timer) is hooked to push events into the kqueue's ready list and wake blocked waiters via `tq_t`. A new `kernel/kqueue/` module houses all kqueue logic. User-space gets a `<kqueue.h>` header, `usys.pl` stubs, and a comprehensive `kqueuetest.c`.

---

### Steps

**Step 1 — Define kqueue data structures and constants** 
Create [kernel/inc/kqueue.h](kernel/inc/kqueue.h) with:
- Filter constants: `EVFILT_READ` (-1), `EVFILT_WRITE` (-2), `EVFILT_TIMER` (-3), `EVFILT_SIGNAL` (-4), `EVFILT_PROC` (-5), `EVFILT_VNODE` (-6) (BSD convention: negative values)
- Flag constants: `EV_ADD`, `EV_DELETE`, `EV_ENABLE`, `EV_DISABLE`, `EV_ONESHOT`, `EV_CLEAR`, `EV_EOF`, `EV_ERROR`
- Filter-specific `fflags`: `NOTE_EXIT`, `NOTE_FORK`, `NOTE_EXEC`, `NOTE_TRACK` (EVFILT_PROC); `NOTE_DELETE`, `NOTE_WRITE`, `NOTE_RENAME`, `NOTE_ATTRIB`, `NOTE_EXTEND`, `NOTE_LINK` (EVFILT_VNODE)
- `struct kevent` (user-space ABI): `{ uintptr_t ident; short filter; unsigned short flags; unsigned int fflags; int64_t data; void *udata; }`

**Step 2 — Define internal kqueue kernel types**
Create [kernel/inc/kqueue_types.h](kernel/inc/kqueue_types.h) with:
- `struct knote` — one registered filter: `list_entry` (for kqueue's registered list + ready list), `ident`, `filter`, `flags`, `fflags`, `data`, `udata`, `status` (active/disabled/triggered), `kq` backpointer, `timer_node` (for EVFILT_TIMER), `attached_file` (for READ/WRITE), `attached_inode` (for VNODE), filter-specific hook data
- `struct kqueue` — the kqueue descriptor: `spinlock_t lock`, `tq_t waitq` (for blocking in `kevent_wait`), `struct list_head registered` (all knotes), `struct list_head ready` (triggered knotes ready for delivery), `int nready` (count), `int closed` flag
- `struct knote_ops` — per-filter vtable: `int (*attach)(struct knote *)`, `void (*detach)(struct knote *)`, `int (*event)(struct knote *, long hint)` — called by subsystems to check/generate events

**Step 3 — Implement kqueue core logic**
Create [kernel/kqueue/kqueue.c](kernel/kqueue/kqueue.c):
- **`kqueue_create()`** — allocate `struct kqueue` from slab, init lock/waitq/lists, call `vfs_custom_fd_alloc()` with `kqueue_file_ops`, return fd
- **`kqueue_register(kq, changelist, nchanges)`** — iterate changelist: for `EV_ADD` find-or-create `knote`, call filter's `attach()`, activate; for `EV_DELETE` call `detach()`, remove, free; `EV_ENABLE`/`EV_DISABLE` toggle status
- **`kqueue_wait(kq, eventlist, nevents, timeout)`** — if ready list non-empty, copy events out immediately (up to `nevents`); if empty, set timer if timeout > 0, sleep on `kq->waitq`; on wakeup, drain ready list into user eventlist; handle `EV_ONESHOT` (auto-delete) and `EV_CLEAR` (reset triggered state)
- **`kqueue_close(kq)`** — detach all knotes, cancel timers, wake all waiters, free kqueue
- **`knote_enqueue(kn)`** — internal: add knote to kqueue ready list (if not already there), increment `nready`, call `tq_wakeup(&kq->waitq, 0, NULL)` to wake one waiter
- **`kqueue_file_ops`** — `{ .release = kqueue_close, .poll = kqueue_poll }` (kqueue fd itself is pollable — returns `POLLIN` when ready list non-empty)
- Slab caches: `__kqueue_cache` for `struct kqueue`, `__knote_cache` for `struct knote`

**Step 4 — Implement filter-specific attach/detach/event ops**
Create [kernel/kqueue/kqueue_filters.c](kernel/kqueue/kqueue_filters.c):

- **EVFILT_READ / EVFILT_WRITE** — `attach()`: resolve `ident` (fd) → `vfs_file`, store in `kn->attached_file`. Needs a **notification hook** in the file: add a `struct list_head knote_list` and `spinlock_t knote_lock` to `struct vfs_file` ([kernel/inc/vfs/vfs_types.h](kernel/inc/vfs/vfs_types.h#L388)). `attach()` adds `kn` to `file->knote_list`, then does an immediate poll check. `detach()` removes from list. The event sources (pipe write, socket recv) call `vfs_file_knote_notify(file, hint)` which walks `file->knote_list` and calls `knote_enqueue()` on matching knotes.

- **EVFILT_TIMER** — `attach()`: init `timer_node` with callback `kqueue_timer_callback`; `timer_add()` with `MS_TO_RAWTICKS(kn->data)` expiry; callback calls `knote_enqueue(kn)` and re-arms if not `EV_ONESHOT`. `detach()`: `timer_remove()`. No external hooks needed.

- **EVFILT_SIGNAL** — `attach()`: `ident` = signal number. Add a per-process `struct list_head kqueue_signal_knotes[NSIG]` to `sigacts_t` ([kernel/inc/signal_types.h](kernel/inc/signal_types.h#L32)). `attach()` adds knote to `sigacts->kqueue_signal_knotes[signo]`. Hook into `__signal_send()` ([kernel/proc/signal.c](kernel/proc/signal.c#L444)): after enqueuing the signal, walk `kqueue_signal_knotes[signo]` and call `knote_enqueue()`. `detach()`: remove from list.

- **EVFILT_PROC** — `attach()`: `ident` = target PID. Store knote in a global hash table `knote_proc_table[PID]` or add a `struct list_head kqueue_proc_knotes` to `struct thread` ([kernel/inc/proc/thread_types.h](kernel/inc/proc/thread_types.h#L49)). Hook into:
  - `exit()` in [kernel/proc/exit.c](kernel/proc/exit.c#L225) → `knote_enqueue` with `NOTE_EXIT`
  - `thread_clone()` in [kernel/proc/clone.c](kernel/proc/clone.c#L219) → `knote_enqueue` with `NOTE_FORK`
  - `exec()` in [kernel/exec.c](kernel/exec.c#L328) → `knote_enqueue` with `NOTE_EXEC`
  - `detach()`: remove from target thread's list

- **EVFILT_VNODE** — `attach()`: `ident` = fd, resolve to inode. Add `struct list_head kqueue_vnode_knotes` to `struct vfs_inode` ([kernel/inc/vfs/vfs_types.h](kernel/inc/vfs/vfs_types.h#L211)). Register knote on inode's list. Hook into:
  - `vfs_unlink()` at [kernel/vfs/inode.c](kernel/vfs/inode.c#L972) → `NOTE_DELETE`
  - `vfs_filewrite()` at [kernel/vfs/file.c](kernel/vfs/file.c#L501) → `NOTE_WRITE`
  - `vfs_move()` at [kernel/vfs/inode.c](kernel/vfs/inode.c#L1142) → `NOTE_RENAME`
  - `vfs_itruncate()` → `NOTE_EXTEND` or `NOTE_ATTRIB`
  - `vfs_link()` → `NOTE_LINK`
  - `detach()`: remove from inode's list

**Step 5 — Add notification hooks into existing subsystems**
Modify existing files to call kqueue notification:

| Subsystem | File | Hook Location | Notification |
|-----------|------|--------------|--------------|
| Pipe write | [kernel/vfs/pipe.c](kernel/vfs/pipe.c) | After `nwrite++` in `pipewrite()` | `vfs_file_knote_notify(read_file, EVFILT_READ)` |
| Pipe read  | [kernel/vfs/pipe.c](kernel/vfs/pipe.c) | After `nread++` in `piperead()` | `vfs_file_knote_notify(write_file, EVFILT_WRITE)` |
| Pipe close | [kernel/vfs/pipe.c](kernel/vfs/pipe.c) | In `pipeclose()` | `vfs_file_knote_notify(file, EVFILT_READ\|EVFILT_WRITE)` with EOF |
| lwIP recv  | [kernel/lwip_port/sys_socket.c](kernel/lwip_port/sys_socket.c) | After netconn recv callback | `vfs_file_knote_notify(file, EVFILT_READ)` |
| lwIP send  | [kernel/lwip_port/sys_socket.c](kernel/lwip_port/sys_socket.c) | After send completes | `vfs_file_knote_notify(file, EVFILT_WRITE)` |
| lwIP accept| [kernel/lwip_port/sys_socket.c](kernel/lwip_port/sys_socket.c) | After new connection ready | `vfs_file_knote_notify(listen_file, EVFILT_READ)` |
| Signal send| [kernel/proc/signal.c](kernel/proc/signal.c#L444) | End of `__signal_send()` | Walk `kqueue_signal_knotes[signo]` → `knote_enqueue()` |
| Exit       | [kernel/proc/exit.c](kernel/proc/exit.c#L225) | After setting ZOMBIE | Walk thread's `kqueue_proc_knotes` → `knote_enqueue(NOTE_EXIT)` |
| Fork       | [kernel/proc/clone.c](kernel/proc/clone.c#L219) | After child creation | Walk parent's `kqueue_proc_knotes` → `knote_enqueue(NOTE_FORK)` |
| Exec       | [kernel/exec.c](kernel/exec.c#L328) | After exec commit | Walk thread's `kqueue_proc_knotes` → `knote_enqueue(NOTE_EXEC)` |
| Unlink     | [kernel/vfs/inode.c](kernel/vfs/inode.c#L972) | After `dir->ops->unlink()` | Walk inode's `kqueue_vnode_knotes` → `knote_enqueue(NOTE_DELETE)` |
| Write      | [kernel/vfs/file.c](kernel/vfs/file.c#L501) | After `file->ops->write()` | Walk inode's `kqueue_vnode_knotes` → `knote_enqueue(NOTE_WRITE)` |
| Rename     | [kernel/vfs/inode.c](kernel/vfs/inode.c#L1142) | After `old_dir->ops->move()` | Walk inode's `kqueue_vnode_knotes` → `knote_enqueue(NOTE_RENAME)` |

**Step 6 — Add syscall plumbing**
- Assign syscall numbers in [kernel/inc/syscall.h](kernel/inc/syscall.h): `SYS_kqueue` (65), `SYS_kevent_register` (66), `SYS_kevent_wait` (67)
- Add handler prototypes and dispatch entries in [kernel/irq/syscall.c](kernel/irq/syscall.c#L186)
- Implement `sys_kqueue()`, `sys_kevent_register()`, `sys_kevent_wait()` wrappers in [kernel/kqueue/kqueue_syscall.c](kernel/kqueue/kqueue_syscall.c) that parse user args via `argint`/`argaddr`, copy `struct kevent` arrays in/out with `either_copyin`/`either_copyout`, and dispatch to the core functions

**Step 7 — Build system integration**
- Create [kernel/kqueue/CMakeLists.txt](kernel/kqueue/CMakeLists.txt): `add_library(kqueue_core OBJECT kqueue.c kqueue_filters.c kqueue_syscall.c)`
- Add `add_subdirectory(kqueue)` in [kernel/CMakeLists.txt](kernel/CMakeLists.txt#L103)
- Add `$<TARGET_OBJECTS:kqueue_core>` to `KERNEL_OBJ_LIBS` and `kqueue_core` to `KERNEL_LINK_LIBS`

**Step 8 — Initialization**
- Add `kqueue_init()` function in [kernel/kqueue/kqueue.c](kernel/kqueue/kqueue.c) to init slab caches (`__kqueue_cache`, `__knote_cache`) and global data structures
- Call `kqueue_init()` from an early kernel init function (pattern: similar to `pipe_init()` called from VFS init path)

**Step 9 — User-space support**
- Add syscall stubs in [user/usys.pl](user/usys.pl): `entry("kqueue")`, `entry("kevent_register")`, `entry("kevent_wait")`
- Add function prototypes in [user/user.h](user/user.h): `int kqueue(void)`, `int kevent_register(int kqfd, struct kevent *changelist, int nchanges)`, `int kevent_wait(int kqfd, struct kevent *eventlist, int nevents, int timeout_ms)`
- Create [user/kqueue.h](user/kqueue.h) — user-space header re-exporting `struct kevent`, filter/flag constants (either include the kernel header or duplicate for user space)

**Step 10 — User-space test program**
Create [user/kqueuetest.c](user/kqueuetest.c) exercising all 6 filters:
1. **Pipe read/write test** — create pipe, register `EVFILT_READ` on read end, write from another process, verify event delivery
2. **Timer test** — register `EVFILT_TIMER` with 100ms interval, wait, verify multiple firings
3. **Signal test** — register `EVFILT_SIGNAL` for `SIGUSR1`, raise signal, verify event
4. **Process test** — fork child, register `EVFILT_PROC` with `NOTE_EXIT`, child exits, verify event
5. **Vnode test** — create file, register `EVFILT_VNODE` with `NOTE_WRITE`, write to file, verify event
6. **Multiple events test** — register several filters on one kqueue, verify multiplexed delivery
7. **Edge cases** — `EV_ONESHOT`, `EV_DISABLE`/`EV_ENABLE`, `EV_CLEAR`, `EV_DELETE`, timeout=0 (nonblocking), invalid fd
- Add `kqueuetest.c` to `USER_PROGRAMS_C_FILES` in [user/CMakeLists.txt](user/CMakeLists.txt#L107)

---

### Key Design Decisions

- **Callback-driven over poll-scan**: Every event source explicitly calls `knote_enqueue()` instead of periodically scanning. This requires modifying 6+ subsystems but avoids the busy-polling latency of the current `poll()`.
- **`knote_list` on `vfs_file`/`vfs_inode`/`thread`/`sigacts`**: Embedding knote lists directly in existing structures (rather than a global hash table) gives O(1) notification dispatch with minimal overhead when kqueue is not in use (empty list head = 16 bytes + no branches in hot path unless list is non-empty).
- **Slab-allocated knotes**: Using slab caches for `struct knote` and `struct kqueue` matches the kernel's existing memory management patterns (pipes, signals).
- **Separate `kevent_register` / `kevent_wait`**: Cleaner API separation vs BSD's combined `kevent()`. The register call never blocks; the wait call never modifies filters.
- **Locking order**: kqueue lock → file/inode/sigacts knote_lock. Never hold kqueue lock when calling into subsystems; subsystems hold their own knote_lock when calling `knote_enqueue()` which acquires the kqueue lock.

### File Summary

| New Files | Purpose |
|-----------|---------|
| [kernel/inc/kqueue.h](kernel/inc/kqueue.h) | Constants, `struct kevent` ABI |
| [kernel/inc/kqueue_types.h](kernel/inc/kqueue_types.h) | Internal types: `struct kqueue`, `struct knote` |
| [kernel/kqueue/kqueue.c](kernel/kqueue/kqueue.c) | Core: create, register, wait, close, knote_enqueue |
| [kernel/kqueue/kqueue_filters.c](kernel/kqueue/kqueue_filters.c) | Filter attach/detach/event ops |
| [kernel/kqueue/kqueue_syscall.c](kernel/kqueue/kqueue_syscall.c) | Syscall wrappers |
| [kernel/kqueue/CMakeLists.txt](kernel/kqueue/CMakeLists.txt) | Build definition |
| [user/kqueue.h](user/kqueue.h) | User-space kqueue header |
| [user/kqueuetest.c](user/kqueuetest.c) | Test program |

| Modified Files | Changes |
|----------------|---------|
| [kernel/inc/syscall.h](kernel/inc/syscall.h) | Add `SYS_kqueue` (65), `SYS_kevent_register` (66), `SYS_kevent_wait` (67) |
| [kernel/irq/syscall.c](kernel/irq/syscall.c) | Add dispatch entries |
| [kernel/inc/vfs/vfs_types.h](kernel/inc/vfs/vfs_types.h) | Add `knote_list` + `knote_lock` to `struct vfs_file` and `struct vfs_inode` |
| [kernel/inc/signal_types.h](kernel/inc/signal_types.h) | Add `kqueue_signal_knotes[NSIG]` to `sigacts_t` |
| [kernel/inc/proc/thread_types.h](kernel/inc/proc/thread_types.h) | Add `kqueue_proc_knotes` + `knote_lock` to `struct thread` |
| [kernel/vfs/pipe.c](kernel/vfs/pipe.c) | Call `vfs_file_knote_notify()` on read/write/close |
| [kernel/lwip_port/sys_socket.c](kernel/lwip_port/sys_socket.c) | Call `vfs_file_knote_notify()` on recv/send/accept |
| [kernel/proc/signal.c](kernel/proc/signal.c) | Call signal knote notification in `__signal_send()` |
| [kernel/proc/exit.c](kernel/proc/exit.c) | Call proc knote notification on exit |
| [kernel/proc/clone.c](kernel/proc/clone.c) | Call proc knote notification on fork |
| [kernel/exec.c](kernel/exec.c) | Call proc knote notification on exec |
| [kernel/vfs/inode.c](kernel/vfs/inode.c) | Call vnode knote notification on unlink/rename/link |
| [kernel/vfs/file.c](kernel/vfs/file.c) | Call vnode knote notification on write; init knote_list in file alloc path |
| [kernel/CMakeLists.txt](kernel/CMakeLists.txt) | Add `kqueue` subdirectory and link |
| [user/usys.pl](user/usys.pl) | Add 3 syscall entries |
| [user/user.h](user/user.h) | Add 3 function prototypes |
| [user/CMakeLists.txt](user/CMakeLists.txt) | Add `kqueuetest` to program list |

### Verification

1. **Build**: `cmake .. && make -j16` — must compile without errors
2. **Smoke test**: Boot xv6, run `kqueuetest` — all subtests pass
3. **Pipe test**: kqueue detects pipe readability without busy-waiting
4. **Timer test**: EVFILT_TIMER fires within expected tolerance (~±10ms at HZ=1000)
5. **Signal test**: EVFILT_SIGNAL catches `SIGUSR1` delivered to self
6. **Process test**: EVFILT_PROC/NOTE_EXIT fires when child exits
7. **Vnode test**: EVFILT_VNODE/NOTE_WRITE fires when file is written
8. **Edge cases**: EV_ONESHOT delivers exactly once; EV_DELETE removes cleanly; timeout=0 returns immediately; invalid fd returns `EV_ERROR`
