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
/* dev/cdev.h (-> dev/dev_types.h) and tty/tty.h (-> tty/tty_types.h,
 * tty/session.h -> tty/session_types.h) — Phase 2 Wave 4 (console.c
 * port): typed layouts for device_t/cdev_t/device_ops_t/cdev_ops_t
 * (building the console cdev's vtable natively) and struct
 * tty/termios/session (line-discipline glue, termios.c_oflag read in
 * consolewrite). Neither header has static-inline bodies bindgen can't
 * process (see the rwlock.h note above for what that problem looks
 * like), so both are plain #include additions. Subsumes the previous
 * standalone `tty/session_types.h` include below — `session_types.h`
 * has no include guard, so including it both directly and transitively
 * (via tty.h -> session.h) double-defines `struct session`; tty.h's
 * transitive include is kept as the sole one. */
#include "dev/cdev.h"
#include "tty/tty.h"
/* vfs/pipe_types.h — Phase 2 Wave 11 (tty.c/ptmx.c port): full `struct
 * pipe` layout (nread/nwrite) read directly (via smp_load_acquire) by
 * tty_poll()/ptmx's master poll for a lock-free readiness check. Before
 * this, `pipe` was only forward-declared (via tty_types.h's `struct
 * pipe;`) and bindgen emitted it as an opaque zero-sized type — fine for
 * the pointer-only uses in pty.rs/console.rs, not enough for a field
 * read. Purely additive: every existing use of `*mut pipe` stays a
 * pointer, so this only adds visibility, it doesn't change any ABI. */
#include "vfs/pipe_types.h"
/* vfs/vfs_types.h + vfs/fs.h -- Phase 2 Wave 13 (vfs/inode.c port):
 * explicit includes per the *_types.h discipline. Both were already
 * pulled in transitively (vfs/file.h -> vfs/vfs_types.h, already
 * included below) -- bindgen already emitted full field layouts for
 * vfs_inode/vfs_superblock/vfs_inode_ops/vfs_superblock_ops/vfs_dentry/
 * vfs_dir_iter/vfs_fdtable/fs_struct before this change (verified by
 * inspecting the generated bindings). Made explicit here so a future
 * edit to vfs/file.h's includes can't silently drop this module's type
 * visibility. vfs/fs.h's function prototypes are not added to
 * build.rs's allowlist_function: kernel/vfs/inode.rs declares its own
 * local `unsafe extern "C"` block for the handful of fs.c entry points
 * it calls, matching the established per-file convention (irq/trap.rs,
 * tty/session.rs, mm/vm.rs, ...) rather than reaching through bindgen's
 * generated bindings module. */
#include "vfs/vfs_types.h"
#include "vfs/fs.h"
/* uabi/statfs.h -- Phase 2 Wave 17 (vfs/vfs_syscall.c port): `struct
 * statfs` was only forward-declared (`struct statfs;` in vfs_types.h,
 * for the `vfs_superblock_ops.statfs` callback's pointer parameter),
 * so bindgen emitted an opaque `{ _unused: [u8; 0] }` stand-in.
 * kernel/vfs/vfs_syscall.rs (sys_statfs) needs to populate real fields
 * (f_bsize/f_frsize/f_blocks/f_bfree/f_bavail/f_namelen) before calling
 * that same callback, so the full definition must be visible. Purely
 * additive: every existing `*mut statfs` use stays a pointer of the
 * same C type. */
#include "uabi/statfs.h"
/* vfs/tmpfs/tmpfs_private.h -- Phase 2 Wave 18 (tmpfs port): tmpfs is the
 * first filesystem *driver* (as opposed to vfs core) to move to Rust.
 * Unlike every other wrapper.h header this one is NOT under kernel/inc/
 * -- it is tmpfs's private implementation header, living next to its
 * (former) .c files at kernel/vfs/tmpfs/. It stays a real, unmodified C
 * header (not deleted) because kernel/vfs/devtmpfs/superblock.c -- still
 * C, ported in a later wave -- `#include`s it directly to reuse
 * tmpfs_alloc_inode/tmpfs_alloc_superblock/tmpfs_make_directory/
 * tmpfs_free and the struct tmpfs_inode/tmpfs_superblock layouts via
 * container_of. Resolved via the CMAKE_SOURCE_DIR entry already present
 * in RUST_BINDGEN_INCLUDES (kernel/CMakeLists.txt), so the path below is
 * repo-root-relative, matching how devtmpfs's own `#include
 * "../tmpfs/tmpfs_private.h"` reaches the same file from a sibling
 * directory. Bindgen needs real field layouts for struct tmpfs_inode's
 * anonymous dir/sym/file union (embedded-data overlay) so the Rust port
 * can access `__bindgen_anon_1` exactly like `vfs/inode.c`'s bitfields
 * did in Wave 13 -- no opaque stand-ins. */
#include "kernel/vfs/tmpfs/tmpfs_private.h"
/* dev/buf.h -- Phase 2 Wave 19 (xv6fs port): the classic xv6 buffer-cache
 * `struct buf` (kernel/bio.c, unchanged C) layout. xv6fs's log/inode/
 * truncate/block_cache code reads/writes `bp->data`/`bp->blockno` directly
 * after `bread()`, so the real field layout is needed (no opaque
 * stand-in) -- same rationale as every other *_types.h addition in this
 * file. No static-inline bodies (plain struct + two #defines), so this is
 * a clean additive include. */
#include "dev/buf.h"
/* kernel/vfs/xv6fs/xv6fs_private.h -- Phase 2 Wave 19 (xv6fs port): real
 * bindgen layouts for `struct xv6fs_superblock`/`xv6fs_inode`/`xv6fs_log`/
 * `xv6fs_logheader` (this driver's private in-memory structures) plus,
 * transitively, `vfs/xv6fs/ondisk.h`'s on-disk layout structs (`struct
 * superblock`/`dinode`/`dirent` -- ABI shared with mkfs, untouched) and
 * `block_cache.h`'s `struct xv6fs_block_cache`/`free_extent` (the
 * Wave-1 Rust rbtree-indexed free-extent cache). Resolved via the
 * repo-root `CMAKE_SOURCE_DIR` entry already present in
 * `RUST_BINDGEN_INCLUDES`, exactly like `tmpfs_private.h` above; the
 * header's own same-directory quote-includes (`#include "block_cache.h"`)
 * resolve relative to its own path the same way the C compiler does.
 * `xv6fs_private.h` itself is untouched (still `#include`s `proc/tq.h`
 * for `tq_t`): empirically verified this parses fine under bindgen's
 * clang front-end as-is, because `build.rs` already stubs out the
 * `__atomic_load_n`/`__atomic_store_n`/`__atomic_exchange_n` builtins
 * that make `proc/thread.h` -> `thread_group.h`'s `_Atomic`-typed inline
 * helpers unparseable (the hazard the `proc/sched.h` comment above
 * describes predates that stubbing; this wave is the first to actually
 * need a header that pulls the chain in and confirms the stub now covers
 * it). */
#include "kernel/vfs/xv6fs/xv6fs_private.h"
#include "signal_types.h"
#include "hlist_type.h"
#include "hlist.h"
#include "kobject.h"
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
