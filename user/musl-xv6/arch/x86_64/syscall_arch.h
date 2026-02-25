/*
 * syscall_arch.h — xv6 x86_64 syscall interface for musl
 *
 * Linux x86-64 syscall calling convention:
 *   Syscall number : rax
 *   Arguments      : rdi, rsi, rdx, r10, r8, r9  (up to 6)
 *   Return value   : rax
 *   Preserved      : rdi, rsi, rdx, r10, r8, r9, rbx, rbp, r12–r15, rsp
 *   Clobbered      : rcx, r11  ("transaction registers")
 *
 * The SYSCALL instruction saves user RIP → rcx, user RFLAGS → r11.
 * Callers must save rcx/r11 before SYSCALL if they need them.
 * The 4th argument uses r10 (not rcx) because of this clobbering.
 */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

static __inline long __syscall0(long n)
{
	long ret;
	__asm__ __volatile__ ("syscall"
		: "=a"(ret)
		: "a"(n)
		: "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall1(long n, long a1)
{
	long ret;
	__asm__ __volatile__ ("syscall"
		: "=a"(ret)
		: "a"(n), "D"(a1)
		: "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall2(long n, long a1, long a2)
{
	long ret;
	__asm__ __volatile__ ("syscall"
		: "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2)
		: "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	long ret;
	__asm__ __volatile__ ("syscall"
		: "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3)
		: "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	__asm__ __volatile__ ("syscall"
		: "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10)
		: "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8  __asm__("r8")  = a5;
	__asm__ __volatile__ ("syscall"
		: "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8)
		: "rcx", "r11", "memory");
	return ret;
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	long ret;
	register long r10 __asm__("r10") = a4;
	register long r8  __asm__("r8")  = a5;
	register long r9  __asm__("r9")  = a6;
	__asm__ __volatile__ ("syscall"
		: "=a"(ret)
		: "a"(n), "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
		: "rcx", "r11", "memory");
	return ret;
}

#define SYS_clone  1
