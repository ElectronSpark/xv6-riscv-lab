#include "types.h"
#include "param.h"
#include <mm/memlayout.h>
#include "string.h"
#include "riscv.h"
#include "defs.h"
#include <smp/percpu.h>
#include "printf.h"
#include "signal.h"
#include "proc/sched.h"
#include "proc/workqueue.h"
#include "proc/thread.h"
#include "kobject.h"
#include "dev/dev.h"
#include <mm/pcache.h>
#include <mm/vm.h>
#include "xarray.h"
#include "vfs/fs.h"
#include "vfs/pipe.h"
#include "vfs/unix_socket.h"
#include "netlink.h"
#include "kqueue_types.h"
#include "ipc.h"
#include "tty/tty.h"
#include "tty/session.h"
#include "trap.h"
#include "lock/rcu.h"
#include "sbi.h"
#include <smp/ipi.h>
#include "dev/fdt.h"
#include <mm/early_allocator.h>
#include "timer/goldfish_rtc.h"
#include "dev/uart.h"
#include "dev/plic.h"
#include "dev/pci.h"
#include "dev/virtio.h"
#include "platform.h"
#include "diag.h"

uint64 __physical_memory_start;
uint64 __physical_memory_end;
uint64 __physical_total_pages;

volatile STATIC int started = 0;
extern void _entry(); // entry.S
extern char end[];    // first address after kernel.
                      // defined by kernel.ld.

