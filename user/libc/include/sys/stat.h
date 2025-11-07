#ifndef _XV6_SYS_STAT_H
#define _XV6_SYS_STAT_H

#include "kernel/inc/stat.h"
#include "sys/types.h"

#ifdef __cplusplus
extern "C" {
#endif

int stat(const char *path, struct stat *buf);

#ifdef __cplusplus
}
#endif

#endif /* _XV6_SYS_STAT_H */
