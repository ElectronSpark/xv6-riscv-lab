#!/usr/bin/env python3
"""
Generate assembly offset definitions from C structure definitions.

This script compiles a small C program to determine structure member offsets
and generates a header file with #define statements for use in assembly files.

Usage:
    gen_asm_offsets.py [options]

Options:
    --output FILE           Output header file (default: asm-offsets.h)
    --include DIR           Add include directory (can be specified multiple times)
    --compiler PATH         C compiler to use (default: gcc)
    --struct NAME:FILE      Add structure NAME from FILE (can be repeated)
    --config FILE           Read configuration from JSON file
    -v, --verbose           Verbose output

Examples:
    # Generate offsets for specific structures
    ./gen_asm_offsets.py --include ../kernel/inc \\
        --struct trapframe:trapframe.h \\
        --struct proc:proc.h \\
        --output asm-offsets.h

    # Use a config file
    ./gen_asm_offsets.py --config asm-offsets-config.json
"""

import argparse
import json
import os
import subprocess
import sys
import tempfile
from pathlib import Path


class AsmOffsetsGenerator:
    def __init__(self, compiler='gcc', includes=None, verbose=False):
        self.compiler = compiler
        self.includes = includes or []
        self.verbose = verbose
        self.structures = []  # List of (struct_name, header_file) tuples
    
    def add_structure(self, struct_name, header_file):
        """Add a structure to generate offsets for."""
        self.structures.append((struct_name, header_file))
    
    def log(self, msg):
        """Print verbose log message."""
        if self.verbose:
            print(f"[gen_asm_offsets] {msg}", file=sys.stderr)
    
    def generate_c_code(self):
        """Generate C code that will output offset information using inline assembly."""
        code = []
        code.append('// Generate offsets using inline assembly - Linux kernel style')
        code.append('')
        
        # Include all necessary headers
        included_files = set()
        
        # First include any extra headers from config
        if hasattr(self, 'extra_headers'):
            for header in self.extra_headers:
                if header not in included_files:
                    code.append(f'#include "{header}"')
                    included_files.add(header)
        
        # Then include structure-specific headers
        for struct_name, header_file in self.structures:
            if header_file not in included_files:
                code.append(f'#include "{header_file}"')
                included_files.add(header_file)
        
        code.append('')
        code.append('/**')
        code.append(' * Generate a #define for a constant value')
        code.append(' */')
        code.append('#define DEFINE(sym, val) \\')
        code.append('    asm volatile("\\n.ascii \\"->define " #sym " %0 \\"" : : "i" (val))')
        code.append('')
        code.append('/**')
        code.append(' * Generate a #define for a structure member offset')
        code.append(' */')
        code.append('#define OFFSET(sym, str, mem) \\')
        code.append('    DEFINE(sym, __builtin_offsetof(struct str, mem))')
        code.append('')
        code.append('/**')
        code.append(' * Generate a #define for the size of a structure or type')
        code.append(' */')
        code.append('#define SIZE(sym, str) \\')
        code.append('    DEFINE(sym, sizeof(struct str))')
        code.append('')
        code.append('/**')
        code.append(' * Generate a blank line in the output')
        code.append(' */')
        code.append('#define BLANK() \\')
        code.append('    asm volatile("\\n.ascii \\"->\\"")')
        code.append('')
        code.append('/**')
        code.append(' * Generate a comment in the output')
        code.append(' */')
        code.append('#define COMMENT(x) \\')
        code.append('    asm volatile("\\n.ascii \\"->##" x "\\"")')
        code.append('')
        code.append('void generate_asm_offsets(void) {')
        
        return '\n'.join(code)
    
    def generate_offset_calls(self, struct_name, fields=None):
        """Generate the offset printing calls for a structure.
        
        Args:
            struct_name: Name of the structure
            fields: List of field names, or None to auto-detect
        """
        prefix = struct_name.upper()
        code = []
        code.append(f'    COMMENT("{struct_name} structure offsets");')
        
        if fields:
            # Use specified fields
            for field in fields:
                offset_name = f"{prefix}_{field.upper()}"
                code.append(f'    OFFSET({offset_name}, {struct_name}, {field});')
        else:
            # For auto-detection, we'll parse the structure later
            # For now, just indicate we need all fields
            code.append(f'    // Fields for {struct_name} need to be specified')
        
        code.append(f'    SIZE({prefix}_SIZE, {struct_name});')
        code.append('')
        return '\n'.join(code)
    
    def compile_and_run(self, c_code):
        """Compile and run the C code to get offset definitions."""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Write C source
            src_file = Path(tmpdir) / 'gen_offsets.c'
            src_file.write_text(c_code)
            
            # Compile
            exe_file = Path(tmpdir) / 'gen_offsets'
            compile_cmd = [self.compiler]
            for inc_dir in self.includes:
                compile_cmd.extend(['-I', inc_dir])
            compile_cmd.extend(['-o', str(exe_file), str(src_file)])
            
            self.log(f"Compiling: {' '.join(compile_cmd)}")
            
            try:
                result = subprocess.run(
                    compile_cmd,
                    capture_output=True,
                    text=True,
                    check=True
                )
                if result.stderr:
                    self.log(f"Compiler warnings/errors:\n{result.stderr}")
            except subprocess.CalledProcessError as e:
                print(f"Error: Compilation failed:", file=sys.stderr)
                print(e.stderr, file=sys.stderr)
                sys.exit(1)
            
            # Run the program
            self.log(f"Running: {exe_file}")
            try:
                result = subprocess.run(
                    [str(exe_file)],
                    capture_output=True,
                    text=True,
                    check=True
                )
                return result.stdout
            except subprocess.CalledProcessError as e:
                print(f"Error: Execution failed:", file=sys.stderr)
                print(e.stderr, file=sys.stderr)
                sys.exit(1)
    
    def generate(self, output_file, struct_configs):
        """Generate the asm-offsets header file.
        
        Args:
            output_file: Path to output header file
            struct_configs: Dict mapping struct_name -> field_list or 'all'
        """
        # Generate C code with user-specified offsets
        c_code_parts = [self.generate_c_code()]
        
        for struct_name, header_file in self.structures:
            fields = struct_configs.get(struct_name, [])
            if fields == 'all' or not fields:
                # Will need to parse the header to get all fields
                fields = self._parse_struct_fields(struct_name, header_file)
            
            c_code_parts.append(self.generate_offset_calls(struct_name, fields))
        
        c_code_parts.append('}')
        
        c_code = '\n'.join(c_code_parts)
        
        if self.verbose:
            self.log("Generated C code:")
            for i, line in enumerate(c_code.split('\n'), 1):
                print(f"{i:3}: {line}", file=sys.stderr)
        
        # Compile to assembly instead of executable
        asm_output = self.compile_to_assembly(c_code)
        
        # Parse assembly to extract offsets
        output = self.parse_assembly_offsets(asm_output)
        
        # Write output
        output_path = Path(output_file)
        output_path.write_text(output)
        self.log(f"Generated {output_file}")
    
    def compile_to_assembly(self, c_code):
        """Compile C code to assembly and return the assembly text."""
        with tempfile.TemporaryDirectory() as tmpdir:
            # Write C source
            src_file = Path(tmpdir) / 'gen_offsets.c'
            src_file.write_text(c_code)
            
            # Compile to assembly
            asm_file = Path(tmpdir) / 'gen_offsets.s'
            compile_cmd = [self.compiler, '-S']
            for inc_dir in self.includes:
                compile_cmd.extend(['-I', inc_dir])
            compile_cmd.extend(['-o', str(asm_file), str(src_file)])
            
            self.log(f"Compiling: {' '.join(compile_cmd)}")
            
            try:
                result = subprocess.run(
                    compile_cmd,
                    capture_output=True,
                    text=True,
                    check=True
                )
                if result.stderr:
                    self.log(f"Compiler warnings/errors:\n{result.stderr}")
            except subprocess.CalledProcessError as e:
                print(f"Error: Compilation failed:", file=sys.stderr)
                print(e.stderr, file=sys.stderr)
                sys.exit(1)
            
            # Read assembly output
            return asm_file.read_text()
    
    def parse_assembly_offsets(self, asm_text):
        """Parse assembly output to extract offset definitions."""
        import re
        
        lines = []
        lines.append('/* Auto-generated by gen_asm_offsets.py */')
        lines.append('/* DO NOT EDIT - Changes will be overwritten */')
        
        # Pattern to match our special assembly directives
        # Looking for: .ascii "->define SYMBOL VALUE "
        define_pattern = re.compile(r'\.ascii\s+"->define\s+(\S+)\s+(\S+)\s+"')
        comment_pattern = re.compile(r'\.ascii\s+"->##(.+)"')
        blank_pattern = re.compile(r'\.ascii\s+"->"')
        
        for line in asm_text.split('\n'):
            stripped = line.strip()
            
            # Check for define directive
            match = define_pattern.search(stripped)
            if match:
                symbol = match.group(1)
                value = match.group(2)
                lines.append(f'#define {symbol} {value}')
                continue
            
            # Check for comment directive
            match = comment_pattern.search(stripped)
            if match:
                comment = match.group(1)
                lines.append(f'\n/* {comment} */')
                continue
            
            # Check for blank line directive
            if blank_pattern.search(stripped):
                lines.append('')
                continue
        
        return '\n'.join(lines) + '\n'
    
    def _parse_struct_fields(self, struct_name, header_file):
        """Parse a header file to extract structure field names."""
        fields = []
        
        # Find the header file
        header_path = None
        for inc_dir in self.includes:
            candidate = Path(inc_dir) / header_file
            if candidate.exists():
                header_path = candidate
                break
        
        if not header_path:
            # Try relative path
            header_path = Path(header_file)
            if not header_path.exists():
                print(f"Warning: Could not find {header_file}, skipping field auto-detection", 
                      file=sys.stderr)
                return []
        
        self.log(f"Parsing {header_path} for struct {struct_name}")
        
        # Simple parser for C structures
        content = header_path.read_text()
        
        # Remove all comments first to avoid confusion
        import re
        # Remove single-line comments
        content = re.sub(r'//.*', '', content)
        # Remove multi-line comments
        content = re.sub(r'/\*.*?\*/', '', content, flags=re.DOTALL)
        
        lines = content.split('\n')
        
        in_struct = False
        struct_brace_count = 0
        nested_brace_count = 0
        
        for line in lines:
            stripped = line.strip()
            
            # Check for struct definition
            if f'struct {struct_name}' in stripped and '{' in stripped:
                in_struct = True
                struct_brace_count = 1
                continue
            elif f'struct {struct_name}' in stripped:
                in_struct = True
                continue
            
            if in_struct:
                # Track brace levels
                open_braces = stripped.count('{')
                close_braces = stripped.count('}')
                
                # Skip preprocessor directives and empty lines
                if not stripped or stripped.startswith('#'):
                    continue
                
                # If we see an opening brace for a nested struct/union
                if open_braces > 0 and nested_brace_count == 0:
                    # Check if this is a nested anonymous struct/union
                    if 'struct' in stripped or 'union' in stripped:
                        nested_brace_count += open_braces
                        nested_brace_count -= close_braces
                        continue
                
                # If we're inside a nested struct/union, skip until we exit
                if nested_brace_count > 0:
                    nested_brace_count += open_braces
                    nested_brace_count -= close_braces
                    continue
                
                # Update main struct brace count
                struct_brace_count += open_braces
                struct_brace_count -= close_braces
                
                # Check for end of struct
                if struct_brace_count <= 0 and '}' in stripped:
                    break
                
                # Extract field name (simple heuristic)
                if ';' in stripped:
                    # Parse field declaration
                    parts = stripped.split()
                    if len(parts) >= 2:
                        # Last part before ';' is usually the field name (possibly with array brackets)
                        field_part = parts[-1].rstrip(';')
                        # Remove array notation
                        if '[' in field_part:
                            field_part = field_part[:field_part.index('[')]
                        # Remove pointer asterisks
                        field_part = field_part.lstrip('*')
                        if field_part and field_part.isidentifier():
                            fields.append(field_part)
        
        self.log(f"Found {len(fields)} fields in struct {struct_name}: {', '.join(fields[:5])}{'...' if len(fields) > 5 else ''}")
        return fields


