/*
 * gdbstub_arch.h — Architecture abstraction layer for the in-kernel GDB stub.
 *
 * Each architecture implements these functions in its own gdbstub_arch.c.
 * The generic gdbstub.c calls them to handle register access, breakpoint
 * encoding, and single-step mechanisms without #ifdef clutter.
 */
#ifndef __GDBSTUB_ARCH_H
#define __GDBSTUB_ARCH_H

#include "types.h"

struct utrapframe;

/* ──────────────────────────────────────────────────────────────────────────── */
/* Register access                                                             */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Number of GP registers GDB expects for this architecture.
 *   RISC-V:  33  (x0..x31 + pc)
 *   x86_64:  24  (rax..r15 + rip + rflags + cs + ss + ds + es + fs + gs)
 */
extern const int gdb_arch_num_regs;

/*
 * Size in bytes of the register with the given GDB register number.
 * Used to encode/decode variable-width registers in g/G/p/P packets.
 *   RISC-V:  always 8 (all registers are 64-bit)
 *   x86_64:  8 for GP/rip, 4 for eflags and segment registers
 */
int    gdb_arch_reg_size(int regnum);

/* Read a register by its GDB register number. */
uint64 gdb_arch_get_reg(struct utrapframe *tf, int regnum);

/* Write a register by its GDB register number. */
void   gdb_arch_set_reg(struct utrapframe *tf, int regnum, uint64 val);

/* ──────────────────────────────────────────────────────────────────────────── */
/* PC access                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Get the program counter from a trapframe. */
uint64 gdb_arch_get_pc(struct utrapframe *tf);

/* Set the program counter in a trapframe. */
void   gdb_arch_set_pc(struct utrapframe *tf, uint64 pc);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Software breakpoint encoding                                                */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Determine the breakpoint instruction length for a given address.
 * On RISC-V, reads the instruction at 'addr' to decide between 2-byte
 * (compressed ebreak) and 4-byte (ebreak).
 * On x86_64, always returns 1 (INT3 = 0xCC).
 *
 * 'insn_bytes' / 'nbytes' provide the first few bytes at 'addr' so the
 * arch code can inspect the instruction without doing its own I/O.
 * Pass NULL/0 if unavailable; the arch code will use a default length.
 */
int    gdb_arch_brk_len(uint64 addr, const void *insn_bytes, int nbytes);

/*
 * Fill 'buf' with the breakpoint instruction encoding.
 * 'len' is the desired breakpoint length (from gdb_arch_brk_len).
 * Returns the number of bytes written to 'buf'.
 */
int    gdb_arch_brk_encode(void *buf, int len);

/*
 * Determine how many bytes to advance the PC to skip past a user-mode
 * breakpoint instruction (e.g. ebreak / int3) that is NOT one of our
 * software breakpoints (i.e. a user-placed ebreak / waitgdb).
 *
 * 'insn_bytes' / 'nbytes' provide the first few bytes at 'pc'.
 */
int    gdb_arch_user_brk_len(uint64 pc, const void *insn_bytes, int nbytes);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Single-step support                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Returns 1 if the architecture supports hardware single-step
 * (e.g. x86_64 RFLAGS.TF).
 * Returns 0 if software breakpoint-based stepping is needed (RISC-V).
 */
int    gdb_arch_has_hw_step(void);

/*
 * Enable or disable hardware single-step on the given trapframe.
 * Only called when gdb_arch_has_hw_step() returns 1.
 *   enable=1 → set TF (x86) so next instruction triggers #DB
 *   enable=0 → clear TF
 */
void   gdb_arch_set_hw_step(struct utrapframe *tf, int enable);

/*
 * Decode the instruction at 'pc' and return the next PC.
 * Only called when gdb_arch_has_hw_step() returns 0 (software step).
 *
 * For conditional branches, returns the branch-taken target;
 * the caller will also place a breakpoint at pc + insn_len for fall-through.
 *
 * Sets *insn_len_out to the instruction length at 'pc' (2 or 4).
 * Sets *is_branch_out to 1 if the instruction is a conditional branch.
 *
 * 'read_mem' is a callback to read target memory (for instruction fetch).
 * Returns 0 on decode failure — caller falls back to pc + 4.
 */
typedef int (*gdb_read_mem_fn)(void *dst, uint64 addr, int len);

uint64 gdb_arch_decode_next_pc(struct utrapframe *tf, uint64 pc,
                                int *insn_len_out, int *is_branch_out,
                                gdb_read_mem_fn read_mem);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Byte-level register access (supports FP registers > 8 bytes)                */
/* ──────────────────────────────────────────────────────────────────────────── */

struct fpu_state;

/*
 * Read register 'regnum' into raw byte buffer 'buf'.
 * For GP registers, reads from the trapframe.
 * For FP registers, reads from the fpu_state buffer (NULL → zeroes).
 * Returns the number of bytes written to buf (== gdb_arch_reg_size(regnum)).
 */
int gdb_arch_read_reg_bytes(struct utrapframe *tf, struct fpu_state *fps,
                            int regnum, void *buf);

/*
 * Write register 'regnum' from raw byte buffer 'buf'.
 * For GP registers, writes to the trapframe.
 * For FP registers, writes to the fpu_state buffer.
 * Returns the number of bytes consumed from buf (== gdb_arch_reg_size(regnum)).
 */
int gdb_arch_write_reg_bytes(struct utrapframe *tf, struct fpu_state *fps,
                             int regnum, const void *buf);

/* ──────────────────────────────────────────────────────────────────────────── */
/* Target description                                                          */
/* ──────────────────────────────────────────────────────────────────────────── */

/*
 * Return a static XML target description string for qXfer:features:read.
 * This tells GDB exactly which registers we provide (avoiding the default
 * assumption of FPU/SSE registers on x86_64, for example).
 *
 * Sets *len_out to the string length (excluding NUL).
 */
const char *gdb_arch_target_xml(int *len_out);

#endif /* __GDBSTUB_ARCH_H */
