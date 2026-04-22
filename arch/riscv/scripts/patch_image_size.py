#!/usr/bin/env python3
"""
Patch the Linux RISC-V boot header in a flat binary with the actual image size.

The image_size field is at offset 0x10 (16 bytes) and is 64-bit little-endian.
This is required by U-Boot's booti command.
"""

import sys
import os

def main():
    if len(sys.argv) != 2:
        print(f"Usage: {sys.argv[0]} <binary_file>", file=sys.stderr)
        sys.exit(1)
    
    filename = sys.argv[1]
    
    if not os.path.exists(filename):
        print(f"Error: {filename} not found", file=sys.stderr)
        sys.exit(1)
    
    # Get file size
    size = os.path.getsize(filename)
    
    # Patch the image_size field at offset 0x10
    with open(filename, 'r+b') as f:
        f.seek(16)  # offset 0x10
        f.write(size.to_bytes(8, 'little'))
    
    print(f"Patched image_size={size} (0x{size:x}) at offset 0x10 in {filename}")

if __name__ == '__main__':
    main()
