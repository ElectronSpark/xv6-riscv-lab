# xv6-RISCV Enhanced: Development Roadmap

This document outlines the planned enhancements to the xv6-RISCV operating system, organized into **near-term** (system completeness) and **long-term** (ambitious) goals.

---

## Near-Term Goals (System Completeness)

These features focus on building a complete, functional Unix-like system with modern OS capabilities.

### 1. Dynamic Memory Probing and Page Frame Management
**Dependencies**: None (foundational improvement)  
**Priority**: Critical

- Dynamic memory detection and probing at boot time
- Support non-contiguous memory regions (multiple DRAM banks)
- Improved page frame allocator with better metadata management
- Memory zone management (DMA, Normal, High memory zones)
- Per-CPU page frame caches for allocation performance
- Memory hot-add/hot-remove support (basic)
- Better memory statistics and accounting
- NUMA-aware memory allocation (if multi-node support added)

**Implementation Notes**:
- Current: Fixed memory layout, simple buddy allocator
- Target: Dynamic memory discovery, zone-based allocation, optimized page frame management
- Benefits: Support varied hardware configs, better memory utilization, improved allocation performance
- Files: `kernel/mm/page_alloc.c`, `kernel/mm/memblock.c` (new), `kernel/mm/zone.c` (new), `kernel/start.c`

**Memory Architecture**:
```
Current:
  Fixed memory layout → Buddy allocator → Page allocation

Target:
  Boot probe → Memory zones (DMA/Normal/High) → Per-CPU caches
  → Buddy allocator (zone-aware) → Optimized page allocation
```

---

### 2. Interrupt Handling Architecture ⚡ **CURRENT FOCUS**
**Dependencies**: None (foundational improvement)  
**Priority**: Critical

- Separate interrupt stacks from kernel stacks (per-CPU interrupt stacks)
- Implement top-half (hard IRQ context) and bottom-half (deferred processing)
- Bottom-half mechanisms: softirq, tasklets, or workqueues
- Interrupt threading support (optional threaded IRQs)
- Improve interrupt latency and reduce time spent with interrupts disabled
- Nested interrupt handling (if supported by RISC-V configuration)

**Implementation Notes**:
- Current: Interrupts use kernel stack, all processing in interrupt context
- Target: Dedicated interrupt stacks, minimal top-half with deferred bottom-half
- Top-half: Acknowledge interrupt, minimal critical work, schedule bottom-half
- Bottom-half: Process bulk of interrupt work with interrupts enabled
- Benefits: Better stack isolation, improved responsiveness, safer interrupt handling
- Files: `kernel/trap.c`, `kernel/inc/riscv.h`, `kernel/proc/softirq.c` (new), `kernel/proc/tasklet.c` (new)

**Stack Architecture**:
```
Current:
  Process → Kernel Stack (handles both syscalls and interrupts)

Target:
  Process → Kernel Stack (syscalls only)
  Interrupt → Interrupt Stack → Top-half → Schedule bottom-half
  Bottom-half → Kernel thread or softirq context (safe for sleeping)
```

---

### 3. Process Scheduling Improvements
**Dependencies**: Interrupt handling improvements (optional, but beneficial)  
**Priority**: High

- Implement Multilevel Feedback Queue (MLFQ) or Completely Fair Scheduler (CFS)
- Add process priority levels and nice values
- Support CPU affinity for multi-core systems
- Implement real-time scheduling classes (SCHED_FIFO, SCHED_RR)

**Implementation Notes**:
- Current: Simple round-robin scheduler
- Target: Fair, priority-based scheduling with interactive process boost
- Interrupt improvements help reduce scheduling latency
- Files: `kernel/proc/proc.c`, `kernel/proc/sched.c` (new)

---

### 4. Enhanced Kernel Thread Support
**Dependencies**: Interrupt handling (work queues integrate with bottom-half)  
**Priority**: High

- Improve kernel thread creation and management APIs
- Add thread pools for async operations
- Implement work queues for deferred work (integrates with interrupt bottom-half)
- Support kernel thread priorities
- Per-CPU worker threads for interrupt processing

**Implementation Notes**:
- Current: Basic kernel threads via `kthread_create()`
- Target: Full-featured threading with thread-local storage
- Work queues can be used as bottom-half mechanism for interrupts
- Files: `kernel/proc/kthread.c`, `kernel/proc/workqueue.c` (new)

---

### 6. Multi-User Support
**Dependencies**: VFS enhancements (partially complete)  
**Priority**: High

- User and group ID management
- Permission checking (read/write/execute)
- `/etc/passwd` and `/etc/group` file parsing
- `setuid`/`setgid` syscall support
- File ownership and permission bits

**Implementation Notes**:
- Current: Single-user system (all processes run as root)
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
- Job control (foreground/background processes)
- Terminal control sequences (ANSI escape codes)
- Pseudo-terminals (PTY) for terminal emulation
- Signal generation from terminal (Ctrl-C, Ctrl-Z)

**Implementation Notes**:
- Current: Basic console I/O
- Target: Full TTY subsystem with job control
- Files: `kernel/dev/tty.c` (new), `kernel/dev/pty.c` (new)

