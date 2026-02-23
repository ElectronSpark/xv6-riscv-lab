.global vfork
.type vfork,@function
vfork:
	/* xv6 SYS_vfork (2) - kernel sets up CLONE_VFORK | SIGCHLD */
	movq $2, %rax    /* SYS_vfork */
	syscall
	/* syscall result is in RAX; __syscall_ret expects it in RDI */
	movq %rax, %rdi
	.hidden __syscall_ret
	jmp __syscall_ret
