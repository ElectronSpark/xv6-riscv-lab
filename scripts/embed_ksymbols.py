#!/usr/bin/env python3
"""
Embed kernel symbols into the .ksymbols section of a kernel binary.

This script patches the kernel ELF or flat binary with the actual
symbol data, replacing the placeholder zeros.

Usage:
    embed_ksymbols.py <kernel_binary> <symbols_file> [--elf|--flat]

For ELF files, it locates the .ksymbols section and patches it.
For flat files, it uses the provided offset.

The symbol data must fit within the placeholder size.
"""

import sys
import os
import struct

def find_elf_section(f, section_name):
    """Find a section in an ELF file, return (offset, size)."""
    f.seek(0)
    magic = f.read(4)
    if magic != b'\x7fELF':
        return None
    
    # Read ELF header
    f.seek(0)
    e_ident = f.read(16)
    is_64bit = e_ident[4] == 2
    is_le = e_ident[5] == 1
    
    if is_64bit:
        # 64-bit ELF header
        f.seek(40)  # e_shoff
        if is_le:
            e_shoff = struct.unpack('<Q', f.read(8))[0]
            f.seek(58)  # e_shentsize
            e_shentsize = struct.unpack('<H', f.read(2))[0]
            e_shnum = struct.unpack('<H', f.read(2))[0]
            e_shstrndx = struct.unpack('<H', f.read(2))[0]
        else:
            e_shoff = struct.unpack('>Q', f.read(8))[0]
            f.seek(58)
            e_shentsize = struct.unpack('>H', f.read(2))[0]
            e_shnum = struct.unpack('>H', f.read(2))[0]
            e_shstrndx = struct.unpack('>H', f.read(2))[0]
        
        # Read section string table
        f.seek(e_shoff + e_shstrndx * e_shentsize)
        if is_le:
            f.seek(e_shoff + e_shstrndx * e_shentsize + 24)
            sh_offset = struct.unpack('<Q', f.read(8))[0]
            sh_size = struct.unpack('<Q', f.read(8))[0]
        else:
            f.seek(e_shoff + e_shstrndx * e_shentsize + 24)
            sh_offset = struct.unpack('>Q', f.read(8))[0]
            sh_size = struct.unpack('>Q', f.read(8))[0]
        
        f.seek(sh_offset)
        strtab = f.read(sh_size)
        
        # Find the target section
        for i in range(e_shnum):
            f.seek(e_shoff + i * e_shentsize)
            if is_le:
                sh_name = struct.unpack('<I', f.read(4))[0]
                sh_type = struct.unpack('<I', f.read(4))[0]
                sh_flags = struct.unpack('<Q', f.read(8))[0]
                sh_addr = struct.unpack('<Q', f.read(8))[0]
                sh_offset = struct.unpack('<Q', f.read(8))[0]
                sh_size = struct.unpack('<Q', f.read(8))[0]
            else:
                sh_name = struct.unpack('>I', f.read(4))[0]
                sh_type = struct.unpack('>I', f.read(4))[0]
                sh_flags = struct.unpack('>Q', f.read(8))[0]
                sh_addr = struct.unpack('>Q', f.read(8))[0]
                sh_offset = struct.unpack('>Q', f.read(8))[0]
                sh_size = struct.unpack('>Q', f.read(8))[0]
            
            # Get section name
            name_end = strtab.find(b'\x00', sh_name)
            name = strtab[sh_name:name_end].decode('utf-8', errors='ignore')
            
            if name == section_name:
                return (sh_offset, sh_size, sh_addr)
    
    return None

def main():
    if len(sys.argv) < 3:
        print(f"Usage: {sys.argv[0]} <kernel_binary> <symbols_file> [--elf|--flat] [offset]", 
              file=sys.stderr)
        sys.exit(1)
    
    kernel_file = sys.argv[1]
    symbols_file = sys.argv[2]
    mode = 'elf'
    flat_offset = None
    
    for arg in sys.argv[3:]:
        if arg == '--elf':
            mode = 'elf'
        elif arg == '--flat':
            mode = 'flat'
        elif arg.startswith('0x'):
            flat_offset = int(arg, 16)
        else:
            try:
                flat_offset = int(arg)
            except ValueError:
                pass
    
    if not os.path.exists(kernel_file):
        print(f"Error: {kernel_file} not found", file=sys.stderr)
        sys.exit(1)
    
    if not os.path.exists(symbols_file):
        print(f"Error: {symbols_file} not found", file=sys.stderr)
        sys.exit(1)
    
    # Read symbols
    with open(symbols_file, 'rb') as f:
        sym_data = f.read()
    
    sym_size = len(sym_data)
    
    if mode == 'elf':
        with open(kernel_file, 'r+b') as f:
            section_info = find_elf_section(f, '.ksymbols')
            if section_info is None:
                print("Error: .ksymbols section not found in ELF", file=sys.stderr)
                sys.exit(1)
            
            offset, size, addr = section_info
            
            if sym_size > size:
                print(f"Error: Symbol data ({sym_size} bytes) exceeds "
                      f"placeholder size ({size} bytes)", file=sys.stderr)
                sys.exit(1)
            
            # Write symbol data
            f.seek(offset)
            f.write(sym_data)
            
            # Pad with zeros if needed
            if sym_size < size:
                f.write(b'\x00' * (size - sym_size))
            
            print(f"Embedded {sym_size} bytes into .ksymbols section "
                  f"(offset: 0x{offset:x}, size: {size}, addr: 0x{addr:x})")
    
    elif mode == 'flat':
        if flat_offset is None:
            print("Error: --flat mode requires offset argument", file=sys.stderr)
            sys.exit(1)
        
        with open(kernel_file, 'r+b') as f:
            f.seek(0, 2)  # End
            file_size = f.tell()
            
            if flat_offset >= file_size:
                print(f"Error: Offset {flat_offset} exceeds file size {file_size}", 
                      file=sys.stderr)
                sys.exit(1)
            
            f.seek(flat_offset)
            f.write(sym_data)
            
            print(f"Embedded {sym_size} bytes at offset 0x{flat_offset:x}")

if __name__ == '__main__':
    main()
