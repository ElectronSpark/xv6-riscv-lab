//! Boot orchestrator — Rust port of `kernel/start_kernel.c` (Phase 2 Wave
//! 27, see `docs/rustify/phase2_plan.md`). This is the widest fan-out file
//! in the whole port: nearly every subsystem's `_init()` entry point is
//! called from here, in an order the plan calls out as "a real invariant"
//! (`docs/rustify/phase2_plan.md` §1/§2). The function bodies below
//! reproduce that order **verbatim**, including the C's own ordering
//! comments (carried over unchanged where they still apply).
//!
//! ## Call graph
//!
//! `start()` (`kernel/start.rs`, Wave 3) calls [`start_kernel`] on every
//! hart, on `stack0`, once paging is still off and the boot-hart CAS has
//! already run. [`start_kernel`] dispatches to [`__start_kernel_main_hart`]
//! (boot hart only) or [`__start_kernel_secondary_hart`] (every other
//! hart), then both paths converge on a shared tail: log the hart's
//! `intr_sp`, start this CPU's RCU kthread, and fall into the idle loop.
//! [`start_kernel_post_init`] is a *separate* entry point, called once by
//! `proc/thread.rs`'s `init_entry` from **thread context** on the boot
//! hart only (it calls `sleep_ms`/blocks, which `start_kernel`'s bare-metal
//! call chain cannot do) — see that file's call site.
//!
//! ## Hart synchronization protocol
//!
//! Boot hart and secondary harts are NOT symmetric:
//!
//! * The boot hart runs the *entire* subsystem-init sequence
//!   ([`__start_kernel_main_hart`]) alone first, while every secondary hart
//!   spins in [`__start_kernel_secondary_hart`] on `smp_cond_load_acquire(&
//!   STARTED, ...)` — a plain acquire-load spin loop (`cpu_relax()` between
//!   polls), matching the C `smp_cond_load_acquire` macro exactly. This
//!   guarantees no secondary hart touches any global kernel structure
//!   before the boot hart has finished initializing it — secondary harts
//!   don't even exist yet from OpenSBI's point of view (see below), but
//!   this same flag also gates re-entry if a secondary hart's `_entry`
//!   somehow raced ahead of the boot hart reaching `start_kernel`.
//! * Secondary harts are not woken by hardware reset/interrupt: OpenSBI's
//!   HSM extension parks every non-boot hart at firmware level until
//!   explicitly started. The boot hart releases them from
//!   [`start_kernel_post_init`] (thread context, **after** the root
//!   filesystem is mounted and `/bin/init`'s VFS root is installed) by
//!   `STARTED.store(1, Release)` followed by `sbi_start_secondary_harts(
//!   &_entry as *const u8 as u64)` — a Linux-style explicit
//!   `sbi_hart_start()` per hart, entering at the same `_entry` symbol
//!   `entry.S`/the boot hart used. `sleep_ms(100)` after the SBI call gives
//!   them time to reach the `smp_cond_load_acquire` spin before boot
//!   continues — the C's own comment, carried over verbatim below.
//! * Once released, each secondary hart runs [`__start_kernel_secondary_hart`]
//!   through its own minimal per-hart bring-up (paging on, `tp` retargeted
//!   to the trampoline VA, trap vector, PLIC, per-CPU RCU) and then joins
//!   the same tail/idle-loop code the boot hart runs.
//!
//! ## Test-gate mechanism (RWLOCK_TEST / SEMAPHORE_TEST / WORKQUEUE_RUNTIME_SMOKE_TEST)
//!
//! The C original gated three optional in-kernel test-suite *launches*
//! (the suites themselves, `kernel/lock/{rwsem,semaphore}_test.rs` and the
//! workqueue smoke test, are always compiled in) behind `#ifdef
//! RWAD_WRITE_TEST` / `#ifdef SEMAPHORE_RUNTIME_TEST` / `#ifdef
//! WORKQUEUE_RUNTIME_SMOKE_TEST` — C preprocessor defines applied via
//! `target_compile_definitions(... PRIVATE RWAD_WRITE_TEST=1 ...)` in
//! `kernel/CMakeLists.txt`, themselves gated on `ENV{RWLOCK_TEST}` /
//! `ENV{SEMAPHORE_TEST}` / `ENV{WORKQUEUE_RUNTIME_SMOKE_TEST}` at `cmake`
//! configure time. That mechanism only worked because `start_kernel.c` was
//! a plain C target source — `target_compile_definitions` has no meaning
//! for a `cargo build` invocation.
//!
//! Replacement: three Cargo features (`rwlock_test`, `semaphore_test`,
//! `workqueue_smoke_test`, declared in `kernel/Cargo.toml`), gating the
//! same three call sites below via `#[cfg(feature = "...")]`.
//! `kernel/CMakeLists.txt` computes the feature list from the exact same
//! three `ENV{...}` checks and passes `--features <list>` to the `cargo
//! build` custom command, so `RWLOCK_TEST=1 SEMAPHORE_TEST=1 cmake ..`
//! keeps working end-to-end unchanged from the outside. A `file(GENERATE)`
//! stamp file recording the resolved feature list is added to that custom
//! command's `DEPENDS` specifically because CMake's `Unix Makefiles`
//! generator does not, by itself, notice that only the *command line*
//! (not any dependency file) changed between two `cmake ..` reconfigures —
//! without the stamp, toggling `RWLOCK_TEST` and reconfiguring would leave
//! a stale `libxv6_rust.a` and silently keep the old gate state. See the
//! comment above `RUST_TEST_FEATURES` in `kernel/CMakeLists.txt`.

