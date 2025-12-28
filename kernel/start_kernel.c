#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "sched.h"
#include "signal.h"
#include "workqueue.h"
#include "kobject.h"
#include "dev.h"
#include "pcache.h"
#include "vfs/fs.h"
#include "rcu.h"

volatile STATIC int started = 0;

// just to test the kernel thread creation
// will exit immediately after printing the args
static inline void __idle(uint64 arg1, uint64 arg2) {
    printf("kernel thread started with arg1: %lx, arg2: %lx\n", arg1, arg2);
}

static void __start_kernel_main_hart(void) {
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
    procinit();      // process table
    rcu_init();      // RCU subsystem initialization
    scheduler_init(); // initialize the scheduler
    workqueue_init(); // workqueue subsystem initialization
    trapinit();      // trap vectors
    trapinithart();  // install kernel trap vector
    plicinit();      // set up interrupt controller
    plicinithart();  // ask PLIC for device interrupts
    pci_init();
    signal_init();   // signal handling initialization  
    binit();         // buffer cache
    // Legacy iinit() and fileinit() removed - VFS handles these
    userinit();      // first user process
    sched_timer_init();
    struct proc *idle_proc = NULL;
    int kpid = kernel_proc_create("idle_process", &idle_proc, __idle, 128, 256, KERNEL_STACK_ORDER); // Create an idle kernel thread
    wakeup_proc(idle_proc);
    printf("Idle kernel thread created with pid: %d\n", kpid);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void __start_kernel_secondary_hart(void) {
    while(started == 0)
      ;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    printf("hart %d starting\n", cpuid());
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
    rcu_cpu_init(cpuid()); // Initialize RCU for this CPU
}

void start_kernel(void) {
    if(cpuid() == 0){
        __start_kernel_main_hart();
    } else {
        __start_kernel_secondary_hart();
    }

    scheduler_run();     
}

// Initialization that requires a process context
void start_kernel_post_init(void) {
    consoledevinit(); // Initialize and register the console character device
    virtio_disk_init(); // emulated hard disk
    sockinit();
    pcache_global_init(); // page cache subsystem initialization

    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    // VFS initialization - mounts xv6fs and sets up root filesystem
    vfs_init();
    
    // Set up root directory for init process (must be after vfs_init)
    install_user_root();

#ifdef RWAD_WRITE_TEST
    // forward decl for rwlock tests
    void rwlock_launch_tests(void);
    // launch rwlock tests
    rwlock_launch_tests();
#endif
#ifdef SEMAPHORE_RUNTIME_TEST
    void semaphore_launch_tests(void);
    semaphore_launch_tests();
#endif
    started = 1;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    rcu_run_tests();
}
