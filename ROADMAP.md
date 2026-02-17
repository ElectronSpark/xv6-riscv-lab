# xv6-RISCV Enhanced: Development Roadmap

This document outlines the planned enhancements to xv6-RISCV, organized into **near-term** (system completeness) and **long-term** (ambitious) goals, with emphasis on current status (Feb 2026).

---

## Near-Term Goals (System Completeness)

These features focus on building a complete, functional Unix-like system with modern OS capabilities.

### 1. Dynamic Memory Probing and Page Frame Management
**Dependencies**: None (foundational improvement)  
**Priority**: Critical  
**Status**: 🔄 In progress with core runtime support in place (as of Feb 2026)

- ✅ Dynamic memory detection via FDT (Flattened Device Tree)
- ✅ Runtime-configurable physical memory boundaries
- ✅ Page array allocated dynamically during boot
- ✅ Device MMIO addresses configurable at runtime (UART, PLIC, VirtIO, etc.)
- ⏳ Support non-contiguous memory regions (multiple DRAM banks) - planned
- ⏳ Memory zone management (DMA, Normal, High memory zones) - planned
- ⏳ Per-CPU page frame caches for allocation performance - basic implementation
- ⏳ Memory hot-add/hot-remove support (basic) - planned
- ⏳ Better memory statistics and accounting - planned
- ⏳ NUMA-aware memory allocation (if multi-node support added) - planned

**Implementation Notes**:
- Completed: Dynamic memory detection and page array allocation
- Completed: Device addresses (UART0, PLIC, VirtIO, E1000, etc.) are now runtime variables
- Completed: Support for multiple kernel base addresses (QEMU: 0x80200000, Orange Pi: 0x00200000)
- Target: Zone-based allocation, optimized page frame management
- Benefits: Support varied hardware configs (QEMU and real hardware), better memory utilization
- Files: `kernel/mm/page.c`, `kernel/mm/kalloc.c`, `kernel/start_kernel.c`, `kernel/inc/mm/memlayout.h`

**Memory Architecture**:
```
Current State:
  FDT probe → Dynamic page array allocation → Buddy allocator → Page allocation
  Device addresses: Runtime configurable via extern variables

Target:
  Boot probe → Memory zones (DMA/Normal/High) → Per-CPU caches
  → Buddy allocator (zone-aware) → Optimized page allocation
```

---

### 2.5. Orange Pi RV2 Hardware Support ✅ **COMPLETED**
**Dependencies**: Dynamic Memory Probing (completed)  
**Priority**: High  
**Status**: ✅ Completed (January 2026)

- ✅ Configurable kernel base address (QEMU: 0x80200000, Orange Pi: 0x00200000)
- ✅ U-Boot boot scripts with dual-boot menu (boot.cmd, xv6.cmd, default.cmd)
- ✅ Flat binary output (xv6.bin) for direct hardware loading
- ✅ Compressed image support (xv6.bin.gz, fs.img.gz) for faster SD card transfers
- ✅ SBI console output for early boot messages (before UART init)
- ✅ CMake deployment target (`make deploy`) for SCP to device
- ✅ Separate linker scripts generated from template (kernel.ld.in)

**Implementation Notes**:
- CMake generates linker script from `kernel.ld.in` with platform-specific KERNEL_BASE
- Build for Orange Pi: `PLATFORM=orangepi cmake .. && make`
- Gzip-compressed images generated automatically; boot scripts try compressed first, fallback to raw
- SBI console uses legacy putchar/getchar for early output
- Files: `kernel/CMakeLists.txt`, `kernel/kernel.ld.in`, `boot/boot.cmd`, `boot/xv6.cmd`, `boot/default.cmd`, `kernel/console.c`, `kernel/sbi.c`

---

### 2. Interrupt Handling Architecture ✅ **COMPLETED**
**Dependencies**: None (foundational improvement)  
**Priority**: Critical  
**Status**: ✅ Completed (January 2026)