use core::ffi::{c_int, c_void};
use core::sync::atomic::{AtomicI32, Ordering};

use crate::machine;

// P3-1B mesh sweep: each of these is a same-crate `pub(crate)` item as of
// this wave, with `start_kernel.rs` as its only caller anywhere in the
// tree (boot-time, called-once entry points) -- referenced via a crate
// path instead of an `extern "C"` redeclaration. `irq`/`timer`/`ipi`'s
// submodules are `pub mod` (see their own `mod.rs`), so the
// fully-qualified submodule path is used, avoiding any glob-re-export
// ambiguity risk entirely. `proc`'s submodules are private (only visible
// within `proc`'s own subtree, which `start_kernel.rs` -- a sibling
// top-level module -- is not part of), so those seven go through
// `proc/mod.rs`'s `pub use submodule::*;` crate-level re-export instead
// (verified individually: none of these seven names is defined more than
// once anywhere under `proc/`, so the glob resolves unambiguously).
use crate::ipi::{cpus_init, ipi_init, mycpu_init};
use crate::irq::irq_core::irq_desc_init;
use crate::irq::plic::{plicinit, plicinithart};
use crate::irq::trap::{trapinit, trapinithart};
use crate::proc::{
    scheduler_init, scheduler_yield,
    workqueue_init, workqueue_runtime_smoke_test, workqueue_test_launch_tests,
};
// NO-STANDALONE-FN: the thread bring-up entry points are now associated fns on
// `Thread` (`proctab_init`/`userinit`/`install_user_root`/`idle_init`).
use crate::proc::thread::Thread;
use crate::timer::sched_timer::sched_timer_init;
// P3-1C mesh sweep: printf.rs/vfs/tty/console.rs are in scope for this
// wave, so these become plain crate-path imports instead of `extern "C"`
// redeclarations (all no-arg/no-return, identical signatures).
use crate::console::{consoledevinit, consoleinit};
use crate::printf::printfinit;
use crate::tty::ptmx::ptmxinit;
use crate::tty::tty::tty_init;
use crate::tty::tty_dev::ttydevinit;
use crate::vfs::fs::vfs_init;
use crate::vfs::pipe::pipe_init;

// ===========================================================================
// Early physical-memory bounds — canonical definitions.
// ===========================================================================
//
// `uint64 __physical_memory_start/__physical_memory_end/
// __physical_total_pages;` in the C original: plain (zero-initialized,
// `.bss`) file-scope globals, `extern`-declared and read by a dozen other
// files across the crate (`printf.rs`, `ipi.rs`, `mm/vm_pgtab.rs`,
// `mm/page.rs`, `dev/fdt.rs`, `proc/sysproc.rs`, ...) but, before this
// wave, defined nowhere in Rust — their one and only definition lived here,
// in `start_kernel.c`. Moving the storage into Rust keeps every other
// file's existing `extern "C" { static ... }` declaration working
// unchanged (same symbol name, same zero-initialized `.bss` placement).

/// Set from the FDT early memory scan in [`__start_kernel_main_hart`], then
/// refined by `fdt_apply_platform_config()`. Read by `mm/kalloc.rs`,
/// `mm/vm_pgtab.rs`'s `kernbase()`, and the backtrace/panic path's stack
/// bounds check, among others.
pub(crate) static mut __physical_memory_start: u64 = 0;

