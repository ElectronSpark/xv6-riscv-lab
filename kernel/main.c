#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "sched.h"
#include "signal.h"

volatile STATIC int started = 0;

// just to test the kernel thread creation
// will exit immediately after printing the args
static inline void __idle(uint64 arg1, uint64 arg2) {
    printf("kernel thread started with arg1: %lx, arg2: %lx\n", arg1, arg2);
}

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  if(cpuid() == 0){
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    ksymbols_init(); // Initialize kernel symbols
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    procinit();      // process table
    scheduler_init(); // initialize the scheduler
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    binit();         // buffer cache
    iinit();         // inode table
    fileinit();      // file table
    virtio_disk_init(); // emulated hard disk
    pci_init();
    sockinit();
    signal_init();   // signal handling initialization  
    userinit();      // first user process
    struct proc *idle_proc = myproc();
    int kpid = kernel_proc_create(&idle_proc, __idle, 128, 256, KERNEL_STACK_ORDER); // Create an idle kernel thread
    wakeup_proc(idle_proc);
    printf("Idle kernel thread created with pid: %d\n", kpid);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    started = 1;
  } else {
    while(started == 0)
      ;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
  }

  scheduler_run();        
}
