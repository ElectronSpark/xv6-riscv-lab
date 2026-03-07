#ifndef _LINUX_LIMITS_H
#define _LINUX_LIMITS_H

#include <limits.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef XATTR_SIZE_MAX
#define XATTR_SIZE_MAX 65536
#endif

#ifndef XATTR_LIST_MAX
#define XATTR_LIST_MAX 65536
#endif

#endif