/// See [`__physical_memory_start`]. `mm/vm_pgtab.rs`'s `physstop()`.
pub(crate) static mut __physical_memory_end: u64 = 0;

/// `(mem_size >> 12)` — total managed physical pages, consumed by
/// `mm/page.rs`'s buddy allocator sizing.
pub(crate) static mut __physical_total_pages: u64 = 0;

// ===========================================================================
// Secondary-hart release flag.
// ===========================================================================
//
// `volatile STATIC int started = 0;` in the C original — file-private
// (nothing outside `start_kernel.c` ever referenced `started` by name; the
// `STATIC` macro is a no-op `static` in the kernel build, see
// `kernel/inc/compiler.h`), so this stays a plain, non-`#[no_mangle]` Rust
// static. `AtomicI32` + explicit `Acquire`/`Release` reproduces the C's
// `smp_cond_load_acquire`/`__atomic_store_n(..., __ATOMIC_RELEASE)` pair
// exactly (those macros compile to `__ATOMIC_ACQUIRE`/`__ATOMIC_RELEASE`
// gcc builtins — see `kernel/inc/smp/atomic.h`).
static STARTED: AtomicI32 = AtomicI32::new(0);

/// Mirrors `CPU_FLAG_BOOT_HART` (`kernel/inc/smp/percpu.h`). Duplicated
/// locally rather than imported, matching the existing per-file convention
/// for this same constant (see `kernel/timer/timer_core.rs`).
const CPU_FLAG_BOOT_HART: u64 = 2;

// ===========================================================================
// Externs — every subsystem `_init()` entry point this file's boot
// sequence calls, grouped by origin. All of these are C-ABI functions;
// most are themselves Rust now (ported in earlier waves, `#[no_mangle] pub
// extern "C" fn` elsewhere in this crate) but, matching this crate's
// established convention (see `irq/trap.rs`, `ipi.rs`), are still
// redeclared locally via `extern "C"` here exactly as the C original's
// `#include`-derived header declarations would be, rather than reached
// through a `crate::module::` path. A handful (marked below) are still
// genuine C, deferred to Wave 28.
// ===========================================================================

unsafe extern "C" {
    // printf.rs (variadic, so it cannot be declared `safe`).

    // entry.S — `_entry` label; address taken below for
    // `sbi_start_secondary_harts`, never called directly from Rust.
    static _entry: u8;
}

// P3-D3c: every subsystem `_init()` entry point below is a plain
// crate-path import now that its `#[no_mangle]` export is gone (the
// `extern "C"` redeclarations that used to sit in the block above are
// deleted). `early_allocator_init` is genuinely `unsafe fn`; its call
// site already sits in an `unsafe` block. The `*_launch_tests` suites
// are always linked in (compiled unconditionally); only the calls below
// are feature-gated — launched by this Rust call, not by symbol name
// from any external harness (verified against test/cmake/QemuTests.cmake:
// the ctest suites only set `RWLOCK_TEST=1`-style env gates that become
// cargo features).
use crate::bufcache::binit;
use crate::kobject::kobject_global_init;
use crate::lock::rwsem_test::rwsem_launch_tests;
use crate::lock::semaphore_test::semaphore_launch_tests;
use crate::mm::early_allocator::early_allocator_init;
use crate::mm::pcache_test::pcache_launch_tests;
use crate::mm::{kvminit, kvminithart};
use crate::timer::sched_timer::sleep_ms;

// P3-D3a: `pcache_global_init` (mm/pcache.rs, an ordinary safe Rust fn
// now that its `#[no_mangle]` export is gone) and `kinit` (genuinely
// `unsafe fn` in `crate::mm::kalloc`; its call site already sits in an
// `unsafe` context) are reached as crate-path items instead of the
// `extern "C"` redeclarations that used to sit in the block above.
use crate::mm::{kinit, pcache_global_init};
// P3-D3b: lock/rcu.rs's boot entry points are plain `pub(crate) unsafe
// fn`s now that their `#[no_mangle]` exports are gone; reached by crate
// path (call sites below already sit in `unsafe` blocks).
use crate::lock::rcu::{rcu_cpu_init, rcu_init, rcu_kthread_start_cpu};
// NO-STANDALONE-FN: `idle_thread_init` is now the associated fn
// `Thread::idle_init` (this file has the only calls).
// P3-1D mesh sweep: dev/{fdt,dev,netdev,x1_emac,x1_sdhci,nullrand}.rs,
// sbi.rs, backtrace.rs, pci.rs, virtio_disk.rs, ramdisk.rs, sysnet.rs are
// all in scope for this wave; these become plain crate-path imports
// instead of `extern "C"` redeclarations (identical signatures).
use crate::backtrace::ksymbols_init;
use crate::dev::dev::dev_table_init;
use crate::dev::fdt::{fdt_apply_platform_config, fdt_early_scan_memory, fdt_init};
use crate::dev::netdev::netdev_init;
use crate::dev::nullrand::nullranddevinit;
use crate::dev::x1_emac::x1_emac_init;
use crate::dev::x1_sdhci::x1_sdhci_init;
use crate::pci::pci_init;
use crate::ramdisk::ramdisk_init;
use crate::sbi::{sbi_probe_extensions, sbi_start_secondary_harts};
use crate::sysnet::sockinit;
use crate::virtio_disk::virtio_disk_init;

