//! Rust port of `kernel/irq/trap.c` (Phase 2 Wave 6 — the kernel's trap
//! entry/exit surface).
//!
//! This file owns the C-ABI symbols named directly from `kernelvec.S` /
//! `trampoline.S` (see the module-level `# Safety` notes below for the
//! exact asm contract) plus the signal-frame push/restore pair called
//! from `proc/signal.rs`. `kernel/irq/syscall.c` (the syscall dispatch
//! table) and the two `.S` files themselves stay untouched -- this wave
//! is scoped to `trap.c` alone.
//!
//! # Asm contract (verified against `kernelvec.S` / `trampoline.S`)
//!
//! * `kernelvec.S`'s trap entry saves registers onto either the current
//!   kernel stack or (if already `CPU_IN_ITR()`) the per-hart interrupt
//!   stack, then does `call kernel_irq` (if `scause`'s top bit is set) or
//!   `call kerneltrap` (exception), passing `a0 = sp` (the trapframe
//!   pointer) and `a1 = s0` (the frame-pointer chain at trap time, used
//!   only for backtraces). Both are non-mangled `pub extern "C" fn`s
//!   below with that exact two-argument signature.
//! * `trampoline.S`'s data slots are filled in either at build time
//!   (`.dword usertrap`, `.dword user_kirq_entrance` -- direct symbol
//!   references, resolved by the linker) or at boot time by
//!   [`trapinit`] (`trampoline_uservec`) and by `mm/vm_pgtab.rs`'s
//!   `kvminit` (`trampoline_ksatp` -- out of this file's scope, see its
//!   own module doc). `uservec`'s asm jumps to `usertrap` for exceptions
//!   or `user_kirq_entrance` for interrupts, both called via `jr` (not
//!   `call`) with `a0` = the trapframe base VA; `usertrap` ignores its
//!   would-be argument register (its real C signature took no
//!   parameters) and `user_kirq_entrance(ksp, s0)` reads `a0`/`a1` per
//!   its C signature (the C original never used `ksp` either -- see the
//!   comment on the port below).
//! * `userret` is reached only through the [`TRAMPOLINE_USERRET`]
//!   function pointer computed once in [`trapinit`] from the
//!   `trampoline`/`userret` linker-provided addresses:
//!   `trampoline_userret = TRAMPOLINE + (userret - trampoline)`. This is
//!   the exact relocation math from the C original, preserved verbatim
//!   below. [`trapinit`] must run (on the boot hart, before SMP starts)
//!   before any hart can reach [`usertrapret`] -- the C original trusted
//!   this ordering implicitly (a null function-pointer call would fault);
//!   this port turns that into an explicit `.expect()` panic instead
//!   (same failure mode -- "must not happen" -- with a clearer message).
//! * `sig_trampoline` (`kernel/proc/sig_trampoline.S`) is never called
//!   directly from this file either, in the C original or here: its
//!   address (`SIG_TRAMPOLINE`, a fixed per-process virtual address, see
//!   `mm/vm_pgtab.rs`) is written into `trapframe.sepc` by
//!   [`push_sigframe`] so that the *next* `sret` lands there in user
//!   mode. No extern declaration for it is needed in this file.
//!
//! # Interrupt-context discipline
//!
//! [`kernel_irq`] and [`user_kirq_entrance`] run with the per-hart
//! interrupt-stack / `CPU_FLAG_IN_ITR` bookkeeping from [`enter_irq`] /
//! [`exit_irq`] bracketing them: no sleeping, no allocation, no locks
//! that can block. Every function this file calls out to on that path
//! (`do_irq`, `vm_cpu_offline`, `trapinithart`) upholds the same
//! contract (verified against their own implementations elsewhere in
//! the crate). [`usertrap`]'s page-fault paths run on the thread's own
//! kernel stack (not the interrupt stack) and are explicitly allowed to
//! sleep (`vma_validate` may block on a file-backed fault).
//!
//! # Unsafe scoping
//!
//! Every `unsafe` block below is scoped to the smallest raw-pointer
//! dereference or inline-asm primitive it needs, with a `// SAFETY:`
//! comment citing the invariant that makes it sound (mirrors
//! `irq/irq_core.rs` / `printf.rs`'s style, Wave 4/5 exemplars). Ported
//! functions that were plain (non-`unsafe`) C functions stay
//! `pub extern "C" fn` (not `unsafe extern "C" fn`), matching every
//! other cross-module C-ABI port in this crate (`mm/vm.rs`'s
//! `vma_validate` et al.) -- the C original gave callers no unsafety
//! contract to opt into either, so this preserves the original API
//! surface rather than inventing a new one at this wave's boundary.

#![allow(non_upper_case_globals)]

use core::ffi::{c_char, c_int, c_void, CStr};
use core::mem::size_of;

use crate::bindings::{
    cpu_local, ksiginfo as ksiginfo_t, sigaction as sigaction_t, siginfo_t, sigset_t, stack_t,
    thread, trapframe, MAXVA, NCPU, PAGE_SHIFT, PGSIZE, PROT_EXEC, PROT_READ,
    PROT_WRITE, PTE_R, PTE_W, VMA_FLAG_USER,
};
use crate::machine;
use crate::machine::CpuLocal;
// P3-1B mesh sweep: both are same-crate `pub(crate)` items as of this wave
// (only caller anywhere in the tree is this file), referenced via their
// fully-qualified submodule paths rather than `extern "C"`
// redeclarations.
use crate::irq::irq_core::do_irq;
use crate::irq::syscall::syscall;
// P3-1C mesh sweep: printf.rs is in scope for this wave, so `panic_disable_bt`
// becomes a plain crate-path import instead of an `extern "C"` redeclaration.
use crate::printf::{panic_disable_bt, Cs, Ptr};
// P3-1D mesh sweep: backtrace.rs is in scope for this wave; signature is
// identical, so this becomes a plain crate-path import instead of an
// `extern "C"` redeclaration.
use crate::backtrace::print_backtrace;
// P3-D2a: `scheduler_yield` (proc/sched.rs) is a plain crate-path item
// now that its `extern "C"` redeclaration is gone.
use crate::proc::scheduler_yield;
// P3-D2b: `kill`/`killed`/`handle_signal` (proc/signal.rs) likewise.
use crate::proc::{handle_signal, kill, killed};

