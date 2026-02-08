/**
 * @file clone_flags.h
 * @brief Clone flags for thread creation
 *
 * Defines flags used with clone() system call to control process/thread creation.
 * Most flags are inspired by Linux but with different values.
 * Most are currently unused - defined for future compatibility.
 *
 * @see https://man7.org/linux/man-pages/man2/clone.2.html
 */

#ifndef __KERNEL_CLONE_FLAGS_H
#define __KERNEL_CLONE_FLAGS_H

#define CLONE_CHILD_CLEARTID 0x00010000 // Clear child TID in child's memory on exit
#define CLONE_CHILD_SETTID 0x00020000   // Set child TID in child's memory on creation
#define CLONE_CLEAR_SIGHAND 0x00040000 // Clear signal handlers in child
#define CLONE_DETACHED 0x00080000       // Create detached thread
#define CLONE_FILES 0x00100000         // Share file descriptor table
#define CLONE_FS 0x00200000            // Share filesystem info
#define CLONE_INTO_CGROUP 0x00400000  // Move to new cgroup
#define CLONE_IO 0x00800000            // Share I/O context
#define CLONE_NEWCGROUP 0x01000000   // New cgroup namespace
#define CLONE_NEWIPC 0x02000000      // New IPC namespace
#define CLONE_NEWNET 0x04000000      // New network namespace
#define CLONE_NEWNS 0x08000000       // New mount namespace
#define CLONE_NEWPID 0x10000000      // New PID namespace
#define CLONE_NEWUSER 0x20000000     // New user namespace
#define CLONE_NEWUTS 0x40000000      // New UTS namespace
#define CLONE_PARENT 0x80000000      // Child shares parent's parent
#define CLONE_PARENT_SETTID 0x0010000000 // Set parent TID in parent's memory on child creation
#define CLONE_PID 0x0020000000          // Share PID namespace
#define CLONE_PIDFD 0x0040000000        // Store PID file descriptor
#define CLONE_PTRACE 0x0080000000       // Child is traced
#define CLONE_SETTLS 0x0100000000        // Set TLS in child
#define CLONE_SIGHAND 0x0200000000      // Share signal handlers
#define CLONE_SIGSTOPPED 0x0400000000   // Start child stopped
#define CLONE_SYSTEM 0x0800000000        // Child is a system thread
#define CLONE_THREAD 0x1000000000       // Share thread group
#define CLONE_UNTRACED 0x2000000000     // Child cannot be traced
#define CLONE_VFORK 0x4000000000        // Fork without copying page tables
#define CLONE_VM 0x8000000000          // Share memory space

/**
 * @brief Arguments for clone() system call
 * 
 * This structure is shared between user space and kernel space.
 * User space passes a pointer to this structure to clone().
 */
struct clone_args {
    uint64 flags;       // Clone flags (CLONE_*)
    uint64 stack;       // User stack pointer (required if CLONE_VM)
    uint64 stack_size;  // Size of the user stack
    uint64 entry;       // Entry point for child (required if CLONE_VM)
    uint64 esignal;     // Signal to be sent to parent on child exit
    uint64 tls;         // Thread Local Storage descriptor
    uint64 ctid;        // Child TID address (for CLONE_CHILD_SETTID/CLEARTID)
    uint64 ptid;        // Parent TID address (for CLONE_PARENT_SETTID)
};

#endif // __KERNEL_CLONE_FLAGS_H
