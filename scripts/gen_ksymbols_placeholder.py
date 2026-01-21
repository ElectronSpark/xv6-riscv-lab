#!/usr/bin/env python3
"""
Generate a fixed-size kernel symbols placeholder assembly file.

This script creates a .ksymbols section with a specified size,
filled with zeros. The actual symbols will be embedded later.

Usage:
    gen_ksymbols_placeholder.py <size_in_bytes> <output.S>

The size should include padding for address length variation.
"""

import sys
import os

def main():
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]} <size_in_bytes> <output.S>", file=sys.stderr)
        sys.exit(1)
    
    try:
        # Accept hex (0x...) or decimal
        size_str = sys.argv[1]
        if size_str.startswith('0x') or size_str.startswith('0X'):
            size = int(size_str, 16)
        else:
            size = int(size_str)
    except ValueError:
        print(f"Error: Invalid size '{sys.argv[1]}'", file=sys.stderr)
        sys.exit(1)
    
    output_file = sys.argv[2]
    
    # Round up to page boundary
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
        
        # Use .space directive for efficiency (fills with zeros)
        f.write(f'    .space {size}, 0\n')
        
        f.write('\n__ksymbols_placeholder_end:\n')
    
    print(f"Generated {output_file} with {size} bytes (0x{size:x}) placeholder")

if __name__ == '__main__':
    main()
