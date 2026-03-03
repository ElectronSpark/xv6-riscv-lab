/**
 * @file trap.c
 * @brief x86_64 IDT setup, PIC 8259A, and trap handler.
 *
 * arch_trap_init()  — builds IDT, loads GDT, sets SYSCALL MSRs.
 * arch_irq_init()   — initializes PIC 8259A, masks all IRQs except timer.
 * x86_trap_handler  — C-level handler called from trapvec.S.
 */

#include "types.h"
#include "printf.h"
#include "x86.h"
#include "defs.h"
#include "string.h"
#include "arch/trap.h"
#include "proc/thread.h"
#include "arch_thread.h"
#include "signal.h"
#include "seg.h"
#include "trapframe.h"
#include "lapic.h"
#include "ioapic.h"
#include <mm/vm.h>
#include <smp/percpu.h>
#include <proc/sched.h>
#include "memlayout.h"

extern pagetable_t kernel_pagetable;

/* ── Provided by trapvec.S — table of 256 stub addresses ── */
extern uint64 vectors[];

/* ── Trampoline stubs (linker compatibility) ── */
extern uint64 trampoline_ksatp; /* defined in trapvec.S */
extern char trampoline[];
extern void sig_trampoline(void); /* defined in sig_trampoline.S */

extern char userret[];
static void (*trampoline_userret)(uint64 tf, uint64 user_cr3) = NULL;

/*
 * usertrapret — return to user mode.
 *
 * Sets up kernel state in the utrapframe, maps the trapframe page,
 * restores the user GS base into KERNEL_GS_BASE (so userret's swapgs
 * loads it into GS_BASE for user code), stores the trapframe VA and
 * kernel stack in RIP-relative variables, and returns via iretq.
 */