// ===========================================================================
// scause / SSTATUS / signal ABI constants (`kernel/inc/trap.h`,
// `kernel/inc/riscv.h`, `kernel/inc/uabi/signal.h`, `kernel/inc/uabi/
// signo.h`). None of these headers are in `wrapper.h` -- `trap.h` is
// deliberately excluded (see `irq_core.rs`'s doc comment: its
// `__scause_to_str` static-inline body would trip bindgen the same way
// `rwlock.h` did), and the signal ABI constants below are the same ones
// `proc/signal.rs` already duplicates locally rather than bindgen'ing.
// ===========================================================================

const RISCV_ENV_CALL_FROM_U_MODE: u64 = 8;
const RISCV_INSTRUCTION_PAGE_FAULT: u64 = 12;
const RISCV_LOAD_PAGE_FAULT: u64 = 13;
const RISCV_STORE_PAGE_FAULT: u64 = 15;

const SSTATUS_SPP: u64 = 1 << 8;
const SSTATUS_SPIE: u64 = 1 << 5;

const SA_SIGINFO: c_int = 0x00000004;
const SA_ONSTACK: c_int = 0x00000008;
const SS_ONSTACK: c_int = 0x2;
const SS_DISABLE: c_int = 0x4;
const SIGSEGV: c_int = 11;
const MINSIGSTKSZ: u64 = PGSIZE as u64; // `(1UL << PGSHIFT)` == PGSIZE.

const CPU_FLAG_NEEDS_RESCHED: u64 = 1;
const CPU_FLAG_IN_ITR: u64 = 4;

// ===========================================================================
// Memory-layout constants (`kernel/inc/mm/memlayout.h`, `kernel/inc/
// param.h`). Duplicated locally rather than bindgen'd, matching the
// precedent already set by `mm/vm_pgtab.rs`'s private `TRAMPOLINE`/
// `SIG_TRAMPOLINE` consts (that module's copies are private to `mod
// vm_pgtab` and unreachable from here).
// ===========================================================================

const TRAMPOLINE: u64 = MAXVA - PGSIZE as u64;
const SIG_TRAMPOLINE: u64 = TRAMPOLINE - (PGSIZE as u64) * 3;
const KIRQSTACKTOP: u64 = MAXVA - ((PGSIZE as u64) << 6);
const INTR_STACK_ORDER: u64 = 2;
const INTR_STACK_SIZE: u64 = 1u64 << (PAGE_SHIFT as u64 + INTR_STACK_ORDER);

/// `KIRQSTACK(hartid)` macro: each hart's interrupt stack, with guard
/// pages above and below (`kernel/inc/mm/memlayout.h`).
#[inline(always)]
const fn kirqstack(hartid: u64) -> u64 {
    KIRQSTACKTOP - (hartid + 1) * (INTR_STACK_SIZE << 1)
}

/// `MAKE_SATP(pagetable)` macro (`kernel/inc/riscv.h`). A private local
/// duplicate of the canonical helper -- `mm/vm_pgtab.rs` has its own
/// (also private) copy for the same reason; folding both into a shared
/// `machine.rs` helper is a follow-up, not in this wave's touch scope.
#[inline(always)]
fn make_satp(pagetable: *mut u64) -> u64 {
    (8u64 << 60) | ((pagetable as u64) >> 12)
}

// ===========================================================================
// Externs -- every cross-module C-ABI symbol this file calls, declared
// locally rather than imported by Rust path. This matches the
// established convention across the whole crate (see `proc/signal.rs`,
// `irq/irq_core.rs`, `printf.rs`'s own local redeclarations of
// `memset`/`memmove`/`print_backtrace`/etc.) rather than reaching through
// `crate::mm::...` / `crate::proc::...` module paths.
// ===========================================================================

unsafe extern "C" {
    // string.rs
    fn memset(s: *mut c_void, c: c_int, n: usize) -> *mut c_void;

    // printf.rs panic/print infra
    safe fn __panic_start();
    safe fn __panic_end() -> !;
    // printf is variadic, so it cannot be declared `safe`.

    // mm/vm_pgtab.rs
    safe fn kvmmap(kpgtbl: *mut u64, va: u64, pa: u64, sz: u64, perm: c_int);

    // proc/exit.rs
    safe fn exit(status: c_int) -> !;

    // kernelvec.S -- address taken (`w_stvec((uint64)kernelvec)`), never
    // called directly from Rust.
    safe fn kernelvec();

    // proc/swtch.S
    fn __switch_noreturn(sp: u64, s0: u64, addr: unsafe extern "C" fn(u64, u64));

    // trampoline.S linker-provided labels + the boot-filled
    // `trampoline_uservec` data slot (see `trapinit`).
    safe static trampoline: u8;
    safe static uservec: u8;
    safe static userret: u8;
    safe static mut trampoline_uservec: u64;

    // Linker-script symbol: base of the kernel page table (`kernel.ld`).
    safe static _data_ktlb: u8;

    // `kernel/ipi/ipi.c`'s `struct cpu_local cpus[NCPU]` (still C --
    // Wave 9). Mirrors `lock/rcu.rs`'s own typed extern for the same
    // array.
    #[link_name = "cpus"]
    static mut CPUS: [cpu_local; NCPU as usize];
}

// P3-D3a: the mm/vm.rs entry points are ordinary (safe) Rust fns now that
// their `#[no_mangle]` exports are gone; reached as crate-path items
// instead of the `extern "C"` redeclarations that used to sit in the
// block above (signatures identical). `page_alloc` stays a genuinely
// `unsafe fn` (`crate::mm::page`); its one call site below already sits
// in an `unsafe` block, so the plain `use` keeps it unchanged too.
use crate::mm::{
    page_alloc, vm_copyin, vm_copyout, vm_cpu_offline, vm_cpu_online, vm_find_area, vm_rlock,
    vm_runlock, vm_try_growstack, vma_validate,
};

/// `trampoline_userret = TRAMPOLINE + (userret - trampoline)`, computed
/// once by [`trapinit`]. `None` until then; every real call site runs
/// strictly after boot-time `trapinit()` (see the module doc's asm
/// contract note).
static mut TRAMPOLINE_USERRET: Option<unsafe extern "C" fn(u64, u64)> = None;

// ===========================================================================
// Small CSR-write primitives not yet in `machine.rs`. Local duplicates,
// same convention as `mm/vm_pgtab.rs`'s private `w_satp` (see iteration
// 12's note in RUST_REWRITE.md: folding these into the canonical
// `machine.rs` is a follow-up, not this wave's touch scope).
// ===========================================================================

/// Write SSTATUS. Mirrors the C `w_sstatus()` inline.
#[inline(always)]
fn write_sstatus(v: u64) {
    // SAFETY: `csrw sstatus` writes a supervisor CSR; cannot violate
    // memory safety by itself (see `machine::write_satp`'s identical
    // reasoning for why this omits `nomem`).
    unsafe {
        core::arch::asm!("csrw sstatus, {0}", in(reg) v, options(nostack, preserves_flags));
    }
}

