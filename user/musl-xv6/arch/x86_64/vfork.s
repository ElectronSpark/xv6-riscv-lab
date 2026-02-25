/*
 * vfork — xv6 x86_64 vfork wrapper for musl
 *
 * Linux x86-64 syscall convention:
 *   Syscall number in rax, return in rax.
 *   rcx and r11 are clobbered by SYSCALL (transaction registers).
 *   They are caller-saved in the C ABI, so no save/restore needed.
 *
 * vfork shares the parent's address space, so we must NOT modify
 * the stack (push/pop) before syscall — the child could corrupt it.
 */
.global vfork
.type vfork,@function
vfork:
	movq $2, %rax    /* SYS_vfork */
	syscall
	/* syscall result is in RAX; __syscall_ret expects it in RDI */
	movq %rax, %rdi
	.hidden __syscall_ret
	jmp __syscall_ret