void usertrapret(void) {
    struct thread *p = current;
    struct trapframe *tf = &p->trapframe->trapframe;

    if (killed(p))
        exit(-1);

    handle_signal();

    /* handle_signal() may have marked us killed (e.g. unhandled SIGSEGV). */
    if (killed(p))
        exit(-1);

    /* Check if GDB wants to interrupt this process (Ctrl-C). */
    {
        extern int gdbstub_check_interrupt(struct thread *);
        gdbstub_check_interrupt(p);
    }

    if (NEEDS_RESCHED()) {
        scheduler_yield();
    }

    /* Disable interrupts while setting up the return path. */
    intr_off();

    /* Restore user FS base (TLS) for user-space threads. */
    wrmsr(MSR_FS_BASE, p->trapframe->tp);

    /*
     * Set up GS MSRs explicitly — Linux strategy:
     *   MSR_GS_BASE      = user's GS base  (what user code sees after iretq)
     *   MSR_KERNEL_GS_BASE = per-CPU addr  (what kernel sees after swapgs on
     *                                       the next SYSCALL/trap entry)
     *
     * Setting both explicitly avoids relying on the swapgs chain being in
     * exactly the right state on every possible entry path (e.g. the very
     * first entry to user mode before any swapgs has ever run).
     * userret no longer needs to execute swapgs; GS is already correct.
     *
     * IMPORTANT: Capture per-CPU pointer and CPU id BEFORE writing user GS
     * to MSR_GS_BASE, because mycpu()/cpuid() read from MSR_GS_BASE.
     */
    struct cpu_local *my_cpu = mycpu();
    int cpu = cpuid();

    wrmsr(MSR_GS_BASE, p->trapframe->user_gs_base);
    wrmsr(MSR_KERNEL_GS_BASE, (uint64)my_cpu);

    /* Store kernel state in utrapframe for next trap entry. */
    p->trapframe->kernel_sp = p->ksp;
    p->trapframe->kernel_satp = (uint64)kernel_pagetable; /* RISC-V compat */

    /* Set TSS RSP0 so the CPU switches to the kernel stack on ring 3→0
     * for IDT-based traps (exceptions, interrupts).
     * GS MSRs are explicitly set above (Linux strategy): usertrapret writes
     * both MSR_GS_BASE and MSR_KERNEL_GS_BASE directly so the next
     * alltraps/syscall_entry swapgs reliably yields the per-CPU GS. */
    x86_tss_set_rsp0(p->ksp);

    /*
     * Tell alltraps where to pivot RSP for user->kernel transitions.
     * All IDT vectors use IST=1 so the CPU initially pushes onto the
     * per-CPU IST stack (mapped in all page tables).  After CR3 swap,
     * alltraps copies the 7-qword interrupt frame from IST to this
     * per-thread kernel stack and switches RSP, so that all subsequent
     * C code -- including scheduler context switches -- runs on a
     * per-thread stack instead of the shared IST stack.
     *
     * SMP-safe: stored in per-CPU struct via saved my_cpu pointer
     * (GS_BASE already points to user value at this point).
     */
    my_cpu->intr_kstack_top = p->ksp;

    /* Guard: if vm was freed (process being torn down), don't proceed. */
    if (p->vm == NULL || p->vm->pagetable == NULL) {
        printf("usertrapret: pid %d '%s' has no VM, exiting\n", p->pid,
               p->name);
        intr_on();
        exit(-1);
    }

    /* Validate trapframe_pte before vm_cpu_online */
    if (p->vm->trapframe_pte) {
        uint64 pte_val = (uint64)p->vm->trapframe_pte;
        if (pte_val > 0x40000000UL || pte_val < 0x100000UL) {
            printf("usertrapret: BAD trapframe_pte=%p pid=%d name=%s\n",
                   p->vm->trapframe_pte, p->pid, p->name);
            intr_on();
            exit(-1);
        }
    }
    /* Mark the current CPU offline for the kernel VM (reverse of user VM) */
    vm_cpu_offline(kernel_vm, cpu);

    uint64 trapframe_base = vm_cpu_online(p->vm, cpu, p);

    /*
     * Store the kernel stack top for the next SYSCALL entry.
     * syscall_entry reads this from per-CPU via %gs:72 after swapgs
     * (Linux strategy: switch CR3 first, then load kernel stack).
     */
    my_cpu->syscall_kstack_top = p->ksp;

    /* Ensure user RFLAGS is valid and has IF set.
     * Bit 1 in RFLAGS is architecturally fixed to 1.
     */
    tf->rflags |= (0x200 | 0x2);

    if (trampoline_userret == NULL)
        panic("usertrapret: trampoline_userret not initialized");

    /* Lazy FPU: if this thread owns the FPU on this CPU, clear TS so
     * FP/SSE works in user mode.  Otherwise set TS so the first FP
     * instruction triggers #NM for lazy switching. */
    {
        uint64 cr0;
        asm volatile("movq %%cr0, %0" : "=r"(cr0));
        if (THREAD_FPU_USED(p) &&
            my_cpu->fpu_owner_tid == p->pid &&
            p->fpu_seq == my_cpu->fpu_seq)
            cr0 &= ~(1ULL << 3); /* clear CR0.TS */
        else
            cr0 |= (1ULL << 3); /* set CR0.TS   */
        asm volatile("movq %0, %%cr0" : : "r"(cr0));
    }

    /* Final sanity check — should not happen after the earlier guard,
     * but if it does, kill the process instead of panicking. */
    if (p->vm == NULL || p->vm->pagetable == NULL) {
        printf("usertrapret: pid %d '%s' lost VM before iretq\n", p->pid,
               p->name);
        intr_on();
        exit(-1);
    }

    trampoline_userret(trapframe_base, (uint64)p->vm->pagetable);

    __builtin_unreachable();
}

