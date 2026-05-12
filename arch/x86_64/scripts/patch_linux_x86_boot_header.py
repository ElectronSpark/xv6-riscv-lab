#!/usr/bin/env python3

import argparse
import os
import struct


def put_u8(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 1] = struct.pack("<B", val & 0xFF)


def put_u16(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 2] = struct.pack("<H", val & 0xFFFF)


def put_u32(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 4] = struct.pack("<I", val & 0xFFFFFFFF)


def put_u64(buf: bytearray, off: int, val: int) -> None:
    buf[off:off + 8] = struct.pack("<Q", val & 0xFFFFFFFFFFFFFFFF)


def main() -> None:
    parser = argparse.ArgumentParser(description="Patch Linux x86 boot protocol header")
    parser.add_argument("image", help="Path to flat kernel image")
    parser.add_argument("--code32-start", default="0x00100000", help="32-bit entry/load address")
    parser.add_argument("--cmdline-size", type=int, default=2048, help="Max command line size")
    parser.add_argument("--protocol-version", default="0x020C", help="Linux boot protocol version")
    args = parser.parse_args()

    image_path = args.image
    code32_start = int(args.code32_start, 0)
    protocol_version = int(args.protocol_version, 0)

    with open(image_path, "rb") as f:
        data = bytearray(f.read())

    min_size = 0x26C
    if len(data) < min_size:
        data.extend(b"\x00" * (min_size - len(data)))

    file_size = len(data)
    syssize_paras = (file_size + 15) // 16

    # Linux x86 boot protocol header offsets.
    put_u8(data, 0x1F1, 0)                 # setup_sects (0 => 4 sectors)
    put_u16(data, 0x1F2, 0)                # root_flags
    put_u32(data, 0x1F4, syssize_paras)    # syssize
    put_u16(data, 0x1FA, 0xFFFF)           # vid_mode
    put_u16(data, 0x1FC, 0)                # root_dev
    put_u16(data, 0x1FE, 0xAA55)           # boot_flag
    data[0x200:0x202] = b"\xEB\x58"       # jump over setup header
    data[0x202:0x206] = b"HdrS"            # header signature
    put_u16(data, 0x206, protocol_version) # version

    put_u16(data, 0x208, 0)                # realmode_swtch
    put_u16(data, 0x20C, 0)                # start_sys_seg
    put_u16(data, 0x20E, 0)                # kernel_version
    put_u8(data, 0x210, 0xFF)              # type_of_loader
    put_u8(data, 0x211, 0x01)              # loadflags: LOADED_HIGH
    put_u16(data, 0x212, 0x8000)           # setup_move_size
    put_u32(data, 0x214, code32_start)     # code32_start

    put_u32(data, 0x218, 0)                # ramdisk_image
    put_u32(data, 0x21C, 0)                # ramdisk_size
    put_u32(data, 0x220, 0)                # bootsect_kludge
    put_u16(data, 0x224, 0x8000)           # heap_end_ptr
    put_u8(data, 0x226, 0)                 # ext_loader_ver
    put_u8(data, 0x227, 0)                 # ext_loader_type
    put_u32(data, 0x228, 0)                # cmd_line_ptr (loader-provided)
    put_u32(data, 0x22C, 0x7FFFFFFF)       # initrd_addr_max
    put_u32(data, 0x230, 0x00200000)       # kernel_alignment (2 MiB)
    put_u8(data, 0x234, 0)                 # relocatable_kernel
    put_u8(data, 0x235, 21)                # min_alignment (2 MiB)
    put_u16(data, 0x236, 0x0009)           # xloadflags: XLF_KERNEL_64 | XLF_EFI_HANDOVER_64
    put_u32(data, 0x238, args.cmdline_size) # cmdline_size
    put_u32(data, 0x23C, 0)                # hardware_subarch
    put_u64(data, 0x240, 0)                # hardware_subarch_data
    put_u32(data, 0x248, 0)                # payload_offset
    put_u32(data, 0x24C, 0)                # payload_length
    put_u64(data, 0x250, 0)                # setup_data
    put_u64(data, 0x258, code32_start)     # pref_address
    put_u32(data, 0x260, (len(data) + 0xFFF) & ~0xFFF) # init_size
    put_u32(data, 0x264, 0)                # handover_offset
    put_u32(data, 0x268, 0)                # kernel_info_offset

    with open(image_path, "wb") as f:
        f.write(data)

    print(
        f"Patched Linux x86 boot header in {os.path.basename(image_path)}: "
        f"version=0x{protocol_version:04x}, code32_start=0x{code32_start:08x}, "
        f"syssize={syssize_paras}"
    )


if __name__ == "__main__":
    main()
