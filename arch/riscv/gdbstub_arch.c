/*
 * gdbstub_arch.c — RISC-V architecture support for the in-kernel GDB stub.
 *
 * Provides register access (GDB RISC-V register order):
 *   0..31  — x0..x31 (GP integer registers)
 *   32     — pc (sepc)
 *   33..64 — f0..f31 (double-precision FP registers)
 *   65     — fflags (bits 4:0 of fcsr)
 *   66     — frm (bits 7:5 of fcsr)
 *   67     — fcsr (full FP CSR)
 *
 * Also: software breakpoint encoding (EBREAK / C.EBREAK) and software
 * single-step via instruction decoding and temporary breakpoints.
 */

#include "types.h"
#include "trapframe.h"
#include "gdbstub_arch.h"
#include "string.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* Constants                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

/* RISC-V EBREAK encodings */
#define RV_EBREAK       0x00100073U   /* 32-bit EBREAK */
#define RV_C_EBREAK     0x9002U       /* 16-bit compressed EBREAK */

/* GDB expects 68 registers for RISC-V: x0..x31, pc, f0..f31, fflags, frm, fcsr */
const int gdb_arch_num_regs = 68;

/* Register sizes:
 *   0..32  — 8 bytes (64-bit GP + pc)
 *   33..64 — 8 bytes (64-bit FP double)
 *   65..67 — 4 bytes (fflags, frm, fcsr) */