int push_sigframe(struct thread *p, int signo, sigaction_t *sa,
                  ksiginfo_t *info) {
    if (sa == NULL || sa->sa_handler == NULL || p == NULL)
        return -1;

    /*
     * Determine the user stack to push the signal frame onto.
     * SA_ONSTACK: use the alternate signal stack if available.
     */
    uint64 new_sp;
    if ((sa->sa_flags & SA_ONSTACK) != 0 &&
        (p->signal.sig_stack.ss_flags & (SS_ONSTACK | SS_DISABLE)) == 0) {
        if (p->signal.sig_stack.ss_size < MINSIGSTKSZ)
            return -1;
        new_sp =
            (uint64)p->signal.sig_stack.ss_sp + p->signal.sig_stack.ss_size;
    } else {
        new_sp = p->trapframe->trapframe.rsp;
    }

    /* 128-byte red zone — the System V AMD64 ABI reserves 128 bytes
     * below RSP that must not be clobbered by signal delivery. */
    new_sp -= 128;
    new_sp &= ~0xFUL;

    /* Reserve space for ucontext_t on the user stack. */
    uint64 uc_addr = (new_sp - sizeof(ucontext_t)) & ~0xFUL;

    /* Reserve space for siginfo_t if SA_SIGINFO. */
    uint64 si_addr = 0;
    if (sa->sa_flags & SA_SIGINFO) {
        assert(info != NULL,
               "push_sigframe: info is NULL when SA_SIGINFO is set");
        si_addr = (uc_addr - sizeof(siginfo_t)) & ~0xFUL;
        new_sp = si_addr;
    } else {
        new_sp = uc_addr;
    }

    /*
     * Push the return address (SIG_TRAMPOLINE) so that when the
     * signal handler executes RET it lands in the trampoline which
     * calls sys_sigreturn.
     *
     * System V AMD64 ABI requires RSP to be 16-byte aligned BEFORE
     * the CALL instruction, i.e. RSP % 16 == 0 *before* the 8-byte
     * return address is pushed, so RSP % 16 == 8 on function entry.
     */
    new_sp = (new_sp & ~0xFUL) - 8;
    uint64 ret_addr = SIG_TRAMPOLINE;

    /* Grow the stack VMA if necessary. */
    if ((sa->sa_flags & SA_ONSTACK) == 0) {
        if (p->vm == NULL)
            exit(-1);
        if (vm_try_growstack(p->vm, new_sp) != 0)
            exit(-1);
    }

    /* Build the ucontext. */
    ucontext_t uc = {0};
    uc.uc_link = (ucontext_t *)p->signal.sig_ucontext;
    uc.uc_sigmask = p->signal.sig_mask;
    memmove(&uc.uc_mcontext, p->trapframe, sizeof(mcontext_t));
    memmove(&uc.uc_stack, &p->signal.sig_stack, sizeof(stack_t));

    /* Save FP state into the signal frame if this thread has used FP. */
    if (THREAD_FPU_USED(p) && p->fpu_state != NULL) {
        if (mycpu()->fpu_owner_tid == p->pid &&
            p->fpu_seq == mycpu()->fpu_seq) {
            /* FP regs are live in hardware — clear TS and save them. */
            asm volatile("clts");
            fpu_save_state(p->fpu_state);
        }
        memmove(&uc.uc_fpstate, p->fpu_state, sizeof(struct fpu_state));
        uc.uc_fpflags = 1; /* FP state is valid */
    }

    /* Copy ucontext to user stack. */
    if (vm_copyout(p->vm, uc_addr, (void *)&uc, sizeof(ucontext_t)) != 0)
        return -1;

    /* Copy siginfo to user stack. */
    if (sa->sa_flags & SA_SIGINFO) {
        if (vm_copyout(p->vm, si_addr, &info->info, sizeof(siginfo_t)) != 0)
            return -1;
    }

    /* Write the return address (trampoline) below the frame. */
    if (vm_copyout(p->vm, new_sp, (void *)&ret_addr, sizeof(ret_addr)) != 0)
        return -1;

    /*
     * Redirect execution to the signal handler.
     * x86_64 System V calling convention: RDI, RSI, RDX, …
     */
    p->trapframe->trapframe.rip = (uint64)sa->sa_handler;
    p->trapframe->trapframe.rsp = new_sp;
    arch_tf_set_arg0(p->trapframe, (uint64)signo); /* arg1: signal number  */
    arch_tf_set_arg1(p->trapframe, si_addr);       /* arg2: siginfo_t *    */
    arch_tf_set_arg2(p->trapframe, uc_addr);       /* arg3: ucontext_t *   */

    p->signal.sig_ucontext = uc_addr;

    return 0;
}

int restore_sigframe(struct thread *p, ucontext_t *ret_uc) {
    uint64 sig_ucontext = p->signal.sig_ucontext;

    if (sig_ucontext == 0 || ret_uc == NULL)
        return -1;

    /* Copy the saved ucontext back from user memory. */
    if (vm_copyin(p->vm, (void *)ret_uc, sig_ucontext, sizeof(ucontext_t)) != 0)
        return -1;

    p->signal.sig_ucontext = (uint64)ret_uc->uc_link;
    memmove(p->trapframe, &ret_uc->uc_mcontext, sizeof(mcontext_t));

    /* Restore FP state from the signal frame if it was saved. */
    if (ret_uc->uc_fpflags && p->fpu_state != NULL) {
        memmove(p->fpu_state, &ret_uc->uc_fpstate, sizeof(struct fpu_state));
        /* Invalidate fpu_owner so the next FP use triggers a lazy reload. */
        if (mycpu()->fpu_owner_tid == p->pid)
            mycpu()->fpu_owner_tid = 0;
    }

    return 0;
}

