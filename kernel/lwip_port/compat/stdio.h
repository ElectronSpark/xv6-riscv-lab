/**
 * @file stdio.h
 * @brief Minimal stdio.h shim for lwIP in xv6 kernel space.
 *
 * This shadows newlib's stdio.h to avoid type conflicts.
 * Only provides what lwIP actually needs (snprintf).
 */
#ifndef _LWIP_COMPAT_STDIO_H
#define _LWIP_COMPAT_STDIO_H

#include "types.h"
#include "printf.h"

#include <stdarg.h>

/* snprintf — used by mem.c for sanity-check error messages.
 * We provide a minimal implementation in sys_arch.c. */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

#endif /* _LWIP_COMPAT_STDIO_H */
