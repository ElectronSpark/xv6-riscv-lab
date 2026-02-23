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
#include "arch/trap.h"
#include "proc/thread.h"
#include "signal.h"
#include "seg.h"
#include "trapframe.h"
#include "lapic.h"
#include "ioapic.h"
#include <mm/vm.h>
#include <smp/percpu.h>
#include "memlayout.h"

extern pagetable_t kernel_pagetable;

/* ── Provided by trapvec.S — table of 256 stub addresses ── */
extern uint64 vectors[];

/* ── Trampoline stubs (linker compatibility) ── */
extern uint64 trampoline_ksatp;  /* defined in trapvec.S */
extern char trampoline[];
void sig_trampoline(void) {}

extern char userret[];
static void (*trampoline_userret)(uint64 tf, uint64 user_cr3) = NULL;

/*
 * usertrapret — return to user mode.
 *
 * Sets up kernel state in the utrapframe, maps the trapframe page,
 * configures per-CPU GS scratch, and returns via swapgs + iretq.
 */
void usertrapret(void) {
    struct thread *p = current;
    struct trapframe *tf = &p->trapframe->trapframe;

    if (killed(p))
        exit(-1);

    /* Disable interrupts while setting up the return path. */
    intr_off();

    /* Store kernel state in utrapframe for next trap entry. */
    p->trapframe->kernel_sp = p->ksp;
    p->trapframe->kernel_satp = (uint64)kernel_pagetable; /* RISC-V compat */

    /* Set TSS RSP0 so the CPU switches to the kernel stack on ring 3→0
     * for IDT-based traps (exceptions, interrupts).
     * Note: alltraps no longer uses SWAPGS — it reads the kernel CR3
     * from trampoline_ksatp (RIP-relative in trapvec.S) instead. */
    x86_tss_set_rsp0(p->ksp);

    /* Map this CPU's trapframe page into the user page table
     * and get the utrapframe virtual address. */
    int cpu = cpuid();
    uint64 trapframe_base = vm_cpu_online(p->vm, cpu);

    /* Set up per-CPU scratch for the next SYSCALL entry.
     * When SYSCALL fires, swapgs will load GS base from KERNEL_GS_BASE,
     * giving the entry code access to trapframe_va and scratch space. */
    uint64 cpu_local_base =
        TRAMPOLINE_CPULOCAL + ((uint64)cpu * (uint64)sizeof(struct cpu_local));
    uint64 *syscall_scratch_va =
        (uint64 *)(cpu_local_base + __builtin_offsetof(struct cpu_local,
                                                       syscall_scratch));
    uint64 *syscall_tf_va =
        (uint64 *)(cpu_local_base + __builtin_offsetof(struct cpu_local,
                                                       syscall_trapframe_va));
    *syscall_tf_va = trapframe_base;

    /* Seed SWAPGS state for SYSCALL entry path.
     *
     * The trampoline (userret) executes SWAPGS before IRETQ:
     *   - KERNEL_GS_BASE → GS_BASE (user gets 0)
     *   - GS_BASE → KERNEL_GS_BASE (kernel gets scratch pointer)
     *
     * So on next SYSCALL entry, swapgs loads GS_BASE from
     * KERNEL_GS_BASE, giving access to per-CPU scratch/trapframe.
     *
     * IDT-based traps (alltraps) do NOT use SWAPGS — they read the
     * kernel CR3 from trampoline_ksatp (RIP-relative in trapvec.S).
     */
    wrmsr(MSR_KERNEL_GS_BASE, 0);
    wrmsr(MSR_GS_BASE, (uint64)syscall_scratch_va);

    /* Ensure user RFLAGS is valid and has IF set.
     * Bit 1 in RFLAGS is architecturally fixed to 1.
     */
    tf->rflags |= (0x200 | 0x2);

    if (trampoline_userret == NULL)
        panic("usertrapret: trampoline_userret not initialized");

    if (p->vm == NULL || p->vm->pagetable == NULL)
        panic("usertrapret: missing user pagetable");

    trampoline_userret(trapframe_base, (uint64)p->vm->pagetable);

    __builtin_unreachable();
}