- ✅ Separate interrupt stacks from kernel stacks (per-CPU interrupt stacks, 16KB each)
- ✅ Per-CPU state in `percpu.h` with CPU flags (`CPU_FLAG_IN_ITR`, `CPU_FLAG_NEEDS_RESCHED`)
- ✅ Nested interrupt handling support
- ⏳ Bottom-half mechanisms (softirq, tasklets) - planned for future
- ⏳ Interrupt threading support - planned for future

**Implementation Notes**:
- Completed: Dedicated 16KB per-CPU interrupt stacks
- Completed: `CPU_SET_IN_ITR()`/`CPU_CLEAR_IN_ITR()` for interrupt context tracking
- Completed: `NEEDS_RESCHED` flag for deferred rescheduling
- Benefits: Better stack isolation, improved responsiveness, safer interrupt handling
- Files: `kernel/inc/smp/percpu.h`, `kernel/inc/smp/percpu_types.h`, `kernel/irq/trap.c`, `kernel/irq/`

**Stack Architecture** (Implemented):
```
Thread → Kernel Stack (syscalls)
Interrupt → Per-CPU Interrupt Stack (16KB) → Top-half processing
          → RCU quiescent state via rcu_check_callbacks()
          → Deferred work via per-CPU RCU kthreads
```

---

### 3. Thread Scheduling Improvements ✅ **SUBSTANTIALLY COMPLETED**
**Dependencies**: Interrupt handling improvements (completed)  
**Priority**: High  
**Status**: 🔄 Scheduler infrastructure completed, policies in progress

- ✅ Pluggable scheduling class infrastructure (`struct sched_class`)
- ✅ Per-CPU run queues (`struct rq`) with per-CPU locking
- ✅ Scheduling entity abstraction (`struct sched_entity`)
- ✅ CPU affinity support (`cpumask_t affinity_mask`)
- ✅ 64 major priority levels with two-layer O(1) bitmask lookup
- ✅ 4 minor priority subqueues within each major priority (256 total levels)
- ✅ FIFO scheduling class with load balancing (`sched_fifo.c`)
- ✅ IDLE scheduling class for idle threads
- ✅ Comprehensive scheduler test suite (`rq_test.c`)
- ✅ Linux-style `on_rq`/`on_cpu` semantics with lock-based wakeup synchronization
- ⏳ CFS-like fair scheduler - planned
- ⏳ Nice values and dynamic priority - planned
- ⏳ Real-time scheduling (SCHED_RR) - planned

**Implementation Notes**:
- Completed: Linux-style `sched_class` with callbacks (enqueue/dequeue/pick_next/put_prev)
- Completed: `sched_entity` separates scheduling state from thread structure
- Completed: Task switch flow: pick_next_task → set_next_task → context_switch → put_prev_task
- Completed: Run queue selection with CPU affinity (`rq_select_task_rq()`)
- Completed: Two-layer ready mask (8-bit top + 64-bit secondary) for O(1) priority lookup
- Completed: FIFO subqueues with minor priority ordering and load balancing
- Completed: Wakeup synchronization with retry loop for CPU migration races
- Files: `kernel/proc/sched.c`, `kernel/proc/rq.c`, `kernel/proc/sched_fifo.c`, `kernel/proc/rq_test.c` (new), `kernel/inc/proc/rq.h`, `kernel/inc/proc/rq_types.h`, `kernel/proc/SCHEDULER_DESIGN.md`

**Scheduling Architecture** (Implemented):
```
struct thread → struct sched_entity → struct rq (per-CPU)
                                  → struct sched_class (FIFO/IDLE/future CFS)

Priority System (256 levels):
  Major Priority (6-bit, 0-63): Two-layer O(1) lookup
    → ready_mask (8-bit): Group presence (8 groups of 8 priorities)
    → ready_mask_secondary (64-bit): Individual priority presence
  Minor Priority (2-bit, 0-3): FIFO subqueue index within major

Task Switch:
  pick_next_task(rq) → set_next_task(se) → __switch_to()
  → context_switch_finish() → put_prev_task(se) / rq_dequeue_task()
```