/* ─────────────────── I/O helpers ─────────────────── */

static inline void outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8 inb(uint16 port) {
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ─────────────────── PIC 8259A ─────────────────── */

#define PIC1_CMD 0x20
#define PIC1_DATA 0x21
#define PIC2_CMD 0xA0
#define PIC2_DATA 0xA1

#define PIC_EOI 0x20

/* IRQ vector base: IRQ 0 → vector 32 */
#define IRQ_BASE 32

static void pic_init(void) {
    /* ICW1: begin init + ICW4 needed */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);

    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_BASE);     /* master: IRQ 0-7  → vectors 32-39 */
    outb(PIC2_DATA, IRQ_BASE + 8); /* slave:  IRQ 8-15 → vectors 40-47 */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04); /* master: slave on IRQ 2 */
    outb(PIC2_DATA, 0x02); /* slave:  cascade identity 2 */

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, 0x01);
    outb(PIC2_DATA, 0x01);

    /* Mask ALL IRQs — we use the I/O APIC now */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

/* ── IDT ── */
static struct idt_gate idt[IDT_ENTRIES] __attribute__((aligned(4096)));
static struct x86_desc_ptr idtr;

void x86_idt_init(void) {
    extern void vector0(void);
    uint64 trapvec_page0 = PGROUNDDOWN((uint64)vector0);
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(&idt[i], TRAPVEC_ALIAS_BASE + (vectors[i] - trapvec_page0),
                     SEG_KCODE, GATE_INTERRUPT, 1);

    /* Vector 3 (INT3 / breakpoint) must be DPL=3 so user-mode
     * processes can trigger it for GDB breakpoints / waitgdb. */
    idt_set_gate(&idt[3], TRAPVEC_ALIAS_BASE + (vectors[3] - trapvec_page0),
                 SEG_KCODE, GATE_USER_INT, 1);

    idtr.limit = sizeof(idt) - 1;
    idtr.base = (uint64)idt;
    asm volatile("lidt %0" : : "m"(idtr));
}

void x86_idt_remap(uint64 idt_va) {
    idtr.base = idt_va;
    asm volatile("lidt %0" : : "m"(idtr));
}

uint64 x86_idt_page_pa(void) { return (uint64)idt; }

/* ── arch_trap_init / arch_trap_init_hart ── */

void arch_trap_init(void) {
    trampoline_userret =
        (void *)(TRAMPOLINE + ((uint64)userret - (uint64)trampoline));
    x86_gdt_init();
    x86_idt_init();
    x86_syscall_init();

    /*
     * Remap GDT+TSS and IDT to high-canonical addresses (CPU_ENTRY_AREA)
     * so they are accessible when running under user CR3.
     * iretq reads the GDT to validate CS/SS, and the IDT/TSS must be
     * reachable for fault delivery during ring transitions.
     * The physical pages were already mapped in arch_vm_init().
     */
    x86_gdt_remap(CPU_ENTRY_GDT);
    x86_idt_remap(CPU_ENTRY_IDT);
}

void arch_trap_init_hart(void) {
    static int bsp_trap_done = 0;
    if (!bsp_trap_done) {
        /* BSP: GDT/TSS/SYSCALL already set up by arch_trap_init() */
        bsp_trap_done = 1;
    } else {
        /* AP: load GDT, TSS, and SYSCALL MSRs */
        x86_gdt_init_ap();
    }
    x86_tss_set_ist1(mycpu()->intr_sp);
    asm volatile("lidt %0" : : "m"(idtr));
}

/* ── Interrupt controller init ── */

/* Defined in timer/timer.c */
extern void arch_timer_init(void);

/* Forward declarations for debugcon helpers defined later */
static inline void dbg_outb(uint16 port, uint8 val);
static void dbg_puts(const char *s);
static void dbg_hex(uint64 v);

