/*
 * pipe.c - Pipe implementation
 *
 * Provides pipe read/write/close operations for kernel and user space.
 * Legacy pipealloc removed - VFS uses vfs_pipealloc in kernel/vfs/file.c instead.
 */

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "mutex_types.h"
#include "pipe.h"
#include "vm.h"
#include "sched.h"

void
pipeclose(struct pipe *pi, int writable)
{
  spin_acquire(&pi->lock);
  if(writable){
    pi->writeopen = 0;
    wakeup_on_chan(&pi->nread);
  } else {
    pi->readopen = 0;
    wakeup_on_chan(&pi->nwrite);
  }
  if(pi->readopen == 0 && pi->writeopen == 0){
    spin_release(&pi->lock);
    kfree((char*)pi);
  } else
    spin_release(&pi->lock);
}

int
pipewrite(struct pipe *pi, uint64 addr, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  spin_acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      spin_release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){ //DOC: pipewrite-full
      wakeup_on_chan(&pi->nread);
      sleep_on_chan(&pi->nwrite, &pi->lock);
    } else {
      char ch;
      if(vm_copyin(pr->vm, &ch, addr + i, 1) == -1)
        break;
      pi->data[pi->nwrite++ % PIPESIZE] = ch;
      i++;
    }
  }
  wakeup_on_chan(&pi->nread);
  spin_release(&pi->lock);

  return i;
}

int
piperead(struct pipe *pi, uint64 addr, int n)
{
  int i;
  struct proc *pr = myproc();
  char ch;

  spin_acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){  //DOC: pipe-empty
    if(killed(pr)){
      spin_release(&pi->lock);
      return -1;
    }
    sleep_on_chan(&pi->nread, &pi->lock); //DOC: piperead-sleep
  }
  for(i = 0; i < n; i++){  //DOC: piperead-copy
    if(pi->nread == pi->nwrite)
      break;
    ch = pi->data[pi->nread++ % PIPESIZE];
    if(vm_copyout(pr->vm, addr + i, &ch, 1) == -1)
      break;
  }
  wakeup_on_chan(&pi->nwrite);  //DOC: piperead-wakeup
  spin_release(&pi->lock);
  return i;
}

// Kernel-mode pipe read (for VFS layer)
int
piperead_kernel(struct pipe *pi, char *buf, int n)
{
  int i;
  struct proc *pr = myproc();

  spin_acquire(&pi->lock);
  while(pi->nread == pi->nwrite && pi->writeopen){
    if(killed(pr)){
      spin_release(&pi->lock);
      return -1;
    }
    sleep_on_chan(&pi->nread, &pi->lock);
  }
  for(i = 0; i < n; i++){
    if(pi->nread == pi->nwrite)
      break;
    buf[i] = pi->data[pi->nread++ % PIPESIZE];
  }
  wakeup_on_chan(&pi->nwrite);
  spin_release(&pi->lock);
  return i;
}

// Kernel-mode pipe write (for VFS layer)
int
pipewrite_kernel(struct pipe *pi, const char *buf, int n)
{
  int i = 0;
  struct proc *pr = myproc();

  spin_acquire(&pi->lock);
  while(i < n){
    if(pi->readopen == 0 || killed(pr)){
      spin_release(&pi->lock);
      return -1;
    }
    if(pi->nwrite == pi->nread + PIPESIZE){
      wakeup_on_chan(&pi->nread);
      sleep_on_chan(&pi->nwrite, &pi->lock);
    } else {
      pi->data[pi->nwrite++ % PIPESIZE] = buf[i];
      i++;
    }
  }
  wakeup_on_chan(&pi->nread);
  spin_release(&pi->lock);
  return i;
}
