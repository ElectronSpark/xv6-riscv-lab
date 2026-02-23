/*
 * crt_arch.h — x86_64 C runtime startup for xv6
 *
 * Stack layout on entry (set by xv6 exec.c):
 *   rsp[0] = argc
 *   rsp[1..argc] = argv pointers
 *   rsp[argc+1] = NULL
 *   ... envp, auxv
 */

__asm__(
".section .text\n"
".global _start\n"
".type _start, @function\n"
"_start:\n"
"   xor %ebp, %ebp\n"        /* mark end of stack frames */
"   mov %rsp, %rdi\n"        /* rdi = pointer to argc on stack */
".weak _DYNAMIC\n"
".hidden _DYNAMIC\n"
"   lea _DYNAMIC(%rip), %rsi\n"  /* rsi = _DYNAMIC (0 for static) */
"   andq $-16, %rsp\n"       /* align stack to 16 bytes */
"   call _start_c\n"         /* jump to musl C startup */
);