// ===========================================================================
// Boot-hart subsystem bring-up.
// ===========================================================================

/// Ported from `kernel/start_kernel.c::__start_kernel_main_hart` — runs
/// once, on the boot hart only, before any secondary hart is released.
/// Order is a real invariant (see the module doc); every line below
/// mirrors the C original's call sequence and comments exactly.
fn __start_kernel_main_hart(hartid: c_int, fdt_base: *mut c_void) {
    // SAFETY: boot-time, single-hart (boot hart only, called at most once
    // per boot from `start_kernel`), sequential subsystem bring-up —
    // every callee here is designed to run exactly once, at exactly this
    // point in the boot sequence, matching the C original's call order
    // 1:1. No other hart is running kernel code yet (secondaries are
    // parked on `STARTED`, see the module doc), so there is no concurrent
    // access to any global this sequence initializes.
    unsafe {
        // Early memory detection from FDT (lightweight scan, no allocations)
        let mut mem_base: u64 = 0x8000_0000; // Default for QEMU
        let mut mem_size: u64 = 128 * 1024 * 1024;
        if fdt_early_scan_memory(fdt_base, &mut mem_base, &mut mem_size) == 0 {
            // Successfully found memory from FDT
        }

        // Set up memory boundaries for early allocator
        __physical_memory_start = mem_base;
        __physical_memory_end = mem_base + mem_size;
        __physical_total_pages = mem_size >> 12;

        // Early allocator uses memory after kernel end
        early_allocator_init(&raw const end as *mut c_void, __physical_memory_end as *mut c_void);
        kobject_global_init();
        printfinit();
        crate::kprintln!(
            "\nxv6 kernel booting (hart {})\n",
            hartid,
        );
        fdt_init(fdt_base);
        // fdt_walk(fdt_base);

        // Apply platform configuration from FDT to kernel globals
        fdt_apply_platform_config();

        sbi_probe_extensions();
        ksymbols_init(); // Initialize kernel symbols
        kinit(); // physical page allocator
        kvminit(); // create kernel page table
        crate::kprintln!("page table initialized");
        kvminithart(); // turn on paging
        crate::kprintln!("paging enabled");
        pipe_init(); // initialize pipe subsystem
        mycpu_init(hartid as u64, true); // Change mycpu pointer to use trampoline stack
        crate::kprintln!("mycpu initialized");
        rcu_init(); // RCU subsystem initialization
        dev_table_init(); // Initialize the device table
        Thread::proctab_init(); // process table
        tty_init(); // Initialize TTY subsystem
        scheduler_init(); // initialize the scheduler
        workqueue_init(); // workqueue subsystem initialization
        irq_desc_init(); // IRQ descriptor initialization
        trapinit(); // trap vectors
        trapinithart(); // install kernel trap vector
        plicinit(); // set up interrupt controller
        plicinithart(); // ask PLIC for device interrupts
        ipi_init(); // inter-processor interrupts
        consoleinit();
        netdev_init();
        pci_init();
        crate::proc::Signal::init(); // signal handling initialization
        binit(); // buffer cache
        // Legacy iinit() and fileinit() removed - VFS handles these
        Thread::userinit(); // first user thread
        // idle_thread_init must be called before sched_timer_init (or any
        // workqueue_create) because idle_thread_init calls rq_cpu_activate() to
        // mark this CPU as active. Without an active CPU, rq_select_task_rq()
        // returns NULL and new threads cannot be enqueued to any run queue.
        Thread::idle_init();
        sched_timer_init();
        // Post-init device drivers: spawned as kthreads so they run with
        // the scheduler active and can use sleep_ms() / scheduler_yield().
        x1_emac_init(); // SpacemiT X1 EMAC (probes via FDT)
        x1_sdhci_init(); // SpacemiT X1 SDHCI SD/eMMC (probes via FDT)
        // goldfish_rtc_init();  // Goldfish RTC driver (1-second alarm)
        core::sync::atomic::fence(Ordering::SeqCst);
    }
}

