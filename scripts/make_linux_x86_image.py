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


def build_setup_block(setup_sects: int, protocol_version: int, code32_start: int, cmdline_size: int, syssize_paras: int) -> bytearray:
    total_setup_bytes = (setup_sects + 1) * 512
    setup = bytearray(total_setup_bytes)

    setup[0:2] = b"\xEB\x3C"
    setup[2:4] = b"\x90\x90"
    put_u8(setup, 0x1F1, setup_sects)
    put_u32(setup, 0x1F4, syssize_paras)
    put_u16(setup, 0x1FE, 0xAA55)

    setup[0x202:0x204] = b"\xEB\x00"
    setup[0x206:0x20A] = b"HdrS"
    put_u16(setup, 0x20A, protocol_version)
    put_u8(setup, 0x210, 0xFF)
    put_u8(setup, 0x211, 0x01)
    put_u32(setup, 0x214, code32_start)
    put_u32(setup, 0x228, 0)
    put_u32(setup, 0x230, 0x00200000)
    put_u8(setup, 0x234, 1)
    put_u8(setup, 0x235, 21)
    put_u16(setup, 0x236, 0x0001)
    put_u32(setup, 0x238, cmdline_size)

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
