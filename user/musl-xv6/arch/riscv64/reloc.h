/*
 * reloc.h — RISC-V ELF relocation types for musl dynamic linker
 *
 * Standard RISC-V ELF relocations used by ld.so.
 */

#define LDSO_ARCH "riscv64"

#define REL_SYMBOLIC    R_RISCV_64
#define REL_OFFSET      R_RISCV_RELATIVE
#define REL_GOT         R_RISCV_64
#define REL_PLT         R_RISCV_JUMP_SLOT
#define REL_COPY        R_RISCV_COPY
#define REL_DTPMOD      R_RISCV_TLS_DTPMOD64
#define REL_DTPOFF      R_RISCV_TLS_DTPREL64
#define REL_TPOFF       R_RISCV_TLS_TPREL64
#define REL_TLSDESC     R_RISCV_TLSDESC

#define CRTJMP(pc, sp) __asm__ __volatile__( \
    "mv sp, %1 ; jr %0" : : "r"(pc), "r"(sp) : "memory" )

#define GETFUNCSYM(fp, sym, got) __asm__ ( \
    ".hidden " #sym "\n" \
    "lla %0, " #sym "\n" \
    : "=r"(*(fp)) : : "memory" )

/* RISC-V relocation types */
#define R_RISCV_NONE            0
#define R_RISCV_32              1
#define R_RISCV_64              2
#define R_RISCV_RELATIVE        3
#define R_RISCV_COPY            4
#define R_RISCV_JUMP_SLOT       5
#define R_RISCV_TLS_DTPMOD32    6
#define R_RISCV_TLS_DTPMOD64    7
#define R_RISCV_TLS_DTPREL32    8
#define R_RISCV_TLS_DTPREL64    9
#define R_RISCV_TLS_TPREL32     10
#define R_RISCV_TLS_TPREL64     11
#define R_RISCV_TLSDESC         12