---

### 4. Enhanced Kernel Thread Support ✅ **CORE IMPLEMENTED**
**Dependencies**: Interrupt handling (completed)  
**Priority**: High  
**Status**: 🔄 Core infrastructure completed, advanced features in progress

- ✅ Kernel thread creation and management (`kthread_create()`)
- ✅ Work queues for deferred work (`kernel/proc/workqueue.c`)
- ✅ Per-CPU RCU callback kthreads
- ✅ Per-CPU worker threads (manager and worker threads)
- ✅ Kernel thread scheduling via `sched_entity`
- ⏳ Thread pools for async operations - planned
- ⏳ Thread-local storage - planned

**Implementation Notes**:
- Completed: Work queues with manager and worker threads
- Completed: Per-CPU RCU kthreads (`rcu_kthread_start_cpu()` called per-CPU in `start_kernel()`)
- Completed: Kernel threads use `sched_entity` for scheduling
- RCU callbacks processed by dedicated per-CPU kthreads, not in scheduler path
- Files: `kernel/proc/kthread.c`, `kernel/proc/workqueue.c`, `kernel/lock/rcu.c`

**RCU Kthread Architecture**:
```
start_kernel() → rcu_kthread_start_cpu(cpuid()) (per-CPU initialization)
Context switch → rcu_check_callbacks() (note quiescent state)
Per-CPU RCU kthread → rcu_process_callbacks_for_cpu()
                    → Invoke ready callbacks (grace period elapsed)
```

---

### 5. VM Locking and Reference Counting ✅ **COMPLETED**
**Dependencies**: None  
**Priority**: High  
**Status**: ✅ Completed (January 2026)

- ✅ Two-level VM locking (rwlock for VMA tree, spinlock for pagetable)
- ✅ VM reference counting with `vm_dup()` and `vm_put()`
- ✅ CPU tracking bitmap for TLB shootdown optimization
- ✅ Process flag macros converted to bit-field operations
- ✅ fd_table and fs_struct reference counting and cloning
- ✅ Clone flags support for process creation

**Implementation Notes**:
- `vm->rw_lock`: Rwlock protects VMA tree structure (read for lookup, write for modify)
- `vm->spinlock`: Spinlock protects pagetable PTE modifications
- `vm->cpumask`: Tracks which CPUs are using this VM (for future TLB shootdown)
- `vm->refcount`: Reference counting for shared VMs
- Locking order: Sleep locks (VFS, VM rwlock) before spinlocks (pgtable, proc)
- Files: `kernel/mm/vm.c`, `kernel/inc/mm/vm.h`, `kernel/inc/mm/vm_types.h`, `kernel/vfs/file.c`, `kernel/vfs/fs.c`

---

### 6. Multi-User and Permission Model
**Dependencies**: VFS enhancements (in progress)  
**Priority**: High

- User and group ID management
- Permission checking (read/write/execute)
- `/etc/passwd` and `/etc/group` file parsing
- `setuid`/`setgid` syscall support
- File ownership and permission bits

**Implementation Notes**:
- Current: Single-user system (all tasks run as root credentials)
- Target: Full Unix permission model
- Files: `kernel/proc/proc.h`, `kernel/vfs/vfs_ops.c`, `user/login.c` (new)

---

### 7. Standard C Library Expansion
**Dependencies**: System call additions  
**Priority**: Medium-High

- POSIX-compliant file I/O (full stdio.h)
- String manipulation (complete string.h)
- Time functions (time.h, localtime, strftime)
- Math library (libm integration)
- Environment variables (getenv/setenv)

**Implementation Notes**:
- Current: Minimal libc in `user/ulib.c`
- Target: Mostly POSIX-compliant userspace libc
- Files: `user/libc/` directory expansion

---

### 8. TTY and Terminal Improvements
**Dependencies**: Device driver layer  
**Priority**: Medium

- Line discipline (cooked/raw mode)
- Job control (foreground/background process groups)
- Terminal control sequences (ANSI escape codes)
- Pseudo-terminals (PTY) for terminal emulation
- Signal generation from terminal (Ctrl-C, Ctrl-Z)