/// Write STVEC (trap-vector base). Mirrors the C `w_stvec()` inline.
#[inline(always)]
fn write_stvec(v: u64) {
    // SAFETY: `csrw stvec` writes a supervisor CSR; no memory effect,
    // cannot trap.
    unsafe {
        core::arch::asm!("csrw stvec, {0}", in(reg) v, options(nomem, nostack, preserves_flags));
    }
}

/// Write SSCRATCH. Mirrors the C `w_sscratch()` inline.
#[inline(always)]
fn write_sscratch(v: u64) {
    // SAFETY: `csrw sscratch` writes a supervisor scratch CSR; no memory
    // effect, cannot trap.
    unsafe {
        core::arch::asm!("csrw sscratch, {0}", in(reg) v, options(nomem, nostack, preserves_flags));
    }
}

/// Read the `gp` (global pointer) register. Mirrors the C `r_gp()`
/// inline.
#[inline(always)]
fn read_gp() -> u64 {
    let v: u64;
    // SAFETY: `mv {0}, gp` reads a general-purpose register with no
    // memory effects and cannot trap.
    unsafe {
        core::arch::asm!("mv {0}, gp", out(reg) v, options(nomem, nostack, preserves_flags));
    }
    v
}

// ===========================================================================
// Per-CPU flag / interrupt-depth helpers (`kernel/inc/smp/percpu.h`'s
// `CPU_FLAG_*` macros). `machine.rs`'s `CpuLocal` does not yet expose
// `intr_stacks`/`intr_sp`/`intr_depth`/flag-clear -- accessed here via
// `CpuLocal::as_ptr()`'s documented raw-pointer escape hatch, same
// pattern `printf.rs`'s `__panic_start` uses for `CPU_FLAG_CRASHED` via
// `flags_or` and RUST_REWRITE.md's own working notes call out as the
// expected fallback ("where they exist").
// ===========================================================================

#[inline]
fn needs_resched() -> bool {
    CpuLocal::current().flags() & CPU_FLAG_NEEDS_RESCHED != 0
}

#[inline]
fn cpu_in_itr() -> bool {
    CpuLocal::current().flags() & CPU_FLAG_IN_ITR != 0
}

#[inline]
fn cpu_set_in_itr() {
    let cpu = CpuLocal::current();
    // SAFETY: mutates only this hart's own `cpu_local.flags` field.
    unsafe { (*cpu.as_ptr()).flags |= CPU_FLAG_IN_ITR };
}

#[inline]
fn cpu_clear_in_itr() {
    let cpu = CpuLocal::current();
    // SAFETY: see `cpu_set_in_itr`.
    unsafe { (*cpu.as_ptr()).flags &= !CPU_FLAG_IN_ITR };
}

#[inline]
fn intr_depth() -> c_int {
    let cpu = CpuLocal::current();
    // SAFETY: reads only this hart's own `cpu_local.intr_depth` field.
    unsafe { (*cpu.as_ptr()).intr_depth }
}

#[inline]
fn intr_depth_inc() {
    let cpu = CpuLocal::current();
    // SAFETY: see `intr_depth`.
    unsafe { (*cpu.as_ptr()).intr_depth += 1 };
}

#[inline]
fn intr_depth_dec() {
    let cpu = CpuLocal::current();
    // SAFETY: see `intr_depth`.
    unsafe { (*cpu.as_ptr()).intr_depth -= 1 };
}

/// Post-increment `intr_depth`, returning the value *before* the
/// increment. Mirrors the C `mycpu()->intr_depth++` expression used as a
/// condition in [`kerneltrap`].
#[inline]
fn intr_depth_postinc() -> c_int {
    let cpu = CpuLocal::current();
    // SAFETY: see `intr_depth`.
    unsafe {
        let p = cpu.as_ptr();
        let old = (*p).intr_depth;
        (*p).intr_depth = old + 1;
        old
    }
}

// ===========================================================================
// scause decoding (`kernel/inc/trap.h`'s `__scause_to_str` static
// inline, reimplemented).
// ===========================================================================

fn scause_to_str(scause: u64) -> &'static CStr {
    if scause & 0x8000_0000_0000_0000 != 0 {
        // Interrupts are negative, exceptions are positive.
        match scause & 0x7FFF_FFFF_FFFF_FFFF {
            0 => c"User software interrupt",
            1 => c"Supervisor software interrupt",
            4 => c"User timer interrupt",
            5 => c"Supervisor timer interrupt",
            8 => c"User external interrupt",
            9 => c"Supervisor external interrupt",
            _ => c"Unknown interrupt",
        }
    } else {
        match scause {
            0 => c"Instruction address misaligned",
            1 => c"Instruction access fault",
            2 => c"Illegal instruction",
            3 => c"Breakpoint",
            5 => c"Load access fault",
            6 => c"Store/AMO address misaligned",
            7 => c"Store/AMO access fault",
            8 => c"Environment call from U-mode",
            9 => c"Environment call from S-mode",
            12 => c"Instruction page fault",
            13 => c"Load page fault",
            15 => c"Store/AMO page fault",
            _ => c"Unknown exception",
        }
    }
}

// ===========================================================================
// panic / assert helpers -- replicate the `panic(fmt, ...)` /
// `assert(expr, fmt, ...)` macro expansions from `kernel/inc/printf.h`
// (`__panic(PANIC, ...)` / `__panic(ASSERTION_FAILURE, ...)`). Same
// pattern as `irq_core.rs`'s `irq_assert`; kept as two small helpers
// plus `kassert!`/`kpanic!` macros so call sites read like the C.
// ===========================================================================

// The C `__panic(type, fmt, ...)` macro stringizes `type` (`PANIC` or
// `ASSERTION_FAILURE`) at compile time and concatenates it directly into
// the format-string literal (`#type " %s:%d: In function '%s':\n"` --
// three conversions: file, line, function). Mirrored here as two
// separate literals (one per `type`) rather than a runtime `%s`
// substitution, to keep the rendered diagnostic byte-identical to the C
// original.

#[cold]
fn kpanic_msg(line: u32, function: &CStr, msg: &CStr) -> ! {
    __panic_start();
    crate::kprintln!(
        "PANIC {}:{}: In function '{}':",
        Cs(c"kernel/irq/trap.rs".as_ptr()),
        line,
        Cs(function.as_ptr()),
    );
    crate::kprint!("{}", Cs(msg.as_ptr()));
    crate::kprintln!();
    __panic_end()
}

