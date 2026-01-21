#include "param.h"
        # qemu -kernel loads the kernel at 0x80200000
        # and causes each hart (i.e. CPU) to jump there.
        # kernel.ld causes the following code to
        # be placed at 0x80200000.

        # Linux RISC-V Image Header (64 bytes)
        # This allows U-Boot's 'booti' command to properly boot xv6
        # and pass the DTB address in a1 register.
        #
        # For ELF kernels, this header is embedded at the entry point.
        # The kernel remains in ELF format (not flat binary).
        #
        # See: Linux arch/riscv/include/asm/image.h
        # See: https://www.kernel.org/doc/html/latest/arch/riscv/boot-image-header.html
.section .text.entry
.global _entry
_entry:
        # code0: Jump over the header (must be 4-byte instruction, not compressed)
        # Use .4byte to emit raw instruction bytes for JAL
        # JAL x0, offset encodes as: imm[20|10:1|11|19:12] | rd=x0 | opcode=1101111
        # We jump to _start_kernel which is 64 bytes (0x40) ahead
        .4byte 0x0400006f   # jal x0, 0x40 (64 bytes forward)

        # code1: unused (4 bytes)
        .4byte 0

        # text_offset: offset from start of RAM (64-bit, little-endian)
        # For QEMU: kernel at 0x80200000, RAM at 0x80000000 -> offset = 0x200000
        # For Orange Pi: kernel at 0x200000, RAM at 0x0 -> offset = 0x200000
        .8byte 0x200000

        # image_size: effective image size (64-bit, little-endian)
        # For ELF kernels, this can be 0 (bootloader uses ELF headers)
        # For flat binaries, this should be patched by the build system
        .8byte 0

        # flags: bit 0 = kernel endianness (0=LE, 1=BE) (64-bit, little-endian)
        .8byte 0

        # version: header version 0.2 (32-bit, little-endian)
        # bits 0:15 = minor (2), bits 16:31 = major (0)
        .4byte 0x0002

        # res1: reserved (32-bit)
        .4byte 0

        # res2: reserved (64-bit)
        .8byte 0

        # magic: "RISCV\0\0\0" = 0x5643534952 at offset 0x30 (64-bit, little-endian)
        .ascii "RISCV\0\0\0"

        # magic2: "RSC\x05" = 0x05435352 at offset 0x38 (32-bit, little-endian)
        .4byte 0x05435352

        # res3: reserved for PE/COFF offset (32-bit)
        .4byte 0

        # Header is 64 bytes total, _start_kernel follows at offset 0x40

.global _start_kernel
_start_kernel:
        # OpenSBI/U-Boot passes hartid in a0, dtb pointer in a1
        mv tp, a0

        # set up a stack for C.
        # stack0 is declared in start.c,
        # with a 4096-byte stack per CPU.
        # sp = stack0 + ((hartid + 1) * 4096)
        la sp, stack0
        li a2, KERNEL_STACK_SIZE
        addi a3, a0, 1
        mul a2, a2, a3
        add sp, sp, a2
        li a2, 4096    # Make room for PCB
        sub sp, sp, a2

        # jump to start() in start.c
        # a0 still contains hartid, a1 contains dtb pointer
        call start
spin:
        wfi
        j spin
