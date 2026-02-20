/*
 * atomic_arch.h — RISC-V atomic operations for musl
 *
 * RISC-V has hardware atomic instructions (A extension).
 * These match musl's generic riscv64 arch.
 */

#define a_barrier a_barrier
static inline void a_barrier()
{
    __asm__ __volatile__ ("fence rw,rw" ::: "memory");
}

#define a_cas a_cas
static inline int a_cas(volatile int *p, int t, int s)
{
    int old, tmp;
    __asm__ __volatile__ (
        "1: lr.w.aqrl %0, (%2)\n"
        "   bne %0, %3, 2f\n"
        "   sc.w.aqrl %1, %4, (%2)\n"
        "   bnez %1, 1b\n"
        "2:"
        : "=&r"(old), "=&r"(tmp)
        : "r"(p), "r"(t), "r"(s)
        : "memory");
    return old;
}

#define a_cas_p a_cas_p
static inline void *a_cas_p(volatile void *p, void *t, void *s)
{
    long old;
    int tmp;
    __asm__ __volatile__ (
        "1: lr.d.aqrl %0, (%2)\n"
        "   bne %0, %3, 2f\n"
        "   sc.d.aqrl %1, %4, (%2)\n"
        "   bnez %1, 1b\n"
        "2:"
        : "=&r"(old), "=&r"(tmp)
        : "r"(p), "r"((long)t), "r"((long)s)
        : "memory");
    return (void *)old;
}

#define a_swap a_swap
static inline int a_swap(volatile int *p, int v)
{
    int old;
    __asm__ __volatile__ (
        "amoswap.w.aqrl %0, %1, (%2)"
        : "=r"(old) : "r"(v), "r"(p) : "memory");
    return old;
}

#define a_fetch_add a_fetch_add
static inline int a_fetch_add(volatile int *p, int v)
{
    int old;
    __asm__ __volatile__ (
        "amoadd.w.aqrl %0, %1, (%2)"
        : "=r"(old) : "r"(v), "r"(p) : "memory");
    return old;
}

#define a_and a_and
static inline void a_and(volatile int *p, int v)
{
    __asm__ __volatile__ (
        "amoand.w.aqrl zero, %0, (%1)"
        :: "r"(v), "r"(p) : "memory");
}

#define a_or a_or
static inline void a_or(volatile int *p, int v)
{
    __asm__ __volatile__ (
        "amoor.w.aqrl zero, %0, (%1)"
        :: "r"(v), "r"(p) : "memory");
}

#define a_and_64 a_and_64
static inline void a_and_64(volatile uint64_t *p, uint64_t v)
{
    __asm__ __volatile__ (
        "amoand.d.aqrl zero, %0, (%1)"
        :: "r"(v), "r"(p) : "memory");
}

#define a_or_64 a_or_64
static inline void a_or_64(volatile uint64_t *p, uint64_t v)
{
    __asm__ __volatile__ (
        "amoor.d.aqrl zero, %0, (%1)"
        :: "r"(v), "r"(p) : "memory");
}

#define a_ctz_64 a_ctz_64
static inline int a_ctz_64(uint64_t x)
{
    /* No ctz instruction in base RISC-V; use de Bruijn */
    static const char debruijn64[64] = {
        0, 1, 2, 53, 3, 7, 54, 27, 4, 38, 41, 8, 34, 55, 48, 28,
        62, 5, 39, 46, 44, 42, 22, 9, 24, 35, 59, 56, 49, 18, 29, 11,
        63, 52, 6, 26, 37, 40, 33, 47, 61, 45, 43, 21, 23, 58, 17, 10,
        51, 25, 36, 32, 60, 20, 57, 16, 50, 31, 19, 15, 30, 14, 13, 12
    };
    return debruijn64[(x & -x) * 0x022fdd63cc95386dull >> 58];
}

#define a_clz_64 a_clz_64
static inline int a_clz_64(uint64_t x)
{
    x |= x >> 1;
    x |= x >> 2;
    x |= x >> 4;
    x |= x >> 8;
    x |= x >> 16;
    x |= x >> 32;
    x++;
    return 63 - a_ctz_64(x);
}

#define a_spin a_spin
static inline void a_spin()
{
    /* RISC-V pause hint (Zihintpause extension, NOP on older cores) */
    __asm__ __volatile__ (".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory");
}
