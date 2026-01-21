#!/usr/bin/env python3
"""
Estimate kernel symbol file size from a relocatable object or executable.

This script analyzes the DWARF debug info to estimate how large the
symbol file will be after gen_addrline.sh processes it.

Usage:
    estimate_symbol_size.py <objdump> <kernel.o> [margin_percent]

Output:
    Prints the estimated size in bytes to stdout.
    Exits with 0 on success, 1 on error.

The estimate accounts for:
- File/symbol headers
- Address lines (hex addresses vary in length)
- Safety margin for address length variation
"""

import sys
import os
import subprocess
import re

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <objdump> <kernel.o> [margin_percent]", file=sys.stderr)
        sys.exit(1)
    
    objdump = sys.argv[1]
    kernel_obj = sys.argv[2]
    margin_percent = int(sys.argv[3]) if len(sys.argv) > 3 else 20
    
    if not os.path.exists(kernel_obj):
        print(f"Error: {kernel_obj} not found", file=sys.stderr)
        sys.exit(1)
    
    # Count entries in DWARF line info
    try:
        result = subprocess.run(
            [objdump, '--dwarf=decodedline', kernel_obj],
            capture_output=True,
            text=True,
            timeout=300
        )
    except subprocess.TimeoutExpired:
        print("Error: objdump timed out", file=sys.stderr)
        sys.exit(1)
    except FileNotFoundError:
        print(f"Error: {objdump} not found", file=sys.stderr)
        sys.exit(1)
    
    if result.returncode != 0:
        print(f"Error: objdump failed: {result.stderr}", file=sys.stderr)
        sys.exit(1)
    
    lines = result.stdout.splitlines()
    
    # Count unique files and address entries
    files = set()
    symbols = set()
    address_lines = 0
    
    current_file = None
    for line in lines:
        line = line.strip()
        if not line:
            continue
        
        # File header line contains filename
        if line.endswith(':') and not line.startswith('0x'):
            current_file = line[:-1]
            files.add(current_file)
            continue
        
        # Address line: starts with address (0x... or hex)
        parts = line.split()
        if len(parts) >= 2:
            # Try to parse as address line
            try:
                if parts[0].startswith('0x'):
                    int(parts[0], 16)
                    address_lines += 1
            except ValueError:
                pass
    
    # Estimate size:
    # - Each file header: ~50 bytes (path + colon + newline)
    # - Each symbol header: ~30 bytes (colon + name + newline)
    # - Each address line: ~40 bytes (start end line\n with 16-char addresses)
    
    avg_file_header = 60
    avg_symbol_header = 40
    avg_address_line = 45  # Conservative: "0123456789abcdef 0123456789abcdef 12345\n"
    
    # Estimate symbols = files (rough, each file has ~1 main function worth mentioning)
    # Actually, we'll be generous since gen_addrline.sh collapses entries
    estimated_symbols = len(files)
    
    base_size = (
        len(files) * avg_file_header +
        estimated_symbols * avg_symbol_header +
        address_lines * avg_address_line
    )
    
    # Add margin for address length variation and safety
    final_size = int(base_size * (1 + margin_percent / 100))
    
    # Round up to 4KB page boundary
    PAGE_SIZE = 4096
    final_size = ((final_size + PAGE_SIZE - 1) // PAGE_SIZE) * PAGE_SIZE
    
    # Print result
    print(final_size)
    
    # Debug info to stderr
    print(f"Files: {len(files)}, Address lines: {address_lines}, "
          f"Base: {base_size}, Final: {final_size} (0x{final_size:x})",
          file=sys.stderr)

if __name__ == '__main__':
    main()
