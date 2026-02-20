# musl libc xv6 Architecture Port

This directory contains the architecture-specific files needed to build
[musl libc](https://musl.libc.org/) for the xv6 RISC-V kernel.

## Overview

musl is built as a cross-compiled C library targeting `riscv64-xv6-musl`.
The files here are overlaid on top of musl's `arch/riscv64/` directory,
replacing certain headers with xv6-specific versions.

## Key Differences from Linux riscv64

| Feature | Linux | xv6 |
|---------|-------|-----|
| Syscall numbers | Linux ABI | Custom xv6 numbers |
| `struct stat` | 18+ fields | 5 fields (dev, ino, mode, nlink, size) |
| `sigset_t` | 128 bits | 64 bits |
| `struct termios.c_cc` | NCCS=32 | NCCS=16 |
| `clone()` | 5 register args | Single pointer to `struct clone_args` |
| Thread pointer | Set via `clone(CLONE_SETTLS)` | Same |
| futex | Available | Available (custom) |
| brk | Available | Available (custom) |

## Files

- `syscall_arch.h` - Syscall invocation (ecall, a7=num, a0-a5=args)
- `crt_arch.h` - C runtime entry point (_start)
- `pthread_arch.h` - Thread pointer access for TLS
- `reloc.h` - ELF relocation definitions for ld.so
- `atomic_arch.h` - RISC-V atomic operations (AMO instructions)
- `__clone.s` - Clone wrapper (musl convention → xv6 struct clone_args)
- `bits/syscall.h.in` - Syscall number mappings (musl names → xv6 numbers)
- `bits/stat.h` - struct stat layout matching xv6 kernel
- `bits/signal.h` - Signal numbers and sigaction structure
- `bits/termios.h` - Terminal I/O with NCCS=16
- `bits/fcntl.h` - File control flags matching xv6 kernel
- `bits/ioctl.h` - ioctl request codes
- `bits/mman.h` - Memory mapping constants
- `bits/limits.h` - Architecture limits
- `bits/alltypes.h.in` - Basic type definitions

## Building

See `build_musl.sh` for the automated build process. The general steps:

1. Clone musl source
2. Overlay xv6 arch files
3. Configure with `--target=riscv64-xv6-musl`
4. Build with the RISC-V cross-compiler
5. Install to sysroot

## Usage

After building, link programs with:
```
riscv64-xv6-musl-gcc -o myprogram myprogram.c
```

For dynamic linking, ensure `ld-musl-riscv64.so.1` is available in the
xv6 filesystem at `/lib/ld-musl-riscv64.so.1`.