**Implementation Notes**:
- Current: Basic console I/O
- Target: Full TTY subsystem with job control
- Files: `kernel/tty/` (current subsystem), `kernel/tty/TTY_DESIGN.md` (design/status), `kernel/dev/pty.c` (planned)

---

### 9. Block Device Layer
**Dependencies**: Device driver framework (core pieces available)  
**Priority**: Medium

- Generic block device interface
- I/O scheduler (CFQ, deadline, or noop)
- Buffer cache for block I/O
- Support for multiple block devices (virtio-blk, SATA)
- Partition table parsing (MBR/GPT)

**Implementation Notes**:
- Current: Direct virtio_disk access
- Target: Layered block I/O subsystem
- Files: `kernel/dev/block.c` (new), `kernel/dev/genhd.c` (new)

---

### 10. Pseudo-Filesystems (Phase 1)
**Dependencies**: VFS (complete)  
**Priority**: Medium

- `/dev` filesystem (devfs) for device nodes
- Basic `/proc` filesystem (process info)
  - `/proc/<pid>/status`, `/proc/<pid>/cmdline`
  - `/proc/meminfo`, `/proc/cpuinfo`
- `/sys` basics (kernel parameters)

**Implementation Notes**:
- Current: Static device files
- Target: Dynamic device and process info
- Files: `kernel/vfs/devfs.c` (new), `kernel/vfs/procfs.c` (new)

---

### 11. Asynchronous VFS Operations
**Dependencies**: Work queues, kernel threads  
**Priority**: Medium

- Non-blocking I/O support (O_NONBLOCK)
- Async I/O syscalls (aio_read, aio_write)
- I/O completion queues
- Epoll/select/poll implementations

**Implementation Notes**:
- Current: Blocking I/O only
- Target: Full async I/O support
- Files: `kernel/vfs/aio.c` (new), `kernel/proc/poll.c` (new)

---

### 12. Network Stack Enhancements
**Dependencies**: Block device layer (for persistent config), Interrupt handling (for better NIC performance)  
**Priority**: Medium-Low

- TCP connection management improvements
- UDP socket support
- UNIX domain sockets
- Netlink sockets for kernel communication
- Network configuration tools (ifconfig, route)

**Implementation Notes**:
- Current: Basic E1000 NIC driver, minimal TCP/IP
- Target: Complete TCP/IP stack with socket API
- Files: `kernel/net/` expansion, `user/net/` tools

---

### 13. File System Features
**Dependencies**: Block device layer  
**Priority**: Medium

- EXT2 file system support (read/write)
- File system journaling (basic)
- Hard links (complete implementation)
- Extended attributes (xattrs)
- File locking (flock, fcntl locks)

**Implementation Notes**:
- Current: Custom xv6fs, tmpfs
- Target: Multiple FS support with advanced features
- Files: `kernel/vfs/ext2.c` (new), `kernel/vfs/lock.c` (new)

---

## Long-Term Goals (Ambitious)

These are stretch goals that would transform xv6 into a self-sufficient development platform and general-purpose OS.

### 14. Self-Hosting Capability ⭐ **TOP PRIORITY**
**Dependencies**: Most near-term goals, especially LibC and filesystems  
**Priority**: Highest long-term goal

Milestones:
- Toolchain bootstrap (compiler, assembler, linker, build tools)
- Core development userspace (editor, shell improvements, core utils, debugger path)
- Self-compilation of kernel and userspace inside xv6

**Implementation Notes**:
- **Major milestone**: full self-hosting
- Requires stable FS, expanded LibC, and mature memory management
- Estimated effort: 6-12 months
- Files: system-wide toolchain/userland integration (including `user/gcc/`, `user/binutils/`)

---

### 15. Web Server Hosting Capability ⭐ **MAJOR MILESTONE**
**Dependencies**: Network stack, threading, filesystem stability  
**Priority**: High long-term goal

