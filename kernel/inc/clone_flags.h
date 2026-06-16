/**
 * @file clone_flags.h
 * @brief Clone flags for thread creation
 *
 * Defines flags used with clone() system call to control process/thread
 * creation. Values match the Linux ABI so that musl libc's pthread_create
 * (which uses standard Linux CLONE_* constants) works without translation.
 *
 * @see https://man7.org/linux/man-pages/man2/clone.2.html
 */

#ifndef __KERNEL_CLONE_FLAGS_H
#define __KERNEL_CLONE_FLAGS_H

/* --------------------------------------------------------------------------
 * Standard Linux CLONE flags — low 32 bits
 * --------------------------------------------------------------------------
 * NOTE: The low 8 bits of the flags word carry the exit signal (e.g. SIGCHLD).
 * -------------------------------------------------------------------------- */
#define CLONE_VM             0x00000100  // Share memory space
#define CLONE_FS             0x00000200  // Share filesystem info (root, cwd, umask)
#define CLONE_FILES          0x00000400  // Share file descriptor table
#define CLONE_SIGHAND        0x00000800  // Share signal handlers
#define CLONE_PIDFD          0x00001000  // Store PID file descriptor
#define CLONE_PTRACE         0x00002000  // Child is traced
#define CLONE_VFORK          0x00004000  // Parent blocks until child exits/execs
#define CLONE_PARENT         0x00008000  // Child shares parent's parent
#define CLONE_THREAD         0x00010000  // Share thread group
#define CLONE_NEWNS          0x00020000  // New mount namespace
#define CLONE_SYSVSEM        0x00040000  // Share SysV semaphore undo values
#define CLONE_SETTLS         0x00080000  // Set TLS descriptor
#define CLONE_PARENT_SETTID  0x00100000  // Set parent TID in parent's memory
#define CLONE_CHILD_CLEARTID 0x00200000  // Clear child TID in child's memory on exit
#define CLONE_DETACHED       0x00400000  // Create detached thread (unused)
#define CLONE_UNTRACED       0x00800000  // Child cannot be traced
#define CLONE_CHILD_SETTID   0x01000000  // Set child TID in child's memory
#define CLONE_NEWCGROUP      0x02000000  // New cgroup namespace (unused)
#define CLONE_NEWUTS         0x04000000  // New UTS namespace (unused)
#define CLONE_NEWIPC         0x08000000  // New IPC namespace (unused)
#define CLONE_NEWUSER        0x10000000  // New user namespace (unused)
#define CLONE_NEWPID         0x20000000  // New PID namespace (unused)
#define CLONE_NEWNET         0x40000000  // New network namespace (unused)
#define CLONE_IO             0x80000000  // Share I/O context (unused)

/* xv6-specific extensions (use bits above 32 to avoid conflicts) */
#define CLONE_CLEAR_SIGHAND  0x100000000ULL  // Clear signal handlers in child
#define CLONE_INTO_CGROUP    0x200000000ULL  // Move to new cgroup (unused)
#define CLONE_PID            0x400000000ULL  // Share PID namespace (unused)
#define CLONE_SYSTEM         0x800000000ULL  // Child is a system thread
#define CLONE_SIGSTOPPED     0x1000000000ULL // Start child stopped (unused)

/**
 * @brief Arguments for clone() system call
 *
 * This structure is shared between user space and kernel space.
 * User space passes a pointer to this structure to clone().
 */
struct clone_args {
    uint64 flags;      // Clone flags (CLONE_*)
    uint64 stack;      // User stack pointer (required if CLONE_VM)
    uint64 stack_size; // Size of the user stack
    uint64 entry;      // Entry point for child (required if CLONE_VM)
    uint64 esignal;    // Signal to be sent to parent on child exit
    uint64 tls;        // Thread Local Storage descriptor
    uint64 ctid;       // Child TID address (for CLONE_CHILD_SETTID/CLEARTID)
    uint64 ptid;       // Parent TID address (for CLONE_PARENT_SETTID)
    uint64 pidfd;      // Parent PID fd address (for clone3 CLONE_PIDFD)
};

#endif // __KERNEL_CLONE_FLAGS_H
