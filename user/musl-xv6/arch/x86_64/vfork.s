.global vfork
.type vfork,@function
vfork:
	/* xv6 SYS_vfork (2) - kernel sets up CLONE_VM | CLONE_VFORK | SIGCHLD */
	movq $2, %rax    /* SYS_vfork */
	syscall
	.hidden __syscall_ret
	jmp __syscall_ret
