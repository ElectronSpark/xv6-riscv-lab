#include "types.h"

extern int fpu_xsave_enabled;
extern int fpu_avx_enabled;
extern uint32 fpu_xsave_mask_lo;
extern uint32 fpu_xsave_mask_hi;

#define CR0_MP       (1ULL << 1)
#define CR0_EM       (1ULL << 2)
#define CR4_OSFXSR   (1ULL << 9)
#define CR4_OSXMMEXCPT (1ULL << 10)
#define CR4_OSXSAVE  (1ULL << 18)

#define CPUID1_ECX_XSAVE (1U << 26)
#define CPUID1_ECX_AVX   (1U << 28)

static inline void cpuid_count(uint32 leaf, uint32 subleaf,
                               uint32 *a, uint32 *b, uint32 *c, uint32 *d)
{
    asm volatile("cpuid"
                 : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                 : "a"(leaf), "c"(subleaf));
}

static inline uint64 xgetbv(uint32 index)
{
    uint32 lo, hi;
    asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(index));
    return ((uint64)hi << 32) | lo;
}

static inline void xsetbv(uint32 index, uint64 value)
{
    uint32 lo = (uint32)value;
    uint32 hi = (uint32)(value >> 32);
    asm volatile("xsetbv" : : "c"(index), "a"(lo), "d"(hi) : "memory");
}

void fpu_cpu_init(void)
{
    uint64 cr0, cr4;
    uint32 a, b, c, d;
    uint32 max_leaf;

    asm volatile("movq %%cr0, %0" : "=r"(cr0));
    cr0 &= ~CR0_EM;
    cr0 |= CR0_MP;
    asm volatile("movq %0, %%cr0" : : "r"(cr0) : "memory");

    asm volatile("movq %%cr4, %0" : "=r"(cr4));
    cr4 |= CR4_OSFXSR | CR4_OSXMMEXCPT;
    asm volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

    cpuid_count(0, 0, &max_leaf, &b, &c, &d);
    if (max_leaf < 1)
        return;

    cpuid_count(1, 0, &a, &b, &c, &d);
    uint32 ecx1 = c;
    if ((ecx1 & CPUID1_ECX_XSAVE) == 0)
        return;

    cr4 |= CR4_OSXSAVE;
    asm volatile("movq %0, %%cr4" : : "r"(cr4) : "memory");

    uint64 supported = 0x3;
    if (max_leaf >= 0x0d) {
        cpuid_count(0x0d, 0, &a, &b, &c, &d);
        supported = ((uint64)d << 32) | a;
    }

    uint64 requested = 0x3; /* x87 + SSE */
    if ((ecx1 & CPUID1_ECX_AVX) && ((supported & 0x7) == 0x7))
        requested |= 0x4;   /* AVX YMM upper halves */

    if ((supported & requested) != requested)
        requested = 0x3;

    xsetbv(0, requested);
    uint64 xcr0 = xgetbv(0);
    if ((xcr0 & 0x3) != 0x3)
        return;

    fpu_xsave_mask_lo = (uint32)xcr0;
    fpu_xsave_mask_hi = (uint32)(xcr0 >> 32);
    fpu_avx_enabled = (xcr0 & 0x4) != 0;
    fpu_xsave_enabled = 1;
}