int gdb_arch_reg_size(int regnum)
{
    if (regnum <= 64) return 8;
    return 4;   /* fflags, frm, fcsr */
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Register access                                                             */
/*                                                                             */
/* GDB RISC-V register order: x0..x31, pc                                      */
/* ──────────────────────────────────────────────────────────────────────────── */

uint64 gdb_arch_get_reg(struct utrapframe *tf, int regnum)
{
    struct trapframe *t = &tf->trapframe;
    switch (regnum) {
    case 0:  return 0;                 /* x0 = zero */
    case 1:  return t->ra;             /* x1 */
    case 2:  return t->sp;             /* x2 */
    case 3:  return tf->gp;            /* x3 */
    case 4:  return tf->tp;            /* x4 */
    case 5:  return t->t0;             /* x5 */
    case 6:  return t->t1;             /* x6 */
    case 7:  return t->t2;             /* x7 */
    case 8:  return t->s0;             /* x8 / fp */
    case 9:  return tf->s1;            /* x9 */
    case 10: return t->a0;             /* x10 */
    case 11: return t->a1;             /* x11 */
    case 12: return t->a2;             /* x12 */
    case 13: return t->a3;             /* x13 */
    case 14: return t->a4;             /* x14 */
    case 15: return t->a5;             /* x15 */
    case 16: return t->a6;             /* x16 */
    case 17: return t->a7;             /* x17 */
    case 18: return tf->s2;            /* x18 */
    case 19: return tf->s3;            /* x19 */
    case 20: return tf->s4;            /* x20 */
    case 21: return tf->s5;            /* x21 */
    case 22: return tf->s6;            /* x22 */
    case 23: return tf->s7;            /* x23 */
    case 24: return tf->s8;            /* x24 */
    case 25: return tf->s9;            /* x25 */
    case 26: return tf->s10;           /* x26 */
    case 27: return tf->s11;           /* x27 */
    case 28: return t->t3;             /* x28 */
    case 29: return t->t4;             /* x29 */
    case 30: return t->t5;             /* x30 */
    case 31: return t->t6;             /* x31 */
    case 32: return t->sepc;           /* pc  */
    default: return 0;
    }
}

void gdb_arch_set_reg(struct utrapframe *tf, int regnum, uint64 val)
{
    struct trapframe *t = &tf->trapframe;
    switch (regnum) {
    case 0:  break;                    /* x0 is hardwired */
    case 1:  t->ra   = val; break;
    case 2:  t->sp   = val; break;
    case 3:  tf->gp  = val; break;
    case 4:  tf->tp  = val; break;
    case 5:  t->t0   = val; break;
    case 6:  t->t1   = val; break;
    case 7:  t->t2   = val; break;
    case 8:  t->s0   = val; break;
    case 9:  tf->s1  = val; break;
    case 10: t->a0   = val; break;
    case 11: t->a1   = val; break;
    case 12: t->a2   = val; break;
    case 13: t->a3   = val; break;
    case 14: t->a4   = val; break;
    case 15: t->a5   = val; break;
    case 16: t->a6   = val; break;
    case 17: t->a7   = val; break;
    case 18: tf->s2  = val; break;
    case 19: tf->s3  = val; break;
    case 20: tf->s4  = val; break;
    case 21: tf->s5  = val; break;
    case 22: tf->s6  = val; break;
    case 23: tf->s7  = val; break;
    case 24: tf->s8  = val; break;
    case 25: tf->s9  = val; break;
    case 26: tf->s10 = val; break;
    case 27: tf->s11 = val; break;
    case 28: t->t3   = val; break;
    case 29: t->t4   = val; break;
    case 30: t->t5   = val; break;
    case 31: t->t6   = val; break;
    case 32: t->sepc = val; break;
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* PC access                                                                   */
/* ──────────────────────────────────────────────────────────────────────────── */

uint64 gdb_arch_get_pc(struct utrapframe *tf)
{
    return tf->trapframe.sepc;
}

void gdb_arch_set_pc(struct utrapframe *tf, uint64 pc)
{
    tf->trapframe.sepc = pc;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Breakpoint encoding                                                         */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Is a 16-bit value a compressed instruction? (bottom 2 bits != 0b11) */
static inline int rv_is_compressed(uint16 insn)
{
    return (insn & 0x3) != 0x3;
}

int gdb_arch_brk_len(uint64 addr, const void *insn_bytes, int nbytes)
{
    if (insn_bytes && nbytes >= 2) {
        uint16 lo16 = *(const uint16 *)insn_bytes;
        if (rv_is_compressed(lo16))
            return 2;
    }
    return 4;  /* default: 4-byte EBREAK */
}

int gdb_arch_brk_encode(void *buf, int len)
{
    if (len == 2) {
        uint16 brk = RV_C_EBREAK;
        *(uint16 *)buf = brk;
        return 2;
    }
    uint32 brk = RV_EBREAK;
    *(uint32 *)buf = brk;
    return 4;
}

int gdb_arch_user_brk_len(uint64 pc, const void *insn_bytes, int nbytes)
{
    if (insn_bytes && nbytes >= 2) {
        uint16 lo16 = *(const uint16 *)insn_bytes;
        if (rv_is_compressed(lo16))
            return 2;
    }
    return 4;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Single-step support                                                         */
/*                                                                             */
/* RISC-V does NOT have a hardware single-step flag.  We decode the current    */
/* instruction to find the possible next PC(s) and insert temporary software   */
/* breakpoints.                                                                */
/* ──────────────────────────────────────────────────────────────────────────── */

int gdb_arch_has_hw_step(void)
{
    return 0;  /* RISC-V: software step only */
}

void gdb_arch_set_hw_step(struct utrapframe *tf, int enable)
{
    (void)tf; (void)enable;
    /* Not supported on RISC-V */
}

/* Sign-extend a value from bit 'bit' (0-indexed) */
static inline int64 sign_extend(uint64 val, int bit)
{
    uint64 mask = 1ULL << bit;
    return (int64)((val ^ mask) - mask);
}

/*
 * Decode the instruction at 'pc' and return the next PC.
 * For conditional branches, returns the branch-taken target
 * (caller should also place a bp at pc + insn_len for fall-through).
 * Returns 0 if we can't decode — caller should bp at pc + 4 as fallback.
 *
 * Sets *insn_len_out to the length of the current instruction (2 or 4).
 * Sets *is_branch_out to 1 if the instruction is a conditional branch
 * (meaning we need breakpoints at BOTH next_pc AND pc + insn_len).
 */
uint64 gdb_arch_decode_next_pc(struct utrapframe *tf, uint64 pc,
                                int *insn_len_out, int *is_branch_out,
                                gdb_read_mem_fn read_mem)
{
    uint32 insn = 0;
    *is_branch_out = 0;

    if (read_mem(&insn, pc, 4) != 0) {
        /* Can't read — assume 4-byte, step to pc+4 */
        *insn_len_out = 4;
        return pc + 4;
    }

    uint16 lo16 = (uint16)(insn & 0xFFFF);

    if (rv_is_compressed(lo16)) {
        /* ── Compressed instruction (2 bytes) ── */
        *insn_len_out = 2;
        uint16 ci = lo16;
        uint16 op = ci & 0x3;
        uint16 funct3 = (ci >> 13) & 0x7;

        if (op == 1) {
            if (funct3 == 5) {
                /* C.J: offset in bits [12:2], target = pc + sext(offset) */
                int32 imm = 0;
                imm |= ((ci >> 3) & 0x7) << 1;   /* [3:1] */
                imm |= ((ci >> 11) & 0x1) << 4;   /* [4] */
                imm |= ((ci >> 2) & 0x1) << 5;    /* [5] */
                imm |= ((ci >> 7) & 0x1) << 6;    /* [6] */
                imm |= ((ci >> 6) & 0x1) << 7;    /* [7] */
                imm |= ((ci >> 9) & 0x3) << 8;    /* [9:8] */
                imm |= ((ci >> 8) & 0x1) << 10;   /* [10] */
                imm |= ((ci >> 12) & 0x1) << 11;  /* [11] sign */
                imm = (int32)sign_extend(imm, 11);
                return pc + imm;
            }
            if (funct3 == 6 || funct3 == 7) {
                /* C.BEQZ / C.BNEZ */
                int32 imm = 0;
                imm |= ((ci >> 3) & 0x3) << 1;    /* [2:1] */
                imm |= ((ci >> 10) & 0x3) << 3;   /* [4:3] */
                imm |= ((ci >> 2) & 0x1) << 5;    /* [5] */
                imm |= ((ci >> 5) & 0x3) << 6;    /* [7:6] */
                imm |= ((ci >> 12) & 0x1) << 8;   /* [8] sign */
                imm = (int32)sign_extend(imm, 8);
                *is_branch_out = 1;
                /* Evaluate condition */
                int rs1_idx = 8 + ((ci >> 7) & 0x7);  /* s0..s7  = x8..x15 */
                uint64 rs1_val = gdb_arch_get_reg(tf, rs1_idx);
                if (funct3 == 6) {
                    /* C.BEQZ: branch if rs1 == 0 */
                    return (rs1_val == 0) ? pc + imm : pc + 2;
                } else {
                    /* C.BNEZ: branch if rs1 != 0 */
                    return (rs1_val != 0) ? pc + imm : pc + 2;
                }
            }
        }
        if (op == 2) {
            if (funct3 == 4) {
                uint16 bit12 = (ci >> 12) & 1;
                uint16 rs1 = (ci >> 7) & 0x1f;
                uint16 rs2 = (ci >> 2) & 0x1f;
                if (bit12 == 0 && rs2 == 0 && rs1 != 0) {
                    /* C.JR: jalr x0, rs1, 0 */
                    return gdb_arch_get_reg(tf, rs1) & ~1ULL;
                }
                if (bit12 == 1 && rs2 == 0 && rs1 != 0) {
                    /* C.JALR: jalr ra, rs1, 0 */
                    return gdb_arch_get_reg(tf, rs1) & ~1ULL;
                }
            }
        }
        /* Other compressed instructions: fall through to pc+2 */
        return pc + 2;
    }

    /* ── 32-bit instruction ── */
    *insn_len_out = 4;
    uint32 opcode = insn & 0x7f;

    switch (opcode) {
    case 0x6F: {
        /* JAL: J-type */
        int32 imm = 0;
        imm |= ((insn >> 21) & 0x3FF) << 1;    /* [10:1] */
        imm |= ((insn >> 20) & 0x1) << 11;      /* [11] */
        imm |= ((insn >> 12) & 0xFF) << 12;     /* [19:12] */
        imm |= ((insn >> 31) & 0x1) << 20;      /* [20] sign */
        imm = (int32)sign_extend(imm, 20);
        return pc + imm;
    }
    case 0x67: {
        /* JALR: I-type  (opcode=1100111) */
        int rs1 = (insn >> 15) & 0x1f;
        int32 imm = (int32)(insn >> 20);
        imm = (int32)sign_extend(imm, 11);
        uint64 base = gdb_arch_get_reg(tf, rs1);
        return (base + imm) & ~1ULL;
    }
    case 0x63: {
        /* Branch: B-type */
        int funct3 = (insn >> 12) & 0x7;
        int rs1 = (insn >> 15) & 0x1f;
        int rs2 = (insn >> 20) & 0x1f;
        int32 imm = 0;
        imm |= ((insn >> 8) & 0xF) << 1;       /* [4:1] */
        imm |= ((insn >> 25) & 0x3F) << 5;      /* [10:5] */
        imm |= ((insn >> 7) & 0x1) << 11;       /* [11] */
        imm |= ((insn >> 31) & 0x1) << 12;      /* [12] sign */
        imm = (int32)sign_extend(imm, 12);

        uint64 v1 = gdb_arch_get_reg(tf, rs1);
        uint64 v2 = gdb_arch_get_reg(tf, rs2);
        int taken = 0;

        switch (funct3) {
        case 0: taken = (v1 == v2);                break; /* BEQ  */
        case 1: taken = (v1 != v2);                break; /* BNE  */
        case 4: taken = ((int64)v1 < (int64)v2);  break; /* BLT  */
        case 5: taken = ((int64)v1 >= (int64)v2); break; /* BGE  */
        case 6: taken = (v1 < v2);                 break; /* BLTU */
        case 7: taken = (v1 >= v2);                break; /* BGEU */
        default: break;
        }

        *is_branch_out = 1;
        return taken ? (pc + imm) : (pc + 4);
    }
    default:
        return pc + 4;
    }
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Byte-level register access (GP + FP)                                        */
/* ──────────────────────────────────────────────────────────────────────────── */

int gdb_arch_read_reg_bytes(struct utrapframe *tf, struct fpu_state *fps,
                            int regnum, void *buf)
{
    int sz = gdb_arch_reg_size(regnum);
    memset(buf, 0, sz);

    if (regnum <= 32) {
        /* GP register or pc — delegate to existing accessor */
        uint64 val = gdb_arch_get_reg(tf, regnum);
        memcpy(buf, &val, sz);
    } else if (regnum <= 64) {
        /* f0..f31 — read from fpu_state */
        if (fps != NULL) {
            uint64 val = fps->f[regnum - 33];
            memcpy(buf, &val, 8);
        }
    } else if (regnum == 65) {
        /* fflags — bits 4:0 of fcsr */
        if (fps != NULL) {
            uint32 val = fps->fcsr & 0x1F;
            memcpy(buf, &val, 4);
        }
    } else if (regnum == 66) {
        /* frm — bits 7:5 of fcsr */
        if (fps != NULL) {
            uint32 val = (fps->fcsr >> 5) & 0x7;
            memcpy(buf, &val, 4);
        }
    } else if (regnum == 67) {
        /* fcsr — full register */
        if (fps != NULL) {
            uint32 val = fps->fcsr;
            memcpy(buf, &val, 4);
        }
    }
    return sz;
}

int gdb_arch_write_reg_bytes(struct utrapframe *tf, struct fpu_state *fps,
                             int regnum, const void *buf)
{
    int sz = gdb_arch_reg_size(regnum);

    if (regnum <= 32) {
        uint64 val = 0;
        memcpy(&val, buf, sz);
        gdb_arch_set_reg(tf, regnum, val);
    } else if (regnum <= 64 && fps != NULL) {
        uint64 val = 0;
        memcpy(&val, buf, 8);
        fps->f[regnum - 33] = val;
    } else if (regnum == 65 && fps != NULL) {
        uint32 val = 0;
        memcpy(&val, buf, 4);
        fps->fcsr = (fps->fcsr & ~0x1FU) | (val & 0x1F);
    } else if (regnum == 66 && fps != NULL) {
        uint32 val = 0;
        memcpy(&val, buf, 4);
        fps->fcsr = (fps->fcsr & ~0xE0U) | ((val & 0x7) << 5);
    } else if (regnum == 67 && fps != NULL) {
        uint32 val = 0;
        memcpy(&val, buf, 4);
        fps->fcsr = val;
    }
    return sz;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Target description XML                                                      */
/*                                                                             */
/* Tells GDB exactly which registers we provide.  Without this, GDB            */
/* assumes the full RISC-V register set including FP regs and rejects           */
/* our g-packet as too short.                                                  */
/* ──────────────────────────────────────────────────────────────────────────── */

static const char riscv_target_xml[] =
    "<?xml version=\"1.0\"?>\n"
    "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
    "<target version=\"1.0\">\n"
    "  <architecture>riscv:rv64</architecture>\n"
    "  <feature name=\"org.gnu.gdb.riscv.cpu\">\n"
    "    <reg name=\"zero\" bitsize=\"64\" type=\"int64\" regnum=\"0\"/>\n"
    "    <reg name=\"ra\" bitsize=\"64\" type=\"code_ptr\"/>\n"
    "    <reg name=\"sp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
    "    <reg name=\"gp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
    "    <reg name=\"tp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
    "    <reg name=\"t0\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"t1\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"t2\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"fp\" bitsize=\"64\" type=\"data_ptr\"/>\n"
    "    <reg name=\"s1\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a0\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a1\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a2\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a3\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a4\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a5\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a6\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"a7\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s2\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s3\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s4\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s5\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s6\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s7\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s8\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s9\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s10\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"s11\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"t3\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"t4\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"t5\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"t6\" bitsize=\"64\" type=\"int64\"/>\n"
    "    <reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\"/>\n"
    "  </feature>\n"
    "  <feature name=\"org.gnu.gdb.riscv.fpu\">\n"
    "    <reg name=\"ft0\" bitsize=\"64\" type=\"ieee_double\" regnum=\"33\"/>\n"
    "    <reg name=\"ft1\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft2\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft3\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft4\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft5\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft6\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft7\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs0\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs1\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa0\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa1\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa2\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa3\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa4\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa5\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa6\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fa7\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs2\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs3\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs4\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs5\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs6\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs7\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs8\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs9\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs10\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fs11\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft8\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft9\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft10\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"ft11\" bitsize=\"64\" type=\"ieee_double\"/>\n"
    "    <reg name=\"fflags\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"frm\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "    <reg name=\"fcsr\" bitsize=\"32\" type=\"int\" group=\"float\"/>\n"
    "  </feature>\n"
    "</target>\n";

const char *gdb_arch_target_xml(int *len_out)
{
    *len_out = sizeof(riscv_target_xml) - 1;
    return riscv_target_xml;
}
