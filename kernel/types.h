#ifndef __KERNEL_TYPES_H
#define __KERNEL_TYPES_H

typedef unsigned int   uint;
typedef unsigned short ushort;
typedef unsigned char  uchar;

typedef unsigned char uint8;
typedef unsigned short uint16;
typedef unsigned int  uint32;
typedef unsigned long uint64;

typedef uint64 pde_t;

typedef typeof(sizeof(0)) size_t;

#ifndef NULL
#define NULL ((void*)0)
#endif      /* NULL */

#endif      /* __KERNEL_TYPES_H */
