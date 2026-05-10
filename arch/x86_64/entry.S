.section .text.entry
.code32
.global _entry

.equ KERNEL_STACK_SIZE_ASM, 32768
.equ KERNEL_STACK_PCB_RESERVE_ASM, 4096

.section .multiboot
.align 4
    .long 0x1BADB002
    .long 0x00000003
    .long -(0x1BADB002 + 0x00000003)

.section .note.Xen,"a"
.align 4
    .long 4
    .long 4
    .long 18
    .asciz "Xen"
.align 4
    .long 0x00100000

/*
 * .text.entry has VMA == LMA == physical address.
 * All other sections (.text, .data, .bss, .rodata) have
 * VMA = PAGE_OFFSET + LMA (higher-half).
 *
 * Boot sequence:
 *   1) 32-bit code: setup page tables with identity + higher-half map,
 *      enable paging, enter long mode (still at physical VA)
 *   2) 64-bit trampoline: save boot params, enable SSE, jump to
 *      higher-half _higher_half_entry
 *   3) 64-bit higher-half: clear BSS, set up stack, call start()
 */
.section .text.entry
_entry:
    cli
    cld

    movb $'B', %al
    movw $0x00E9, %dx
    outb %al, %dx

    /* Save multiboot info pointer */
    movl %ebx, boot_params_ptr

    /* Early boot stack (in .text.entry, physical VMA) */
    movl $early_boot_stack_top, %esp

    call setup_page_tables

    lgdt gdt64_desc

    mov %cr4, %eax
    orl $0x20, %eax
    mov %eax, %cr4

    movl $0xC0000080, %ecx
    rdmsr
    orl $0x00000900, %eax       /* LME (bit 8) | NXE (bit 11) */
    wrmsr

    movl $pml4_table, %eax
    mov %eax, %cr3

    mov %cr0, %eax
    orl $0x80000001, %eax
    mov %eax, %cr0

    ljmpl *long_mode_ptr

/*
 * setup_page_tables: build boot page tables with both an identity map
 * (PML4[0]) and a higher-half map (PML4[256]) pointing to the same PDPT.
 *
 * PAGE_OFFSET = 0xFFFF800000000000 → PML4 index 256.
 *
 * Maps PA 0..1 GiB via pd_low_table and PA 2..3 GiB via pd_kernel_table,
 * using 2 MiB large pages.
 */
