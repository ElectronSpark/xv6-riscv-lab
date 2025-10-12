#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "sched.h"
#include "signal.h"
#include "workqueue.h"
#include "kobject.h"
#include "dev.h"
#include "pcache.h"

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
    kobject_global_init();
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    ksymbols_init(); // Initialize kernel symbols
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    dev_table_init(); // Initialize the device table
    consoledevinit(); // Initialize and register the console character device
    procinit();      // process table
    workqueue_init(); // workqueue subsystem initialization
    scheduler_init(); // initialize the scheduler
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
  plicinithart();  // ask PLIC for device interrupts
  virtio_disk_init(); // emulated hard disk
  binit();         // buffer cache
  iinit();         // inode table
  fileinit();      // file table
    pci_init();
    sockinit();
    signal_init();   // signal handling initialization  
    userinit();      // first user process
    struct proc *idle_proc = myproc();
    int kpid = kernel_proc_create("idle_process", &idle_proc, __idle, 128, 256, KERNEL_STACK_ORDER); // Create an idle kernel thread
    wakeup_proc(idle_proc);
    printf("Idle kernel thread created with pid: %d\n", kpid);
    pcache_global_init(); // Initialize the page cache subsystem
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
