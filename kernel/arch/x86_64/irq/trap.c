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

/* ── Provided by trapvec.S — table of 256 stub addresses ── */
extern uint64 vectors[];

/* ── Trampoline / signal stubs (needed by the linker; still stubs) ── */
uint64 trampoline_ksatp = 0;
char trampoline[4096];
char _trampoline_data[4096];

void sig_trampoline(void) {}

/*
 * usertrapret — return to user mode via iretq.
 *
 * Builds the iret frame on the kernel stack from the thread's utrapframe
 * and restores all GPRs before executing iretq.
 *
 * iretq pops: RIP, CS, RFLAGS, RSP, SS  (in that order from the stack).
 */
void usertrapret(void) {
    struct thread *p = current;
    struct trapframe *tf = &p->trapframe->trapframe;

    /* Disable interrupts — we're manually building the iret frame. */
    intr_off();

    /* Save kernel stack pointer for next trap entry.
     * The TSS RSP0 will be used by the CPU when transitioning
     * from ring 3 → ring 0. */
    p->trapframe->kernel_sp = p->ksp;

    /*
     * Use inline assembly to:
     * 1. Load all GPRs from the trapframe
     * 2. Push the 5-element iret frame (SS, RSP, RFLAGS, CS, RIP)
     * 3. Execute iretq
     *
     * We pass the trapframe pointer in a register and do everything
     * from assembly to avoid the compiler clobbering state.
     */
    asm volatile(
        /* Push iret frame: SS, RSP, RFLAGS, CS, RIP */
        "pushq %[ss]\n\t"
        "pushq %[rsp]\n\t"
        "pushq %[rflags]\n\t"
        "pushq %[cs]\n\t"
        "pushq %[rip]\n\t"

        /* Load GPRs from trapframe.
         * rdi holds tf pointer; we load it last since we need it. */
        "movq 0x00(%[tf]), %%r15\n\t"
        "movq 0x08(%[tf]), %%r14\n\t"
        "movq 0x10(%[tf]), %%r13\n\t"
        "movq 0x18(%[tf]), %%r12\n\t"
        "movq 0x20(%[tf]), %%r11\n\t"
        "movq 0x28(%[tf]), %%r10\n\t"
        "movq 0x30(%[tf]), %%r9\n\t"
        "movq 0x38(%[tf]), %%r8\n\t"
        "movq 0x40(%[tf]), %%rbp\n\t"
        /* skip rdi (0x48) — load last */
        "movq 0x50(%[tf]), %%rsi\n\t"
        "movq 0x58(%[tf]), %%rdx\n\t"
        "movq 0x60(%[tf]), %%rcx\n\t"
        "movq 0x68(%[tf]), %%rbx\n\t"
        "movq 0x70(%[tf]), %%rax\n\t"
        "movq 0x48(%[tf]), %%rdi\n\t"

        "iretq\n\t"
        :
        : [tf]     "D" (tf),
          [ss]     "r" ((uint64)(SEG_UDATA | DPL_USER)),
          [rsp]    "r" (tf->rsp),
          [rflags] "r" (tf->rflags | 0x200),  /* ensure IF set */
          [cs]     "r" ((uint64)(SEG_UCODE | DPL_USER)),
          [rip]    "r" (tf->rip)
        : "memory"
    );

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

    /* Mask all IRQs initially */
    outb(PIC1_DATA, 0xFF);
    outb(PIC2_DATA, 0xFF);
}

static void pic_unmask(int irq) {
    if (irq < 8) {
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << irq));
    } else {
        outb(PIC2_DATA, inb(PIC2_DATA) & ~(1 << (irq - 8)));
        /* Also unmask cascade line (IRQ 2) on master */
        outb(PIC1_DATA, inb(PIC1_DATA) & ~(1 << 2));
    }
}

static void pic_eoi(int irq) {
    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

/* ── IDT ── */
static struct idt_gate idt[IDT_ENTRIES] __attribute__((aligned(16)));
static struct x86_desc_ptr idtr;

void x86_idt_init(void) {
    for (int i = 0; i < IDT_ENTRIES; i++)
        idt_set_gate(&idt[i], vectors[i], SEG_KCODE, GATE_INTERRUPT, 0);

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64)idt;
    asm volatile("lidt %0" : : "m"(idtr));
}

/* ── arch_trap_init / arch_trap_init_hart ── */

void arch_trap_init(void) {
    x86_gdt_init();
    x86_idt_init();
    x86_syscall_init();
}

void arch_trap_init_hart(void) {
    asm volatile("lidt %0" : : "m"(idtr));
}

/* ── Interrupt controller init ── */

/* Defined in timer/timer.c */
extern void arch_timer_init(void);

void arch_irq_init(void) {
    pic_init();
    arch_timer_init();        /* program PIT channel 0 for 100 Hz */
    /* Unmask IRQ 0 (PIT timer) */
    pic_unmask(0);
    printf("[x86] PIC 8259A initialized, IRQ 0 (timer) unmasked\n");
}

void arch_irq_init_hart(void) {
    /* Per-CPU LAPIC init would go here (SMP) */
}

int plic_claim(void)          { return 0; }
void plic_complete(int irq)   { (void)irq; }
void plic_enable_irq(int irq) { (void)irq; }

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

/* ── x86_trap_handler ── */

void x86_trap_handler(struct trapframe *tf) {
    uint64 vec = tf->trapno;

    if (vec < 32) {
        /* CPU exception */
        const char *name = exception_names[vec];

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

        if (vec == 14) {
            uint64 cr2 = read_cr2();
            dbg_puts("\n  cr2=0x"); dbg_hex(cr2);
            printf("\n*** PAGE FAULT: cr2=0x%lx err=0x%lx rip=0x%lx\n",
                   cr2, tf->err, tf->rip);
        }

        dbg_puts("\n");
        printf("\n*** EXCEPTION %ld: %s\n", vec, name ? name : "???");
        printf("  err=0x%lx  rip=0x%lx  cs=0x%lx\n",
               tf->err, tf->rip, tf->cs);
        printf("  rsp=0x%lx  rbp=0x%lx  rflags=0x%lx\n",
               tf->rsp, tf->rbp, tf->rflags);

        /* Fix up trapframe so GDB can unwind to the faulting code.
         * Same technique as RISC-V __trap_panic:
         *   - Write faulting RIP just below the trapframe so the
         *     backtrace walker finds it as a return address.
         *   - Then call panic() which halts with a full backtrace.
         */
        *(uint64 *)((uint64)tf - 8) = tf->rip;
        panic_disable_bt();
        panic("exception");

    } else if (vec >= 32 && vec < 48) {
        /* Hardware IRQ (PIC) */
        int irq = vec - 32;

        if (irq == 0) {
            /* PIT timer tick */
            timer_tick_advance();
        } else {
            /* Other IRQs — log but otherwise ignore */
            dbg_puts("[x86] IRQ ");
            dbg_hex(irq);
            dbg_puts(" (unhandled)\n");
        }

        pic_eoi(irq);

    } else if (vec == 0x80) {
        dbg_puts("[x86] INT 0x80 syscall (stub)\n");
    }
    /* Spurious / unknown vectors: silently ignored */
}