int push_sigframe(struct thread *p, int signo, sigaction_t *sa,
                  ksiginfo_t *info) {
    (void)p; (void)signo; (void)sa; (void)info;
    return -1;
}

int restore_sigframe(struct thread *p, ucontext_t *ret_uc) {
    (void)p; (void)ret_uc;
    return -1;
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

#define PIC1_CMD   0x20
#define PIC1_DATA  0x21
#define PIC2_CMD   0xA0
#define PIC2_DATA  0xA1

#define PIC_EOI    0x20

/* IRQ vector base: IRQ 0 → vector 32 */
#define IRQ_BASE   32

static void pic_init(void) {
    /* ICW1: begin init + ICW4 needed */
    outb(PIC1_CMD, 0x11);
    outb(PIC2_CMD, 0x11);

    /* ICW2: vector offsets */
    outb(PIC1_DATA, IRQ_BASE);       /* master: IRQ 0-7  → vectors 32-39 */
    outb(PIC2_DATA, IRQ_BASE + 8);   /* slave:  IRQ 8-15 → vectors 40-47 */

    /* ICW3: cascade wiring */
    outb(PIC1_DATA, 0x04);           /* master: slave on IRQ 2 */
    outb(PIC2_DATA, 0x02);           /* slave:  cascade identity 2 */

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
        idt_set_gate(&idt[i],
                     TRAPVEC_ALIAS_BASE + (vectors[i] - trapvec_page0),
                     SEG_KCODE, GATE_INTERRUPT, 1);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64)idt;
    asm volatile("lidt %0" : : "m"(idtr));
}

void x86_idt_remap(uint64 idt_va)
{
    idtr.base = idt_va;
    asm volatile("lidt %0" : : "m"(idtr));
}

uint64 x86_idt_page_pa(void)
{
    return (uint64)idt;
}

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
        ioapic_enable(0, T_IRQ0 + 0, bsp_id);  /* IRQ 0 → vector 32 → BSP */

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
        return;     /* BSP: already initialized in arch_irq_init() */
    }
    lapic_init();
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

/* These are only used on RISC-V; on x86 the trap handler dispatches
 * directly and sends LAPIC EOI. */
int plic_claim(void)          { return 0; }
void plic_complete(int irq)   { (void)irq; }

/* ── Exception name table ── */

