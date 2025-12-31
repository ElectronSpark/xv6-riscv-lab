# Enhanced xv6-RISCV Operating System

A significantly extended and modernized version of the xv6 operating system for RISC-V architecture, featuring advanced kernel subsystems and a modern build system.

## Important Notice

**This is a derivative work based on MIT's xv6 operating system.** The original xv6 was created by Frans Kaashoek, Robert Morris, and Russ Cox at MIT PDOS. This enhanced version contains substantial modifications and new features developed independently.

**For information about the original xv6 project**, including its authors, contributors, and contact information, please see [XV6_ORIGINAL_README](XV6_ORIGINAL_README).

**The enhancements and modifications in this repository are separate from the original xv6 project.** Do not contact the original xv6 authors regarding features or issues specific to this enhanced version.

**For a detailed explanation of the relationship between this project and original xv6**, see [ABOUT.md](ABOUT.md).

## Overview

This project originated from MIT's xv6, a re-implementation of Unix Version 6 (v6). The original xv6 loosely follows the structure and style of v6 but is implemented for modern RISC-V multiprocessors using ANSI C.

This enhanced version has been substantially extended with production-grade features, modern kernel subsystems, and improved architecture to create a more fully functional operating system suitable for advanced educational purposes and experimental use.

### Key Improvements Over Original xv6

- **Virtual File System (VFS) Layer**: Complete VFS abstraction supporting multiple file systems
- **Advanced Memory Management**: Slab allocator, page cache, and sophisticated memory subsystems
- **Modern Build System**: CMake-based build system with proper dependency management
- **Extended File System Support**: Multiple file system types (xv6fs, tmpfs)
- **Advanced Synchronization**: Reader-writer locks, completions, and fine-grained locking
- **Device Management**: Character and block device framework with proper abstraction
- **Network Stack**: Basic networking capabilities with E1000 driver support
- **Enhanced Process Management**: Improved scheduling and process lifecycle management
- **Separate Interrupt Stacks**: Dedicated interrupt stacks per CPU for better interrupt handling and nested interrupt support

## Features

### File System
- **Virtual File System (VFS)**: Unified interface for multiple file system types
- **Multiple File Systems**: Native xv6fs and in-memory tmpfs support
- **Hierarchical Mounting**: Support for mounting file systems at arbitrary points
- **Symbolic Links**: Full symbolic link support with loop detection
- **Hard Links**: Multiple directory entries for the same inode
- **Device Files**: Character and block device special files
- **Orphan Inode Management**: Proper handling of unlinked but open files
- **Lazy Unmount**: Graceful unmount even with active file handles

### Memory Management
- **Slab Allocator**: Efficient kernel object caching system
- **Page Cache**: Block-level caching for file system I/O
- **Virtual Memory**: Demand paging and copy-on-write
- **Memory Protection**: Per-process address spaces
- **Dedicated Interrupt Stacks**: Separate per-CPU interrupt stacks (16KB) for robust nested interrupt handling

### Synchronization Primitives
- **Mutexes**: Sleeping locks for kernel synchronization
- **Spinlocks**: CPU-level locks for low-level synchronization
- **Reader-Writer Locks**: Efficient shared/exclusive locking
- **Completion Variables**: Event-based synchronization
- **Atomic Operations**: Lock-free primitive operations
- **RCU (Read-Copy-Update)**: Lock-free read-side synchronization for scalable concurrent access (Note: RCU infrastructure is planned/in development for future path lookup optimization)

### Device Support
- **Character Devices**: Console, null, zero, random
- **Block Devices**: VirtIO disk driver
- **Network Devices**: E1000 network interface card
- **PCI Support**: PCI device enumeration and management

### Networking
- Basic TCP/IP stack
- E1000 Ethernet driver
- Socket interface
- Network file operations

### Process Features
- **Process Management**: Fork, exec, wait, exit
- **Signals**: Basic signal handling (SIGKILL, etc.)
- **Pipes**: Anonymous and named pipes (FIFOs)
- **File Descriptor Table**: Per-process fd management
- **Working Directory**: Per-process current and root directories (chroot support)

### System Calls
Complete POSIX-style system call interface including:
- File operations: open, read, write, close, stat, lseek
- Directory operations: mkdir, rmdir, chdir, chroot
- File system operations: mount, umount, link, unlink, symlink
- Process operations: fork, exec, wait, exit, kill
- Pipe and socket operations: pipe, connect
- Device operations: mknod

## Architecture

### Directory Structure