def load_config(config_file):
    """Load configuration from JSON file."""
    with open(config_file, 'r') as f:
        return json.load(f)


def main():
    parser = argparse.ArgumentParser(
        description='Generate assembly offset definitions from C structures',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__
    )
    parser.add_argument('--output', '-o', default='asm-offsets.h',
                        help='Output header file (default: asm-offsets.h)')
    parser.add_argument('--include', '-I', action='append', dest='includes',
                        help='Add include directory (can be specified multiple times)')
    parser.add_argument('--header', '-H', action='append', dest='headers',
                        help='Add prerequisite header file (can be specified multiple times)')
    parser.add_argument('--compiler', '-c', default='gcc',
                        help='C compiler to use (default: gcc)')
    parser.add_argument('--struct', '-s', action='append', dest='structs',
                        help='Add structure as NAME:FILE[:field1,field2,...] or NAME:FILE:all')
    parser.add_argument('--config', '-f',
                        help='Read configuration from JSON file')
    parser.add_argument('--verbose', '-v', action='store_true',
                        help='Verbose output')
    
    args = parser.parse_args()
    
    # Create generator
    generator = AsmOffsetsGenerator(
        compiler=args.compiler,
        includes=args.includes or [],
        verbose=args.verbose
    )
    
    struct_configs = {}
    
    # Load from config file if specified
    if args.config:
        config = load_config(args.config)
        for inc in config.get('includes', []):
            generator.includes.append(inc)
        # Add extra headers if specified
        generator.extra_headers = config.get('headers', [])
        for struct_def in config.get('structures', []):
            struct_name = struct_def['name']
            header_file = struct_def['header']
            generator.add_structure(struct_name, header_file)
            struct_configs[struct_name] = struct_def.get('fields', 'all')
    
    # Add prerequisite headers from command line
    if args.headers:
        if not hasattr(generator, 'extra_headers'):
            generator.extra_headers = []
        generator.extra_headers.extend(args.headers)
    
    # Add structures from command line
    if args.structs:
        for struct_spec in args.structs:
            parts = struct_spec.split(':')
            if len(parts) < 2:
                print(f"Error: Invalid struct specification: {struct_spec}", file=sys.stderr)
                print("Format: NAME:FILE or NAME:FILE:field1,field2,... or NAME:FILE:all", 
                      file=sys.stderr)
                sys.exit(1)
            
            struct_name = parts[0]
            header_file = parts[1]
            fields = 'all' if len(parts) == 2 else (parts[2].split(',') if parts[2] != 'all' else 'all')
            
            generator.add_structure(struct_name, header_file)
            struct_configs[struct_name] = fields
    
    if not generator.structures:
        print("Error: No structures specified", file=sys.stderr)
        print("Use --struct or --config to specify structures", file=sys.stderr)
        sys.exit(1)
    
    # Generate the header
    generator.generate(args.output, struct_configs)
    print(f"Generated {args.output}")


if __name__ == '__main__':
    main()
