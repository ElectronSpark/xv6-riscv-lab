/*
 * crt_arch.h — x86_64 C runtime startup for xv6
 *
 * Included by both crt/crt1.c (START="_start") and ldso/dlstart.c
 * (START="_dlstart").  The START macro selects the entry-point name
 * and the matching *_c helper.
 *
 * Stack layout on entry (set by xv6 exec.c):
 *   rsp[0] = argc
 *   rsp[1..argc] = argv pointers
 *   rsp[argc+1] = NULL
 *   ... envp, auxv
 */

__asm__(
".section .text\n"
".global " START "\n"
".type " START ", @function\n"
START ":\n"
"   xor %ebp, %ebp\n"        /* mark end of stack frames */
"   mov %rsp, %rdi\n"        /* rdi = pointer to argc on stack */
".weak _DYNAMIC\n"
".hidden _DYNAMIC\n"
"   lea _DYNAMIC(%rip), %rsi\n"  /* rsi = _DYNAMIC (0 for static) */
"   andq $-16, %rsp\n"       /* align stack to 16 bytes */
"   call " START "_c\n"      /* _start_c or _dlstart_c */
);
