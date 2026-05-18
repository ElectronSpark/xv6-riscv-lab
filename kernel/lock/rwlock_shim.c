/**
 * @file rwlock_shim.c
 * @brief Thin extern-C wrappers around inline rwlock primitives.
 *
 * The Rust port of rwlock.c (kernel/mm/src/lock/rwlock.rs) calls these
 * non-inline trampolines instead of the inline CAS primitives in rwlock.h.
 * This keeps the inline header (with its `_Atomic` fields and CAS macros)
 * fully owned by the C side while letting the Rust spin loops drive the
 * higher-level policy.
 */

#include "types.h"
#include "lock/rwlock.h"
#include "timer/timer.h"
#include "riscv.h"
#include "smp/percpu.h"

bool __rwl_try_rlock(struct rwlock *rw)            { return rwlock_try_rlock(rw); }
bool __rwl_try_wlock(struct rwlock *rw, bool exp)  { return rwlock_try_wlock(rw, exp); }

uint64 __rwl_state(struct rwlock *rw)              { return RWLOCK_STATE(rw); }
bool   __rwl_w_holding(struct rwlock *rw)          { return (bool)RWLOCK_W_HOLDING(rw); }
uint64 __rwl_state_r_count(uint64 s)               { return RWLOCK_STATE_R_COUNT(s); }

void __rwl_atomic_sub_reader(struct rwlock *rw)    { atomic_sub(&rw->state, RWLOCK_STATE_READER_BIAS); }
void __rwl_store_unlocked(struct rwlock *rw)       { smp_store_release(&rw->state, RWLOCK_STATE_UNLOCKED); }
void __rwl_store_holder_none(struct rwlock *rw)    { smp_store_release(&rw->w_holder, RWLOCK_NONE_HOLDER); }

void   __rwl_cpu_relax(void)                       { cpu_relax(); }
uint64 __rwl_r_time(void)                          { return r_time(); }
uint64 __rwl_expedite_threshold(void)              { return RWLOCK_EXPEDITE_THRESHOLD; }

int  __rwl_intr_off_save(void)                     { return intr_off_save(); }
void __rwl_intr_restore(int s)                     { intr_restore(s); }

void __rwl_push_off(void)                          { push_off(); }
void __rwl_pop_off(void)                           { pop_off(); }
