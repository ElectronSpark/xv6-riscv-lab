#ifndef __XV6_USER_DEFINES_H
#define __XV6_USER_DEFINES_H

#include "stddef.h"
#include "stdint.h"
#include "sys/types.h"
#include "sys/stat.h"
#include "fcntl.h"
#include "unistd.h"
#include "signal.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "errno.h"
#include "time.h"

#ifdef __cplusplus
extern "C" {
#endif

char* gets(char*, int max);

#ifdef __cplusplus
}
#endif

#endif              /* __XV6_USER_DEFINES_H */