#[cold]
fn kassert_fail(line: u32, function: &CStr, msg: &CStr) -> ! {
    __panic_start();
    crate::kprintln!(
        "ASSERTION_FAILURE {}:{}: In function '{}':",
        Cs(c"kernel/irq/trap.rs".as_ptr()),
        line,
        Cs(function.as_ptr()),
    );
    crate::kprint!("{}", Cs(msg.as_ptr()));
    crate::kprintln!();
    __panic_end()
}

macro_rules! kassert {
    ($cond:expr, $func:expr, $msg:expr) => {
        if !($cond) {
            kassert_fail(line!(), $func, $msg)
        }
    };
}

macro_rules! kpanic {
    ($func:expr, $msg:expr) => {
        kpanic_msg(line!(), $func, $msg)
    };
}

// ===========================================================================
// trapinit / trapinithart
// ===========================================================================

/// Rust port of `trapinit()`. Called once, on the boot hart, before any
/// hart can reach [`usertrapret`] (hard boot-order invariant -- see the
/// module doc).
// P3-1B: only caller is `start_kernel.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn trapinit() {
    // SAFETY: boot-time, single-hart execution (before any other hart is
    // started and before any `usertrapret()` call anywhere in the
    // kernel); every static/extern touched below is either write-once
    // here or, for `CPUS`, exclusively owned by this hart's slice of the
    // loop until other harts start.
    unsafe {
        let trampoline_addr = (&raw const trampoline) as u64;
        let userret_addr = (&raw const userret) as u64;
        let uservec_addr = (&raw const uservec) as u64;

        let userret_offset = userret_addr - trampoline_addr;
        let target = TRAMPOLINE + userret_offset;
        TRAMPOLINE_USERRET = Some(core::mem::transmute::<u64, unsafe extern "C" fn(u64, u64)>(
            target,
        ));
        crate::kprintln!("trapinit: trampoline_userret at {}", Ptr(target));

        // send syscalls, interrupts, and exceptions to uservec in
        // trampoline.S
        trampoline_uservec = TRAMPOLINE + (uservec_addr - trampoline_addr);
        crate::kprintln!(
            "trapinit: trampoline_uservec at {}",
            Ptr(trampoline_uservec),
        );

        // Allocate and map interrupt stacks for each CPU hart.
        let kpgtbl = (&raw const _data_ktlb) as *const u8 as *mut u64;
        for i in 0..(NCPU as u64) {
            let stack_mem = page_alloc(INTR_STACK_ORDER, 0);
            kassert!(
                !stack_mem.is_null(),
                c"trapinit",
                c"trapinit: page_alloc for intr_stacks failed"
            );
            memset(stack_mem, 0, INTR_STACK_SIZE as usize);
            kvmmap(
                kpgtbl,
                kirqstack(i),
                stack_mem as u64,
                INTR_STACK_SIZE,
                (PTE_R | PTE_W) as c_int,
            );
            // `cpu_local.intr_stacks` is C-typed `void **` but only ever
            // holds a plain scalar VA here (matches the C original's
            // `cpus[i].intr_stacks = (void *)KIRQSTACK(i);` -- an
            // implicit-conversion-with-warning in C, never dereferenced
            // as an actual array anywhere in the kernel; preserved
            // as-is for 1:1 fidelity rather than "fixed" at this wave's
            // boundary).
            CPUS[i as usize].intr_stacks = kirqstack(i) as *mut *mut c_void;
            CPUS[i as usize].intr_sp = CPUS[i as usize].intr_stacks as u64 + INTR_STACK_SIZE;
            crate::kprintln!(
                "trapinit: CPU {} intr_stack at {:x} -> {}",
                i as c_int,
                kirqstack(i),
                Ptr(stack_mem as u64),
            );
        }
    }
}

/// Rust port of `trapinithart()`: set up to take exceptions and traps
/// while in the kernel.
// P3-1B: called internally (`user_kirq_entrance`/`usertrap`, this file)
// and from `start_kernel.rs` (crate-path `use`, not an `extern`
// redeclaration) -- demoted.
pub(crate) extern "C" fn trapinithart() {
    let intr_sp = {
        let cpu = CpuLocal::current();
        // SAFETY: reads this hart's own `intr_sp`, set once for every
        // hart by `trapinit()` before any hart reaches this function.
        unsafe { (*cpu.as_ptr()).intr_sp }
    };
    write_sscratch(intr_sp);
    write_stvec(kernelvec as usize as u64);
}

// ===========================================================================
// kerneltrap diagnostics
// ===========================================================================

fn kerneltrap_dump_regs(tf: *mut trapframe) {
    // SAFETY: `tf` is a live trapframe for the duration of this call (see
    // callers).
    unsafe {
        crate::kprintln!("kerneltrap_dump_regs:");
        crate::kprintln!("pc: 0x{:x}", (*tf).sepc);
        crate::kprintln!(
            "ra: 0x{:x}, sp: 0x{:x}, s0: 0x{:x}",
            (*tf).ra,
            (*tf).sp,
            (*tf).s0,
        );
        crate::kprintln!(
            "tp: 0x{:x}, t0: 0x{:x}, t1: 0x{:x}, t2: 0x{:x}",
            machine::read_tp(),
            (*tf).t0,
            (*tf).t1,
            (*tf).t2,
        );
        crate::kprintln!(
            "a0: 0x{:x}, a1: 0x{:x}, a2: 0x{:x}, a3: 0x{:x}",
            (*tf).a0,
            (*tf).a1,
            (*tf).a2,
            (*tf).a3,
        );
        crate::kprintln!(
            "a4: 0x{:x}, a5: 0x{:x}, a6: 0x{:x}, a7: 0x{:x}",
            (*tf).a4,
            (*tf).a5,
            (*tf).a6,
            (*tf).a7,
        );
        crate::kprintln!(
            "t3: 0x{:x}, t4: 0x{:x}, t5: 0x{:x}, t6: 0x{:x}",
            (*tf).t3,
            (*tf).t4,
            (*tf).t5,
            (*tf).t6,
        );
        crate::kprintln!("gp: 0x{:x}", read_gp());
    }
}