/// Ported from `kernel/start_kernel.c::__start_kernel_secondary_hart` —
/// runs once per secondary hart, after the boot hart releases [`STARTED`].
fn __start_kernel_secondary_hart(hartid: c_int) {
    // SAFETY: boot-time, this-hart-only bring-up sequence; the
    // `smp_cond_load_acquire`-equivalent spin below guarantees the boot
    // hart's `__start_kernel_main_hart` (and `cpus_init()`/`mycpu_init()`
    // called from `start_kernel` before this function) has already run to
    // completion, so `cpus[]` is initialized and this hart's own slot is
    // safe to touch. Matches the C original's call order exactly.
    unsafe {
        // Set tp to physical address first. cpus[] was already zeroed by boot
        // hart's cpus_init(), and intr_sp will be set by trapinit() before we
        // proceed.
        mycpu_init(hartid as u64, false);

        while STARTED.load(Ordering::Acquire) == 0 {
            machine::cpu_relax();
        }

        // First turn on paging (still using physical TP)
        kvminithart(); // turn on paging
        // Now switch TP to trampoline virtual address (paging is now on)
        mycpu_init(hartid as u64, true);
        Thread::idle_init();
        trapinithart(); // install kernel trap vector
        plicinithart(); // ask PLIC for device interrupts
        rcu_cpu_init(machine::cpuid()); // Initialize RCU for this CPU
    }
}

/// Ported from `kernel/start_kernel.c::start_kernel` — called by
/// [`crate::start::start`] on every hart (a direct crate-path call since
/// P3-D3c; no more `#[no_mangle] extern "C"` surface). See the module doc
/// for the full boot-order/hart-synchronization contract.
pub(crate) fn start_kernel(hartid: c_int, fdt_base: *mut c_void, is_boot_hart: bool) {
    // Boot hart initializes all cpu structs first, before any hart sets tp
    if is_boot_hart {
        // SAFETY: boot hart, runs once, before any secondary hart is
        // released (see the module doc) — no concurrent access to `cpus[]`
        // or this hart's own flags is possible yet.
        unsafe {
            cpus_init();
            mycpu_init(hartid as u64, false);
        }
        machine::CpuLocal::current().flags_or(CPU_FLAG_BOOT_HART);
        __start_kernel_main_hart(hartid, fdt_base);
    } else {
        __start_kernel_secondary_hart(hartid);
    }

    // SAFETY: `mycpu()` (this hart's own per-CPU slot, already initialized
    // above on both paths) and `printf` are both safe to call from any
    // hart at this point.
    unsafe {
        crate::kprintln!(
            "hart {} initialized. intr_sp: {}",
            hartid,
            crate::printf::Ptr((*machine::CpuLocal::current().as_ptr()).intr_sp as u64),
        );
    }

    // Start the RCU kthread for this CPU before entering idle loop
    // SAFETY: single call per hart, scheduler is up on both the boot-hart
    // and secondary-hart paths by this point.
    unsafe {
        rcu_kthread_start_cpu(machine::cpuid());
    }

    // Idle loop
    loop {
        // `scheduler_yield` (kernel/proc/sched.rs) is a safe Rust fn,
        // callable from any hart once the scheduler is initialized (true
        // on both paths above).
        scheduler_yield();
        machine::intr_on();
        machine::wfi();
        machine::intr_off();
    }
}

// ===========================================================================
// Thread-context post-init.
// ===========================================================================