---

### 9. Block Device Layer
**Dependencies**: Device driver framework (partially exists)  
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

**Phase A: Toolchain Bootstrap**
- Port GCC or Clang to run under xv6
- Assembler (GNU as or custom)
- Linker (GNU ld or lld)
- Make and build utilities

**Phase B: System Development Tools**
- Text editors (vi/nano port)
- Shell scripting improvements (bash-like features)
- Core utilities (grep, sed, awk ports)
- Debugger (gdb stub or simple debugger)

**Phase C: Self-Compilation**
- Compile xv6 kernel under xv6
- Build all userspace tools under xv6
- Package management (simple)

**Implementation Notes**:
- **MAJOR MILESTONE**: Achieve full self-hosting
- Requires: Stable FS, complete LibC, sufficient memory management
- Estimated effort: 6-12 months of development
- Files: Entire system recompilation, `user/gcc/` port, `user/binutils/` port

---

### 15. Web Server Hosting Capability ⭐ **MAJOR MILESTONE**
**Dependencies**: Network stack, threading, filesystem stability  
**Priority**: High long-term goal

- Stable TCP/IP stack with multiple connections
- HTTP/1.1 server implementation
- CGI or FastCGI support for dynamic content
- Static file serving
- Virtual hosting support

**Implementation Notes**:
- Goal: Host a simple website (static pages + basic CGI)
- Demonstrates OS maturity and network stack reliability
- Files: `user/httpd/` (new), CGI runtime

**Example Use Cases**:
- Personal blog or documentation site
- OS status dashboard
- Simple REST API for system monitoring

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

**Potential Approaches**:
- Simple framebuffer console (80x25 text or basic graphics)
- Minimalist window system (inspired by Plan 9 or X11 basics)
- GUI toolkit (simple widget library)

**Implementation Notes**:
- **No concrete plan yet** - exploratory only
- Significant effort required
- Alternative: VNC/X11 client to remote display
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
- **Purely for fun** - not a practical goal
- Useful for rapid kernel prototyping
- Files: `kernel/micropython/` (new)

---

## Dependency Diagram

**Near-Term Goals:**
1. **Memory Management** (dynamic probing, zones) - Foundational
   - Feeds into → Interrupt Handling, Scheduling, all memory-intensive features
2. **Interrupt Handling** [CURRENT] (separate stacks, top/bottom-half)
   - Depends on → Memory Management
   - Feeds into → Kernel Threads (work queues), Network Stack
3. **Scheduling** (MLFQ/CFS) 
   - Benefits from → Interrupt improvements
4. **Kernel Threads** (work queues for bottom-half)
   - Depends on → Interrupt Handling
5. Multi-User Support (depends on VFS - done)
6. LibC Expansion → Feeds into Async VFS, Self-hosting
7. TTY/Terminal → Block Device Layer → Pseudo-FS → Async VFS
8. Network Stack (benefits from Interrupt + Memory improvements)
9. FS Features (ext2, xattrs)

**Long-Term Goals:**
- ALL NEAR-TERM → **14. Self-Hosting** ⭐ (TOP PRIORITY)  
- Network + Threading + FS → **15. Web Server Hosting** ⭐ (MAJOR GOAL)
- 16. Dynamic Linking → 18. Kernel Modules  
- 17. Advanced Pseudo-FS  
- 19. GUI (exploratory)  
- 20. MicroPython (for fun)

---

## Recommended Implementation Order

### Phase 1: Core Infrastructure (3-6 months) ⚡ **CURRENT PHASE**
1. **Dynamic memory probing and page frame management**
2. **Interrupt handling architecture** (separate stacks, top/bottom half)
3. Process scheduling improvements
4. Enhanced kernel threads (with work queue bottom-half support)
5. Multi-user support

### Phase 2: I/O Infrastructure (3-6 months)
6. TTY/terminal subsystem
7. Block device layer
8. File system features (ext2)
9. Pseudo-filesystems (devfs, basic procfs)
10. Async VFS operations

### Phase 3: System Completeness (3-6 months)
11. Standard LibC expansion
12. Network stack enhancements (benefits from interrupt + memory improvements)
13. File locking and advanced FS features

### Phase 4: Self-Hosting Push (6-12 months) ⭐
14. **Toolchain bootstrap** (GCC/Clang port)
15. **System utilities** (editors, build tools)
16. **Self-compilation** milestone
17. Dynamic linking for smaller binaries

### Phase 5: Advanced Features (6-12 months) ⭐
18. **Web server hosting** capability
19. Advanced pseudo-filesystems
20. Kernel modules system

### Phase 6: Experimental (ongoing)
21. GUI framework (exploratory)
22. MicroPython (for fun)

---

## Success Metrics

### Near-Term Success:
- [ ] **Dynamic memory probing supports varied hardware configurations**
- [ ] **Interrupt handling separated from process context**
- [ ] **Bottom-half processing reduces interrupt latency**
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
- **Experimental Features**: GUI and kernel MicroPython are low priority - mention only for future exploration.

Last Updated: December 30, 2025
