.global vfork
.type vfork,@function
vfork:
	/* xv6 has a dedicated SYS_vfork (2) */
	li a7, 2 /* SYS_vfork */
	ecall
	.hidden __syscall_ret
	j __syscall_ret