/// Ported from `kernel/start_kernel.c::start_kernel_post_init`. Called once
/// by `proc/thread.rs`'s `init_entry`, from the first user thread's kernel
/// context (**not** the bare-metal boot path above) — this is why it can
/// call `sleep_ms`/anything that sleeps, unlike [`start_kernel`] itself.
// P3-D3c: `#[no_mangle] extern "C"` dropped -- the only caller
// (`proc/thread.rs`'s `init_entry`) already imports it by crate path.
pub(crate) fn start_kernel_post_init() {
    // SAFETY: runs exactly once, on the boot hart, in `init_entry`'s
    // thread context, strictly before any secondary hart is released
    // (`STARTED.store` below is the release point) — matches the C
    // original's call order 1:1.
    unsafe {
        consoledevinit(); // Initialize and register the console character device
        nullranddevinit(); // Register /dev/null, /dev/random, /dev/zero
        ttydevinit(); // Register /dev/tty (controlling terminal device)
        ptmxinit(); // Register /dev/ptmx (PTY multiplexer)
        virtio_disk_init(); // emulated hard disk (QEMU)
        ramdisk_init(); // ramdisk from FDT initrd (real hardware)
        sockinit();
        pcache_global_init(); // page cache subsystem initialization

        // File system initialization must be run in the context of a
        // regular thread (e.g., because it calls sleep), and thus cannot
        // be run from main().
        // VFS initialization - mounts xv6fs and sets up root filesystem
        vfs_init();

        // Set up root directory for init process (must be after vfs_init)
        Thread::install_user_root();
    }

    // #ifdef RWAD_WRITE_TEST in the C original -- see the module doc's
    // "Test-gate mechanism" section. rwsem_launch_tests() itself is always
    // linked in; only this call is feature-gated.
    #[cfg(feature = "rwlock_test")]
    // SAFETY: single call, thread context, same point in boot order as the
    // C original's `#ifdef`-gated call site.
    unsafe {
        // launch rwsem tests
        rwsem_launch_tests();
    }

    // #ifdef SEMAPHORE_RUNTIME_TEST in the C original -- same mechanism.
    #[cfg(feature = "semaphore_test")]
    // SAFETY: see the rwlock_test block above.
    unsafe {
        semaphore_launch_tests();
    }

    // Phase 4 (docs/rustify/test_port_plan.md) in-kernel suite:
    // mm/pcache_test.rs. Same Wave-27 mechanism as the two gates above --
    // `pcache_global_init()` has already run (see above), so the real
    // pcache subsystem is up before this launches.
    #[cfg(feature = "pcache_test")]
    // SAFETY: single call, thread context, same point in boot order as the
    // rwlock_test/semaphore_test gates above.
    unsafe {
        pcache_launch_tests();
    }

    // Release secondary harts to proceed with their initialization
    // SAFETY: `printf` is safe to call from thread context.
    unsafe {
        crate::kprintln!("Releasing secondary harts...");
    }
    STARTED.store(1, Ordering::Release);
    // Start secondary harts using SBI HSM extension.
    // Linux-style: boot hart explicitly starts other harts after
    // initialization. OpenSBI keeps other harts stopped until we request them
    // via sbi_hart_start().
    // SAFETY: `_entry` is entry.S's linker-provided label; its address is
    // a plain integer argument to the SBI call, taken the same way
    // `mm/vm_pgtab.rs` takes the addresses of the other linker symbols
    // (`etext`, `_rodata`, ...) it needs as integers. `sleep_ms` blocks
    // this thread only, giving the just-released secondary harts time to
    // reach their own `smp_cond_load_acquire`-equivalent spin (see the
    // module doc) before boot proceeds.
    unsafe {
        sbi_start_secondary_harts(&raw const _entry as u64);
        sleep_ms(100); // Give secondary harts time to start
    }

    // #ifdef WORKQUEUE_RUNTIME_SMOKE_TEST in the C original -- same
    // mechanism as the two test gates above.
    #[cfg(feature = "workqueue_smoke_test")]
    // SAFETY: single call, thread context.
    unsafe {
        workqueue_runtime_smoke_test();
    }

    // Phase 4 (docs/rustify/test_port_plan.md) in-kernel suite:
    // proc/workqueue_test.rs. Same Wave-27 mechanism, an independent gate
    // from `workqueue_smoke_test` above (see that module's doc). Placed
    // after the secondary-hart release (like `workqueue_smoke_test`) so
    // its multi-worker concurrency cases can actually use both harts.
    #[cfg(feature = "workqueue_test")]
    // SAFETY: single call, thread context.
    unsafe {
        workqueue_test_launch_tests();
    }

    // RCU processing is now done per-CPU in idle loops
    // rcu_run_tests();

    // #ifdef RQ_RUNTIME_TEST
    // Run queue priority tests
    // void rq_test_run(void);
    // rq_test_run();
    // #endif
}

// `end` — linker-provided "first address after kernel" label
// (`PROVIDE(end = .);` in `kernel/kernel.ld.in`), needed only by
// `__start_kernel_main_hart`'s `early_allocator_init` call above. Declared
// last (after the function that uses it) purely to keep the large function
// block above uncluttered; matches the `_entry`/`etext`/... declaration
// style already established in `mm/vm_pgtab.rs`.
unsafe extern "C" {
    static end: u8;
}
