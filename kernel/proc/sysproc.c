#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "timer.h"
#include "sched.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  vma_t *vma = myproc()->vm->heap;
  if (vma == NULL) {
    return -1; // No heap VMA found
  }
  addr = myproc()->vm->heap->start + myproc()->vm->heap_size;
  // printf("sbrk: addr %p n %d -> ", (void*)addr, n);
  if(growproc(n) < 0) {
    // printf("growproc failed\n");
    return -1;
  }
  // printf("new addr %p\n", (void*)(myproc()->vm->heap->start + myproc()->vm->heap_size));
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;
  uint ticks1;

  // uint64 fp = r_sp();

  // print_backtrace(fp);

  argint(0, &n);
  if(n < 0)
    n = 0;
  ticks0 = get_jiffs();
  ticks1 = ticks0;
  while(ticks1 - ticks0 < n){
    if(killed(myproc())){
      return -1;
    }
    sleep_on_chan(&ticks, NULL);
    ticks1 = get_jiffs();
  }
  return 0;
}

uint64
sys_kill(void)
{
  int pid;
  int signum;

  argint(0, &pid);
  argint(1, &signum);
  
  return kill(pid, signum);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  return get_jiffs();
}
