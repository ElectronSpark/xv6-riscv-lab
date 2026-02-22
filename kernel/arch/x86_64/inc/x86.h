#ifndef _X86_H_
#define _X86_H_

#include "types.h"

#define PGSIZE 4096
#define PGSHIFT 12
#define MAXVA (1UL << 47)

#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) ((a) & ~(PGSIZE - 1))

#define PTE_V (1L << 0)
#define PTE_R (1L << 1)
#define PTE_W (1L << 2)
#define PTE_X (1L << 3)
#define PTE_U (1L << 4)
#define PTE_G (1L << 5)
#define PTE_A (1L << 6)
#define PTE_D (1L << 7)
#define PTE_RSW_w (1L << 8)

#define PA2PTE(pa) ((((uint64)(pa)) >> 12) << 10)
#define PTE2PA(pte) ((((uint64)(pte)) >> 10) << 12)
#define PTE_FLAGS(pte) (((pte) & 0x3FFUL) & (~(PTE_A | PTE_D)))

#define PAGETABLE_LEVELS 4  /* PML4 -> PDPT -> PD -> PT */

#define PXMASK 0x1FFUL
#define PXSHIFT(level) (PGSHIFT + (9 * (level)))
#define PX(level, va) ((((uint64)(va)) >> PXSHIFT(level)) & PXMASK)

extern uint64 __x86_tp;

static inline void intr_on(void)  { asm volatile("sti" ::: "memory"); }
static inline void intr_off(void) { asm volatile("cli" ::: "memory"); }

static inline int intr_get(void) {
	uint64 rflags;
	asm volatile("pushfq; popq %0" : "=r"(rflags));
	return (rflags >> 9) & 1;   /* IF flag = bit 9 */
}

static inline int intr_off_save(void) {
	int was = intr_get();
	intr_off();
	return was;
}

static inline void intr_restore(int enabled) {
	if (enabled)
		intr_on();
}

#define SIE_SSIE (1L << 1)
static inline uint64 r_sie(void) { return 0; }
static inline void w_sie(uint64 value) { (void)value; }

static inline void sfence_vma(void) {}

#define MAKE_SATP(pagetable) ((uint64)(pagetable))
static inline uint64 r_satp(void) { return 0; }
static inline void w_satp(uint64 value) { (void)value; }

static inline uint64 r_sp(void) {
	uint64 value;
	asm volatile("movq %%rsp, %0" : "=r"(value));
	return value;
}

static inline uint64 r_fp(void) {
	uint64 value;
	asm volatile("movq %%rbp, %0" : "=r"(value));
	return value;
}

static inline uint64 r_tp(void) { return __x86_tp; }
static inline void w_tp(uint64 value) { __x86_tp = value; }
static inline uint64 r_gp(void) { return 0; }
static inline void w_gp(uint64 value) { (void)value; }
static inline uint64 r_ra(void) { return 0; }

static inline uint64 r_time(void) {
	/* Monotonic time — TSC-based when available, else jiffies-based.
	 * Frequency = __timebase_frequency (set by arch_timer_init). */
	extern uint64 x86_r_time(void);
	return x86_r_time();
}

static inline void arch_wait_for_interrupt(void) {
	asm volatile("hlt" ::: "memory");
}

#endif // _X86_H_