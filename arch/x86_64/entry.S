.section .text.entry
.code32
.global _entry

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

.section .text.entry
_entry:
    cli
    cld

    movb $'B', %al
    movw $0x00E9, %dx
    outb %al, %dx

    movl %ebx, boot_params_ptr
    /* BSP (hartid 0): stack top = boot_stacks + 1 * KERNEL_STACK_SIZE */
    movl $(boot_stacks + 16384), %esp

    /*
     * Clear BSS section so all globals start at zero on warm reboot.
     * Must happen BEFORE setup_page_tables because the boot page tables
     * (pml4_table, pdpt_table, etc.) live in BSS.
     * On a cold boot the ELF loader already zeroes BSS, but after a
     * keyboard-controller reset (reboot) QEMU preserves RAM contents.
     */
    movl $_bss, %edi
    movl $_bss_end, %ecx
    subl %edi, %ecx
    shrl $2, %ecx               /* count in 4-byte dwords */
    xorl %eax, %eax
    rep stosl

    /* Re-save boot_params_ptr (it's in BSS, just got zeroed) */
    movl %ebx, boot_params_ptr

    call setup_page_tables

    lgdt gdt64_desc

    mov %cr4, %eax
    orl $0x20, %eax
    mov %eax, %cr4

    movl $0xC0000080, %ecx
    rdmsr
    orl $0x00000100, %eax
    wrmsr

    movl $pml4_table, %eax
    mov %eax, %cr3

    mov %cr0, %eax
    orl $0x80000001, %eax
    mov %eax, %cr0

    ljmpl *long_mode_ptr

setup_page_tables:
    movl $pdpt_table, %eax
    orl $0x3, %eax
    movl %eax, pml4_table

    movl $pd_low_table, %eax
    orl $0x3, %eax
    movl %eax, pdpt_table

    movl $pd_kernel_table, %eax
    orl $0x3, %eax
    movl %eax, pdpt_table + 16

    xorl %ecx, %ecx
1:
    movl %ecx, %eax
    shll $21, %eax
    orl $0x83, %eax
    movl %eax, pd_low_table(,%ecx,8)
    incl %ecx
    cmpl $512, %ecx
    jne 1b

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

.code64
long_mode_entry:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss

    /* BSP (hartid 0): sp = boot_stacks + 1 * KERNEL_STACK_SIZE - 4096 (PCB) */
    movl $(boot_stacks + 16384 - 4096), %esp
    xorq %rbp, %rbp

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
    orq  $((1 << 9) | (1 << 10)), %rax  /* set CR4.OSFXSR | CR4.OSXMMEXCPT */
    movq %rax, %cr4

    /*
     * Two-stage mycpu initialisation (RISC-V style).
     * Set MSR_GS_BASE = &cpus[0] so that r_tp() / mycpu() return a
     * valid (zeroed) pointer from the very first instruction in C.
     * mycpu_init(0, false) will overwrite it with the real value.
     */
    movl    $0xC0000101, %ecx       /* MSR_GS_BASE */
    leaq    cpus(%rip), %rax
    movq    %rax, %rdx
    shrq    $32, %rdx
    wrmsr

    call serial_init64
    leaq banner_msg(%rip), %rsi
    call debug_puts64
    leaq banner_msg(%rip), %rsi
    call serial_puts64

    movl boot_params_ptr(%rip), %edi
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

.section .bss
.align 4096
pml4_table:
    .space 4096
pdpt_table:
    .space 4096
pd_low_table:
    .space 4096
pd_kernel_table:
    .space 4096

.align 8
boot_params_ptr:
    .long 0

/*
 * Per-CPU boot stacks for ALL harts (BSP + APs), RISC-V style.
 * Must be aligned to KERNEL_STACK_SIZE (16KB) so idle_thread_init()
 * can find the stack base by masking SP.
 * Layout: boot_stacks[0..KERNEL_STACK_SIZE-1]  = hart 0 (BSP)
 *         boot_stacks[KERNEL_STACK_SIZE..2*KSS-1] = hart 1
 *         ...
 */
.globl boot_stacks
.align 16384                    /* KERNEL_STACK_SIZE alignment         */
boot_stacks:
    .space 16384 * 8            /* KERNEL_STACK_SIZE * NCPU             */

.section .rodata
banner_msg:
    .asciz "[xv6 x86_64] entered long mode\n"