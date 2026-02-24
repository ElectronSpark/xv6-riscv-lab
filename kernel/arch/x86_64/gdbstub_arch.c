/*
 * gdbstub_arch.c — x86_64 architecture support for the in-kernel GDB stub.
 *
 * Provides register access (GDB x86-64 register order), INT3 breakpoint
 * encoding, and hardware single-step via RFLAGS.TF.
 *
 * GDB x86-64 register numbering (from gdb/features/i386/64bit-core.xml):
 *   0  rax    4  rsi    8   r8    12 r12   16 rip    20 ds
 *   1  rbx    5  rdi    9   r9    13 r13   17 eflags 21 es
 *   2  rcx    6  rbp    10  r10   14 r14   18 cs     22 fs
 *   3  rdx    7  rsp    11  r11   15 r15   19 ss     23 gs
 *
 *   24 st0    25 st1    26 st2    27 st3   (80-bit FPU data registers)
 *   28 st4    29 st5    30 st6    31 st7
 *
 *   32 fctrl  33 fstat  34 ftag   35 fiseg (32-bit FPU control registers)
 *   36 fioff  37 foseg  38 fooff  39 fop
 *
 * We report 40 registers.  The FPU registers (24-39) are always zero
 * because xv6 does not save/restore FPU state.  GDB's org.gnu.gdb.i386.core
 * feature REQUIRES these registers — without them GDB rejects the target
 * description with "Architecture rejected target-supplied description".
 */

#include "types.h"
#include "trapframe.h"
#include "gdbstub_arch.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Constants                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

/* INT3 opcode */
#define X86_INT3    0xCCU

/* RFLAGS Trap Flag bit */
#define RFLAGS_TF   (1ULL << 8)

/* GDB expects 40 registers for x86-64 (GP + segments + FPU) */
const int gdb_arch_num_regs = 40;

/* ──────────────────────────────────────────────────────────────────────────── */
/* Register sizes                                                              */
/*                                                                             */
/* Registers 0-16 (rax..r15, rip) are 64-bit = 8 bytes.                        */
/* Register 17 (eflags) and 18-23 (segment regs) are 32-bit = 4 bytes.         */
/* Registers 24-31 (st0-st7) are 80-bit = 10 bytes.                            */
/* Registers 32-39 (fctrl..fop) are 32-bit = 4 bytes.                          */
/* ──────────────────────────────────────────────────────────────────────────── */

