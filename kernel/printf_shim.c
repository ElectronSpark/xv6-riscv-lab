//
// printf() variadic C-ABI shim.
//
// Phase 2 Wave 4 (see docs/rustify/phase2_plan.md, Step 0): rustc 1.95
// (the stable toolchain pinned for this build) rejects
// `#![feature(c_variadic)]` outright ("may not be used on the stable
// release channel"), so a C-variadic function cannot yet be *defined*
// in Rust. Calling one (as every other file in the tree, C or Rust,
// already does for `printf(fmt, ...)`) has always been stable and is
// unaffected.
//
// This file is therefore the last-resort C remnant of the printf.c
// port: it does nothing but `va_start`/`va_end` bookkeeping and three
// one-line `va_arg` fetchers. All format-string parsing, output
// buffering, and console dispatch lives in kernel/printf.rs's
// `printf_rust()`. Delete this file (and the matching
// `__printf_va_arg_*`/`printf_rust` externs in printf.rs) once
// `c_variadic` stabilizes and reimplement `printf()` directly in Rust
// with `VaList`.
//
// The three fetchers below replicate -- one for one -- the three
// distinct `va_arg(ap, T)` call shapes the original kernel/printf.c
// used for its supported conversions:
//   - `va_arg(ap, int)`    for %d %u %x and the %*s field-width arg
//   - `va_arg(ap, uint64)` for %ld/%lld %lu/%llu %lx/%llx and %p
//   - `va_arg(ap, char *)` for %s
// so the argument-promotion/consumption contract seen by callers is
// byte-for-byte unchanged from the C original.
//

#include <stdarg.h>
#include "types.h"

long long __printf_va_arg_int(va_list *ap) { return (long long)va_arg(*ap, int); }
uint64 __printf_va_arg_u64(va_list *ap) { return (uint64)va_arg(*ap, uint64); }
char *__printf_va_arg_str(va_list *ap) { return va_arg(*ap, char *); }

extern int printf_rust(const char *fmt, va_list *ap);

int printf(char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = printf_rust(fmt, &ap);
    va_end(ap);
    return ret;
}