```
xv6-riscv-6/
├── kernel/              # Kernel source code
│   ├── inc/            # Kernel headers
│   │   └── vfs/       # VFS headers
│   ├── vfs/           # Virtual File System implementation
│   ├── mm/            # Memory management subsystem
│   ├── proc/          # Process management
│   ├── lock/          # Synchronization primitives
│   ├── dev/           # Device drivers
│   └── ...
├── user/               # User-space programs
├── mkfs/              # File system creation tool
├── test/              # Kernel unit tests
├── build/             # Build output directory
├── CMakeLists.txt     # Main CMake configuration
└── README.md          # This file
```

### Key Components

- **VFS Core** (`kernel/vfs/`): Virtual file system abstraction layer
- **File Systems** (`kernel/vfs/xv6fs/`, `kernel/vfs/tmpfs/`): File system implementations
- **Memory Management** (`kernel/mm/`): Physical and virtual memory, slab allocator
- **Process Management** (`kernel/proc/`): Scheduler, context switching, fork/exec
- **Locking** (`kernel/lock/`): Mutexes, spinlocks, rwlocks
- **Devices** (`kernel/dev/`): Device driver framework

## Building and Running

### Prerequisites

You have two options for setting up the build environment:

#### Option 1: Using Docker (Recommended for Quick Start)

The easiest way to get started is using the provided Docker container:

```bash
# Run the container setup script
./container.sh
```

This script will:
1. Build a Docker image with all required tools (RISC-V toolchain, QEMU, CMake)
2. Launch an interactive container with the project directory mounted
3. Provide a ready-to-use build environment

Inside the container, you can build and run xv6 directly:
```bash
# Inside the Docker container
export LAB=fs
mkdir -p build && cd build
cmake ..
make -j$(nproc)
make qemu
```

#### Option 2: Native Installation

If you prefer to install tools natively:

1. **RISC-V Toolchain**: Install the RISC-V GNU toolchain
   ```bash
   # On Ubuntu/Debian
   sudo apt-get install gcc-riscv64-unknown-elf
   
   # Or build from source
   git clone https://github.com/riscv/riscv-gnu-toolchain
   cd riscv-gnu-toolchain
   ./configure --prefix=/opt/riscv --with-arch=rv64gc --with-abi=lp64d
   make
   export PATH=/opt/riscv/bin:$PATH
   ```

2. **QEMU**: Install QEMU with RISC-V support
   ```bash
   # On Ubuntu/Debian
   sudo apt-get install qemu-system-misc
   
   # Or build from source for latest version
   ```

3. **CMake**: Version 3.10 or higher
   ```bash
   sudo apt-get install cmake
   ```

4. **Perl**: Required for system call generation
   ```bash
   sudo apt-get install perl
   ```

### Building

The project uses CMake for building. **The old Makefile in the root directory is obsolete and will not work.**

The LAB environment variable controls which features are included.

**Currently Supported LAB Configurations:**
- `fs`: File system features with VFS (single core) - **Recommended**
- `util`: Basic utilities (6 cores)

**Note**: Due to substantial architectural changes during enhancement, other LAB configurations (syscall, pgtbl, traps, lazy, lock, mmap, net) are currently not functional and are being updated.

**Important**: The root directory `Makefile` is obsolete and will fail. Use CMake as shown below.

```bash
# Set LAB environment variable
export LAB=fs

# Create and enter build directory
mkdir -p build
cd build

# Configure with CMake
cmake ..

# Build kernel and user programs
make -j$(nproc)
```

**Note**: Do not use the root directory Makefile - it is obsolete and for reference only. Always use the CMake build system as described above.

### Running

Run xv6 in QEMU (must be executed from the `build` directory):

```bash
cd build
make qemu
```

Run with GDB debugging (from the `build` directory):

```bash
# In one terminal
cd build
make qemu-gdb

# In another terminal
cd build
gdb-multiarch kernel/kernel
(gdb) target remote localhost:26000
(gdb) b main
(gdb) c
```

#### Using Visual Studio Code (Optional)

The project includes VSCode configuration for integrated debugging:

**Prerequisites:**
- Visual Studio Code installed
- C/C++ extension (`ms-vscode.cpptools`)
- CMake Tools extension (`ms-vscode.cmake-tools`)
- `gdb-multiarch` installed on your system

**Debugging with VSCode:**

1. Open the project folder in VSCode
2. Set the `LAB` environment variable in `.vscode/tasks.json` (default is `fs`)
3. Press `F5` or use "Run and Debug" panel
4. Select "kernel-debug" configuration

