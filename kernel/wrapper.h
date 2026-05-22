/*
 * Bindgen wrapper header.
 *
 * Pulls in the kernel C headers that the Rust port needs to see typed
 * declarations for. Bindgen is invoked from build.rs and emits Rust
 * declarations from this single translation unit. Keep this file minimal
 * and only add headers the Rust crate actually consumes.
 */

#include "types.h"
#include "riscv.h"
#include "errno.h"
#include "param.h"
#include "defs.h"

#include "list.h"
#include "rbtree.h"
#include "lock/rwsem.h"
#include "lock/spinlock.h"
#include "lock/completion.h"
#include "lock/mutex.h"
#include "lock/semaphore.h"
/* rwlock.h has many static-inline atomic helpers that bindgen cannot
 * process cleanly; expose only the type layout via rwlock_types.h and
 * let kernel/lock/rwlock_shim.c front for the inline primitives. */
#include "lock/rwlock_types.h"

#include "proc/tq.h"
#include "proc/pgroup_types.h"
#include "proc/thread_group_types.h"
#include "proc/workqueue_types.h"
#include "proc/workqueue.h"
#include "proc/rq_types.h"
#include "smp/percpu.h"
#include "tty/session_types.h"
#include "signal_types.h"
#include "hlist_type.h"
#include "hlist.h"
#include "lock/rcu.h"
/* proc/thread.h transitively includes thread_group.h whose inline
 * `__atomic_load_n` on `_Atomic int` is rejected by clang. We rely on
 * `proc/thread_types.h` (included below) for the `struct thread` layout. */
#include "proc/sched.h"
#include "timer/timer.h"
#include "signal.h"

#include <mm/memlayout.h>
#include <mm/page.h>
#include <mm/page_type.h>
#include <mm/slab.h>
#include <mm/vm.h>
#include <mm/vm_types.h>
#include <mm/pcache_types.h>

#include <vfs/file.h>
#include <smp/percpu_types.h>
#include <proc/thread_types.h>

/* Platform info + extern MMIO base symbols needed by Rust port of kvmmake. */
#include <dev/fdt.h>
#include <dev/uart.h>
#include <dev/virtio.h>
#include <dev/pci.h>
#include <dev/plic.h>
#include <dev/e1000_dev.h>
#include <timer/goldfish_rtc.h>
