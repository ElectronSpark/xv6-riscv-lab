#ifndef _XV6_SYS_TYPES_H
#define _XV6_SYS_TYPES_H

#include "stddef.h"
#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int pid_t;
typedef int uid_t;
typedef int gid_t;
typedef long off_t;
typedef unsigned int mode_t;
typedef unsigned long dev_t;
typedef unsigned long ino_t;

#ifdef __cplusplus
}
#endif

#endif /* _XV6_SYS_TYPES_H */
