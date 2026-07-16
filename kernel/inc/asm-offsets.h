/* Structure-offset constants for the assembly sources (P3-5: STATIC file).
 *
 * Formerly generated at build time by scripts/gen_asm_offsets.py from the
 * C headers (kernel/inc/trapframe.h, kernel/inc/smp/percpu_types.h,
 * kernel/inc/proc/thread_types.h); every value below is byte-identical to
 * the last generated output (riscv64-unknown-elf-gcc, rv64gc/lp64d).
 *
 * The layouts are now owned by the native Rust types, and THEY enforce
 * this file: every value below that names a field of trapframe/
 * utrapframe/context/cpu_local/thread is pinned by a hardcoded
 * `const _: () = assert!(offset_of!(...) == N)` in the owning module
 * (kernel/irq/trap.rs, kernel/proc/sched.rs, kernel/ipi.rs,
 * kernel/proc/thread.rs), compiled for the riscv64gc target — any layout
 * drift fails the kernel build before an image can be linked.
 *
 * Consumers: kernel/irq/kernelvec.S + kernel/irq/trampoline.S
 * (TRAPFRAME_*, UTRAPFRAME_*, CPU_LOCAL_FLAGS, CPU_LOCAL_INTR_SP).
 * kernel/proc/swtch.S hardcodes the CONTEXT_* values as literal
 * displacements; no .S file consumes THREAD_* (P3-N9 finding). The
 * unconsumed defines are kept as pinned reference values.
 *
 * If you edit one of those structs, update the owning Rust type, its
 * asserts, and this file together.
 */

/* trapframe structure offsets */
#define TRAPFRAME_RA 0
#define TRAPFRAME_SP 8
#define TRAPFRAME_S0 16
#define TRAPFRAME_T0 24
#define TRAPFRAME_T1 32
#define TRAPFRAME_T2 40
#define TRAPFRAME_A0 48
#define TRAPFRAME_A1 56
#define TRAPFRAME_A2 64
#define TRAPFRAME_A3 72
#define TRAPFRAME_A4 80
#define TRAPFRAME_A5 88
#define TRAPFRAME_A6 96
#define TRAPFRAME_A7 104
#define TRAPFRAME_T3 112
#define TRAPFRAME_T4 120
#define TRAPFRAME_T5 128
#define TRAPFRAME_T6 136
#define TRAPFRAME_SEPC 144
#define TRAPFRAME_SSTATUS 152
#define TRAPFRAME_SCAUSE 160
#define TRAPFRAME_STVAL 168
#define TRAPFRAME_STVEC 176
#define TRAPFRAME_SIZE 184

/* utrapframe structure offsets */
#define UTRAPFRAME_TRAPFRAME 0
#define UTRAPFRAME_S1 184
#define UTRAPFRAME_S2 192
#define UTRAPFRAME_S3 200
#define UTRAPFRAME_S4 208
#define UTRAPFRAME_S5 216
#define UTRAPFRAME_S6 224
#define UTRAPFRAME_S7 232
#define UTRAPFRAME_S8 240
#define UTRAPFRAME_S9 248
#define UTRAPFRAME_S10 256
#define UTRAPFRAME_S11 264
#define UTRAPFRAME_IRQ_SP 272
#define UTRAPFRAME_IRQ_ENTRY 280
#define UTRAPFRAME_KERNEL_SATP 288
#define UTRAPFRAME_KERNEL_SP 296
#define UTRAPFRAME_KERNEL_TRAP 304
#define UTRAPFRAME_TP 312
#define UTRAPFRAME_KERNEL_HARTID 320
#define UTRAPFRAME_GP 328
#define UTRAPFRAME_KERNEL_GP 336
#define UTRAPFRAME_SIZE 344

/* context structure offsets */
#define CONTEXT_RA 0
#define CONTEXT_SP 8
#define CONTEXT_S0 16
#define CONTEXT_S1 24
#define CONTEXT_S2 32
#define CONTEXT_S3 40
#define CONTEXT_S4 48
#define CONTEXT_S5 56
#define CONTEXT_S6 64
#define CONTEXT_S7 72
#define CONTEXT_S8 80
#define CONTEXT_S9 88
#define CONTEXT_S10 96
#define CONTEXT_S11 104
#define CONTEXT_SIZE 128

/* cpu_local structure offsets */
#define CPU_LOCAL_PROC 0
#define CPU_LOCAL_IDLE_THREAD 8
#define CPU_LOCAL_INTR_STACKS 16
#define CPU_LOCAL_INTR_SP 24
#define CPU_LOCAL_INTR_DEPTH 32
#define CPU_LOCAL_NOFF 36
#define CPU_LOCAL_SPIN_DEPTH 40
#define CPU_LOCAL_INTENA 44
#define CPU_LOCAL_FLAGS 48
#define CPU_LOCAL_RCU_TIMESTAMP 56
#define CPU_LOCAL_SIZE 64

/* thread structure offsets */
#define THREAD_LOCK 0
#define THREAD_STATE 64
#define THREAD_FLAGS 72
#define THREAD_SCHED_ENTITY 80
#define THREAD_KSP 88
#define THREAD_CHAN 96
#define THREAD_SCHED_ENTRY 104
#define THREAD_WQ 120
#define THREAD_TRAPFRAME 128
#define THREAD_VM 136
#define THREAD_CLONE_FLAGS 144
#define THREAD_KENTRY 152
#define THREAD_ARG 160
#define THREAD_NAME 176
#define THREAD_KSTACK_ORDER 192
#define THREAD_KSTACK 200
#define THREAD_TRAPFRAME_VBASE 208
#define THREAD_FS 216
#define THREAD_RCU_READ_LOCK_NESTING 232
#define THREAD_SIGACTS 256
#define THREAD_PARENT 264
#define THREAD_VFORK_PARENT 272
#define THREAD_THREAD_GROUP 280
#define THREAD_PGROUP 288
#define THREAD_SESSION 296
#define THREAD_SID 304
#define THREAD_PGID 308
#define THREAD_TGID 312
#define THREAD_PID 316
#define THREAD_TG_ENTRY 320
#define THREAD_PG_ENTRY 336
#define THREAD_SID_ENTRY 352
#define THREAD_SIBLINGS 368
#define THREAD_CHILDREN 384
#define THREAD_CHILDREN_COUNT 400
#define THREAD_XSTATE 404
#define THREAD_WQ_ENTRY 408
#define THREAD_PROCTAB_ENTRY 448
#define THREAD_DMP_LIST_ENTRY 472
#define THREAD_SIGNAL 488
#define THREAD_RCU_HEAD 1072
#define THREAD_SIZE 1152
