/**
 * @file ap_trampoline.S
 * @brief Application Processor (AP) boot trampoline for x86_64 SMP.
 *
 * This code is copied to physical address AP_BOOT_PA (0x8000) by the BSP
 * before sending INIT-SIPI-SIPI.  Each AP starts executing here in
 * 16-bit real mode and transitions through 32-bit protected mode to
 * 64-bit long mode, then calls start_kernel(hartid, NULL, false).
 *
 * Stack assignment is race-free: each AP atomically increments
 * ap_boot_next_cpu to obtain its unique hartid (1, 2, 3, ...).
 * The BSP pre-reserves stacks for all cores in a static array
 * (boot_stacks in entry.S, KERNEL_STACK_SIZE * NCPU, aligned).
 *
 * The BSP patches the data area at the end before copying:
 *   ap_boot_cr3         -- physical address of kernel PML4
 *   ap_boot_cpus_base   -- &cpus[0] physical address
 *   ap_boot_cpu_stride  -- sizeof(struct cpu_local)
 *   ap_boot_stacks_base -- physical address of boot_stacks array
 *   ap_boot_next_cpu    -- atomic counter, starts at 1 (BSP is 0)
 *   ap_boot_target      -- address of start_kernel()
 */

#define AP_BOOT_PA  0x8000

/*
 * Macro to convert a label offset to its runtime physical address.
 * All labels are relative to ap_trampoline_start in .rodata, but at
 * runtime the code executes from AP_BOOT_PA.
 */
#define AP_ADDR(label) (AP_BOOT_PA + (label - ap_trampoline_start))

.section .rodata.ap_trampoline, "a"
.globl ap_trampoline_start
ap_trampoline_start:

/* ════════════════════════════════════════════════════════════════
 *  16-bit real-mode entry — SIPI vector points here (AP_BOOT_PA)
 * ════════════════════════════════════════════════════════════════ */
.code16
    cli
    cld
    xorw    %ax, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* Load bootstrap GDT (32-bit base via operand-size override) */
    lgdtl   AP_ADDR(ap_boot_gdt_desc)

    /* Enable Protected Mode */
    movl    %cr0, %eax
    orl     $1, %eax
    movl    %eax, %cr0

    /* Far jump to 32-bit code (operand-size override for 32-bit offset) */
    .byte   0x66, 0xea
    .long   AP_ADDR(ap_entry32)
    .word   0x08                    /* 32-bit code segment selector */

/* ════════════════════════════════════════════════════════════════
 *  32-bit protected mode — set up long mode and enable paging
 * ════════════════════════════════════════════════════════════════ */
.code32
ap_entry32:
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss

    /* Enable PAE (CR4.PAE = bit 5) */
    movl    %cr4, %eax
    orl     $(1 << 5), %eax
    movl    %eax, %cr4

    /* Load kernel page table (PML4 physical address) */
    movl    AP_ADDR(ap_boot_cr3), %eax
    movl    %eax, %cr3

    /* Enable Long Mode (EFER.LME = bit 8) and NXE (bit 11) */
    movl    $0xC0000080, %ecx       /* MSR_EFER */
    rdmsr
    orl     $((1 << 8) | (1 << 11)), %eax
    wrmsr

    /* Enable Paging (CR0.PG = bit 31) — activates long mode */
    movl    %cr0, %eax
    orl     $(1 << 31), %eax
    movl    %eax, %cr0

    /* Far jump to 64-bit code segment */
    ljmpl   $0x18, $AP_ADDR(ap_entry64)

/* ════════════════════════════════════════════════════════════════
 *  64-bit long mode — set up stack and call start_kernel
 * ════════════════════════════════════════════════════════════════ */
