//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"
#include "memlayout.h"
#include "slab.h"
#include "hlist.h"
#include "vm.h"

struct devsw devsw[NDEV];

/* File Descriptor Table */
#define FD_HASH_BUCKETS 31

struct {
  struct spinlock lock;
  slab_cache_t fd_cache;
  struct {
    hlist_t fd_table; // File descriptor hash table
    hlist_bucket_t buckets[FD_HASH_BUCKETS];
  };
  int next_fd_no; // Next file descriptor number
} ftable;

static ht_hash_t __fdtab_hash(void *node)
{
  struct file *f = (struct file *)node;
  return hlist_hash_int(f->fd_no);
}

static int __fdtab_hash_cmp(hlist_t* ht, void *node1, void *node2)
{
  struct file *f1 = (struct file *)node1;
  struct file *f2 = (struct file *)node2;
  return f1->fd_no - f2->fd_no;
}

static hlist_entry_t *__fdtab_hash_get_entry(void *node)
{
  struct file *f = (struct file *)node;
  return &f->fd_entry;
}

static void *__fdtab_hash_get_node(hlist_entry_t *entry)
{
  return (void *)container_of(entry, struct file, fd_entry);
}

static void __fdtab_init(void)
{
  hlist_func_t funcs = {
    .hash = __fdtab_hash,
    .get_node = __fdtab_hash_get_node,
    .get_entry = __fdtab_hash_get_entry,
    .cmp_node = __fdtab_hash_cmp,
  };
  ftable.next_fd_no = 0;
  hlist_init(&ftable.fd_table, FD_HASH_BUCKETS, &funcs);
  slab_cache_init(&ftable.fd_cache, "File Descriptor Cache", sizeof(struct file), SLAB_FLAG_STATIC);
  spin_init(&ftable.lock, "ftable");
}

static inline void __fdtab_lock(void)
{
  spin_acquire(&ftable.lock);
}

static inline void __fdtab_unlock(void)
{
  spin_release(&ftable.lock);
}

static inline struct file *__fdtab_get_fd(int fd_no)
{
  struct file dummy = { .fd_no = fd_no };
  struct file *f = hlist_get(&ftable.fd_table, &dummy);
  return f;
}

static inline struct file *__fdtab_remove_fd(int fd_no)
{
  struct file dummy = { .fd_no = fd_no };
  struct file *f = hlist_pop(&ftable.fd_table, &dummy);
  return f;
}

static inline void __fdtab_remove_file(struct file *file)
{
  struct file *existing = __fdtab_remove_fd(file->fd_no);
  assert(existing != NULL, "File descriptor %d: not found", file->fd_no);
  assert(existing == file, "File descriptor %d: removed a different file", file->fd_no);
}

static inline int __fdtab_add_file(struct file *f)
{
  if (f->fd_no < 0) {
    return -1; // Invalid file descriptor number
  }
  struct file *existing = hlist_get(&ftable.fd_table, f);
  if (existing) {
    return -1; // File descriptor already exists
  }
  existing = hlist_put(&ftable.fd_table, f);
  if (existing == f) {
    return -1; // Failed to insert
  }
  assert(existing == NULL, "File descriptor %d already exists", f->fd_no);
  return 0; // Success
}

static inline int __fdtab_get_next_fd_no(void)
{
  int fd_no = ftable.next_fd_no;
  struct file *existing = __fdtab_get_fd(fd_no);

  while (existing != NULL) {
    fd_no++;
    if (fd_no < 0) {
      fd_no = 0; // Reset to 0 if overflow
    }
    existing = __fdtab_get_fd(fd_no);
  }
  ftable.next_fd_no = fd_no + 1; // Increment for next allocation
  if (ftable.next_fd_no < 0) {
    ftable.next_fd_no = 0; // Reset to 0 if overflow
  }
  return fd_no;
}

static inline struct file *__fdtab_alloc_file(void)
{
  int fd_no = __fdtab_get_next_fd_no();
  if (fd_no < 0) {
    return NULL; // No available file descriptor numbers
  }
  struct file *f = slab_alloc(&ftable.fd_cache);
  if (f == NULL) {
    return NULL; // Allocation failed
  }
  memset(f, 0, sizeof(struct file)); // Initialize the file structure
  f->ref = 1;
  f->fd_no = fd_no;
  f->type = FD_NONE; // Set type to none initially
  hlist_entry_init(&f->fd_entry); // Initialize the file descriptor entry
  if (__fdtab_add_file(f) != 0) {
    slab_free(f);
    return NULL; // Failed to add file to the hash table
  }
  return f;
}

/* file descriptor */
void
fileinit(void)
{
  __fdtab_init();
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  __fdtab_lock();
  f = __fdtab_alloc_file();
  __fdtab_unlock();
  return f;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  __fdtab_lock();
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  __fdtab_unlock();
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  __fdtab_lock();
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    __fdtab_unlock();
    return;
  }
  ff = *f;
  __fdtab_remove_file(f);
  slab_free(f);
  __fdtab_unlock();

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  } else if(ff.type == FD_SOCK){
    sockclose(ff.sock);
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  assert(f != NULL, "filestat: addr is NULL");
  assert((uint64)f >= KERNBASE && (uint64)f < PHYSTOP,
         "filestat: invalid file pointer");

  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(vm_copyout(p->vm, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else if(f->type == FD_SOCK){
    r = sockread(f->sock, addr, n);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else if(f->type == FD_SOCK){
    ret = sockwrite(f->sock, addr, n);
  } else {
    panic("filewrite");
  }

  return ret;
}

uint64 sys_dumpfd(void) {
  struct file *f;
  hlist_bucket_t *bucket;
  hlist_entry_t *pos, *tmp;
  int idx = 0;

  __fdtab_lock();
  printf("File Descriptor Table size(%ld):\n", hlist_len(&ftable.fd_table));
  hlist_foreach_entry(&ftable.fd_table, idx, bucket, pos, tmp) {
    f = __fdtab_hash_get_node(pos);
    const char *type_str;
    switch (f->type) {
      case FD_NONE: type_str = "NONE"; break;
      case FD_PIPE: type_str = "PIPE"; break;
      case FD_INODE: type_str = "INODE"; break;
      case FD_DEVICE: type_str = "DEVICE"; break;
      case FD_SOCK: type_str = "SOCK"; break;
      default: type_str = "UNKNOWN"; break;
    }
    printf("FD %d: ref=%d, type=%s, off=%u, major=%d\n",
           f->fd_no, f->ref, type_str, f->off, f->major);
  }
  __fdtab_unlock();

  return 0;
}