The debug configuration will:
- Automatically configure and build xv6 with CMake
- Launch QEMU in debug mode (listening on port 26000)
- Attach GDB to the running kernel
- Stop at kernel entry point

**Features:**
- Set breakpoints in kernel code
- Step through execution
- Inspect variables and memory
- View call stacks
- Use GDB console for advanced debugging

**Note:** VSCode debugging has been tested on Linux. Configurations may need adjustment for other operating systems.

**Troubleshooting:**
- Ensure `gdb-multiarch` path in `.vscode/launch.json` matches your installation
- Check that port 26000 is not already in use
- Verify the compiler path in `.vscode/c_cpp_properties.json`

### Running Tests

```bash
cd build
make test
```

## Usage Examples

### Basic File Operations

```bash
$ ls
.              40755 1 1024
..             40755 1 1024
cat            100644 2 40152
echo           100644 3 39576
grep           100644 4 41768
init           100644 5 39840
kill           100644 6 39728
ln             100644 7 39456
ls             100644 8 41904
mkdir          100644 9 39528
rm             100644 10 39504
sh             100644 11 53232
...
README.md      100644 32 17064
console        20666 33 0

$ mkdir testdir
$ cd testdir
$ echo "Hello, World!" > test.txt
$ cat test.txt
Hello, World!

$ ln -s test.txt link.txt
$ cat link.txt
Hello, World!
```

### Process Management

The `ps` command shows all running processes with their states:

```bash
$ ps
[53362064] Process List:
[53378382] 16 uninterruptible [K] worker_process
[53405303] 14 uninterruptible [K] worker_process
[53424259] 12 uninterruptible [K] worker_process
[53459361] 10 uninterruptible [K] worker_process
[53488608] 1 interruptible [U] init
[53503747] 21 uninterruptible [K] worker_process
[53550390] 19 uninterruptible [K] worker_process
[53566805] 17 uninterruptible [K] worker_process
[53580854] 15 uninterruptible [K] worker_process
[53614719] 13 uninterruptible [K] worker_process
[53635315] 8 uninterruptible [K] worker_process
[53657094] 6 uninterruptible [K] worker_process
[53677716] 4 interruptible [K] manager_process
[53709368] 2 interruptible [K] manager_process
[53738413] 22 interruptible [U] sh
[53763882] 20 uninterruptible [K] worker_process
[53788887] 18 uninterruptible [K] worker_process
[53836754] 11 uninterruptible [K] worker_process
[53855844] 9 uninterruptible [K] worker_process
[53882525] 7 uninterruptible [K] worker_process
[53906966] 5 interruptible [K] pcache_flusher
[53920468] 23 running [U] ps
```

Process states: `running`, `interruptible`, `uninterruptible`  
Process types: `[U]` user process, `[K]` kernel thread

### Memory Management

Check memory usage with the `free` command:

```bash
$ free
[113967004] Buddy: 30417 free + 42 cached = 30459 pages (118.9M)
[114006814] Slab:  74 pages (296KB)

$ free -a
[163643732] Buddy System Statistics:
[163667830] ========================
[163682078] order(0): 0 blocks (0B) + 59 cached (236K)
[163747862] order(1): 0 blocks (0B)
[163792544] order(2): 0 blocks (0B) + 3 cached (48K)
[163909442] order(3): 0 blocks (0B)
[163943975] order(4): 1 blocks (64K)
[163986220] order(5): 1 blocks (128K)
[164021028] order(6): 0 blocks (0B)
[164053873] order(7): 1 blocks (512K)
[164104100] order(8): 0 blocks (0B)
[164142785] order(9): 1 blocks (2.0M)
[164182807] order(10): 29 blocks (116.0M)
[164234792] ------------------------
[164272523] Buddy: 30384 free + 71 cached = 30455 pages (118.9M)

=== SLAB CACHE STATISTICS ===
[164391646] NAME             OBJSZ    TOTAL   ACTIVE     FREE    PAGES
[164488041] 32: objsz=32 total=4 active=10 free=1 pages=4
[164529760] 64: objsz=64 total=0 active=0 free=0 pages=0
[164583449] 128: objsz=128 total=6 active=21 free=1 pages=6
...
[165817909] -----------------------------
[165841735] Slab:  78 pages (312KB)
```

### Debugging Tools

```bash
$ dumpchan
[201040807] Channel Queue Dump:

$ dumppcache
# Dumps page cache statistics
```

### File System Features

