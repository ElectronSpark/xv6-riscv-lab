/**
 * @file rwf.h
 * @brief Per-IO flags for preadv2/pwritev2
 *
 * These flags are carried in struct iov_iter::flags and propagated from
 * the preadv2/pwritev2 syscall layer through the VFS into the page cache
 * and filesystem drivers.  They allow callers to request non-blocking
 * behaviour, synchronous writes, or append semantics on a per-call basis.
 */

#ifndef KERNEL_VFS_RWF_H
#define KERNEL_VFS_RWF_H

/* ── Per-IO request flags (preadv2/pwritev2) ─────────────────────────── */

#define RWF_HIPRI    0x00000001  /* High priority request, poll if possible */
#define RWF_DSYNC    0x00000002  /* per-IO O_DSYNC                         */
#define RWF_SYNC     0x00000004  /* per-IO O_SYNC                          */
#define RWF_NOWAIT   0x00000008  /* per-IO non-blocking mode               */
#define RWF_APPEND   0x00000010  /* per-IO O_APPEND                        */

/* Bitmask of all recognised flags — used for input validation */
#define RWF_SUPPORTED \
    (RWF_HIPRI | RWF_DSYNC | RWF_SYNC | RWF_NOWAIT | RWF_APPEND)

#endif /* KERNEL_VFS_RWF_H */
