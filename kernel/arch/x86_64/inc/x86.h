#ifndef _X86_H_
#define _X86_H_

#include "types.h"

#define PGSIZE 4096
#define PGSHIFT 12
#define MAXVA (~0ULL)

#define PGROUNDUP(sz) (((sz) + PGSIZE - 1) & ~(PGSIZE - 1))
#define PGROUNDDOWN(a) ((a) & ~(PGSIZE - 1))

/*
 * x86_64 page table entry (PTE) format.
 *
 * The shared code in kernel/mm/vm.c uses PTE_V, PTE_R, PTE_W, PTE_X, PTE_U,
 * PA2PTE, PTE2PA, and PTE_FLAGS.  We define them here to match the x86_64
 * hardware page table format:
 *
 *   Bit 0  = Present (P)
 *   Bit 1  = Read/Write (R/W) — 0=read-only, 1=writable
 *   Bit 2  = User/Supervisor (U/S) — 0=kernel-only, 1=user-accessible
 *   Bit 5  = Accessed (A)
 *   Bit 6  = Dirty (D)
 *   Bit 63 = No Execute (NX) — requires EFER.NXE
 *
 * Physical address is stored in bits [51:12] — no RISC-V-style shift.
 * On x86, all present pages are readable; there is no separate "R" bit.
 */
#define PTE_V (1ULL << 0)           /* Present */
#define PTE_R (1ULL << 0)           /* Readable = Present on x86 */
#define PTE_W (1ULL << 1)           /* Writable (R/W bit) */
#define PTE_X 0                     /* No explicit exec bit; use NX to deny */
#define PTE_U (1ULL << 2)           /* User accessible (U/S bit) */
#define PTE_G (1ULL << 8)           /* Global */
#define PTE_A (1ULL << 5)           /* Accessed */
#define PTE_D (1ULL << 6)           /* Dirty */
#define PTE_RSW_w (1ULL << 9)       /* Software: COW marker (AVL bit 9) */
#define PTE_NX (1ULL << 63)         /* No Execute */

#define PA2PTE(pa) ((uint64)(pa) & 0x000FFFFFFFFFF000ULL)
#define PTE2PA(pte) ((uint64)(pte) & 0x000FFFFFFFFFF000ULL)
#define PTE_FLAGS(pte) ((uint64)(pte) & ~0x000FFFFFFFFFF000ULL)

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

static inline void sfence_vma(void) {
	/* Full TLB flush: reload CR3 */
	uint64 cr3;
	asm volatile("movq %%cr3, %0" : "=r"(cr3));
	asm volatile("movq %0, %%cr3" : : "r"(cr3) : "memory");
}

#define MAKE_SATP(pagetable) ((uint64)(pagetable))
static inline uint64 r_satp(void) {
	uint64 val;
	asm volatile("movq %%cr3, %0" : "=r"(val));
	return val;
}
static inline void w_satp(uint64 value) {
	asm volatile("movq %0, %%cr3" : : "r"(value) : "memory");
}

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