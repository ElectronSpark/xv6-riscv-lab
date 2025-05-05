#ifndef __KERNEL_TYPES_H
#define __KERNEL_TYPES_H

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
#ifndef bool
typedef enum { false, true } bool;
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