static void __start_kernel_main_hart(int hartid, void *fdt_base) {
    // Early memory detection (lightweight scan, no allocations)
    uint64 mem_base = platform_default_mem_base();
    uint64 mem_size = 128 * 1024 * 1024;
    if (platform_early_memory(fdt_base, &mem_base, &mem_size) == 0) {
        // memory map parsed
    }

    // Cap the first memory region at 4 GB for the early allocator.
    // If the FDT-reported region crosses the 4 GB boundary, limit it here
    // so the page array stays manageable.  fdt_apply_platform_config() will
    // later split any cross-boundary regions and expose the remainder as
    // highmem.
    #define EARLY_MEM_LIMIT 0x100000000ULL
    if (mem_base + mem_size > EARLY_MEM_LIMIT) {
        mem_size = EARLY_MEM_LIMIT - mem_base;
    }
    #undef EARLY_MEM_LIMIT

    // Set up memory boundaries for early allocator
    __physical_memory_start = mem_base;
    __physical_memory_end = mem_base + mem_size;
    __physical_total_pages = mem_size >> 12;

    // Early allocator uses memory after kernel end
    early_allocator_init((void *)end, (void *)__physical_memory_end);
    kobject_global_init();
    printfinit();
    diaginit();
    printf("\nxv6 kernel booting (hart %d)\n\n", hartid);
    platform_init(fdt_base);
    platform_print_mem_summary();

    // Apply platform configuration to kernel globals
    platform_apply_config();

    platform_probe_extensions();
    ksymbols_init(); // Initialize kernel symbols
    kinit();         // physical page allocator
    arch_vm_init();       // create kernel page table
    kernel_vm_init();     // create kernel VM singleton (shared by all CPUs)
    printf("page table initialized\n");
    platform_post_vm_init();
    pipe_init();               // initialize pipe subsystem
    unix_socket_init();        // initialize AF_UNIX socket subsystem
    netlink_init();            // initialize AF_NETLINK socket subsystem
    kqueue_init();             // initialize kqueue subsystem
    futex_init();              // initialize futex subsystem
    ipc_shm_init();            // initialize System V shared memory
    ipc_sem_init();            // initialize System V semaphores
    ipc_msg_init();            // initialize System V message queues
    tty_init();                // TTY slab cache
    session_cache_init();     // session slab cache
    mycpu_init(hartid, true); // Change mycpu pointer to use trampoline stack
    printf("mycpu initialized\n");
    rcu_init();       // RCU subsystem initialization
    dev_table_init(); // Initialize the device table
    thread_init();    // process table
    scheduler_init(); // initialize the scheduler
    workqueue_init(); // workqueue subsystem initialization
    irq_desc_init();  // IRQ descriptor initialization
    arch_trap_init();       // trap vectors
    arch_trap_init_hart();   // install kernel trap vector
    arch_irq_init();       // set up interrupt controller
    arch_irq_init_hart();   // ask PLIC for device interrupts
    ipi_init();       // inter-processor interrupts
    consoleinit();
    netdev_init();
    signal_init(); // signal handling initialization
    binit();       // buffer cache
    // idle_thread_init must run before userinit because it calls
    // rq_cpu_activate() to mark this CPU active; otherwise scheduler_wakeup()
    // for init may fail to enqueue on any run queue.
    idle_thread_init();
    vm_cpu_online(kernel_vm, cpuid(), current); // CPU starts in kernel mode
    // Legacy iinit() and fileinit() removed - VFS handles these
    userinit(); // first user thread
    // Post-init device drivers & timer: spawned as kthreads so they run
    // with the scheduler active and can use sleep_ms() / scheduler_yield().
    platform_late_device_init();
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

static void __start_kernel_secondary_hart(int hartid) {
    // Set tp to physical address first. cpus[] was already zeroed by boot
    // hart's cpus_init(), and intr_sp will be set by trapinit() before we
    // proceed.
    mycpu_init(hartid, false);

    smp_cond_load_acquire(&started, VAL != 0);

    // Platform-specific per-CPU VM init (RISC-V: turn on paging).
    platform_secondary_cpu_init();
    // Now switch TP to trampoline virtual address (paging is now on)
    mycpu_init(hartid, true);
    idle_thread_init();
    vm_cpu_online(kernel_vm, cpuid(), current); // CPU starts in kernel mode
    arch_trap_init_hart();        // install kernel trap vector
    arch_irq_init_hart();        // ask PLIC for device interrupts
    rcu_cpu_init(cpuid()); // Initialize RCU for this CPU
}

void start_kernel(int hartid, void *fdt_base, bool is_boot_hart) {
    // Boot hart initializes all cpu structs first, before any hart sets tp
    if (is_boot_hart) {
        cpus_init();
        mycpu_init(hartid, false);
        SET_BOOT_HART();
        __start_kernel_main_hart(hartid, fdt_base);
    } else {
        __start_kernel_secondary_hart(hartid);
    }

    // Per-CPU platform services (timer, RCU kthread, etc.)
    platform_start_per_cpu_services(cpuid());

    // Idle loop
    for (;;) {
        scheduler_yield();
        intr_on();
        arch_wait_for_interrupt();
        intr_off();
    }
}

// Initialization that requires a thread context
void start_kernel_post_init(void) {
    consoledevinit();   // Initialize and register the console character device
    nullranddevinit();  // Register /dev/null, /dev/random, /dev/zero
    ttydevinit();       // Register /dev/tty (controlling terminal device)
    ptmxinit();         // Register /dev/ptmx (PTY multiplexer)
    gendisk_init();     // Generic disk layer (partition discovery)
    virtio_disk_init(); // emulated hard disk (QEMU)
    ramdisk_init();     // ramdisk from FDT initrd (real hardware)
    loop_init();        // Loopback block devices (/dev/loop0..7)
    sockinit();
#ifdef USE_LWIP
    lwip_net_init();    // lwIP TCP/IP stack initialization (kthread)
#endif
    xarray_global_init(); // XArray subsystem initialization
    pcache_global_init(); // page cache subsystem initialization

    // File system initialization must be run in the context of a
    // regular thread (e.g., because it calls sleep), and thus cannot
    // be run from main().
    // VFS initialization - mounts xv6fs and sets up root filesystem
    vfs_init();

    // Set up root directory for init process (must be after vfs_init)
    install_user_root();

#ifdef RWAD_WRITE_TEST
    // forward decl for rwsem tests
    void rwsem_launch_tests(void);
    // launch rwsem tests
    rwsem_launch_tests();
#endif
#ifdef SEMAPHORE_RUNTIME_TEST
    void semaphore_launch_tests(void);
    semaphore_launch_tests();
#endif
    // Release secondary CPUs to proceed with their initialization
    printf("Releasing secondary CPUs...\n");
    __atomic_store_n(&started, 1, __ATOMIC_RELEASE);
    platform_start_secondary_cpus((uint64)_entry);
    sleep_ms(100); // Give secondary CPUs time to start
    // RCU processing is now done per-CPU in idle loops
    // rcu_run_tests();

    // Run device table stress tests
    // dev_table_test();

    // #ifdef RQ_RUNTIME_TEST
    // Run queue priority tests
    // void rq_test_run(void);
    // rq_test_run();
    // #endif
}
