#!/usr/bin/env python3
"""
Generate kernel linker script from template with computed section sizes.

Usage:
    gen_kernel_ld.py <template.ld.in> <output.ld> <objdump> <kernel_elf> <kernel_base>

Counts DWARF decoded-line entries in <kernel_elf> (via estimate_symbol_size)
to determine how many __ksymbols_t rb-tree nodes the kernel will need at
runtime, then substitutes @KERNEL_SYMBOLS_IDX_SIZE@ (and @KERNEL_BASE@) in
the linker script template.
"""

import sys
import os

# Import the shared DWARF analysis from estimate_symbol_size.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from estimate_symbol_size import count_dwarf_entries, estimate_ksymbols_idx_size


def main():
    if len(sys.argv) != 6:
        print(f"Usage: {sys.argv[0]} <template> <output> <objdump> <kernel_elf> <kernel_base>",
              file=sys.stderr)
        sys.exit(1)

    template_path, output_path, objdump, kernel_elf, kernel_base = sys.argv[1:6]

    try:
        address_lines, files = count_dwarf_entries(objdump, kernel_elf)
    except (FileNotFoundError, RuntimeError) as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)

    idx_size = estimate_ksymbols_idx_size(address_lines)

    print(f"gen_kernel_ld: {address_lines} DWARF entries, {len(files)} files → "
          f".ksymbols_idx = {idx_size} bytes (0x{idx_size:x})",
          file=sys.stderr)

    # Read template and substitute
    with open(template_path) as f:
        content = f.read()

    content = content.replace('@KERNEL_BASE@', kernel_base)
    content = content.replace('@KERNEL_SYMBOLS_IDX_SIZE@', f'0x{idx_size:x}')

    with open(output_path, 'w') as f:
        f.write(content)


if __name__ == '__main__':
    main()
