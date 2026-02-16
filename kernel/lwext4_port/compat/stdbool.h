/**
 * @file stdbool.h
 * @brief stdbool shim for lwext4 in xv6 kernel space.
 *
 * Intercepts lwext4's #include <stdbool.h>.  We provide the standard
 * _Bool-based definitions so that lwext4 internal code (which relies
 * on implicit pointer-to-_Bool conversions) keeps working.
 *
 * Because compat/ is first in the -I path, this header also reaches
 * the ext4fs port files.  When those files later include the VFS
 * headers, 'bool' resolves to _Bool in that translation unit, which
 * is ABI-compatible on RISC-V (both passed in a full register).
 *
 * The kernel's types.h guards its own enum bool with #ifndef bool,
 * so defining bool as a macro here prevents the enum from appearing.
 */
#ifndef _EXT4_COMPAT_STDBOOL_H
#define _EXT4_COMPAT_STDBOOL_H

#define bool  _Bool
#define true  1
#define false 0
#define __bool_true_false_are_defined 1

#endif /* _EXT4_COMPAT_STDBOOL_H */
