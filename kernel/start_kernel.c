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
#include "trap.h"
#include "rcu.h"

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
    idle_proc_init();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void __start_kernel_secondary_hart(void) {
    while(__atomic_load_n(&started, __ATOMIC_ACQUIRE) == 0)
      ;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mycpu_init(cpuid(), true);
    idle_proc_init();
    printf("hart %d starting, sp: %p\n", cpuid(), __builtin_frame_address(0));
    kvminithart();    // turn on paging
    trapinithart();   // install kernel trap vector
    plicinithart();   // ask PLIC for device interrupts
    rcu_cpu_init(cpuid()); // Initialize RCU for this CPU
}

void start_kernel(int hartid, void *fdt_base, bool is_boot_hart) {
    mycpu_init(hartid, false);
    if(is_boot_hart){
        SET_BOOT_HART();
        __start_kernel_main_hart(hartid, fdt_base);
    } else {
        __start_kernel_secondary_hart();
    }

    // Now we are in idle process context. Just yield to scheduler.
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
    // // Release secondary harts to proceed with their initialization
    // printf("Releasing secondary harts...\n");
    // __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    // // Start secondary harts using SBI HSM extension.
    // // Linux-style: boot hart explicitly starts other harts after initialization.
    // // OpenSBI keeps other harts stopped until we request them via sbi_hart_start().
    // printf("Starting secondary harts...\n");
    // int boot_hart = cpuid();;
    // for (int i = 0; i < NCPU; i++) {
    //     if (i == boot_hart)
    //         continue;
        
    //     long status = sbi_hart_get_status(i);
    //     if (status == SBI_HSM_STATE_STOPPED) {
    //         // Start the hart at _entry with hartid in a0, 0 in a1
    //         long ret = sbi_hart_start(i, (unsigned long)_entry, 0);
    //         if (ret != SBI_SUCCESS && ret != SBI_ERR_ALREADY_AVAILABLE && ret != SBI_ERR_ALREADY_STARTED) {
    //             printf("hart %d: failed to start (error %ld)\n", i, ret);
    //         }
    //     }
    // }
    // // sleep_ms(1000);
    // // rcu_run_tests();
}
