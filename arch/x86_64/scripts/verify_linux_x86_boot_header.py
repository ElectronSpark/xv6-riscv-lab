#!/usr/bin/env python3

import argparse
import struct
import sys


def get_u8(buf: bytes, off: int) -> int:
    return struct.unpack_from("<B", buf, off)[0]


def get_u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def get_u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def get_u64(buf: bytes, off: int) -> int:
    return struct.unpack_from("<Q", buf, off)[0]


def fail(msg: str) -> None:
    print(f"x86 boot header verify failed: {msg}", file=sys.stderr)
    raise SystemExit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Verify Linux x86 boot protocol header")
    parser.add_argument("image", help="Path to flat kernel image")
    parser.add_argument("--min-version", default="0x020C", help="Minimum protocol version")
    args = parser.parse_args()

    min_version = int(args.min_version, 0)

    with open(args.image, "rb") as f:
        data = f.read()

    if len(data) < 0x240:
        fail("image too small for setup header")

    setup_sects = get_u8(data, 0x1F1)
    setup_bytes = (setup_sects + 1) * 512
    if len(data) <= setup_bytes:
        fail("image has no protected-mode payload after setup sectors")

    if get_u16(data, 0x1FE) != 0xAA55:
        fail("boot_flag != 0xAA55")

    if data[0x200] not in (0xEB, 0xE9):
        fail("setup jump opcode is missing")

    if data[0x202:0x206] != b"HdrS":
        fail("missing HdrS signature")

    version = get_u16(data, 0x206)
    if version < min_version:
        fail(f"protocol version 0x{version:04x} < 0x{min_version:04x}")

    loadflags = get_u8(data, 0x211)
    if (loadflags & 0x01) == 0:
        fail("loadflags does not set LOADED_HIGH")

    if get_u8(data, 0x210) == 0:
        fail("type_of_loader is unset")

    xloadflags = get_u16(data, 0x236)
    if (xloadflags & 0x0001) == 0:
        fail("xloadflags does not set XLF_KERNEL_64")
    if (xloadflags & 0x0008) == 0:
        fail("xloadflags does not set XLF_EFI_HANDOVER_64")

    code32_start = get_u32(data, 0x214)
    if code32_start == 0:
        fail("code32_start is zero")

    relocatable = get_u8(data, 0x234)
    if relocatable != 0:
        fail("relocatable_kernel is set for a fixed-address xv6 image")

    if get_u32(data, 0x22C) == 0:
        fail("initrd_addr_max is unset")

    if get_u32(data, 0x230) < 0x100000:
        fail("kernel_alignment is too small")

    if get_u32(data, 0x238) == 0:
        fail("cmdline_size is unset")

    if get_u64(data, 0x258) != code32_start:
        fail("pref_address does not match code32_start")

    if get_u32(data, 0x260) < len(data):
        fail("init_size is smaller than the image")

    syssize_paras = get_u32(data, 0x1F4)
    payload_bytes = len(data) - setup_bytes
    expected_paras = (payload_bytes + 15) // 16
    if syssize_paras != expected_paras:
        fail(f"syssize mismatch: header={syssize_paras}, expected={expected_paras}")

    print(
        f"x86 boot header OK: version=0x{version:04x}, "
        f"code32_start=0x{code32_start:08x}, loadflags=0x{loadflags:02x}, "
        f"relocatable={relocatable}, "
        f"xloadflags=0x{xloadflags:04x}, setup_sects={setup_sects}, "
        f"init_size=0x{get_u32(data, 0x260):x}, syssize={syssize_paras}"
    )


if __name__ == "__main__":
    main()
