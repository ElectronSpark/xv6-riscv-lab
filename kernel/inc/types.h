#ifndef __KERNEL_TYPES_H
#define __KERNEL_TYPES_H

#include "compiler.h"

#ifdef HOST_TEST
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <sys/types.h>

typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef uint8_t  uint8;
typedef uint16_t uint16;
typedef uint32_t uint32;
typedef uint64_t uint64;

typedef int8_t  int8;
typedef int16_t int16;
typedef int32_t int32;
typedef int64_t int64;

typedef uint64_t pde_t;
#ifndef __dev_t_defined
typedef int32_t dev_t;
#endif

#else

typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;

typedef char int8;
typedef short int16;
typedef int  int32;
typedef long int64;

#ifndef __pde_t_defined
#define __pde_t_defined
typedef uint64 pde_t;
#endif

#ifndef __dev_t_defined
#define __dev_t_defined
typedef int32 dev_t;
#endif

#ifndef __size_t_defined
#define __size_t_defined
typedef typeof(sizeof(0)) size_t;
#endif              /* size_t */
#ifndef __ssize_t_defined
#define __ssize_t_defined
typedef typeof(-sizeof(0)) ssize_t;
#endif              /* ssize_t */
#ifndef bool
#if __STDC_VERSION__ < 202311L  // C23 or later
typedef enum { 
    false = 0,
    true = 1
} bool;
#endif
#endif              /* bool */

#endif /* HOST_TEST */

#ifndef NULL
#define NULL ((void*)0)
#endif      /* NULL */

// get the offset of an entry in bytes from its parent type
#ifndef offsetof
#define offsetof(type, member) ({       \
    const type *__pptr = NULL;          \
    (void *)&(__pptr->member) - NULL; })
#endif          /* offsetof */

// given the address of an entry, get the address of its parent type
#ifndef container_of
#define container_of(ptr, type, member) ({                  \
    const typeof(((type *)NULL)->member) *__mptr = (ptr);   \
    (type *)((void *)__mptr - offsetof(type, member)); })
#endif          /* container_of */

#endif      /* __KERNEL_TYPES_H */