static const char *exception_names[32] = {
    [0]  = "#DE Divide Error",
    [1]  = "#DB Debug",
    [2]  = "NMI",
    [3]  = "#BP Breakpoint",
    [4]  = "#OF Overflow",
    [5]  = "#BR Bound Range",
    [6]  = "#UD Invalid Opcode",
    [7]  = "#NM Device Not Available",
    [8]  = "#DF Double Fault",
    [9]  = "(reserved)",
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
    [22] = "(reserved)", [23] = "(reserved)", [24] = "(reserved)",
    [25] = "(reserved)", [26] = "(reserved)", [27] = "(reserved)",
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
    while (*s) dbg_outb(0xE9, (uint8)*s++);
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
    static int user_trap_marked = 0;

    if (from_user && !user_trap_marked) {
        asm volatile("outb %0, %1" : : "a"((uint8)'U'), "Nd"(0xE9));
        user_trap_marked = 1;
    }

    /*
     * For user-mode traps, copy the stack-based trapframe into the thread's
     * utrapframe so that syscall arg fetching and other code that reads
     * p->trapframe->trapframe works correctly.
     */
    if (from_user && current && current->trapframe) {
        current->trapframe->trapframe = *tf;
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
                return; /* fault resolved */
            }
            vm_runlock(current->vm);

            /* Could not resolve — kill the process */
            printf("pid %d %s: fatal page fault cr2=0x%lx err=0x%lx rip=0x%lx\n",
                   current->pid, current->name, cr2, tf->err, tf->rip);
            assert(current->pid != 1, "init exiting");
            kill(current->pid, SIGSEGV);
            return;
        }

        /* Kernel-mode page fault — unrecoverable */
        dbg_puts("\n*** KERNEL PAGE FAULT\n");
        dbg_puts("  cr2=0x"); dbg_hex(cr2);
        dbg_puts(" err=0x"); dbg_hex(tf->err);
        dbg_puts(" rip=0x"); dbg_hex(tf->rip);
        dbg_puts("\n");
        printf("\n*** KERNEL PAGE FAULT: cr2=0x%lx err=0x%lx rip=0x%lx\n",
               cr2, tf->err, tf->rip);
        *(uint64 *)((uint64)tf - 8) = tf->rip;
        panic_disable_bt();
        panic("kernel page fault");
    }

    if (vec < 32) {
        /* CPU exception (not #PF) */
        const char *name = exception_names[vec];

        if (from_user && current) {
            /* User-mode exception — deliver SIGSEGV / SIGFPE / etc. */
            printf("pid %d %s: exception %ld (%s) rip=0x%lx err=0x%lx\n",
                   current->pid, current->name, vec,
                   name ? name : "???", tf->rip, tf->err);
            assert(current->pid != 1, "init exiting");
            kill(current->pid, SIGSEGV);
            return;
        }

        /* Kernel exception */
        dbg_puts("\n*** EXCEPTION: ");
        dbg_puts(name ? name : "???");
        dbg_puts("\n  vector=0x"); dbg_hex(vec);
        dbg_puts(" err=0x");      dbg_hex(tf->err);
        dbg_puts("\n  rip=0x");   dbg_hex(tf->rip);
        dbg_puts(" cs=0x");       dbg_hex(tf->cs);
        dbg_puts("\n  rflags=0x");dbg_hex(tf->rflags);
        dbg_puts(" rsp=0x");      dbg_hex(tf->rsp);
        dbg_puts(" ss=0x");       dbg_hex(tf->ss);
        dbg_puts("\n  rax=0x");   dbg_hex(tf->rax);
        dbg_puts(" rbx=0x");      dbg_hex(tf->rbx);
        dbg_puts(" rcx=0x");      dbg_hex(tf->rcx);
        dbg_puts(" rdx=0x");      dbg_hex(tf->rdx);
        dbg_puts("\n  rdi=0x");   dbg_hex(tf->rdi);
        dbg_puts(" rsi=0x");      dbg_hex(tf->rsi);
        dbg_puts(" rbp=0x");      dbg_hex(tf->rbp);
        dbg_puts("\n  r8 =0x");   dbg_hex(tf->r8);
        dbg_puts(" r9 =0x");      dbg_hex(tf->r9);
        dbg_puts(" r10=0x");      dbg_hex(tf->r10);
        dbg_puts(" r11=0x");      dbg_hex(tf->r11);
        dbg_puts("\n  r12=0x");   dbg_hex(tf->r12);
        dbg_puts(" r13=0x");      dbg_hex(tf->r13);
        dbg_puts(" r14=0x");      dbg_hex(tf->r14);
        dbg_puts(" r15=0x");      dbg_hex(tf->r15);
        dbg_puts("\n");
        printf("\n*** EXCEPTION %ld: %s\n", vec, name ? name : "???");
        printf("  err=0x%lx  rip=0x%lx  cs=0x%lx\n",
               tf->err, tf->rip, tf->cs);
        printf("  rsp=0x%lx  rbp=0x%lx  rflags=0x%lx\n",
               tf->rsp, tf->rbp, tf->rflags);

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
        lapic_timer_rearm();  /* no-op for periodic mode */
        lapic_eoi();

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
     * for the next entry.
     */
    if (from_user && current) {
        usertrapret();  /* noreturn */
    }
    /* Kernel traps return here and fall back to trapret/iretq. */
}

/*
 * usertrap_syscall — C-level SYSCALL handler.
 *
 * Called from syscall_entry (trapvec.S) after registers have been
 * saved into the trapframe page.  Because the trapframe page IS
 * the physical page containing p->trapframe, the saved registers
 * are already in p->trapframe->trapframe.
 */
void usertrap_syscall(void) {
    struct thread *p = current;
    (void)p;
    static int usertrap_syscall_marked = 0;

    if (!usertrap_syscall_marked) {
        asm volatile("outb %0, %1" : : "a"((uint8)'S'), "Nd"(0xE9));
        usertrap_syscall_marked = 1;
    }

    intr_on();
    syscall();
    usertrapret();  /* noreturn */
}