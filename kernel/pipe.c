#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "param.h"
#include "spinlock.h"
#include "proc.h"
#include "fs.h"
#include "mutex_types.h"
#include "file.h"
#include "vm.h"
#include "sched.h"

#define PIPESIZE 512

struct pipe {
  struct spinlock lock;
  char data[PIPESIZE];
  uint nread;     // number of bytes read
  uint nwrite;    // number of bytes written
  int readopen;   // read fd is still open
  int writeopen;  // write fd is still open
};

int
pipealloc(struct file **f0, struct file **f1)
{
  struct pipe *pi;

  pi = 0;
  *f0 = *f1 = 0;
  if((*f0 = filealloc()) == 0 || (*f1 = filealloc()) == 0)
    goto bad;
  if((pi = (struct pipe*)kalloc()) == 0)
    goto bad;
  pi->readopen = 1;
  pi->writeopen = 1;
  pi->nwrite = 0;
  pi->nread = 0;
  spin_init(&pi->lock, "pipe");
  (*f0)->type = FD_PIPE;
  (*f0)->readable = 1;
  (*f0)->writable = 0;
  (*f0)->pipe = pi;
  (*f1)->type = FD_PIPE;
  (*f1)->readable = 0;
  (*f1)->writable = 1;
  (*f1)->pipe = pi;
  return 0;

 bad:
  if(pi)
    kfree((char*)pi);
  if(*f0)
    fileclose(*f0);
  if(*f1)
    fileclose(*f1);
  return -1;
}

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
