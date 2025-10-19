// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "bits.h"
#include "stat.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "fs.h"
#include "buf.h"
#include "file.h"
#include "hlist.h"
#include "slab.h"

#define min(a, b) ((a) < (b) ? (a) : (b))
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb; 

// Read the super block.
STATIC void
readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Init fs
void
fsinit(int dev) {
  readsb(dev, &sb);
  if(sb.magic != FSMAGIC)
    panic("invalid file system");
  initlog(dev, &sb);
}

// Zero a block.
STATIC void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
// returns 0 if out of disk space.
STATIC uint
balloc(uint dev)
{
  int b, bi;
  struct buf *bp;

  bp = 0;
  for(b = 0; b < sb.size; b += BPB){
    bp = bread(dev, BBLOCK(b, sb));
    bi = bits_ctz_ptr_inv(bp->data, BPB / 8);
    if (bi >= 0) {
      bp->data[bi/8] |= (1 << (bi % 8));  // Mark block in use.
      log_write(bp);
      brelse(bp);
      bzero(dev, b + bi);
      return b + bi;
    }
    brelse(bp);
  }
  printf("balloc: out of blocks\n");
  return 0;
}

// Free a disk block.
STATIC void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if((bp->data[bi/8] & m) == 0)
    panic("freeing free block");
  bp->data[bi/8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at block
// sb.inodestart. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a table of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The in-memory
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in table: an entry in the inode table
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a table entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   table entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays in the table and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The itable.lock spin-lock protects the allocation of itable
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold itable.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct {
  struct spinlock lock;
  slab_cache_t inode_cache;
  struct {
    hlist_t inode_list;
    hlist_bucket_t inode_buckets[ITABLE_INODE_HASH_BUCKETS];
  };
} itable;

// Free an inode into the inode cache.
static void
__inode_cache_free(struct inode *ip)
{
  if(ip == 0)
    return;

  if (ip->ref > 0)
    panic("__inode_free: inode still referenced");
  slab_free(ip);
  // iunlock(ip);
}

// Allocate an empty inode struct from the inode cache.
static struct inode *
__inode_cache_alloc(void)
{
  struct inode *ip;

  ip = slab_alloc(&itable.inode_cache);
  if(ip == 0)
    panic("__inode_cache_alloc: slab_alloc failed");
  memset(ip, 0, sizeof(*ip));
  return ip;
}

static ht_hash_t __itable_hash_func(void *node)  {
  struct inode *inode = node;

  return hlist_hash_uint64(inode->dev + (inode->inum << 16));
}

static void *__itable_hlist_get_node(hlist_entry_t *entry) {
  return container_of(entry, struct inode, hlist_entry);
}

static hlist_entry_t *__itable_hlist_get_entry(void *node) {
  struct inode *inode = node;
  return &inode->hlist_entry;
}

static int __itable_hlist_cmp(hlist_t *hlist, void *node1, void *node2) {
  struct inode *inode1 = node1;
  struct inode *inode2 = node2;
  int value1 = (int)(inode1->inum + (inode1->dev << 16));
  int value2 = (int)(inode2->inum + (inode2->dev << 16));

  return value1 - value2;
}

static inline struct inode*
__itable_hlist_get(uint dev, uint inum) {
  // Create a dummy node to search for
  struct inode dummy = { 0 };
  dummy.dev = dev;
  dummy.inum = inum;

  return hlist_get(&itable.inode_list, &dummy);
}

static inline struct inode*
__itable_hlist_pop(uint dev, uint inum) {
  // Create a dummy node to search for
  struct inode dummy = { 0 };
  dummy.dev = dev;
  dummy.inum = inum;

  return hlist_pop(&itable.inode_list, &dummy);
}

static inline int
__itable_hlist_push(struct inode *inode) {
  struct inode *entry = hlist_put(&itable.inode_list, inode);
  if (entry == NULL) {
    return 0; // succeeded
  } else if (entry != inode) {
    return -1; // failed to insert
  } else {
    return -1; // the entry is already in the hash list
  }
}

void
iinit()
{
  int ret = slab_cache_init(&itable.inode_cache, "inode", sizeof(struct inode), SLAB_FLAG_STATIC);
  if (ret != 0) {
    panic("iinit: slab_cache_init failed");
  }
  hlist_func_t hlist_func = {
    .hash = __itable_hash_func,
    .get_node = __itable_hlist_get_node,
    .get_entry = __itable_hlist_get_entry,
    .cmp_node = __itable_hlist_cmp,
  };
  ret = hlist_init(&itable.inode_list, ITABLE_INODE_HASH_BUCKETS, &hlist_func);
  if (ret != 0) {
    panic("iinit: hlist_init failed");
  }

  spin_init(&itable.lock, "itable");
}

STATIC struct inode* iget(uint dev, uint inum);

// Allocate an inode on device dev.
// Mark it as allocated by  giving it type type.
// Returns an unlocked but allocated and referenced inode,
// or NULL if there is no free inode.
struct inode*
ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for(inum = 1; inum < sb.ninodes; inum++){
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode*)bp->data + inum%IPB;
    if(dip->type == 0){  // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp);   // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  printf("ialloc: no inodes\n");
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk.
// Caller must hold ip->lock.
void
iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode*)bp->data + ip->inum%IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
STATIC struct inode*
iget(uint dev, uint inum)
{
  struct inode *ip;

  spin_acquire(&itable.lock);

  // Is the inode already in the table?
  ip = __itable_hlist_get(dev, inum);
  if (ip != NULL) {
    // Found in the hash list.
    if (ip->ref <= 0) {
      panic("iget: found unused inode in itable");
    }
    ip->ref++;
    spin_release(&itable.lock);
    return ip;
  } else {
    // Not found in the hash list, search the inode table.
    ip = __inode_cache_alloc();
    if (ip == NULL) {
      spin_release(&itable.lock);
      panic("iget: __inode_cache_alloc failed");
    }
    mutex_init(&ip->lock, "inode");
  }

  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  if (__itable_hlist_push(ip) != 0) {
    panic("iget: failed to push a newly allocated inode to hash list");
  }
  spin_release(&itable.lock);

  return ip;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode*
idup(struct inode *ip)
{
  spin_acquire(&itable.lock);
  ip->ref++;
  spin_release(&itable.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void
ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if(ip == 0 || ip->ref < 1)
    panic("ilock");

  assert(mutex_lock(&ip->lock) == 0, "ilock: failed to lock inode");
  if(ip->valid == 0){
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode*)bp->data + ip->inum%IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;
    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if(ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void
iunlock(struct inode *ip)
{
  if(ip == 0 || !holding_mutex(&ip->lock) || ip->ref < 1) {
    printf("iunlock: invalid inode %p ref %d holding_mutex %s\n", ip, ip ? ip->ref : -1, holding_mutex(&ip->lock) ? "true" : "false");
    panic("iunlock");
  }

  mutex_unlock(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode table entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void
iput(struct inode *ip)
{
  spin_acquire(&itable.lock);
  if (ip->ref == 1) {
    assert(mutex_lock(&ip->lock) == 0, "ilock: failed to lock inode");
    ip->ref = 0;
    struct inode *popped = __itable_hlist_pop(ip->dev, ip->inum);
    spin_release(&itable.lock);
    if (popped != ip) {
      panic("iput: inode not found in hash list");
    }
    if(ip->valid && ip->nlink == 0){
      // inode has no links and no other references: truncate and free.
      
      // ip->ref == 1 means no other process can have ip locked,
      // so this mutex_lock() won't block (or deadlock).
      
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
    }
    __inode_cache_free(ip);
    return;
  }
  
  ip->ref--;
  spin_release(&itable.lock);
}

// Common idiom: unlock, then put.
void
iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// Inode content
//
// The content (data) associated with each inode is stored
// in blocks on the disk. The first NDIRECT block numbers
// are listed in ip->addrs[].  The next NINDIRECT blocks are
// listed in block ip->addrs[NDIRECT].

// get the address of the nth block in the indirect block
// If the block does not exist, it is allocated.
// Args:
//   entry: The block address entry of the indirect block
//   bn: the block number in the indirect block
//   log: if true, write the indirect block to the log
// Return: 
//   0 if out of disk space.
//   Address of the nth block in the indirect block.
STATIC uint
bmap_ind(uint *entry, uint dev, uint bn)
{
  uint addr, *a;
  struct buf *bp;
  if (entry == NULL) {
    return 0; // No entry, no block.
  }
  // printf("bmap_ind: bn=%d, entry=%d", bn, *entry);
  if((addr = *entry) == 0){
    addr = balloc(dev);
    if(addr == 0)
      return 0;
    *entry = addr;
  }
  bp = bread(dev, addr);
  a = (uint*)bp->data;
  if((addr = a[bn]) == 0){
    addr = balloc(dev);
    if(addr){
      a[bn] = addr;
      log_write(bp);
      // int tmp = 0;
      // if (bn < 2) {
      //   tmp = 0;
      // } else if (bn >= NINDIRECT - 2) {
      //   tmp = NINDIRECT - 5;
      // } else {
      //   tmp = bn - 2;
      // }
      // a = &a[tmp];
      // printf(" -- from: %d [%d %d %d %d %d] OK\n", 
      //        tmp, a[0], a[1], a[2], a[3], a[4]);
    } else {
      // printf(" -- FAILED\n");
    }
  } else {
    // printf(" -- OK\n");
  }
  brelse(bp);
  return addr;
}

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
// returns 0 if out of disk space.
STATIC uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if(bn < NDIRECT){
    if((addr = ip->addrs[bn]) == 0){
      addr = balloc(ip->dev);
      if(addr == 0)
        return 0;
      ip->addrs[bn] = addr;
    }
    return addr;
  }
  bn -= NDIRECT;

  if(bn < NINDIRECT){
    // Load indirect block, allocating if necessary.
    return bmap_ind(&ip->addrs[NDIRECT], ip->dev, bn);
  }
  bn -= NINDIRECT;

  if (bn < NDINDIRECT) {
    // Load double indirect block, allocating if necessary.
    addr = bmap_ind(&ip->addrs[NDIRECT + 1], ip->dev, bn / NINDIRECT);
    if (addr == 0) {
      return 0; // Out of disk space.
    }
    bp = bread(ip->dev, addr);
    a = (uint*)bp->data;
    addr = bmap_ind(&a[bn / NINDIRECT], ip->dev, bn % NINDIRECT);
    brelse(bp); // No need to log_write(bp) here, as we already did it within the previous bmap_ind.
    return addr;
  }

  panic("bmap: out of range");
}

// Free the indirect block and all blocks it points to.
STATIC void
__itrunc_ind(uint *entry, uint dev)
{
  int j;
  struct buf *bp;
  uint *a;
  bp = bread(dev, *entry);
  a = (uint*)bp->data;
  for(j = 0; j < NINDIRECT; j++){
    if(a[j])
      bfree(dev, a[j]);
  }
  brelse(bp);
  bfree(dev, *entry);
  *entry = 0;
}

// Truncate inode (discard contents).
// Caller must hold ip->lock.
void
itrunc(struct inode *ip)
{
  int i;
  struct buf *bp;
  uint *a;

  for(i = 0; i < NDIRECT; i++){
    if(ip->addrs[i]){
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  uint nindirect = ip->addrs[NDIRECT];
  ip->addrs[NDIRECT + 1] = 0;
  uint ndindirect = ip->addrs[NDIRECT + 1];
  ip->addrs[NDIRECT + 1] = 0;

  ip->size = 0;
  iupdate(ip);

  if(nindirect){
    __itrunc_ind(&ip->addrs[NDIRECT], ip->dev);
  }

  if (ndindirect) {
    // Free the double indirect block.
    bp = bread(ip->dev, ndindirect);
    a = (uint*)bp->data;
    for(i = 0; i < NINDIRECT; i++) {
      if(a[i]) {
        __itrunc_ind(&a[i], ip->dev);
      }
    }
    brelse(bp);
    bfree(ip->dev, ndindirect);
  }
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void
stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// Read data from inode.
// Caller must hold ip->lock.
// If user_dst==1, then dst is a user virtual address;
// otherwise, dst is a kernel address.
int
readi(struct inode *ip, int user_dst, uint64 dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return 0;
  if(off + n > ip->size)
    n = ip->size - off;

  for(tot=0; tot<n; tot+=m, off+=m, dst+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyout(user_dst, dst, bp->data + (off % BSIZE), m) == -1) {
      brelse(bp);
      tot = -1;
      break;
    }
    brelse(bp);
  }
  return tot;
}

// Write data to inode.
// Caller must hold ip->lock.
// If user_src==1, then src is a user virtual address;
// otherwise, src is a kernel address.
// Returns the number of bytes successfully written.
// If the return value is less than the requested n,
// there was an error of some kind.
int
writei(struct inode *ip, int user_src, uint64 src, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if(off > ip->size || off + n < off)
    return -1;
  if(off + n > MAXFILE*BSIZE)
    return -1;

  for(tot=0; tot<n; tot+=m, off+=m, src+=m){
    uint addr = bmap(ip, off/BSIZE);
    if(addr == 0)
      break;
    bp = bread(ip->dev, addr);
    m = min(n - tot, BSIZE - off%BSIZE);
    if(either_copyin(bp->data + (off % BSIZE), user_src, src, m) == -1) {
      brelse(bp);
      break;
    }
    log_write(bp);
    brelse(bp);
  }

  if(off > ip->size)
    ip->size = off;

  // write the i-node back to disk even if the size didn't change
  // because the loop above might have called bmap() and added a new
  // block to ip->addrs[].
  iupdate(ip);

  return tot;
}

// Directories

int
namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode*
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if(dp->type != T_DIR)
    panic("dirlookup not DIR");

  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if(de.inum == 0)
      continue;
    if(namecmp(name, de.name) == 0){
      // entry matches path element
      if(poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// Write a new directory entry (name, inum) into the directory dp.
// Returns 0 on success, -1 on failure (e.g. out of disk blocks).
int
dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if((ip = dirlookup(dp, name, 0)) != 0){
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for(off = 0; off < dp->size; off += sizeof(de)){
    if(readi(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if(de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if(writei(dp, 0, (uint64)&de, off, sizeof(de)) != sizeof(de))
    return -1;

  return 0;
}

// Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
STATIC char*
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while(*path == '/')
    path++;
  if(*path == 0)
    return 0;
  s = path;
  while(*path != '/' && *path != 0)
    path++;
  len = path - s;
  if(len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else {
    memmove(name, s, len);
    name[len] = 0;
  }
  while(*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
STATIC struct inode*
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if(*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while((path = skipelem(path, name)) != 0){
    ilock(ip);
    if(ip->type != T_DIR){
      iunlockput(ip);
      return 0;
    }
    if(nameiparent && *path == '\0'){
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if((next = dirlookup(ip, name, 0)) == 0){
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if(nameiparent){
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode*
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode*
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}
