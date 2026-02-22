/**
 * @file trap.c
 * @brief x86_64 IDT setup, interrupt controller stubs, and trap handler.
 *
 * arch_trap_init()  — builds the 256-entry IDT, loads it, installs the GDT
 *                     and sets up SYSCALL/SYSRET MSRs.
 * x86_trap_handler — C-level handler called from assembly (trapvec.S).
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
void usertrapret(void) {}

int push_sigframe(struct thread *p, int signo, sigaction_t *sa,
                  ksiginfo_t *info) {
    (void)p; (void)signo; (void)sa; (void)info;
    return -1;
}

int restore_sigframe(struct thread *p, ucontext_t *ret_uc) {
    (void)p; (void)ret_uc;
    return -1;
}

/* ── IDT ── */
static struct idt_gate idt[IDT_ENTRIES] __attribute__((aligned(16)));
static struct x86_desc_ptr idtr;

/**
 * x86_idt_init — populate all 256 entries and register the IDT.
 *
 * All vectors use GATE_INTERRUPT (clears IF on entry) with IST 0
 * (use the current RSP0 from TSS on ring transitions, or the kernel
 * stack if already in ring 0).
 */
void x86_idt_init(void)
{
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(&idt[i], vectors[i], SEG_KCODE,
                     GATE_INTERRUPT, 0);
    }

    idtr.limit = sizeof(idt) - 1;
    idtr.base  = (uint64)idt;

    asm volatile("lidt %0" : : "m"(idtr));
}

/* ──────────────────────────────────────────────────────────────
 *  arch_trap_init / arch_trap_init_hart  (called from start_kernel)
 * ────────────────────────────────────────────────────────────── */

void arch_trap_init(void)
{
    /* GDT + TSS */
    x86_gdt_init();

    /* IDT — depends on the GDT being loaded (CS selector) */
    x86_idt_init();

    /* SYSCALL/SYSRET MSRs */
    x86_syscall_init();
}

void arch_trap_init_hart(void)
{
    /* Secondary CPUs: reload IDT (shared for now) */
    asm volatile("lidt %0" : : "m"(idtr));
}

/* ──────────────────────────────────────────────────────────────
 *  Interrupt-controller stubs (APIC not yet wired)
 * ────────────────────────────────────────────────────────────── */

void arch_irq_init(void)      { /* TODO: PIC mask-all + I/O APIC init */ }
void arch_irq_init_hart(void) { /* TODO: LAPIC init */ }
int  plic_claim(void)         { return 0; }
void plic_complete(int irq)   { (void)irq; }
void plic_enable_irq(int irq) { (void)irq; }

/* ──────────────────────────────────────────────────────────────
 *  Exception name table (vectors 0-31)
 * ────────────────────────────────────────────────────────────── */

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

/* ── Debugcon helper (port 0xE9) for pre-printf diagnostics ── */
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

static inline uint64 read_cr2(void)
{
    uint64 v;
    asm volatile("movq %%cr2, %0" : "=r"(v));
    return v;
}

/* ──────────────────────────────────────────────────────────────
 *  x86_trap_handler — called from alltraps (trapvec.S)
 * ────────────────────────────────────────────────────────────── */

void x86_trap_handler(struct trapframe *tf)
{
    uint64 vec = tf->trapno;

    if (vec < 32) {
        /*
         * CPU exception.  For now, print diagnostics and halt.
         * When we have a scheduler, page-fault handling etc. will
         * go here.
         */
        const char *name = exception_names[vec];

        /* Debugcon output (always available, even before printf) */
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
            dbg_puts("\n  cr2=0x");
            dbg_hex(cr2);
            printf("\n*** PAGE FAULT: cr2=0x%lx err=0x%lx rip=0x%lx\n",
                   cr2, tf->err, tf->rip);
        }

        dbg_puts("\n");

        /* Also print via printf if console is up */
        printf("\n*** EXCEPTION %ld: %s\n", vec, name ? name : "???");
        printf("  err=0x%lx  rip=0x%lx  cs=0x%lx\n",
               tf->err, tf->rip, tf->cs);
        printf("  rsp=0x%lx  rbp=0x%lx  rflags=0x%lx\n",
               tf->rsp, tf->rbp, tf->rflags);

        /* Halt — in the future we may kill the current process instead */
        for (;;)
            asm volatile("cli; hlt");

    } else if (vec >= 32 && vec < 48) {
        /*
         * Hardware IRQ (vector 32-47 = legacy ISA IRQ 0-15).
         * When APIC is wired, this will dispatch through the IRQ
         * descriptor table.  For now just acknowledge and return.
         */
        dbg_puts("[x86] IRQ ");
        dbg_hex(vec - 32);
        dbg_puts(" (unhandled)\n");

        /* TODO: send EOI to LAPIC */

    } else if (vec == 0x80) {
        /*
         * INT 0x80 — legacy syscall vector (placeholder).
         * Real syscalls go through SYSCALL/SYSRET.
         */
        dbg_puts("[x86] INT 0x80 syscall (stub)\n");

    } else {
        /* Spurious or unknown vector — just ignore */
    }
}