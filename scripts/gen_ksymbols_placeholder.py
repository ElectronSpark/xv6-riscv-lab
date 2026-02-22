#!/usr/bin/env python3
"""
Generate a fixed-size kernel symbols placeholder assembly file.

This script creates a .ksymbols section with a specified size,
filled with zeros. The actual symbols will be embedded later.

Usage:
    gen_ksymbols_placeholder.py <size_in_bytes> <output.S>
    gen_ksymbols_placeholder.py auto <objdump> <kernel_elf> <output.S>

In "auto" mode the size is computed from DWARF decoded-line info
via estimate_symbol_size.count_dwarf_entries().

The size should include padding for address length variation.
"""

import sys
import os


def write_placeholder(output_file: str, size: int):
    """Write a .ksymbols placeholder assembly file of *size* bytes."""
    PAGE_SIZE = 4096
    size = ((size + PAGE_SIZE - 1) // PAGE_SIZE) * PAGE_SIZE

    with open(output_file, 'w') as f:
        f.write("/*\n")
        f.write(" * Auto-generated kernel symbols placeholder\n")
        f.write(f" * Reserved size: {size} bytes (0x{size:x})\n")
        f.write(" *\n")
        f.write(" * This space will be filled with actual symbol data\n")
        f.write(" * after the final kernel link.\n")
        f.write(" */\n\n")

        f.write('.section .ksymbols, "a", @progbits\n')
        f.write('.global __ksymbols_placeholder_start\n')
        f.write('.global __ksymbols_placeholder_end\n\n')

        f.write('__ksymbols_placeholder_start:\n')
        f.write(f'    .space {size}, 0\n')
        f.write('\n__ksymbols_placeholder_end:\n')

    print(f"Generated {output_file} with {size} bytes (0x{size:x}) placeholder")


def main():
    # ------- auto mode: compute size from DWARF data ---------
    if len(sys.argv) >= 2 and sys.argv[1] == 'auto':
        if len(sys.argv) != 5:
            print(f"Usage: {sys.argv[0]} auto <objdump> <kernel_elf> <output.S>",
                  file=sys.stderr)
            sys.exit(1)

        objdump, kernel_elf, output_file = sys.argv[2], sys.argv[3], sys.argv[4]

        # Import the shared DWARF analysis
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from estimate_symbol_size import count_dwarf_entries, estimate_ksymbols_size

        try:
            address_lines, files = count_dwarf_entries(objdump, kernel_elf)
        except (FileNotFoundError, RuntimeError) as e:
            print(f"Error: {e}", file=sys.stderr)
            sys.exit(1)

        size = estimate_ksymbols_size(address_lines, len(files))
        print(f"estimate_symbol_size [ksymbols]: "
              f"files={len(files)}, address_lines={address_lines}, "
              f"size={size} (0x{size:x})", file=sys.stderr)
        write_placeholder(output_file, size)
        return

    # ------- explicit size mode ---------
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <size_in_bytes> <output.S>", file=sys.stderr)
        print(f"       {sys.argv[0]} auto <objdump> <kernel_elf> <output.S>",
              file=sys.stderr)
        sys.exit(1)

    try:
        size_str = sys.argv[1]
        if size_str.startswith('0x') or size_str.startswith('0X'):
            size = int(size_str, 16)
        else:
            size = int(size_str)
    except ValueError:
        print(f"Error: Invalid size '{sys.argv[1]}'", file=sys.stderr)
        sys.exit(1)

    write_placeholder(sys.argv[2], size)


if __name__ == '__main__':
    main()
