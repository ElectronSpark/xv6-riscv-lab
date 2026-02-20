/*
 * crt_arch.h — RISC-V C runtime startup for xv6
 *
 * This is the very first code executed in a dynamically-linked or
 * statically-linked musl binary. It sets up the stack pointer value
 * (already set by the kernel) and jumps to __libc_start_main or
 * __dls2 (for the dynamic linker).
 *
 * Stack layout on entry (set by xv6 exec.c):
 *   sp[0] = argc
 *   sp[1..argc] = argv pointers
 *   sp[argc+1] = NULL
 *   sp[argc+2..+1+envc] = envp pointers
 *   sp[argc+2+envc] = NULL
 *   sp[...] = auxv pairs
 *   sp[...] = AT_NULL, 0
 */

__asm__(
".section .text\n"
".global _start\n"
".type _start, %function\n"
"_start:\n"
"   .option push\n"
"   .option norelax\n"
"   lla gp, __global_pointer$\n"
"   .option pop\n"
"   mv a0, sp\n"           /* a0 = pointer to argc on stack */
".weak _DYNAMIC\n"
".hidden _DYNAMIC\n"
"   lla a1, _DYNAMIC\n"    /* a1 = _DYNAMIC (0 for static) */
"   andi sp, sp, -16\n"    /* align stack */
"   tail _start_c\n"       /* jump to C startup */
);