- Stable multi-connection TCP/IP stack
- HTTP/1.1 server path (static + dynamic content)
- CGI/FastCGI-style execution support
- Optional virtual-hosting support

**Implementation Notes**:
- Goal: host a simple website (static pages + basic CGI)
- Demonstrates network and userspace maturity
- Files: `user/httpd/` (new), CGI runtime

---

### 16. Dynamic Linking and Loading
**Dependencies**: ELF loader improvements, memory management  
**Priority**: Medium

- Shared library support (.so files)
- Dynamic linker (ld.so)
- Position-independent code (PIC) generation
- Lazy binding and symbol resolution
- dlopen/dlsym/dlclose API

**Implementation Notes**:
- Reduces binary sizes significantly
- Required for plugin systems
- Files: `kernel/exec.c` enhancements, `user/ld.so/` (new)

---

### 17. Advanced Pseudo-Filesystems (Phase 2)
**Dependencies**: Basic procfs/sysfs (near-term)  
**Priority**: Medium

- Complete `/proc` implementation
  - `/proc/<pid>/maps` (memory maps)
  - `/proc/<pid>/fd/` (file descriptors)
  - `/proc/sys/` (kernel tunables)
- Full `/sys` filesystem
  - Device hierarchy
  - Kernel module parameters
- `/dev/pts` (pseudo-terminal devices)

**Implementation Notes**:
- Provides Linux-like system introspection
- Files: `kernel/vfs/procfs.c`, `kernel/vfs/sysfs.c` expansion

---

### 18. Kernel Module System
**Dependencies**: Dynamic linking, stable kernel APIs  
**Priority**: Medium

- Loadable kernel modules (.ko files)
- Module loading/unloading (insmod/rmmod)
- Symbol export/import for modules
- Module dependency resolution
- Device driver modules (e.g., additional FS, network drivers)

**Implementation Notes**:
- Allows extensibility without kernel recompilation
- Files: `kernel/module.c` (new), module loader infrastructure

---

### 19. Graphics and GUI Framework (Exploratory)
**Dependencies**: Framebuffer driver, window system  
**Priority**: Low (mention only)

- Framebuffer console baseline
- Minimal window-system prototype
- Lightweight widget/toolkit experiments

**Implementation Notes**:
- Exploratory only (no fixed implementation schedule)
- Significant effort; remote-display approach remains a fallback
- Files: `kernel/dev/fb.c` (framebuffer), `user/wm/` (window manager)

---

### 20. MicroPython in Kernel Space (Experimental)
**Dependencies**: Kernel memory management  
**Priority**: Very Low (for fun)

- Embed MicroPython interpreter in kernel
- Expose kernel APIs to Python scripts
- Kernel scripting and prototyping
- Educational/experimental purposes only

**Implementation Notes**:
- Experimental only (non-critical)
- Potentially useful for rapid kernel prototyping
- Files: `kernel/micropython/` (new)

---

## Dependency Diagram

**Near-Term Goals:**
1. **Memory Management** → foundational for interrupt, scheduler, and high-load subsystems
2. **Interrupt Handling** ✅ completed → enables robust kernel-thread and network paths
3. **Scheduling** ✅ infrastructure complete → pending additional policies (CFS/nice/RT)
4. **Kernel Threads** ✅ core complete → enables async subsystems and callback pipelines
5. Multi-user model depends on VFS/process credential expansion
6. LibC expansion feeds async I/O and self-hosting
7. TTY → block layer → pseudo-FS → async VFS remains the main I/O chain
8. Network and FS feature expansion build on the above

**Long-Term Goals:**
- Near-term completion gates **14. Self-Hosting** ⭐
- Network + threading + FS maturity enable **15. Web Server Hosting** ⭐
- 16 → 18 dependency: dynamic linking precedes kernel modules
- 17, 19, and 20 remain non-blocking exploratory/extension tracks

---

## Recommended Implementation Order

