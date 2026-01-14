#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "percpu.h"
#include "printf.h"
#include "proc/sched.h"
#include "signal.h"
#include "proc/workqueue.h"
#include "kobject.h"
#include "dev.h"
#include "pcache.h"
#include "vfs/fs.h"
#include "trap.h"
#include "rcu.h"
#include "sbi.h"
#include "ipi.h"
#include "timer/goldfish_rtc.h"

volatile STATIC int started = 0;
extern void _entry(); // entry.S

static void __start_kernel_main_hart(int hartid, void *fdt_base) {
    kobject_global_init();
    consoleinit();
    printfinit();
    printf("\n");
    printf("xv6 kernel is booting\n");
    printf("\n");
    printf("hart %d, fdt_base %p, sp: %p\n", hartid, fdt_base, __builtin_frame_address(0));
    ksymbols_init(); // Initialize kernel symbols
    kinit();         // physical page allocator
    kvminit();       // create kernel page table
    kvminithart();   // turn on paging
    mycpu_init(hartid, true);  // Change mycpu pointer to use trampoline stack
    rcu_init();      // RCU subsystem initialization
    dev_table_init(); // Initialize the device table
    procinit();      // process table
    scheduler_init(); // initialize the scheduler
    workqueue_init(); // workqueue subsystem initialization
    irq_desc_init(); // IRQ descriptor initialization
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
    // goldfish_rtc_init();  // Goldfish RTC driver (1-second alarm)
    idle_proc_init();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void __start_kernel_secondary_hart(int hartid) {
    // Set tp to physical address first. cpus[] was already zeroed by boot hart's
    // cpus_init(), and intr_sp will be set by trapinit() before we proceed.
    mycpu_init(hartid, false);

    // while(__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0)
    //   ;
    // __atomic_thread_fence(__ATOMIC_SEQ_CST);
    smp_cond_load_acquire(&started, VAL != 0);

    // First turn on paging (still using physical TP)
    kvminithart();    // turn on paging
    // Now switch TP to trampoline virtual address (paging is now on)
    mycpu_init(hartid, true);
    idle_proc_init();
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
    rcu_cpu_init(cpuid()); // Initialize RCU for this CPU
}

void start_kernel(int hartid, void *fdt_base, bool is_boot_hart) {
    // Boot hart initializes all cpu structs first, before any hart sets tp
    if(is_boot_hart){
        cpus_init();
        mycpu_init(hartid, false);
        SET_BOOT_HART();
        __start_kernel_main_hart(hartid, fdt_base);
    } else {
        __start_kernel_secondary_hart(hartid);
    }

    printf("hart %d initialized. intr_sp: %p\n", hartid, (void*)mycpu()->intr_sp);

    // Now we are in idle process context. Just yield to scheduler.
    //
    // RCU GRACE PERIOD TRACKING:
    // RCU quiescent states are tracked via rcu_check_callbacks() called from
    // context_switch_finish(). Per-CPU RCU kthreads handle callback processing,
    // timestamp overflow checks, and grace period advancement.
    //
    for (;;) {
        scheduler_yield();
        intr_on();
        asm volatile("wfi");
        intr_off();
    } 
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

    // Start per-CPU RCU callback kthreads
    // These handle RCU callback invocation separately from the scheduler path
    rcu_kthread_start();

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
    // Release secondary harts to proceed with their initialization
    printf("Releasing secondary harts...\n");
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    // Start secondary harts using SBI HSM extension.
    // Linux-style: boot hart explicitly starts other harts after initialization.
    // OpenSBI keeps other harts stopped until we request them via sbi_hart_start().
    sbi_start_secondary_harts((unsigned long)_entry);
    sleep_ms(100);

    // RCU processing is now done per-CPU in idle loops
    // rcu_run_tests();
    
    // Run device table stress tests
    // dev_table_test();

    // Initialize IPI subsystem and run demo
    // sleep_ms(100);
    // ipi_init();
    // ipi_demo();
}