/// Rust port of the file-local `__trap_panic()`: an interrupt or trap
/// from an unknown source. Always diverges (every path in the C original
/// ends in `panic()` too -- the trailing `mycpu()->intr_depth--;` after
/// [`kerneltrap`]'s final unconditional call into this function was
/// already unreachable dead code there; typing this `-> !` lets the
/// compiler confirm it instead of silently dropping it, and that trailing
/// decrement is simply not carried over).
#[cold]
fn trap_panic_dump(tf: *mut trapframe, s0: u64) -> ! {
    // SAFETY: `tf` is a live trapframe for a trap that just occurred on
    // this hart, supplied by the kernelvec.S/trampoline.S asm contract;
    // the pre-trapframe stack write 8 bytes below `tf` mirrors the C
    // original's intentional (documented as "to enconvinient gdb back
    // trace") scribble into the stack slot immediately preceding the
    // trapframe, which is live stack memory on both the kernel-stack and
    // interrupt-stack trap-entry paths.
    unsafe {
        crate::kprintln!(
            "scause=0x{:x}({}) sepc=0x{:x} stval=0x{:x}",
            (*tf).scause,
            Cs(scause_to_str((*tf).scause).as_ptr()),
            (*tf).sepc,
            (*tf).stval,
        );
        (*tf).ra = (*tf).sepc;
        *((tf as u64 - 8) as *mut u64) = (*tf).sepc;
    }

    let p = machine::current_thread_ptr();
    if p.is_null() {
        crate::kprintln!("kerneltrap: no current thread");
        kerneltrap_dump_regs(tf);
        panic_disable_bt();
        kpanic!(c"trap_panic_dump", c"kerneltrap");
    }

    // SAFETY: `p` is non-null, checked above; `kstack`/`kstack_order` are
    // plain scalar fields of a live `struct thread` (this can only run
    // while `p` is the thread currently executing).
    unsafe {
        let kstack_size = 1u64 << (PAGE_SHIFT as u64 + (*p).kstack_order as u64);
        print_backtrace((*tf).s0, (*p).kstack, (*p).kstack + kstack_size);
    }
    kerneltrap_dump_regs(tf);
    panic_disable_bt();
    kpanic!(c"trap_panic_dump", c"kerneltrap");
}

// ===========================================================================
// user_kirq_entrance / usertrap
// ===========================================================================

/// Rust port of `user_kirq_entrance(uint64 ksp, uint64 s0)`, reached via
/// `jr t2` from `trampoline.S`'s `uservec` (interrupt path). The C
/// original never used its `ksp` parameter either -- kept here only to
/// preserve the asm-visible two-argument signature.
#[no_mangle]
pub extern "C" fn user_kirq_entrance(_ksp: u64, s0: u64) {
    enter_irq();
    let p = machine::current_thread_ptr();
    // SAFETY: `p` is the thread that just trapped from user mode into
    // this handler (uservec's asm contract); `trapframe` is its live
    // utrapframe.
    unsafe {
        kassert!(
            ((*(*p).trapframe).trapframe.sstatus & SSTATUS_SPP) == 0,
            c"user_kirq_entrance",
            c"usertrap: not from user mode"
        );

        // Mark the current CPU as offline for this process's VM.
        vm_cpu_offline((*p).vm, machine::cpuid());

        // redirect traps to kerneltrap() -- since we are on kernel stack
        trapinithart();

        let tf = &raw mut (*(*p).trapframe).trapframe;
        if do_irq(tf) < 0 {
            trap_panic_dump(tf, s0);
        }
    }
    exit_irq();

    if needs_resched() {
        // If anyone has requested a reschedule, do it now. Switch to
        // kernel stack first (so yield() runs on the right stack).
        // SAFETY: `p.ksp` is this thread's live kernel stack pointer;
        // `__user_kirq_return` matches `sw_noret_cb_t`'s signature
        // (safe-fn-pointer coerces to the extern block's `unsafe fn`
        // parameter type).
        unsafe {
            __switch_noreturn((*p).ksp, s0, __user_kirq_return);
        }
    }
    // Otherwise return to user space. (`__switch_noreturn` never actually
    // returns to this point when taken above -- matches the C original's
    // structure exactly: no early `return` there either, it's simply
    // unreachable in practice.)
    usertrapret();
}

/// Rust port of `__user_kirq_return(uint64 irq_sp, uint64 s0)`: the
/// `sw_noret_cb_t` callback `__switch_noreturn` jumps to after switching
/// onto the thread's kernel stack.
// P3-1B: no caller anywhere outside this file -- referenced only as a
// fn-pointer value passed to `__switch_noreturn` (asm, `proc/swtch.S`)
// from `user_kirq_entrance` above, which coerces fine without
// `#[no_mangle]` (a plain item reference, not a link-name lookup) --
// demoted; kept as `extern "C" fn` (not a plain `fn`) since its type must
// still match `unsafe extern "C" fn(u64, u64)` at that coercion site.
pub(crate) extern "C" fn __user_kirq_return(_irq_sp: u64, _s0: u64) {
    usertrapret();
}

