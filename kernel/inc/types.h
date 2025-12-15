#ifndef __KERNEL_TYPES_H
#define __KERNEL_TYPES_H

#include "compiler.h"

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

typedef uint64 pde_t;

#ifndef size_t
typedef typeof(sizeof(0)) size_t;
#endif              /* size_t */

#if !defined(mode_t)
typedef uint32 mode_t;
#endif             /* mode_t */

#if !defined(ON_HOST_OS)
    #if !defined(ssize_t)
    typedef typeof(-sizeof(int)) ssize_t;
    #endif              /* ssize_t */
    #if !defined(loff_t)
    typedef long long loff_t;
    #endif              /* loff_t */
    #if !defined(dev_t)
    typedef uint32 dev_t;
    #endif              /* dev_t */
#endif // ON_HOST_OS

#ifndef bool
#if __STDC_VERSION__ < 202311L  // C23 or later
typedef enum { 
    false = 0,
    true = 1
} bool;
#endif
#endif              /* bool */

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
