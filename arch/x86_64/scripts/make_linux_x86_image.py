#!/usr/bin/env python3

import argparse
import os
import shutil
import struct
import subprocess
import tempfile


def put_u8(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 1] = struct.pack("<B", val & 0xFF)


def put_u16(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 2] = struct.pack("<H", val & 0xFFFF)


def put_u32(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 4] = struct.pack("<I", val & 0xFFFFFFFF)


def put_u64(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 8] = struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


def put_e820(buf: bytearray, index: int, addr: int, size: int, typ: int) -> None:
    off = 0x2D0 + index * 20
    put_u64(buf, off + 0, addr)
    put_u64(buf, off + 8, size)
    put_u32(buf, off + 16, typ)


def build_real_mode_trampoline(code32_start: int, trampoline_exec_off: int) -> bytes:
    assembler = shutil.which("as")
    objcopy = shutil.which("objcopy") or shutil.which("x86_64-linux-gnu-objcopy")
    if assembler is None or objcopy is None:
        raise SystemExit("building x86 setup trampoline requires as and objcopy")

    source = f"""
        .code16
        .section .text
        .org 0x{trampoline_exec_off:x}
    setup_trampoline:
        cli
        cld
        pushw %cs
        popw %ds
        pushw %cs
        popw %ax
        movzwl %ax, %eax
        shll $4, %eax
        movl %eax, %esi
        subl $0x200, %esi

        movw $(gdt_end - gdt - 1), gdt_desc
        movl %eax, %ebx
        addl $gdt, %ebx
        movl %ebx, gdt_desc + 2
        lgdtl gdt_desc
        movl %cr0, %eax
        orl $1, %eax
        movl %eax, %cr0
        ljmpl $0x08, $(0x10200 + pm32)

        .code32
    pm32:
        movw $0x10, %ax
        movw %ax, %ds
        movw %ax, %es
        movw %ax, %ss
        movl $0x90000, %esp
        movl $0x{code32_start:x}, %eax
        jmp *%eax

        .align 8
    gdt:
        .quad 0
    gdt_code:
        .word 0xffff
        .word 0
        .byte 0
        .byte 0x9a
        .byte 0xcf
        .byte 0
    gdt_data:
        .word 0xffff
        .word 0
        .byte 0
        .byte 0x92
        .byte 0xcf
        .byte 0
    gdt_end:
    gdt_desc:
        .word 0
        .long 0
    """

    with tempfile.TemporaryDirectory() as tmp:
        asm_path = os.path.join(tmp, "setup.S")
        obj_path = os.path.join(tmp, "setup.o")
        bin_path = os.path.join(tmp, "setup.bin")
        with open(asm_path, "w", encoding="utf-8") as f:
            f.write(source)
        subprocess.run([assembler, "--32", "-o", obj_path, asm_path],
                       check=True)
        subprocess.run([objcopy, "-O", "binary", obj_path, bin_path],
                       check=True)
        with open(bin_path, "rb") as f:
            return f.read()