.code64
ap_entry64:
    /* Load 64-bit data segments */
    movw    $0x10, %ax
    movw    %ax, %ds
    movw    %ax, %es
    movw    %ax, %ss
    xorw    %ax, %ax
    movw    %ax, %fs
    movw    %ax, %gs

    /* Enable SSE (required for x86_64 ABI) */
    movq    %cr0, %rax
    andq    $~(1 << 2), %rax       /* clear CR0.EM */
    orq     $(1 << 1), %rax        /* set   CR0.MP */
    movq    %rax, %cr0
    movq    %cr4, %rax
    orq     $((1 << 9) | (1 << 10)), %rax  /* CR4.OSFXSR | CR4.OSXMMEXCPT */
    movq    %rax, %cr4

    /*
     * Atomically claim the next hartid.  ap_boot_next_cpu starts at 1
     * (BSP is hart 0).  Each AP does  lock xaddl  to get a unique index.
     * This is safe even when multiple APs boot simultaneously.
     */
    movl    $1, %eax
    lock xaddl %eax, AP_ADDR(ap_boot_next_cpu)  /* eax = old value = my hartid */
    movl    %eax, %edi              /* hartid — first arg to start_kernel */

    /*
     * Two-stage mycpu initialisation (RISC-V style).
     * Set MSR_GS_BASE = &cpus[hartid] so that r_tp() / mycpu() return
     * a valid pointer before mycpu_init() runs in start_kernel().
     */
    movq    AP_ADDR(ap_boot_cpus_base), %rax
    movl    AP_ADDR(ap_boot_cpu_stride), %ecx
    imulq   %rdi, %rcx              /* rcx = hartid * sizeof(cpu_local) */
    addq    %rcx, %rax              /* rax = &cpus[hartid] */
    movq    %rax, %rdx
    shrq    $32, %rdx
    movl    $0xC0000101, %ecx       /* MSR_GS_BASE */
    wrmsr

    /* Load per-CPU kernel stack: sp = boot_stacks + (hartid+1) * 16384 - 4096
     * The -4096 reserves space at the top for the PCB (like RISC-V entry.S).
     * boot_stacks is KERNEL_STACK_SIZE-aligned so idle_thread_init() works. */
    movq    AP_ADDR(ap_boot_stacks_base), %rcx
    movq    %rdi, %rax
    addq    $1, %rax               /* hartid + 1 */
    shlq    $14, %rax              /* * 16384 (KERNEL_STACK_SIZE) */
    addq    %rcx, %rax
    subq    $4096, %rax            /* room for PCB at top */
    movq    %rax, %rsp

    /* Clear frame pointer for clean backtraces */
    xorq    %rbp, %rbp

    /* Call start_kernel(hartid, fdt_base=NULL, is_boot_hart=false) */
    xorl    %esi, %esi              /* fdt_base = NULL */
    xorl    %edx, %edx              /* is_boot_hart = false */
    movq    AP_ADDR(ap_boot_target), %rax
    call    *%rax

    /* Should never return */
1:  hlt
    jmp     1b

/* ════════════════════════════════════════════════════════════════
 *  Data area — patched by BSP before copying to AP_BOOT_PA
 * ════════════════════════════════════════════════════════════════ */
.balign 16
ap_boot_gdt:
    .quad   0x0000000000000000      /* 0x00: null */
    .quad   0x00cf9a000000ffff      /* 0x08: 32-bit code (D=1 L=0 P=1 E=1) */
    .quad   0x00cf92000000ffff      /* 0x10: 32/64-bit data (P=1 W=1) */
    .quad   0x00af9a000000ffff      /* 0x18: 64-bit code (L=1 D=0 P=1 E=1) */
ap_boot_gdt_desc:
    .word   (4 * 8) - 1            /* limit */
    .long   AP_ADDR(ap_boot_gdt)   /* base (physical address) */

.balign 8
.globl ap_boot_cr3
ap_boot_cr3:
    .quad   0                       /* BSP writes kpml4 physical address */

.balign 8
.globl ap_boot_cpus_base
ap_boot_cpus_base:
    .quad   0                       /* BSP writes &cpus[0] physical address */

.balign 4
.globl ap_boot_cpu_stride
ap_boot_cpu_stride:
    .long   0                       /* BSP writes sizeof(struct cpu_local) */

.balign 8
.globl ap_boot_stacks_base
ap_boot_stacks_base:
    .quad   0                       /* BSP writes &boot_stacks physical addr */

.balign 4
.globl ap_boot_next_cpu
ap_boot_next_cpu:
    .long   1                       /* atomic counter; BSP=0, APs claim 1,2,3... */

.balign 8
.globl ap_boot_target
ap_boot_target:
    .quad   0                       /* BSP writes address of start_kernel */

.globl ap_trampoline_end
ap_trampoline_end:

/* Back to 64-bit mode for the rest of the kernel */
.code64