/// Rust port of `usertrap()`: handle an interrupt, exception, or system
/// call from user space. Called from `trampoline.S` (`jr` to the
/// `trampoline_utrap` slot, exception path).
#[no_mangle]
pub extern "C" fn usertrap() {
    let p = machine::current_thread_ptr();

    // SAFETY: `p` is the thread that just trapped from user mode
    // (uservec's asm contract); every field access below is on `p`'s own
    // live `utrapframe`/`vm`.
    unsafe {
        let scause = (*(*p).trapframe).trapframe.scause;

        kassert!(
            ((*(*p).trapframe).trapframe.sstatus & SSTATUS_SPP) == 0,
            c"usertrap",
            c"usertrap: not from user mode"
        );

        // Mark the current CPU as offline for this process's VM.
        vm_cpu_offline((*p).vm, machine::cpuid());

        // redirect traps to kerneltrap() -- since we are on kernel stack
        trapinithart();

        match scause {
            RISCV_ENV_CALL_FROM_U_MODE => {
                // system call
                if killed(p) != 0 {
                    exit(-1);
                }
                // sepc points to the ecall instruction, but we want to
                // return to the next instruction.
                (*(*p).trapframe).trapframe.sepc += 4;

                // an interrupt will change sepc, scause, and sstatus, so
                // enable only now that we're done with those registers.
                machine::intr_on();

                syscall();
            }
            RISCV_INSTRUCTION_PAGE_FAULT => {
                let va = (*(*p).trapframe).trapframe.stval;
                vm_rlock((*p).vm);
                let vma = vm_find_area((*p).vm, va);
                if !vma.is_null()
                    && vma_validate(vma, va, 1, (VMA_FLAG_USER | PROT_EXEC | PROT_READ) as u64)
                        == 0
                {
                    vm_runlock((*p).vm);
                } else {
                    vm_runlock((*p).vm);
                    kassert!((*p).pid != 1, c"usertrap", c"init exiting");
                    kill((*p).pid, SIGSEGV);
                }
            }
            RISCV_LOAD_PAGE_FAULT => {
                // Load page fault - handle demand paging for read access.
                let va = (*(*p).trapframe).trapframe.stval;
                // First try to grow stack if the address is in stack
                // region.
                vm_try_growstack((*p).vm, va);
                // Now find the VMA (may be stack that just grew, or
                // existing VMA needing demand paging). Hold vm_rlock to
                // protect VMA tree traversal.
                vm_rlock((*p).vm);
                let vma = vm_find_area((*p).vm, va);
                if !vma.is_null()
                    && vma_validate(vma, va, 1, (VMA_FLAG_USER | PROT_READ) as u64) == 0
                {
                    vm_runlock((*p).vm);
                } else {
                    vm_runlock((*p).vm);
                    kassert!((*p).pid != 1, c"usertrap", c"init exiting");
                    kill((*p).pid, SIGSEGV);
                }
            }
            RISCV_STORE_PAGE_FAULT => {
                // Store page fault - handle demand paging for write
                // access.
                let va = (*(*p).trapframe).trapframe.stval;
                vm_try_growstack((*p).vm, va);
                vm_rlock((*p).vm);
                let vma = vm_find_area((*p).vm, va);
                if !vma.is_null()
                    && vma_validate(vma, va, 1, (VMA_FLAG_USER | PROT_WRITE) as u64) == 0
                {
                    vm_runlock((*p).vm);
                } else {
                    vm_runlock((*p).vm);
                    kassert!((*p).pid != 1, c"usertrap", c"init exiting");
                    kill((*p).pid, SIGSEGV);
                }
            }
            _ => {
                kassert!((scause >> 63) == 0, c"usertrap", c"unexpected interrupt");
                kassert!((*p).pid != 1, c"usertrap", c"init exiting");
                kill((*p).pid, SIGSEGV);
            }
        }
    }

    usertrapret();
}

// ===========================================================================
// Signal-frame push / restore. `ucontext_t` (`kernel/inc/uabi/signal.h`)
// is not in `wrapper.h`'s bindgen surface (same reasoning as `trap.h`),
// so it is hand-declared here field-for-field, matching `proc/signal.rs`'s
// own (differently-shaped but byte-layout-identical) private mirror --
// only raw pointers cross the `push_sigframe`/`restore_sigframe` FFI
// boundary between the two files, so the two Rust-side type definitions
// never need to match, only the underlying C layout each targets.
// ===========================================================================