def build_setup_block(setup_sects: int, protocol_version: int, code32_start: int, cmdline_size: int, syssize_paras: int) -> bytearray:
    total_setup_bytes = (setup_sects + 1) * 512
    setup = bytearray(total_setup_bytes)
    init_size = (total_setup_bytes + syssize_paras * 16 + 0xFFF) & ~0xFFF
    trampoline_off = 0x270
    trampoline_exec_off = trampoline_off - 0x200

    setup[0:2] = b"\xEB\x3C"
    setup[2:4] = b"\x90\x90"
    put_u8(setup, 0x3E, 0xE9)
    put_u16(setup, 0x3F, trampoline_off - (0x3E + 3))
    put_u8(setup, 0x1F1, setup_sects)
    put_u16(setup, 0x1F2, 0)              # root_flags
    put_u32(setup, 0x1F4, syssize_paras)
    put_u16(setup, 0x1FA, 0xFFFF)         # vid_mode: normal text mode
    put_u16(setup, 0x1FC, 0)              # root_dev
    put_u16(setup, 0x1FE, 0xAA55)

    put_u8(setup, 0x200, 0xEB)
    put_u8(setup, 0x201, trampoline_off - (0x200 + 2))
    setup[0x202:0x206] = b"HdrS"
    put_u16(setup, 0x206, protocol_version)
    put_u16(setup, 0x208, 0)              # realmode_swtch
    put_u16(setup, 0x20C, 0)              # start_sys_seg
    put_u16(setup, 0x20E, 0)              # kernel_version
    put_u8(setup, 0x210, 0xFF)            # type_of_loader: unknown
    put_u8(setup, 0x211, 0x01)            # loadflags: LOADED_HIGH
    put_u16(setup, 0x212, 0x8000)         # setup_move_size
    put_u32(setup, 0x214, code32_start)
    put_u32(setup, 0x218, 0)              # ramdisk_image
    put_u32(setup, 0x21C, 0)              # ramdisk_size
    put_u32(setup, 0x220, 0)              # bootsect_kludge
    put_u16(setup, 0x224, total_setup_bytes - 0x200)
    put_u8(setup, 0x226, 0)               # ext_loader_ver
    put_u8(setup, 0x227, 0)               # ext_loader_type
    put_u32(setup, 0x228, 0)              # cmd_line_ptr, filled by loader
    put_u32(setup, 0x22C, 0x7FFFFFFF)     # initrd_addr_max
    put_u32(setup, 0x230, 0x00200000)     # kernel_alignment
    put_u8(setup, 0x234, 0)               # fixed-address image
    put_u8(setup, 0x235, 21)              # min_alignment: 2 MiB
    put_u16(setup, 0x236, 0x0009)         # XLF_KERNEL_64 | XLF_EFI_HANDOVER_64
    put_u32(setup, 0x238, cmdline_size)
    put_u32(setup, 0x23C, 0)              # hardware_subarch
    put_u64(setup, 0x240, 0)              # hardware_subarch_data
    put_u32(setup, 0x248, 0)              # payload_offset
    put_u32(setup, 0x24C, 0)              # payload_length
    put_u64(setup, 0x250, 0)              # setup_data
    put_u64(setup, 0x258, code32_start)   # pref_address
    put_u32(setup, 0x260, init_size)
    put_u32(setup, 0x264, 0)              # handover_offset
    put_u32(setup, 0x268, 0)              # kernel_info_offset

    # QEMU's linuxboot loader executes our minimal setup stub, so provide the
    # memory map that Linux setup code would normally discover through BIOS.
    put_u8(setup, 0x1E8, 3)               # e820_entries
    put_e820(setup, 0, 0x00000000, 0x0009FC00, 1)
    put_e820(setup, 1, 0x0009FC00, 0x00060400, 2)
    put_e820(setup, 2, 0x00100000, 0x3FF00000, 1)

    trampoline = build_real_mode_trampoline(code32_start, trampoline_exec_off)
    if len(trampoline) > total_setup_bytes:
        raise SystemExit("x86 setup trampoline is larger than setup area")
    setup[trampoline_off:trampoline_off + len(trampoline) - trampoline_exec_off] = trampoline[trampoline_exec_off:]

    return setup


def main() -> None:
    parser = argparse.ArgumentParser(description="Build Linux x86 image (setup + protected payload)")
    parser.add_argument("payload", help="Flat protected-mode payload binary")
    parser.add_argument("output", help="Output Linux x86 image")
    parser.add_argument("--setup-sects", type=int, default=4, help="Number of setup sectors after boot sector")
    parser.add_argument("--code32-start", default="0x00100000", help="32-bit kernel load/entry address")
    parser.add_argument("--cmdline-size", type=int, default=2048, help="Max command line size")
    parser.add_argument("--protocol-version", default="0x020C", help="Linux boot protocol version")
    args = parser.parse_args()

    setup_sects = args.setup_sects
    if setup_sects < 1 or setup_sects > 63:
        raise SystemExit("setup-sects must be in range [1, 63]")

    code32_start = int(args.code32_start, 0)
    protocol_version = int(args.protocol_version, 0)

    with open(args.payload, "rb") as f:
        payload = f.read()

    syssize_paras = (len(payload) + 15) // 16

    setup = build_setup_block(
        setup_sects=setup_sects,
        protocol_version=protocol_version,
        code32_start=code32_start,
        cmdline_size=args.cmdline_size,
        syssize_paras=syssize_paras,
    )

    image = bytes(setup) + payload

    with open(args.output, "wb") as f:
        f.write(image)

    print(
        f"Built Linux x86 image {os.path.basename(args.output)}: "
        f"setup={(setup_sects + 1) * 512} bytes, payload={len(payload)} bytes, "
        f"version=0x{protocol_version:04x}, code32_start=0x{code32_start:08x}"
    )


if __name__ == "__main__":
    main()