setup_page_tables:
    /* Zero all four page-table pages (warm-reboot safety) */
    movl $pml4_table, %edi
    movl $(4 * 4096 / 4), %ecx
    xorl %eax, %eax
    rep stosl

    /* PML4[0] → pdpt_table (identity map, low canonical) */
    movl $pdpt_table, %eax
    orl $0x3, %eax
    movl %eax, pml4_table

    /* PML4[256] → pdpt_table (higher-half at PAGE_OFFSET) */
    /* Index 256 → byte offset 256 * 8 = 2048 */
    movl %eax, pml4_table + 2048

    /* PDPT[0] → pd_low_table (covers PA 0..1 GiB) */
    movl $pd_low_table, %eax
    orl $0x3, %eax
    movl %eax, pdpt_table

    /* PDPT[2] → pd_kernel_table (covers PA 2..3 GiB) */
    movl $pd_kernel_table, %eax
    orl $0x3, %eax
    movl %eax, pdpt_table + 16

    /* Fill pd_low_table: 512 × 2 MiB pages, PA 0..1 GiB */
    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, pd_low_table(,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b

    /* Fill pd_kernel_table: 512 × 2 MiB pages, PA 2..3 GiB */
    xorl %ecx, %ecx
2:
    movl %ecx, %eax
    shll $21, %eax
    addl $0x80000000, %eax
    orl $0x83, %eax
    movl %eax, pd_kernel_table(,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne 2b

    ret

/* ================================================================== */
/* 64-bit trampoline (still at physical VMA in .text.entry).          */
/* Sets up SSE, then jumps to _higher_half_entry in .text.            */
/* ================================================================== */
.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* Save boot_params_ptr while we can still reach it (RIP-relative
       from this same .text.entry section). */
    movl boot_params_ptr(%rip), %r12d

    /*
     * Enable SSE (required for x86_64 ABI — musl/compiler may emit SSE).
     *   CR0: clear EM (bit 2), set MP (bit 1)
     *   CR4: set OSFXSR (bit 9), OSXMMEXCPT (bit 10)
     */
    movq %cr0, %rax
    andq $~(1 << 2), %rax       /* clear CR0.EM */
    orq  $(1 << 1), %rax        /* set   CR0.MP */
    movq %rax, %cr0

    movq %cr4, %rax
    orq  $((1 << 9) | (1 << 10)), %rax  /* CR4.OSFXSR | CR4.OSXMMEXCPT */
    movq %rax, %cr4

    /* Jump to higher-half entry.
     * _higher_half_entry is in .text (VMA = PAGE_OFFSET + LMA).
     * Use movabsq for the full 64-bit immediate address. */
    movabsq $_higher_half_entry, %rax
    jmpq *%rax

/* ---- Data and tables in .text.entry (physical VMA) ---- */
.code32
.align 8
gdt64:
    .quad 0x0000000000000000
    .quad 0x00af9a000000ffff
    .quad 0x00af92000000ffff

gdt64_desc:
    .word (3 * 8) - 1
    .long gdt64

long_mode_ptr:
    .long long_mode_entry
    .word 0x08

.align 4
boot_params_ptr:
    .long 0

/* Page tables (in .text.entry PROGBITS, zeroed explicitly by setup code) */
.align 4096
pml4_table:
    .space 4096
pdpt_table:
    .space 4096
pd_low_table:
    .space 4096
pd_kernel_table:
    .space 4096

/* Temporary boot stack (used by 32-bit code before higher-half switch) */
.align 16
early_boot_stack:
    .space 4096
early_boot_stack_top:


/* ================================================================== */
/* Higher-half entry (VMA = PAGE_OFFSET + LMA, paging active).        */
/* BSS clearing, real stack setup, and C entry happen here.           */
/* ================================================================== */
.section .text
.code64

.global _higher_half_entry
_higher_half_entry:
    /* ---- Clear BSS ---- */
    leaq _bss(%rip), %rdi
    leaq _bss_end(%rip), %rcx
    subq %rdi, %rcx
    shrq $3, %rcx              /* count in qwords */
    xorq %rax, %rax
    rep stosq

    /* ---- Set up bootstrap stack ---- */
    /* BSP switches to an allocator-backed idle stack before idle_thread_init(). */
    leaq boot_stack0(%rip), %rsp
    addq $(KERNEL_STACK_SIZE_ASM - KERNEL_STACK_PCB_RESERVE_ASM), %rsp
    xorq %rbp, %rbp

    /*
     * Two-stage mycpu initialisation (RISC-V style).
     * Set MSR_GS_BASE = &cpus[0] so that r_tp() / mycpu() return a
     * valid (zeroed) pointer from the very first instruction in C.
     * mycpu_init(0, false) will overwrite it with the real value.
     */
    movl    $0xC0000101, %ecx       /* MSR_GS_BASE */
    movabsq $cpus, %rax
    movq    %rax, %rdx
    shrq    $32, %rdx
    wrmsr

    call serial_init64
    leaq banner_msg(%rip), %rsi
    call debug_puts64
    leaq banner_msg(%rip), %rsi
    call serial_puts64

    /* Call start(boot_params_ptr) — r12d was saved in long_mode_entry */
    movl %r12d, %edi
    call start

3:
    hlt
    jmp 3b

serial_init64:
    movb $0x00, %al
    movw $0x3F9, %dx
    outb %al, %dx

    movb $0x80, %al
    movw $0x3FB, %dx
    outb %al, %dx

    movb $0x01, %al
    movw $0x3F8, %dx
    outb %al, %dx

    movb $0x00, %al
    movw $0x3F9, %dx
    outb %al, %dx

    movb $0x03, %al
    movw $0x3FB, %dx
    outb %al, %dx

    movb $0xC7, %al
    movw $0x3FA, %dx
    outb %al, %dx

    movb $0x0B, %al
    movw $0x3FC, %dx
    outb %al, %dx

    ret

serial_putc64:
    movw $0x3F8, %dx
    movb %dil, %al
    outb %al, %dx
    ret

serial_puts64:
2:
    movzbq (%rsi), %rax
    testb %al, %al
    jz 4f
    cmpb $0x0A, %al
    jne 5f
    movb $0x0D, %dil
    call serial_putc64
5:
    movb %al, %dil
    call serial_putc64
    incq %rsi
    jmp 2b
4:
    ret

debug_puts64:
6:
    movzbq (%rsi), %rax
    testb %al, %al
    jz 7f
    movw $0x00E9, %dx
    outb %al, %dx
    incq %rsi
    jmp 6b
7:
    ret

.global arch_relocate_stack_finish
arch_relocate_stack_finish:
    /*
     * Finish an active-stack migration after C has copied frames and adjusted
     * saved frame pointers.  RDI is the signed address delta to add to RSP/RBP.
     */
    popq %rax
    addq %rdi, %rsp
    addq %rdi, %rbp
    pushq %rax
    retq

/*
 * BSP-only bootstrap stack. Runtime idle stacks are allocated after the buddy
 * allocator is online and are sized by the discovered CPU topology.
 */
.section .bss
.globl boot_stack0
.align KERNEL_STACK_SIZE_ASM     /* KERNEL_STACK_SIZE alignment         */
boot_stack0:
    .space KERNEL_STACK_SIZE_ASM /* KERNEL_STACK_SIZE */

.section .rodata
banner_msg:
    .asciz "[xv6 x86_64] entered long mode\n"
