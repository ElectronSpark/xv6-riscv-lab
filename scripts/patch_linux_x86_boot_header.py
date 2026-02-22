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

    min_size = 0x240
    if len(data) < min_size:
        data.extend(b"\x00" * (min_size - len(data)))

    file_size = len(data)
    syssize_paras = (file_size + 15) // 16

    # Linux x86 boot protocol header offsets.
    put_u8(data, 0x1F1, 0)                 # setup_sects (0 => 4 sectors)
    put_u16(data, 0x1FE, 0xAA55)           # boot_flag
    data[0x202:0x204] = b"\xEB\x00"       # jump (minimal short jump)
    data[0x206:0x20A] = b"HdrS"            # header signature
    put_u16(data, 0x20A, protocol_version) # version
    put_u32(data, 0x1F4, syssize_paras)    # syssize

    put_u8(data, 0x211, 0x01)              # loadflags: LOADED_HIGH
    put_u32(data, 0x214, code32_start)     # code32_start

    put_u32(data, 0x228, 0)                # cmd_line_ptr (loader-provided)
    put_u32(data, 0x230, 0x00200000)       # kernel_alignment (2 MiB)
    put_u8(data, 0x234, 1)                 # relocatable_kernel
    put_u8(data, 0x235, 21)                # min_alignment (2 MiB)
    put_u16(data, 0x236, 0x0001)           # xloadflags: XLF_KERNEL_64
    put_u32(data, 0x238, args.cmdline_size) # cmdline_size

    with open(image_path, "wb") as f:
        f.write(data)

    print(
        f"Patched Linux x86 boot header in {os.path.basename(image_path)}: "
        f"version=0x{protocol_version:04x}, code32_start=0x{code32_start:08x}, "
        f"syssize={syssize_paras}"
    )


if __name__ == "__main__":
    main()
