#ifndef _XV6_ERRNO_H
#define _XV6_ERRNO_H

#include "kernel/inc/errno.h"

extern int errno;
const char *strerror(int errnum);

#endif /* _XV6_ERRNO_H */
