/*
 * syscall_arch.h — xv6 RISC-V syscall interface for musl
 *
 * xv6 uses: ecall, syscall number in a7, args in a0-a5, return in a0.
 * Negative return values are negated errno codes (like Linux).
 *
 * NOTE: xv6 syscall numbers are NOT Linux numbers.
 * The mapping is done via bits/syscall.h.in.
 */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

static __inline long __syscall0(long n)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0");
	__asm__ __volatile__ ("ecall"
		: "=r"(a0)
		: "r"(a7)
		: "memory");
	return a0;
}

static __inline long __syscall1(long n, long a)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	__asm__ __volatile__ ("ecall"
		: "+r"(a0)
		: "r"(a7)
		: "memory");
	return a0;
}

static __inline long __syscall2(long n, long a, long b)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	__asm__ __volatile__ ("ecall"
		: "+r"(a0)
		: "r"(a7), "r"(a1)
		: "memory");
	return a0;
}

static __inline long __syscall3(long n, long a, long b, long c)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	__asm__ __volatile__ ("ecall"
		: "+r"(a0)
		: "r"(a7), "r"(a1), "r"(a2)
		: "memory");
	return a0;
}

static __inline long __syscall4(long n, long a, long b, long c, long d)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	__asm__ __volatile__ ("ecall"
		: "+r"(a0)
		: "r"(a7), "r"(a1), "r"(a2), "r"(a3)
		: "memory");
	return a0;
}

static __inline long __syscall5(long n, long a, long b, long c, long d, long e)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	__asm__ __volatile__ ("ecall"
		: "+r"(a0)
		: "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4)
		: "memory");
	return a0;
}

static __inline long __syscall6(long n, long a, long b, long c, long d, long e, long f)
{
	register long a7 __asm__("a7") = n;
	register long a0 __asm__("a0") = a;
	register long a1 __asm__("a1") = b;
	register long a2 __asm__("a2") = c;
	register long a3 __asm__("a3") = d;
	register long a4 __asm__("a4") = e;
	register long a5 __asm__("a5") = f;
	__asm__ __volatile__ ("ecall"
		: "+r"(a0)
		: "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
		: "memory");
	return a0;
}

/* Tell musl that clone() passes args via struct pointer, not registers.
 * xv6 clone() takes a single pointer to struct clone_args in a0.
 * We override SYS_clone handling in __clone() wrapper instead. */
#define SYS_clone  1