void arch_irq_init(void) {
    dbg_puts("[x86] arch_irq_init: begin\n");

    /* Remap and mask the legacy PIC so it doesn't interfere */
    pic_init();
    dbg_puts("[x86] arch_irq_init: PIC masked\n");

    /* Initialize the Local APIC on this CPU */
    lapic_init();
    dbg_puts("[x86] arch_irq_init: LAPIC done\n");

    /* Initialize the I/O APIC — masks all redirection entries */
    ioapic_init();
    dbg_puts("[x86] arch_irq_init: IOAPIC done\n");

    /* Select and start the tick timer (LAPIC/HPET/PIT) */
    arch_timer_init();

    /* If the chosen timer uses IRQ 0 (PIT or HPET), route it via IOAPIC */
    extern int arch_timer_needs_ioapic_irq0(void);
    int bsp_id = lapic_id();
    if (arch_timer_needs_ioapic_irq0())
        ioapic_enable(0, T_IRQ0 + 0, bsp_id); /* IRQ 0 → vector 32 → BSP */

    dbg_puts("[x86] arch_irq_init: APIC initialized, bsp_id=");
    dbg_hex((uint64)bsp_id);
    dbg_puts("\n");
}

void arch_irq_init_hart(void) {
    /* Per-CPU LAPIC init for secondary CPUs.
     * Skip for BSP — arch_irq_init() already set up the LAPIC and timer.
     * Calling lapic_init() again would re-mask the LVT timer entry. */
    static int bsp_done = 0;
    if (!bsp_done) {
        bsp_done = 1;
        return; /* BSP: already initialized in arch_irq_init() */
    }
    lapic_init();
    extern void arch_timer_init_hart(void);
    arch_timer_init_hart();
}

/*
 * plic_enable_irq — called by register_irq_handler() for device IRQs.
 *
 * On x86, this programs the I/O APIC to route the given ISA IRQ
 * to the BSP's LAPIC at vector T_IRQ0 + irq.
 */
void plic_enable_irq(int irq) {
    ioapic_enable(irq, T_IRQ0 + irq, 0 /* BSP LAPIC ID */);
}

/*
 * plic_enable_irq_level - enable a PCI device IRQ with level-trigger.
 *
 * PCI interrupts are level-triggered, active-low.  Call this after
 * register_irq_handler() for PCI devices to reprogram the IOAPIC entry.
 */
void plic_enable_irq_level(int irq) {
    ioapic_enable_level(irq, T_IRQ0 + irq, 0 /* BSP LAPIC ID */);
}

/* These are only used on RISC-V; on x86 the trap handler dispatches
 * directly and sends LAPIC EOI. */
int plic_claim(void) { return 0; }
void plic_complete(int irq) { (void)irq; }

/* ── Exception name table ── */

static const char *exception_names[32] = {
    [0] = "#DE Divide Error",
    [1] = "#DB Debug",
    [2] = "NMI",
    [3] = "#BP Breakpoint",
    [4] = "#OF Overflow",
    [5] = "#BR Bound Range",
    [6] = "#UD Invalid Opcode",
    [7] = "#NM Device Not Available",
    [8] = "#DF Double Fault",
    [9] = "(reserved)",
    [10] = "#TS Invalid TSS",
    [11] = "#NP Segment Not Present",
    [12] = "#SS Stack-Segment Fault",
    [13] = "#GP General Protection",
    [14] = "#PF Page Fault",
    [15] = "(reserved)",
    [16] = "#MF x87 FP Exception",
    [17] = "#AC Alignment Check",
    [18] = "#MC Machine Check",
    [19] = "#XM SIMD FP Exception",
    [20] = "#VE Virtualization",
    [21] = "#CP Control Protection",
    [22] = "(reserved)",
    [23] = "(reserved)",
    [24] = "(reserved)",
    [25] = "(reserved)",
    [26] = "(reserved)",
    [27] = "(reserved)",
    [28] = "(reserved)",
    [29] = "#SX Security Exception",
    [30] = "#HV VMM Communication",
    [31] = "(reserved)",
};

/* ── Debugcon helpers ── */
static inline void dbg_outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static void dbg_puts(const char *s) {
    while (*s)
        dbg_outb(0xE9, (uint8)*s++);
}
static void dbg_hex(uint64 v) {
    static const char h[] = "0123456789abcdef";
    for (int i = 15; i >= 0; i--)
        dbg_outb(0xE9, (uint8)h[(v >> (i * 4)) & 0xF]);
}

static inline uint64 read_cr2(void) {
    uint64 v;
    asm volatile("movq %%cr2, %0" : "=r"(v));
    return v;
}

/* ── Timer tick advance (defined in timer.c) ── */
extern void timer_tick_advance(void);

/* ── Device IRQ dispatch (defined in kernel/irq/irq.c) ── */
extern int do_device_irq(int hw_irq);

/* ── x86_trap_handler ── */

