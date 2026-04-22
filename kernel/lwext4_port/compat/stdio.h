/**
 * @file stdio.h
 * @brief Minimal stdio.h shim for lwext4 in xv6 kernel space.
 *
 * Shadows the C library stdio.h to avoid type conflicts.
 * Only provides what lwext4 actually needs (snprintf, printf).
 */
#ifndef _EXT4_COMPAT_STDIO_H
#define _EXT4_COMPAT_STDIO_H

#include "types.h"
#include "printf.h"

#include <stdarg.h>

/* snprintf / vsnprintf — used by ext4_debug.c */
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap);

/* stdout / fflush stubs — ext4_debug.h calls fflush(stdout) after prints */
#define stdout ((void *)0)
static inline int fflush(void *stream) { (void)stream; return 0; }

#endif /* _EXT4_COMPAT_STDIO_H */