#[repr(C)]
#[derive(Copy, Clone)]
struct RiscvMcGpState {
    pc: u64,
    ra: u64,
    sp: u64,
    gp: u64,
    tp: u64,
    t0: u64,
    t1: u64,
    t2: u64,
    s0: u64,
    s1: u64,
    a0: u64,
    a1: u64,
    a2: u64,
    a3: u64,
    a4: u64,
    a5: u64,
    a6: u64,
    a7: u64,
    s2: u64,
    s3: u64,
    s4: u64,
    s5: u64,
    s6: u64,
    s7: u64,
    s8: u64,
    s9: u64,
    s10: u64,
    s11: u64,
    t3: u64,
    t4: u64,
    t5: u64,
    t6: u64,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct RiscvMcFpState {
    f: [u64; 32],
    fcsr: u32,
    _pad: u32,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct McContext {
    sc_regs: RiscvMcGpState,
    sc_fpregs: RiscvMcFpState,
}

// `pub(crate)` (was private-to-module) as of the P3-1B mesh sweep, so
// `proc/signal.rs::restore_sigframe`'s typed wrapper (outside `irq`
// entirely) can name it when reinterpreting its own, independently
// written but layout-identical `UContext` mirror.
#[repr(C)]
#[derive(Copy, Clone)]
pub(crate) struct Ucontext {
    uc_link: *mut Ucontext,
    uc_sigmask: sigset_t,
    uc_stack: stack_t,
    uc_mcontext: McContext,
}

/// Rust port of `push_sigframe()`. Will only modify user-space memory
/// and `p->signal.sig_ucontext`; callers that need further thread-struct
/// changes on success must do them themselves (matches the C original's
/// documented contract).
///
/// # Safety (paid by the FFI caller, matching the C original's implicit
/// contract)
/// `p`, if non-null, must point at a live `struct thread`; `sa`, if
/// non-null, at a live `sigaction_t`; `info`, when `SA_SIGINFO` is set,
/// at a live `ksiginfo_t`.
// P3-1B: only caller is `proc/signal.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn push_sigframe(
    p: *mut thread,
    signo: c_int,
    sa: *mut sigaction_t,
    info: *mut ksiginfo_t,
) -> c_int {
    // SAFETY: see function-level `# Safety`; every dereference below is
    // reached only after the corresponding pointer is checked non-null.
    unsafe {
        if sa.is_null() || (*sa).__bindgen_anon_1.sa_handler.is_none() || p.is_null() {
            return -1; // Invalid arguments
        }

        let tf = (*p).trapframe;

        let mut new_sp: u64;
        if ((*sa).sa_flags & SA_ONSTACK) != 0
            && ((*p).signal.sig_stack.ss_flags & (SS_ONSTACK | SS_DISABLE)) == 0
        {
            // Use the alternate stack if SA_ONSTACK is set.
            if (*p).signal.sig_stack.ss_size < MINSIGSTKSZ as usize {
                return -1; // Stack too small
            }
            new_sp =
                (*p).signal.sig_stack.ss_sp as u64 + (*p).signal.sig_stack.ss_size as u64;
        } else {
            new_sp = (*tf).trapframe.sp;
        }

        new_sp -= 0x10;
        new_sp &= !0xF; // align to 16 bytes
        let mut new_ucontext = new_sp - size_of::<Ucontext>() as u64;
        new_ucontext &= !0xF;
        let mut user_siginfo: u64 = 0;
        if ((*sa).sa_flags & SA_SIGINFO) != 0 {
            kassert!(
                !info.is_null(),
                c"push_sigframe",
                c"push_sigframe: info is NULL when SA_SIGINFO is set"
            );
            user_siginfo = new_ucontext - size_of::<siginfo_t>() as u64;
            user_siginfo &= !0xF;
            new_sp = user_siginfo;
        } else {
            new_sp = new_ucontext;
        }

        if ((*sa).sa_flags & SA_ONSTACK) == 0 {
            if (*p).vm.is_null() {
                exit(-1); // No stack area available
            }
            if vm_try_growstack((*p).vm, new_sp) != 0 {
                exit(-1); // No stack area available
            }
        }

        let mut uc: Ucontext = core::mem::zeroed();
        uc.uc_link = (*p).signal.sig_ucontext as *mut Ucontext;
        // Save current mask to restore after handler.
        uc.uc_sigmask = (*p).signal.sig_mask;
        uc.uc_stack = (*p).signal.sig_stack;

        // Copy user-visible registers from trapframe into mcontext.
        let gregs = &mut uc.uc_mcontext.sc_regs;
        gregs.pc = (*tf).trapframe.sepc;
        gregs.ra = (*tf).trapframe.ra;
        gregs.sp = (*tf).trapframe.sp;
        gregs.gp = (*tf).gp;
        gregs.tp = (*tf).tp;
        gregs.t0 = (*tf).trapframe.t0;
        gregs.t1 = (*tf).trapframe.t1;
        gregs.t2 = (*tf).trapframe.t2;
        gregs.s0 = (*tf).trapframe.s0;
        gregs.s1 = (*tf).s1;
        gregs.a0 = (*tf).trapframe.a0;
        gregs.a1 = (*tf).trapframe.a1;
        gregs.a2 = (*tf).trapframe.a2;
        gregs.a3 = (*tf).trapframe.a3;
        gregs.a4 = (*tf).trapframe.a4;
        gregs.a5 = (*tf).trapframe.a5;
        gregs.a6 = (*tf).trapframe.a6;
        gregs.a7 = (*tf).trapframe.a7;
        gregs.s2 = (*tf).s2;
        gregs.s3 = (*tf).s3;
        gregs.s4 = (*tf).s4;
        gregs.s5 = (*tf).s5;
        gregs.s6 = (*tf).s6;
        gregs.s7 = (*tf).s7;
        gregs.s8 = (*tf).s8;
        gregs.s9 = (*tf).s9;
        gregs.s10 = (*tf).s10;
        gregs.s11 = (*tf).s11;
        gregs.t3 = (*tf).trapframe.t3;
        gregs.t4 = (*tf).trapframe.t4;
        gregs.t5 = (*tf).trapframe.t5;
        gregs.t6 = (*tf).trapframe.t6;
        // TODO: save FP state into uc.uc_mcontext.sc_fpregs when FPU is
        // enabled (matches the C original's TODO).

        // Copy the trap frame to the signal trap frame.
        if vm_copyout(
            (*p).vm,
            new_ucontext,
            &uc as *const Ucontext as *const c_void,
            size_of::<Ucontext>() as u64,
        ) != 0
        {
            return -1; // Copy failed
        }
        if ((*sa).sa_flags & SA_SIGINFO) != 0 {
            if vm_copyout(
                (*p).vm,
                user_siginfo,
                &(*info).info as *const siginfo_t as *const c_void,
                size_of::<siginfo_t>() as u64,
            ) != 0
            {
                return -1; // Copy failed
            }
        }

        (*tf).trapframe.sp = new_sp;
        // Set the epc to the signal trampoline.
        (*tf).trapframe.sepc = SIG_TRAMPOLINE;
        (*tf).trapframe.a0 = signo as u64; // Set the first argument
        (*tf).trapframe.a1 = user_siginfo; // Set the second argument
        (*tf).trapframe.a2 = new_ucontext; // Set the third argument
        // Set the handler address.
        (*tf).trapframe.t0 = (*sa)
            .__bindgen_anon_1
            .sa_handler
            .map_or(0, |f| f as usize as u64);
        (*p).signal.sig_ucontext = new_ucontext;

        0 // Success
    }
}

/// Rust port of `restore_sigframe()`.
///
/// # Safety (paid by the FFI caller, matching the C original's implicit
/// contract)
/// `p` must point at a live `struct thread`; `ret_uc`, if non-null, at
/// writable memory at least `size_of::<Ucontext>()` bytes.
// P3-1B: only caller is `proc/signal.rs` (crate-path `use`, not an
// `extern` redeclaration) -- demoted.
pub(crate) extern "C" fn restore_sigframe(p: *mut thread, ret_uc: *mut Ucontext) -> c_int {
    // SAFETY: see function-level `# Safety`.
    unsafe {
        let sig_ucontext = (*p).signal.sig_ucontext;

        if sig_ucontext == 0 || ret_uc.is_null() {
            return -1; // No signal trap frame to restore
        }

        // Copy the signal trap frame back to the user trap frame.
        if vm_copyin(
            (*p).vm,
            ret_uc as *mut c_void,
            sig_ucontext,
            size_of::<Ucontext>() as u64,
        ) != 0
        {
            return -1; // Copy failed
        }

        (*p).signal.sig_ucontext = (*ret_uc).uc_link as u64;

        // Restore user-visible registers from mcontext into trapframe.
        let gregs = &(*ret_uc).uc_mcontext.sc_regs;
        let tf = (*p).trapframe;
        (*tf).trapframe.sepc = gregs.pc;
        (*tf).trapframe.ra = gregs.ra;
        (*tf).trapframe.sp = gregs.sp;
        (*tf).gp = gregs.gp;
        (*tf).tp = gregs.tp;
        (*tf).trapframe.t0 = gregs.t0;
        (*tf).trapframe.t1 = gregs.t1;
        (*tf).trapframe.t2 = gregs.t2;
        (*tf).trapframe.s0 = gregs.s0;
        (*tf).s1 = gregs.s1;
        (*tf).trapframe.a0 = gregs.a0;
        (*tf).trapframe.a1 = gregs.a1;
        (*tf).trapframe.a2 = gregs.a2;
        (*tf).trapframe.a3 = gregs.a3;
        (*tf).trapframe.a4 = gregs.a4;
        (*tf).trapframe.a5 = gregs.a5;
        (*tf).trapframe.a6 = gregs.a6;
        (*tf).trapframe.a7 = gregs.a7;
        (*tf).s2 = gregs.s2;
        (*tf).s3 = gregs.s3;
        (*tf).s4 = gregs.s4;
        (*tf).s5 = gregs.s5;
        (*tf).s6 = gregs.s6;
        (*tf).s7 = gregs.s7;
        (*tf).s8 = gregs.s8;
        (*tf).s9 = gregs.s9;
        (*tf).s10 = gregs.s10;
        (*tf).s11 = gregs.s11;
        (*tf).trapframe.t3 = gregs.t3;
        (*tf).trapframe.t4 = gregs.t4;
        (*tf).trapframe.t5 = gregs.t5;
        (*tf).trapframe.t6 = gregs.t6;
        // TODO: restore FP state from ret_uc->uc_mcontext.sc_fpregs when
        // FPU is enabled (matches the C original's TODO).

        0 // Success
    }
}

// ===========================================================================
// usertrapret
// ===========================================================================

/// Rust port of `usertrapret()`: return to user space.
// P3-1B: only callers are `proc/clone.rs` and `proc/thread.rs` (both
// crate-path `use crate::irq::trap::usertrapret`, not `extern`
// redeclarations) -- demoted.
pub(crate) extern "C" fn usertrapret() {
    let p = machine::current_thread_ptr();

    if killed(p) != 0 {
        // If the thread is terminated, exit it.
        exit(-1);
    }

    handle_signal();

    if needs_resched() {
        scheduler_yield();
    }

    // we're about to switch the destination of traps from kerneltrap() to
    // usertrap(), so turn off interrupts until we're back in user space,
    // where usertrap() is correct.
    machine::intr_off();
    kassert!(
        CpuLocal::current().spin_depth() == 0,
        c"usertrapret",
        c"usertrapret: spin_depth not zero"
    );

    let intr_sp = {
        let cpu = CpuLocal::current();
        // SAFETY: reads this hart's own `intr_sp`.
        unsafe { (*cpu.as_ptr()).intr_sp }
    };

    // SAFETY: `p` is the current thread; `trapframe` is its live
    // utrapframe, exclusively owned by this hart while it is running.
    unsafe {
        let tf = (*p).trapframe;
        // set up trapframe values that uservec will need when the thread
        // next traps into the kernel.
        (*tf).kernel_sp = (*p).ksp;
        (*tf).irq_sp = intr_sp;
    }

    // set up the registers that trampoline.S's sret will use to get to
    // user space.

    // set S Previous Privilege mode to User.
    let mut x = machine::read_sstatus();
    x &= !SSTATUS_SPP; // clear SPP to 0 for user mode
    x |= SSTATUS_SPIE; // enable interrupts in user mode
    write_sstatus(x);

    // Before returning, mark the current CPU as online for this
    // process's VM and get the trapframe base virtual address for this
    // CPU.
    // SAFETY: `p` is the current thread; `vm` is its live address space.
    let (trapframe_base, satp) = unsafe {
        let base = vm_cpu_online((*p).vm, machine::cpuid());
        let satp = make_satp((*(*p).vm).pagetable);
        (base, satp)
    };

    // jump to userret in trampoline.S at the top of memory, which
    // switches to the user page table, restores user registers, and
    // switches to user mode with sret.
    //
    // SAFETY: `trapinit()` has already run (hard boot-order invariant --
    // see the module doc); `TRAMPOLINE_USERRET` is therefore `Some` and
    // has not changed since boot. Single boot-time writer, read-only
    // across all harts thereafter -- same unsynchronized-global-function-
    // pointer pattern as the C original.
    unsafe {
        let f = TRAMPOLINE_USERRET
            .expect("usertrapret: trapinit() must run before usertrapret()");
        f(trapframe_base, satp);
    }
}

// ===========================================================================
// kerneltrap / kernel_irq / enter_irq / exit_irq
// ===========================================================================

/// Rust port of `kerneltrap()`: interrupts and exceptions from kernel
/// code go here via `kernelvec`, on whatever the current kernel stack
/// is.
#[no_mangle]
pub extern "C" fn kerneltrap(sp: *mut trapframe, s0: u64) {
    if cpu_in_itr() {
        crate::kprint!(
            "kerneltrap: exception preempted interrupt. level={}",
            intr_depth(),
        );
        trap_panic_dump(sp, s0);
    }
    if intr_depth_postinc() != 0 {
        crate::kprint!(
            "kerneltrap: nested interrupts not supported. level={}",
            intr_depth(),
        );
        trap_panic_dump(sp, s0);
    }
    // SAFETY: `sp` is a live trapframe supplied by kernelvec.S's asm
    // trap-entry contract.
    let sstatus = unsafe { (*sp).sstatus };
    if (sstatus & SSTATUS_SPP) == 0 {
        crate::kprint!("kerneltrap: not from supervisor mode");
        trap_panic_dump(sp, s0);
    }
    if machine::intr_get() {
        crate::kprint!("kerneltrap: interrupts enabled");
        trap_panic_dump(sp, s0);
    }

    // By now there's no valid exception from kernel mode.
    // SAFETY: format string matches its argument.
    unsafe { crate::kprintln!("kerneltrap: unexpected scause 0x{:x}", (*sp).scause) };
    trap_panic_dump(sp, s0);
}

/// Rust port of `kernel_irq()`.
#[no_mangle]
pub extern "C" fn kernel_irq(sp: *mut trapframe, s0: u64) {
    enter_irq();
    // SAFETY: `sp` is a live trapframe supplied by kernelvec.S's asm
    // trap-entry contract.
    unsafe {
        kassert!(
            ((*sp).sstatus & SSTATUS_SPP) != 0,
            c"kernel_irq",
            c"kerneltrap: not from supervisor mode"
        );
        if do_irq(sp) < 0 {
            trap_panic_dump(sp, s0);
        }
    }
    exit_irq();
}

/// Rust port of `enter_irq()`.
// P3-1B: no caller anywhere outside this file (`kernel_irq`/
// `user_kirq_entrance`, both below) -- demoted.
pub(crate) extern "C" fn enter_irq() {
    if cpu_in_itr() {
        // Special-cased rather than routed through `kassert!`: the C
        // `assert(cond, fmt, ...)` here carries a runtime `%d` argument
        // (`mycpu()->intr_depth`), unlike every other assert in this
        // file, which pass a fixed literal message.
        __panic_start();
        crate::kprintln!(
            "ASSERTION_FAILURE {}:{}: In function '{}':",
            Cs(c"kernel/irq/trap.rs".as_ptr()),
            line!(),
            Cs(c"enter_irq".as_ptr()),
        );
        crate::kprint!(
            "enter_irq: nested interrupts not supported. level={}",
            intr_depth(),
        );
        crate::kprintln!();
        __panic_end();
    }
    intr_depth_inc();
    if intr_depth() == 1 {
        cpu_set_in_itr();
    }
    kassert!(
        !machine::intr_get(),
        c"enter_irq",
        c"kerneltrap: interrupts enabled"
    );
}

/// Rust port of `exit_irq()`.
// P3-1B: no caller anywhere outside this file (`kernel_irq`/
// `user_kirq_entrance`, both above) -- demoted.
pub(crate) extern "C" fn exit_irq() {
    intr_depth_dec();
    if intr_depth() == 0 {
        cpu_clear_in_itr();
    }
}