void x86_trap_handler(struct trapframe *tf) {
    uint64 vec = tf->trapno;
    int from_user = ((tf->cs & 3) == 3);

    /*
     * For user-mode traps, copy the stack-based trapframe into the thread's
     * utrapframe so that syscall arg fetching and other code that reads
     * p->trapframe->trapframe works correctly.
     *
     * Also save the user GS base (now in KERNEL_GS_BASE after swapgs)
     * into the utrapframe so it can be restored on return to user mode.
     */
    if (from_user && current && current->trapframe) {
        current->trapframe->trapframe = *tf;
        current->trapframe->user_gs_base = rdmsr(MSR_KERNEL_GS_BASE);
    }

    /* Mark the current CPU offline for the user VM, online for the kernel VM */
    if (from_user && current && current->vm) {
        vm_cpu_offline(current->vm, cpuid());
        vm_cpu_online(kernel_vm, cpuid(), current);
    }

    /*
     * Page fault (#PF): demand paging for user-mode faults.
     * Error code bits: [0]=P (present), [1]=W (write), [2]=U (user).
     */
    if (vec == 14) {
        uint64 cr2 = read_cr2();

        if (from_user && current && current->vm) {
            /*
             * User-mode page fault — attempt demand paging.
             * x86 PF error code bit 1 = write fault.
             */
            int is_write = (tf->err & 0x2);
            uint64 prot = VMA_FLAG_USER | (is_write ? PROT_WRITE : PROT_READ);

            /* Try growing the stack first */
            vm_try_growstack(current->vm, cr2);

            vm_rlock(current->vm);
            vma_t *vma = vm_find_area(current->vm, cr2);
            if (vma != NULL && vma_validate(vma, cr2, 1, prot) == 0) {
                vm_runlock(current->vm);
                goto user_return; /* fault resolved */
            }
            vm_runlock(current->vm);

            /* Could not resolve — kill the process */
            printf(
                "pid %d %s: fatal page fault cr2=0x%lx err=0x%lx rip=0x%lx\n",
                current->pid, current->name, cr2, tf->err, tf->rip);
            assert(current->pid != 1, "init exiting");
            {
                extern int gdbstub_signal_stop(struct thread *, int);
                gdbstub_signal_stop(current, SIGSEGV);
            }
            kill(current->pid, SIGSEGV);
            goto user_return;
        }

        if (from_user && current) {
            /* User-mode page fault but VM is NULL — process is being
             * torn down.  Just kill it; don't fall through to kernel
             * panic. */
            printf("pid %d %s: page fault with NULL vm cr2=0x%lx rip=0x%lx\n",
                   current->pid, current->name, cr2, tf->rip);
            assert(current->pid != 1, "init exiting");
            kill(current->pid, SIGSEGV);
            goto user_return;
        }

        /* Kernel-mode page fault — unrecoverable */
        dbg_puts("\n*** KERNEL PAGE FAULT\n");
        dbg_puts("  cr2=0x");
        dbg_hex(cr2);
        dbg_puts(" err=0x");
        dbg_hex(tf->err);
        dbg_puts(" rip=0x");
        dbg_hex(tf->rip);
        dbg_puts("\n");
        printf("\n*** KERNEL PAGE FAULT: cr2=0x%lx err=0x%lx rip=0x%lx\n", cr2,
               tf->err, tf->rip);
        *(uint64 *)((uint64)tf - 8) = tf->rip;
        panic_disable_bt();
        panic("kernel page fault");
    }

    if (vec == 3 && from_user && current) {
        /* INT3 breakpoint from user mode — hand off to gdbstub.
         * Used by waitgdb() and GDB software breakpoints.
         *
         * x86 INT3 pushes the address AFTER the 1-byte 0xCC onto the
         * stack, so RIP in the trapframe points one byte past the INT3.
         * Adjust it back so RIP points AT the INT3, matching RISC-V
         * behavior (sepc points at EBREAK).  This lets gdbstub_trap()
         * match breakpoints by address uniformly on both architectures.
         *
         * IMPORTANT: modify the utrapframe (not the stack tf), because
         * gdbstub_trap reads/writes p->trapframe->trapframe, and we
         * return via usertrapret() which also uses the utrapframe.
         * The stack-based tf is NOT used on the return path. */
        current->trapframe->trapframe.rip -= 1;
        extern int gdbstub_trap(struct thread *);
        if (gdbstub_trap(current) != 0) {
            /* GDB stub didn't handle it — deliver SIGTRAP */
            printf("pid %d %s: breakpoint trap rip=0x%lx\n", current->pid,
                   current->name, current->trapframe->trapframe.rip);
            kill(current->pid, SIGTRAP);
        }
        usertrapret(); /* noreturn — returns via utrapframe */
    }

    if (vec == 1 && from_user && current) {
        /* #DB (Debug exception) from user mode — triggered by RFLAGS.TF
         * hardware single-step.  Hand off to gdbstub.
         *
         * Modify the utrapframe (not stack tf) because gdbstub_trap
         * reads/writes the utrapframe, and usertrapret uses it too. */
        current->trapframe->trapframe.rflags &= ~(1ULL << 8);
        extern int gdbstub_trap(struct thread *);
        if (gdbstub_trap(current) != 0) {
            /* GDB stub didn't handle it — deliver SIGTRAP */
            printf("pid %d %s: debug trap rip=0x%lx\n", current->pid,
                   current->name, current->trapframe->trapframe.rip);
            kill(current->pid, SIGTRAP);
        }
        usertrapret(); /* noreturn — returns via utrapframe */
    }

    if (vec < 32) {
        /* #NM (Device Not Available, vector 7) — lazy FPU switching.
         * CR0.TS was set, user code executed an FP/SSE instruction. */
        if (vec == 7 && from_user && current) {
            /* Clear TS so kernel FP save/restore routines work. */
            asm volatile("clts");

            struct cpu_local *c = mycpu();

            /* First time this thread uses FP — allocate fpu_state */
            if (!THREAD_FPU_USED(current)) {
                if (current->fpu_state == NULL) {
                    current->fpu_state = kvmalloc(sizeof(struct fpu_state));
                    if (current->fpu_state == NULL) {
                        printf("pid %d: failed to allocate fpu_state\n",
                               current->pid);
                        kill(current->pid, SIGKILL);
                        goto user_return;
                    }
                    memset(current->fpu_state, 0, sizeof(struct fpu_state));
                }
                fpu_init_state();
                THREAD_SET_FPU_USED(current);
            } else if (c->fpu_owner_tid != current->pid ||
                       current->fpu_seq != c->fpu_seq) {
                /*
                 * Different owner or mismatched seq — restore our
                 * saved FP state and take ownership.  Bump seq so
                 * stale per-CPU entries from other CPUs won't match.
                 */
                fpu_restore_state(current->fpu_state);
                current->fpu_seq++;
            }
            c->fpu_owner_tid = current->pid;
            c->fpu_seq = current->fpu_seq;
            /* TS is clear — FP/SSE will work when we return to user */
            goto user_return;
        }

        /* CPU exception (not #PF, not #BP, not #NM) */
        const char *name = exception_names[vec];

        if (from_user && current) {
            /* User-mode exception — deliver appropriate signal */
            int sig = SIGSEGV;
            if (vec == 0 || vec == 16 || vec == 19)
                sig = SIGFPE; /* #DE, #MF, #XM */
            if (vec == 6)
                sig = SIGILL; /* #UD */
            printf("pid %d %s: exception %ld (%s) rip=0x%lx err=0x%lx\n",
                   current->pid, current->name, vec, name ? name : "???",
                   tf->rip, tf->err);
            assert(current->pid != 1, "init exiting");
            {
                extern int gdbstub_signal_stop(struct thread *, int);
                gdbstub_signal_stop(current, sig);
            }
            kill(current->pid, sig);
            goto user_return;
        }

        /* Kernel exception */
        dbg_puts("\n*** EXCEPTION: ");
        dbg_puts(name ? name : "???");
        dbg_puts("\n  vector=0x");
        dbg_hex(vec);
        dbg_puts(" err=0x");
        dbg_hex(tf->err);
        dbg_puts("\n  rip=0x");
        dbg_hex(tf->rip);
        dbg_puts(" cs=0x");
        dbg_hex(tf->cs);
        dbg_puts("\n  rflags=0x");
        dbg_hex(tf->rflags);
        dbg_puts(" rsp=0x");
        dbg_hex(tf->rsp);
        dbg_puts(" ss=0x");
        dbg_hex(tf->ss);
        dbg_puts("\n  rax=0x");
        dbg_hex(tf->rax);
        dbg_puts(" rbx=0x");
        dbg_hex(tf->rbx);
        dbg_puts(" rcx=0x");
        dbg_hex(tf->rcx);
        dbg_puts(" rdx=0x");
        dbg_hex(tf->rdx);
        dbg_puts("\n  rdi=0x");
        dbg_hex(tf->rdi);
        dbg_puts(" rsi=0x");
        dbg_hex(tf->rsi);
        dbg_puts(" rbp=0x");
        dbg_hex(tf->rbp);
        dbg_puts("\n  r8 =0x");
        dbg_hex(tf->r8);
        dbg_puts(" r9 =0x");
        dbg_hex(tf->r9);
        dbg_puts(" r10=0x");
        dbg_hex(tf->r10);
        dbg_puts(" r11=0x");
        dbg_hex(tf->r11);
        dbg_puts("\n  r12=0x");
        dbg_hex(tf->r12);
        dbg_puts(" r13=0x");
        dbg_hex(tf->r13);
        dbg_puts(" r14=0x");
        dbg_hex(tf->r14);
        dbg_puts(" r15=0x");
        dbg_hex(tf->r15);
        dbg_puts("\n");
        printf("\n*** EXCEPTION %ld: %s\n", vec, name ? name : "???");
        printf("  err=0x%lx  rip=0x%lx  cs=0x%lx\n", tf->err, tf->rip, tf->cs);
        printf("  rsp=0x%lx  rbp=0x%lx  rflags=0x%lx\n", tf->rsp, tf->rbp,
               tf->rflags);

        *(uint64 *)((uint64)tf - 8) = tf->rip;
        panic_disable_bt();
        panic("exception");

    } else if (vec >= T_IRQ0 && vec < T_IRQ0 + 24) {
        /* Hardware IRQ via I/O APIC (ISA IRQ 0-23) */
        int irq = vec - T_IRQ0;

        if (irq == 0) {
            /* PIT timer tick */
            timer_tick_advance();
        } else {
            /* Dispatch through irq_descs[] for registered device handlers */
            do_device_irq(irq);
        }

        lapic_eoi();

    } else if (vec == LAPIC_TIMER_VEC) {
        /* LAPIC timer interrupt (periodic or TSC-deadline) */
        timer_tick_advance();
        extern void lapic_timer_rearm(void);
        lapic_timer_rearm(); /* no-op for periodic mode */
        lapic_eoi();

    } else if (vec == LAPIC_IPI_VEC) {
        /* Inter-processor interrupt */
        extern void x86_ipi_handler(void);
        lapic_eoi(); /* EOI before handler so nested IPIs can arrive */
        x86_ipi_handler();

    } else if (vec == LAPIC_ERROR_VEC) {
        /* LAPIC error interrupt */
        printf("[x86] LAPIC error: ESR=0x%x\n", lapic_read(LAPIC_ESR));
        lapic_eoi();

    } else if (vec == LAPIC_SPURIOUS_VEC) {
        /* Spurious interrupt — do NOT send EOI */
    }

    /*
     * User-mode traps: return through usertrapret so that
     * the trapframe page, GS scratch, and TSS are set up
     * for the next entry.  All user-mode code paths above
     * must reach here (via goto user_return) rather than
     * using plain return, because alltraps did swapgs on
     * entry and trapret does NOT swapgs on exit.
     */
user_return:
    if (from_user && current) {
        usertrapret(); /* noreturn */
    }
    /* Kernel traps return here and fall back to trapret/iretq. */
}

/*
 * usertrap_syscall — C-level SYSCALL handler.
 *
 * Called from syscall_entry (trapvec.S) with RDI = pointer to
 * struct trapframe built on the kernel stack (Linux strategy).
 * We copy it into the per-process utrapframe, just like
 * x86_trap_handler does for IDT-based user traps.
 */
void usertrap_syscall(struct trapframe *tf) {
    struct thread *p = current;
    (void)p;

    /*
     * Copy the stack-based trapframe into the per-process utrapframe.
     * This mirrors what x86_trap_handler does for IDT traps.
     */
    if (p && p->trapframe) {
        p->trapframe->trapframe = *tf;
        /* Save user GS base (now in KERNEL_GS_BASE after swapgs) */
        p->trapframe->user_gs_base = rdmsr(MSR_KERNEL_GS_BASE);
    }

    /* Mark the current CPU offline for the user VM, online for the kernel VM */
    if (p && p->vm) {
        vm_cpu_offline(p->vm, cpuid());
        vm_cpu_online(kernel_vm, cpuid(), p);
    }

    intr_on();
    syscall();
    usertrapret(); /* noreturn */
}