int gdb_arch_reg_size(int regnum)
{
    if (regnum <= 16) return 8;   /* rax..r15, rip */
    if (regnum <= 23) return 4;   /* eflags, cs, ss, ds, es, fs, gs */
    if (regnum <= 31) return 10;  /* st0-st7 (80-bit x87 FPU) */
    return 4;                     /* fctrl, fstat, ftag, fiseg, fioff, foseg, fooff, fop */
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Register access                                                             */
/* ──────────────────────────────────────────────────────────────────────────── */

uint64 gdb_arch_get_reg(struct utrapframe *tf, int regnum)
{
    struct trapframe *t = &tf->trapframe;
    switch (regnum) {
    case 0:  return t->rax;
    case 1:  return t->rbx;
    case 2:  return t->rcx;
    case 3:  return t->rdx;
    case 4:  return t->rsi;
    case 5:  return t->rdi;
    case 6:  return t->rbp;
    case 7:  return t->rsp;
    case 8:  return t->r8;
    case 9:  return t->r9;
    case 10: return t->r10;
    case 11: return t->r11;
    case 12: return t->r12;
    case 13: return t->r13;
    case 14: return t->r14;
    case 15: return t->r15;
    case 16: return t->rip;            /* pc */
    case 17: return t->rflags;         /* eflags */
    case 18: return t->cs;
    case 19: return t->ss;
    case 20: return 0;                 /* ds — not saved */
    case 21: return 0;                 /* es — not saved */
    case 22: return 0;                 /* fs — not saved */
    case 23: return 0;                 /* gs — not saved */
    /* FPU registers 24-39: xv6 doesn't save FPU state, return 0 */
    default: return 0;
    }
}

void gdb_arch_set_reg(struct utrapframe *tf, int regnum, uint64 val)
{
    struct trapframe *t = &tf->trapframe;
    switch (regnum) {
    case 0:  t->rax    = val; break;
    case 1:  t->rbx    = val; break;
    case 2:  t->rcx    = val; break;
    case 3:  t->rdx    = val; break;
    case 4:  t->rsi    = val; break;
    case 5:  t->rdi    = val; break;
    case 6:  t->rbp    = val; break;
    case 7:  t->rsp    = val; break;
    case 8:  t->r8     = val; break;
    case 9:  t->r9     = val; break;
    case 10: t->r10    = val; break;
    case 11: t->r11    = val; break;
    case 12: t->r12    = val; break;
    case 13: t->r13    = val; break;
    case 14: t->r14    = val; break;
    case 15: t->r15    = val; break;
    case 16: t->rip    = val; break;   /* pc */
    case 17: t->rflags = val; break;   /* eflags */
    case 18: t->cs     = val; break;
    case 19: t->ss     = val; break;
    /* ds, es, fs, gs (20-23): silently ignore writes */
    /* FPU registers (24-39): silently ignore writes — xv6 has no FPU state */
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* PC access                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

uint64 gdb_arch_get_pc(struct utrapframe *tf)
{
    return tf->trapframe.rip;
}

void gdb_arch_set_pc(struct utrapframe *tf, uint64 pc)
{
    tf->trapframe.rip = pc;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Breakpoint encoding                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

int gdb_arch_brk_len(uint64 addr, const void *insn_bytes, int nbytes)
{
    (void)addr; (void)insn_bytes; (void)nbytes;
    return 1;  /* INT3 is always 1 byte */
}

int gdb_arch_brk_encode(void *buf, int len)
{
    (void)len;
    *(uint8 *)buf = X86_INT3;
    return 1;
}

int gdb_arch_user_brk_len(uint64 pc, const void *insn_bytes, int nbytes)
{
    (void)pc; (void)insn_bytes; (void)nbytes;
    return 1;  /* INT3 is always 1 byte */
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Single-step support                                                         */
/*                                                                             */
/* x86_64 has hardware single-step via the RFLAGS Trap Flag (TF, bit 8).       */
/* When TF is set, the CPU generates a #DB (vector 1) after executing one      */
/* instruction.  The CPU automatically clears TF when delivering the #DB.      */
/* ──────────────────────────────────────────────────────────────────────────── */

int gdb_arch_has_hw_step(void)
{
    return 1;
}

void gdb_arch_set_hw_step(struct utrapframe *tf, int enable)
{
    if (enable)
        tf->trapframe.rflags |= RFLAGS_TF;
    else
        tf->trapframe.rflags &= ~RFLAGS_TF;
}

/*
 * Not used on x86_64 (hardware single-step is available), but provide
 * a stub so the linker is happy if it's ever called.
 */
uint64 gdb_arch_decode_next_pc(struct utrapframe *tf, uint64 pc,
                                int *insn_len_out, int *is_branch_out,
                                gdb_read_mem_fn read_mem)
{
    (void)tf; (void)read_mem;
    *insn_len_out = 1;
    *is_branch_out = 0;
    return pc + 1;  /* fallback — should not be called */
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Target description XML                                                      */
/*                                                                             */
/* Tells GDB exactly which registers we provide.  The org.gnu.gdb.i386.core    */
/* feature REQUIRES GP registers, segment registers, AND x87 FPU registers     */
/* (st0-st7 + fctrl/fstat/ftag/fiseg/fioff/foseg/fooff/fop).  Without the     */
/* FPU registers, GDB rejects the description with "Architecture rejected      */
/* target-supplied description" and falls back to the default 57+ register     */
/* layout, causing all register communication to break.                        */
/* ──────────────────────────────────────────────────────────────────────────── */

static const char x86_64_target_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "  <architecture>i386:x86-64</architecture>\n"
    "  <feature name=\"org.gnu.gdb.i386.core\">\n"
    "    <reg name=\"rax\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"rbx\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"rcx\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"rdx\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"rsi\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"rdi\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"rbp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
    "    <reg name=\"rsp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
    "    <reg name=\"r8\"  bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"r9\"  bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"r10\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"r11\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"r12\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"r13\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"r14\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"r15\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"rip\" bitsize=\"64\" type=\"code_ptr\"/>\n"
    "    <reg name=\"eflags\" bitsize=\"32\" type=\"int32\"/>\n"
    "    <reg name=\"cs\" bitsize=\"32\" type=\"int32\"/>\n"
    "    <reg name=\"ss\" bitsize=\"32\" type=\"int32\"/>\n"
    "    <reg name=\"ds\" bitsize=\"32\" type=\"int32\"/>\n"
    "    <reg name=\"es\" bitsize=\"32\" type=\"int32\"/>\n"
    "    <reg name=\"fs\" bitsize=\"32\" type=\"int32\"/>\n"
    "    <reg name=\"gs\" bitsize=\"32\" type=\"int32\"/>\n"
    "    <reg name=\"st0\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"st1\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"st2\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"st3\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"st4\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"st5\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"st6\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"st7\" bitsize=\"80\" type=\"i387_ext\"/>\n"
    "    <reg name=\"fctrl\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"fstat\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"ftag\"  bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"fiseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"fioff\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"foseg\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"fooff\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"fop\"   bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "  </feature>\n"
    "</target>\n";

const char *gdb_arch_target_xml(int *len_out)
{
    *len_out = sizeof(x86_64_target_xml) - 1;
    return x86_64_target_xml;
}
