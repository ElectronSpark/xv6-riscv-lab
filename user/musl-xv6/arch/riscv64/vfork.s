.global vfork
.type vfork,@function
vfork:
	/* xv6 has a dedicated SYS_vfork (2) that sets up
	   CLONE_VM | CLONE_VFORK | SIGCHLD internally */
	li a7, 2   /* SYS_vfork */
	ecall
	.hidden __syscall_ret
	j __syscall_ret
