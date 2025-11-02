#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "spinlock.h"
#include "mutex_types.h"
#include "fs.h"
#include "buf.h"
#include "sched.h"

// Simple logging that allows concurrent FS system calls.
//
// A log transaction contains the updates of multiple FS system
// calls. The logging system only commits when there are
// no FS system calls active. Thus there is never
// any reasoning required about whether a commit might
// write an uncommitted system call's updates to disk.
//
// A system call should call begin_op()/end_op() to mark
// its start and end. Usually begin_op() just increments
// the count of in-progress FS system calls and returns.
// But if it thinks the log is close to running out, it
// sleeps until the last outstanding end_op() commits.
//
// The log is a physical re-do log containing disk blocks.
// The on-disk log format:
//   header block, containing block #s for block A, B, C, ...
//   block A
//   block B
//   block C
//   ...
// Log appends are synchronous.

// Contents of the header block, used for both the on-disk header block
// and to keep track in memory of logged block# before commit.
struct logheader {
  int n;
  int block[LOGSIZE];
};

struct log {
  struct spinlock lock;
  int start;
  int size;
  int outstanding; // how many FS sys calls are executing.
  int committing;  // in commit(), please wait.
  int dev;
  struct logheader lh;
};
struct log log;

STATIC void recover_from_log(void);
STATIC void commit();

void
initlog(int dev, struct superblock *sb)
{
  if (sizeof(struct logheader) >= BSIZE)
    panic("initlog: too big logheader");

  spin_init(&log.lock, "log");
  log.start = sb->logstart;
  log.size = sb->nlog;
  log.dev = dev;
  recover_from_log();
}

// Copy committed blocks from log to their home location
STATIC void
install_trans(int recovering)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    void *log_data = NULL;
    page_t *log_page = bread(log.dev, log.start + tail + 1, &log_data); // log block
    void *dst_data = NULL;
    page_t *dst_page = bread(log.dev, log.lh.block[tail], &dst_data); // destination block

    memmove(dst_data, log_data, BSIZE);  // copy block to dst
    int ret = bwrite(log.dev, log.lh.block[tail], dst_page);  // write dst to disk
    assert(ret == 0, "install_trans: bwrite failed: %d", ret);

    if(recovering == 0)
      bunpin(dst_page);

    brelse(log_page);
    brelse(dst_page);
  }
}

// Read the log header from disk into the in-memory log header
STATIC void
read_head(void)
{
  void *data = NULL;
  page_t *page = bread(log.dev, log.start, &data);
  struct logheader *lh = (struct logheader *)data;
  int i;
  log.lh.n = lh->n;
  for (i = 0; i < log.lh.n; i++) {
    log.lh.block[i] = lh->block[i];
  }
  brelse(page);
}

// Write in-memory log header to disk.
// This is the true point at which the
// current transaction commits.
STATIC void
write_head(void)
{
  void *data = NULL;
  page_t *page = bread(log.dev, log.start, &data);
  struct logheader *hb = (struct logheader *)data;
  int i;
  hb->n = log.lh.n;
  for (i = 0; i < log.lh.n; i++) {
    hb->block[i] = log.lh.block[i];
  }
  int ret = bwrite(log.dev, log.start, page);
  assert(ret == 0, "write_head: bwrite failed: %d", ret);
  brelse(page);
}

STATIC void
recover_from_log(void)
{
  read_head();
  install_trans(1); // if committed, copy from log to disk
  log.lh.n = 0;
  write_head(); // clear the log
}

// called at the start of each FS system call.
void
begin_op(void)
{
  spin_acquire(&log.lock);
  while(1){
    if(log.committing){
      sleep_on_chan(&log, &log.lock);
    } else if(log.lh.n + (log.outstanding+1)*MAXOPBLOCKS > LOGSIZE){
      // this op might exhaust log space; wait for commit.
      sleep_on_chan(&log, &log.lock);
    } else {
      log.outstanding += 1;
      spin_release(&log.lock);
      break;
    }
  }
}

// called at the end of each FS system call.
// commits if this was the last outstanding operation.
void
end_op(void)
{
  int do_commit = 0;

  spin_acquire(&log.lock);
  log.outstanding -= 1;
  if(log.committing)
    panic("log.committing");
  if(log.outstanding == 0){
    do_commit = 1;
    log.committing = 1;
  } else {
    // begin_op() may be waiting for log space,
    // and decrementing log.outstanding has decreased
    // the amount of reserved space.
    wakeup_on_chan(&log);
  }
  spin_release(&log.lock);

  if(do_commit){
    // call commit w/o holding locks, since not allowed
    // to sleep with locks.
    commit();
    spin_acquire(&log.lock);
    log.committing = 0;
    wakeup_on_chan(&log);
    spin_release(&log.lock);
  }
}

// Copy modified blocks from cache to log.
STATIC void
write_log(void)
{
  int tail;

  for (tail = 0; tail < log.lh.n; tail++) {
    void *to_data = NULL;
    page_t *to_page = bread(log.dev, log.start + tail + 1, &to_data); // log block
    void *from_data = NULL;
    page_t *from_page = bread(log.dev, log.lh.block[tail], &from_data); // cached data block

    memmove(to_data, from_data, BSIZE);
    int ret = bwrite(log.dev, log.start + tail + 1, to_page);  // write the log entry
    assert(ret == 0, "write_log: bwrite failed: %d", ret);

    brelse(from_page);
    brelse(to_page);
  }
}

STATIC void
commit()
{
  if (log.lh.n > 0) {
    write_log();     // Write modified blocks from cache to log
    write_head();    // Write header to disk -- the real commit
    install_trans(0); // Now install writes to home locations
    log.lh.n = 0;
    write_head();    // Erase the transaction from the log
  }
}

// Caller has modified b->data and is done with the buffer.
// Record the block number and pin in the cache by increasing refcnt.
// commit()/write_log() will do the disk write.
//
// log_write() replaces bwrite(); a typical use is:
//   bp = bread(...)
//   modify bp->data[]
//   log_write(bp)
//   brelse(bp)
void
log_write(uint dev, uint blockno, page_t *page)
{
  int i;

  if (dev != log.dev)
    panic("log_write: unexpected dev");

  spin_acquire(&log.lock);
  if (log.lh.n >= LOGSIZE || log.lh.n >= log.size - 1)
    panic("too big a transaction");
  if (log.outstanding < 1)
    panic("log_write outside of trans");

  for (i = 0; i < log.lh.n; i++) {
    if (log.lh.block[i] == blockno)   // log absorption
      break;
  }
  log.lh.block[i] = blockno;
  if (i == log.lh.n) {  // Add new block to log?
    bpin(page);
    log.lh.n++;
  }
  bmark_dirty(page);
  spin_release(&log.lock);
}

