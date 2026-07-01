#!/usr/bin/env python3
import sys


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: gen_vdso_blob.py <vdso.so> <out.S>", file=sys.stderr)
        return 2

    image, out = sys.argv[1], sys.argv[2]
    with open(out, "w", encoding="ascii") as f:
        f.write('    .section .rodata.x86_vdso_image,"a"\n')
        f.write("    .balign 4096\n")
        f.write("    .globl x86_vdso_image_start\n")
        f.write("x86_vdso_image_start:\n")
        f.write(f'    .incbin "{image}"\n')
        f.write("    .globl x86_vdso_image_end\n")
        f.write("x86_vdso_image_end:\n")
        f.write("    .balign 4096\n")
        f.write('    .section .note.GNU-stack,"",@progbits\n')
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
