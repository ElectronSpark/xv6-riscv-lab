# ASM Offsets Generation System

This directory contains the infrastructure for automatically generating C structure offset definitions that can be used in assembly (.S) files.

## Overview

When writing assembly code that needs to access C structure members, you need to know the byte offsets of each field. Previously, these were manually calculated and written as magic numbers (like `40`, `48`, `56` in trampoline.S). This system automates that process.

## How It Works

1. **asm-offsets.c** - A special C file that uses compiler intrinsics to calculate structure offsets
2. **CMakeLists.txt** - Build rules that:
   - Compile asm-offsets.c to assembly
   - Extract the offset definitions using `sed`
   - Generate **asm-offsets.h** header file
3. **asm-offsets.h** - Generated header file with `#define` statements (DO NOT EDIT MANUALLY)

## Usage in Assembly Files

Include the generated header in your .S files:

```asm
#include "inc/asm-offsets.h"

# Now you can use symbolic names instead of magic numbers:
sd ra, TF_RA(a0)      # Instead of: sd ra, 40(a0)
sd sp, TF_SP(a0)      # Instead of: sd sp, 48(a0)
sd gp, TF_GP(a0)      # Instead of: sd gp, 56(a0)
```

## Available Offsets

The system currently generates offsets for:

### Trapframe (struct trapframe)
- `TF_KERNEL_SATP`, `TF_KERNEL_SP`, `TF_KERNEL_TRAP`
- `TF_EPC`, `TF_KERNEL_HARTID`
- `TF_RA`, `TF_SP`, `TF_GP`, `TF_TP`
- `TF_T0` through `TF_T6` (temporary registers)
- `TF_S0` through `TF_S11` (saved registers)
- `TF_A0` through `TF_A7` (argument registers)
- `TF_SCAUSE`, `TF_STVAL`, `TF_SEPC`, `TF_SSTATUS`
- `TF_KERNEL_GP`, `TF_IRQ_SP`, `TF_IRQ_ENTRY`
- `TRAPFRAME_SIZE` (total size)

### Kernel Trapframe (struct ktrapframe)
- `KTF_RA`, `KTF_SP`, `KTF_S0`, `KTF_TP`
- `KTF_T0` through `KTF_T6`
- `KTF_A0` through `KTF_A7`
- `KTF_SEPC`, `KTF_SSTATUS`, `KTF_SCAUSE`, `KTF_STVAL`
- `KTF_STVEC`, `KTF_SSCRATCH`, `KTF_GP`
- `KTRAPFRAME_SIZE`

### Process (struct proc)
- `PROC_KSTACK`, `PROC_STATE`, `PROC_TRAPFRAME`
- `PROC_CONTEXT`, `PROC_VM`
- `PROC_SIZE`

## Adding New Offsets

To add offsets for new structures:

1. Edit **asm-offsets.c**
2. Add your offsets using the macros:
   ```c
   COMMENT("My structure offsets");
   OFFSET(MY_FIELD, mystruct, fieldname);
   SIZE(MYSTRUCT_SIZE, mystruct);
   BLANK();
   ```
3. Rebuild - the header will be automatically regenerated

## Macros Available in asm-offsets.c

- `OFFSET(sym, str, mem)` - Generate offset for structure member
- `SIZE(sym, str)` - Generate size of structure
- `DEFINE(sym, val)` - Generate any constant definition
- `COMMENT(x)` - Add a comment to the generated header
- `BLANK()` - Add a blank line to the generated header

## Build Integration

The asm-offsets.h header is automatically generated before any kernel compilation begins. The build system ensures:

1. asm-offsets.c is compiled to assembly first
2. The assembly is processed to extract definitions
3. asm-offsets.h is created in kernel/inc/
4. All kernel targets depend on this header being generated

## Example Conversion

**Before (hardcoded offsets):**
```asm
sd ra, 40(a0)
sd sp, 48(a0)
sd gp, 56(a0)
```

**After (symbolic offsets):**
```asm
#include "inc/asm-offsets.h"

sd ra, TF_RA(a0)
sd sp, TF_SP(a0)
sd gp, TF_GP(a0)
```

## Benefits

1. **Maintainability** - Structure changes automatically update offsets
2. **Readability** - Code is self-documenting with symbolic names
3. **Type Safety** - Compile-time checking of structure member existence
4. **Consistency** - Single source of truth for offsets
5. **Error Prevention** - No manual calculation errors

## Files

- `asm-offsets.c` - Source file with offset definitions
- `asm-offsets.h` - Generated header (in git ignore, regenerated on build)
- `CMakeLists.txt` - Build rules for generation
- `README.md` - This file

## Notes

- The generated header is placed in `kernel/inc/` for easy inclusion
- You should add `asm-offsets.h` to `.gitignore` as it's generated
- If you see wrong offsets, rebuild from clean: `rm -rf build && mkdir build && cd build && cmake .. && make`