**Note**: The VFS layer supports mounting file systems via system calls (`sys_mount`, `sys_umount`), but a user-space `mount` command is not yet implemented. Mount operations are currently only available to programs that directly invoke the syscalls.

To use mount functionality, you would need to create a user program that calls:
```c
// Example mount usage (in C program)
mount("tmpfs", "/mnt", NULL, 0, NULL);
// ... use mounted filesystem ...
umount("/mnt");
```

## Configuration

### Build Configuration

The LAB environment variable determines which kernel features are compiled.

**Currently Working Configurations:**
- `fs`: File system features with VFS layer (single core) - **Recommended for general use**
- `util`: Basic utilities and core functionality (6 cores)

**Configurations Under Development:**

Due to the extensive architectural changes (VFS implementation, new memory management, etc.), the following LAB configurations are currently being updated to work with the enhanced kernel:
- `syscall`: System call lab features (not functional)
- `pgtbl`: Page table management features (not functional)
- `traps`: Trap handling features (not functional)
- `lazy`: Lazy allocation features (not functional)
- `lock`: Lock optimization features (not functional)
- `mmap`: Memory mapping features (not functional)
- `net`: Network stack (not functional)

**Recommendation**: Use `LAB=fs` for accessing all enhanced features including the complete VFS implementation.

### Kernel Parameters

Edit `kernel/param.h` to configure:
- `NPROC`: Maximum number of processes (default: 64)
- `NOFILE`: Maximum open files per process (default: 16)
- `MAXOPBLOCKS`: Maximum file system operations per transaction
- `NBUF`: Number of buffer cache entries

## Documentation

### Source Code Documentation

The project includes comprehensive Doxygen documentation for all kernel subsystems.

**Generating Documentation:**

```bash
# Install Doxygen (if not already installed)
sudo apt-get install doxygen graphviz

# Generate documentation
doxygen Doxyfile

# View documentation
firefox docs/html/index.html
# or
xdg-open docs/html/index.html
```

The generated documentation includes:
- API reference for all kernel functions and data structures
- Call graphs and caller graphs
- Include dependency diagrams
- File and directory structure
- Cross-referenced source code browsing

**Alternative: Using CMake**

```bash
cd build
cmake .. -DBUILD_DOCUMENTATION=ON
make doc
```

### Design Documents

- **VFS Design**: [kernel/vfs/VFS_DESIGN.md](kernel/vfs/VFS_DESIGN.md) - Comprehensive VFS architecture documentation
- **Unmount Design**: [kernel/vfs/UNMOUNT_DESIGN.md](kernel/vfs/UNMOUNT_DESIGN.md) - Mount/unmount implementation details

### Original xv6 Documentation

- **Original xv6 README**: [XV6_ORIGINAL_README](XV6_ORIGINAL_README) - Original xv6 project information, authors, and contributors
- **xv6 Book (RISC-V)**: https://pdos.csail.mit.edu/6.828/2023/xv6/book-riscv-rev3.pdf
- **MIT 6.1810 Course**: https://pdos.csail.mit.edu/6.1810/

### Other References

