/*
 * xv6fs logging layer
 * 
 * Per-superblock logging for crash recovery. This is a port of the xv6
 * logging code to work with the VFS layer, making xv6fs independent
 * from the legacy fs layer.
 *
 * A log transaction contains updates from multiple FS operations.
 * The logging system only commits when there are no FS operations active.
 * This ensures atomicity of filesystem operations.
 *
 * Locking order (must acquire in this order to avoid deadlock):
 * 1. vfs_superblock rwlock (if held by caller)
 * 2. vfs_inode mutex (if held by caller)
 * 3. log->lock spinlock (acquired by begin_op/end_op)
 * 4. buffer mutex (acquired by bread during commit)
 *
 * CRITICAL: xv6fs_begin_op may sleep waiting for log space via sleep_on_chan.
 * Callers holding superblock wlock should be aware this can block file I/O
 * operations that need the same log.
 */

#include "types.h"
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "errno.h"
#include "spinlock.h"
#include "buf.h"
#include "sched.h"
#include "printf.h"
#include "vfs/fs.h"
#include "xv6fs_private.h"

/******************************************************************************
 * Log recovery
 ******************************************************************************/

// Copy committed blocks from log to their home location
static void __xv6fs_install_trans(struct xv6fs_log *log, int recovering) {
    for (int tail = 0; tail < log->lh.n; tail++) {
        struct buf *lbuf = bread(log->dev, log->start + tail + 1);  // read log block
        struct buf *dbuf = bread(log->dev, log->lh.block[tail]);     // read dst
        memmove(dbuf->data, lbuf->data, BSIZE);  // copy block to dst
        bwrite(dbuf);  // write dst to disk
        if (recovering == 0)
            bunpin(dbuf);
        brelse(lbuf);
        brelse(dbuf);
    }
}

// Read the log header from disk into the in-memory log header
static void __xv6fs_read_head(struct xv6fs_log *log) {
    struct buf *buf = bread(log->dev, log->start);
    struct xv6fs_logheader *lh = (struct xv6fs_logheader *)(buf->data);
    log->lh.n = lh->n;
    for (int i = 0; i < log->lh.n; i++) {
        log->lh.block[i] = lh->block[i];
    }
    brelse(buf);
}

// Write in-memory log header to disk.
// This is the true point at which the current transaction commits.
static void __xv6fs_write_head(struct xv6fs_log *log) {
    struct buf *buf = bread(log->dev, log->start);
    struct xv6fs_logheader *hb = (struct xv6fs_logheader *)(buf->data);
    hb->n = log->lh.n;
    for (int i = 0; i < log->lh.n; i++) {
        hb->block[i] = log->lh.block[i];
    }
    bwrite(buf);
    brelse(buf);
}

static void __xv6fs_recover_from_log(struct xv6fs_log *log) {
    __xv6fs_read_head(log);
    __xv6fs_install_trans(log, 1);  // if committed, copy from log to disk
    log->lh.n = 0;
    __xv6fs_write_head(log);        // clear the log
}

/******************************************************************************
 * Commit
 ******************************************************************************/

// Copy modified blocks from cache to log
static void __xv6fs_write_log(struct xv6fs_log *log) {
    for (int tail = 0; tail < log->lh.n; tail++) {
        struct buf *to = bread(log->dev, log->start + tail + 1);    // log block
        struct buf *from = bread(log->dev, log->lh.block[tail]);    // cache block
        memmove(to->data, from->data, BSIZE);
        bwrite(to);  // write the log
        brelse(from);
        brelse(to);
    }
}

static void __xv6fs_commit(struct xv6fs_log *log) {
    if (log->lh.n > 0) {
        __xv6fs_write_log(log);     // Write modified blocks from cache to log
        __xv6fs_write_head(log);    // Write header to disk -- the real commit
        __xv6fs_install_trans(log, 0);  // Now install writes to home locations
        log->lh.n = 0;
        __xv6fs_write_head(log);    // Erase the transaction from the log
    }
}

/******************************************************************************
 * Public API
 ******************************************************************************/

// Initialize the log for a xv6fs superblock
void xv6fs_initlog(struct xv6fs_superblock *xv6_sb) {
    struct xv6fs_log *log = &xv6_sb->log;
    struct superblock *disk_sb = &xv6_sb->disk_sb;
    
    if (sizeof(struct xv6fs_logheader) >= BSIZE)
        panic("xv6fs_initlog: too big logheader");
    
    spin_init(&log->lock, "xv6fs_log");
    log->start = disk_sb->logstart;
    log->size = disk_sb->nlog;
    log->dev = xv6_sb->dev;
    log->outstanding = 0;
    log->committing = 0;
    log->lh.n = 0;
    
    __xv6fs_recover_from_log(log);
}

// Called at the start of each FS operation
// CRITICAL: Must be called BEFORE acquiring any VFS-layer locks (superblock, inode)
// to avoid deadlock, since this function may sleep waiting for log space.
void xv6fs_begin_op(struct xv6fs_superblock *xv6_sb) {
    struct xv6fs_log *log = &xv6_sb->log;
    
    spin_acquire(&log->lock);
    while (1) {
        if (log->committing) {
            sleep_on_chan(log, &log->lock);
        } else if (log->lh.n + (log->outstanding + 1) * MAXOPBLOCKS > XV6FS_LOGSIZE) {
            // this op might exhaust log space; wait for commit.
            sleep_on_chan(log, &log->lock);
        } else {
            log->outstanding += 1;
            spin_release(&log->lock);
            break;
        }
    }
}

// Called at the end of each FS operation.
// Commits if this was the last outstanding operation.
void xv6fs_end_op(struct xv6fs_superblock *xv6_sb) {
    struct xv6fs_log *log = &xv6_sb->log;
    int do_commit = 0;
    
    spin_acquire(&log->lock);
    log->outstanding -= 1;
    if (log->committing)
        panic("xv6fs: log.committing");
    if (log->outstanding == 0) {
        do_commit = 1;
        log->committing = 1;
    } else {
        // begin_op() may be waiting for log space
        wakeup_on_chan(log);
    }
    spin_release(&log->lock);
    
    if (do_commit) {
        __xv6fs_commit(log);
        spin_acquire(&log->lock);
        log->committing = 0;
        wakeup_on_chan(log);
        spin_release(&log->lock);
    }
}

// Record the block number for writing.
// Must be called between begin_op and end_op.
void xv6fs_log_write(struct xv6fs_superblock *xv6_sb, struct buf *b) {
    struct xv6fs_log *log = &xv6_sb->log;
    int i;
    
    spin_acquire(&log->lock);
    if (log->lh.n >= XV6FS_LOGSIZE || log->lh.n >= log->size - 1)
        panic("xv6fs: too big a transaction");
    if (log->outstanding < 1)
        panic("xv6fs: log_write outside of trans");
    
    for (i = 0; i < log->lh.n; i++) {
        if (log->lh.block[i] == b->blockno)  // log absorption
            break;
    }
    log->lh.block[i] = b->blockno;
    if (i == log->lh.n) {  // Add new block to log?
        bpin(b);
        log->lh.n++;
    }
    spin_release(&log->lock);
}