### Phase 1: Core Infrastructure (3-6 months) ✅ **MOSTLY COMPLETE (as of Feb 2026)**
1. **Dynamic memory probing and page frame management** - In progress
2. ✅ **Interrupt handling architecture** (separate stacks, per-CPU state) - COMPLETED
3. ✅ **Thread scheduling infrastructure** (sched_class, per-CPU rq, sched_entity) - COMPLETED
4. ✅ **Enhanced kernel threads** (work queues, RCU kthreads) - COMPLETED
5. Multi-user support - Pending

### Phase 2: I/O Infrastructure (3-6 months) ⚡ **ACTIVE PHASE (as of Feb 2026)**
6. TTY/terminal subsystem
7. Block device layer
8. File system features (ext2)
9. Pseudo-filesystems (devfs, basic procfs)
10. Async VFS operations

### Phase 3: System Completeness (3-6 months)
11. Standard LibC expansion
12. Network stack enhancements (benefits from interrupt ✅ + memory improvements)
13. File locking and advanced FS features
14. CFS-like fair scheduler policy (infrastructure ✅ ready)

### Phase 4: Self-Hosting Push (6-12 months) ⭐
15. **Toolchain bootstrap** (GCC/Clang port)
16. **System utilities** (editors, build tools)
17. **Self-compilation** milestone
18. Dynamic linking for smaller binaries

### Phase 5: Advanced Features (6-12 months) ⭐
19. **Web server hosting** capability
20. Advanced pseudo-filesystems
21. Kernel modules system

### Phase 6: Experimental (ongoing)
22. GUI framework (exploratory)
23. MicroPython (for fun)

---

## Success Metrics

### Near-Term Success (as of Feb 2026):
- [x] **Dynamic memory probing via FDT** ✅ (runtime probing and device config active)
- [x] **Interrupt handling separated from thread context** ✅
- [x] **Per-CPU interrupt stacks (16KB)** ✅
- [x] **Pluggable scheduler infrastructure with sched_class** ✅
- [x] **Per-CPU run queues with CPU affinity support** ✅
- [x] **Linux-style on_rq/on_cpu semantics with lock-based wakeup synchronization** ✅
- [x] **Per-CPU RCU kthreads for callback processing** ✅
- [x] **VM locking with rwlock + spinlock separation** ✅
- [x] **Orange Pi RV2 hardware support** ✅
- [x] **SBI console for early boot output** ✅
- [ ] Memory zone management (DMA/Normal/High)
- [ ] Bottom-half processing (softirq/tasklets) reduces interrupt latency
- [ ] CFS-like fair scheduler policy
- [ ] Can run standard Unix utilities (bash, coreutils)
- [ ] Multiple users can log in with proper permissions
- [ ] System remains stable under load (stressfs, usertests)
- [ ] Network services work reliably (TCP echo server, basic HTTP)

### Long-Term Success:
- [ ] **⭐ Can compile its own kernel and userspace**
- [ ] **⭐ Can host a simple website with dynamic content**
- [ ] Can load and unload kernel modules dynamically
- [ ] Supports multiple file systems (xv6fs, tmpfs, ext2)
- [ ] Provides complete POSIX-like development environment

---

## Contributing

When implementing features from this roadmap:

1. **Follow dependency order** - don't skip prerequisites
2. **Update documentation** - keep VFS_DESIGN.md and this file in sync
3. **Write tests** - add to `user/usertests.c` or create new test programs
4. **Maintain code quality** - follow existing style, add Doxygen comments
5. **Consider compatibility** - maintain existing xv6 test compatibility

---

## Notes

- **System Completeness Focus**: Near-term goals prioritize building a complete, stable Unix-like system before attempting ambitious features.
- **Self-Hosting is Key**: The ability to compile xv6 under xv6 validates OS maturity and completeness.
- **Web Hosting Milestone**: Demonstrates network stack reliability and real-world utility.
- **Hardware Support**: Orange Pi RV2 support demonstrates portability to real hardware.
- **Experimental Features**: GUI and kernel MicroPython remain low-priority exploratory tracks.

Last Updated: February 2026