- [RISC-V Specifications](https://riscv.org/technical/specifications/)
- [Linux Kernel Documentation](https://www.kernel.org/doc/) - Design patterns inspiration

## Development
**Integrated Development Environment:**

The project includes full Visual Studio Code support with pre-configured debugging. See the [Using Visual Studio Code](#using-visual-studio-code-optional) section for setup instructions.
### Code Style

- Follow K&R C style
- Use descriptive variable names
- Comment non-obvious code
- Keep functions focused and short

### Adding New Features

1. **New System Call**: 
   - Add prototype to `kernel/inc/defs.h`
   - Implement in appropriate kernel file
   - Add entry to `user/usys.pl`
   - Update syscall number in `kernel/syscall.c`

2. **New File System**:
   - Create implementation in `kernel/vfs/<fsname>/`
   - Implement `vfs_fs_type_ops` and `vfs_superblock_ops`
   - Register via `vfs_register_fs_type()`
   - See `kernel/vfs/tmpfs/` for example

3. **New Device Driver**:
   - Implement driver in `kernel/dev/`
   - Register character or block device
   - Implement required operations

### Debugging

Enable debug output:
```c
// In kernel source
printf("debug: variable = %d\n", var);
```

Use GDB:
```bash
make qemu-gdb
gdb-multiarch kernel/kernel
```

Common GDB commands:
```
(gdb) b sys_open          # Break at system call
(gdb) info threads        # Show CPU threads
(gdb) thread 2            # Switch to CPU 2
(gdb) bt                  # Backtrace
```

## Known Issues and Limitations

1. **No Dentry Cache**: Path resolution may be slower than modern systems
2. **Simple Scheduler**: Round-robin scheduler without priority or real-time support
3. **Limited Networking**: Basic network stack, not full TCP/IP
4. **No SMP Optimization**: Limited multiprocessor scalability
5. **Educational Focus**: Not intended for production use without further hardening

## Development Roadmap

### 🎯 Near-Term Goals (System Completeness)

Building a complete, stable Unix-like operating system:

1. **Dynamic Memory Probing & Page Frames** - Memory detection, zones (DMA/Normal), per-CPU caches
2. **✅ Interrupt Handling** *(COMPLETED)* - Separate interrupt/kernel stacks (16KB per CPU), nested interrupt support, dual-entry trap handlers
3. **Process Scheduling** - Implement MLFQ or CFS, priority levels, CPU affinity
4. **Enhanced Kernel Threads** - Thread pools, work queues (for bottom-half), priorities
5. **Multi-User Support** - User/group IDs, permission checking, setuid/setgid
6. **Standard LibC** - POSIX-compliant file I/O, strings, time, math functions
7. **TTY/Terminal** - Line discipline, job control, PTY support
8. **Block Device Layer** - Generic block I/O, I/O scheduler, partition support
9. **Pseudo-Filesystems** - devfs, basic procfs (/proc/<pid>/status, /proc/meminfo)
10. **Async VFS** - Non-blocking I/O, aio_read/aio_write, epoll/select
11. **Network Enhancements** - TCP improvements, UDP, UNIX sockets (benefits from interrupt improvements)
12. **FS Features** - EXT2 support, journaling, xattrs, file locking

### 🚀 Long-Term Goals (Ambitious)

Transforming xv6 into a self-sufficient development platform:

- **⭐ Self-Hosting** (TOP PRIORITY) - Port GCC/Clang, achieve full self-compilation
- **⭐ Web Server Hosting** - Host simple websites with static + dynamic content
- **Dynamic Linking** - Shared libraries (.so), dlopen/dlsym
- **Advanced Pseudo-FS** - Complete procfs/sysfs, /dev/pts
- **Kernel Modules** - Loadable modules, insmod/rmmod
- **GUI Framework** (exploratory) - Framebuffer console, simple window system
- **MicroPython** (for fun) - Kernel-space Python interpreter

**For detailed implementation order and dependencies**, see [ROADMAP.md](ROADMAP.md).

---

## Contributing

This is an educational/research operating system. When contributing:

1. Maintain code quality and documentation
2. Follow existing architectural patterns
3. Add tests for new features
4. Update relevant documentation
5. Check [ROADMAP.md](ROADMAP.md) for planned features and dependencies

## License

This project inherits the license from the original xv6:

```
The code in the files that constitute xv6 is
Copyright 2006-2024 Frans Kaashoek, Robert Morris, and Russ Cox.
```

**Enhancements and modifications** added in this repository are provided under the same license terms. See the [LICENSE](LICENSE) file for complete details.

**Important**: The original xv6 authors are not responsible for and have not endorsed the modifications in this enhanced version. All enhancements are independent work.

## Attribution

### Original xv6

- **Created by**: Frans Kaashoek, Robert Morris, and Russ Cox (MIT PDOS)
- **Original Purpose**: Teaching operating system for MIT's 6.1810
- **More Information**: See [XV6_ORIGINAL_README](XV6_ORIGINAL_README) for complete acknowledgments and contributor list
- **Course Website**: https://pdos.csail.mit.edu/6.1810/

### Enhanced Version

This enhanced version includes significant new implementations:
- Complete VFS layer with multiple file system support
- Advanced memory management with slab allocator
- Modern synchronization primitives (rwlocks, completions)
- CMake-based build system
- Extended system call interface
- Device driver framework
- Network stack integration

**These enhancements are independent modifications and are not part of the original xv6 project.**

## Contact and Support

**For questions about the original xv6**: Please see [XV6_ORIGINAL_README](XV6_ORIGINAL_README) for contact information.

**For questions about the enhancements in this repository**: This is an independent derivative work. Issues or questions about the enhanced features should be directed to the maintainers of this repository, not to the original xv6 authors.

---

**Status**: Active Development  
**Target Platform**: RISC-V 64-bit (RV64GC)  
**Emulation**: QEMU virt machine  
**Build System**: CMake 3.10+  
**Last Updated**: December 2025
