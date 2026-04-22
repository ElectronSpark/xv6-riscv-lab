#ifndef __USER_ABI_UTSNAME_H
#define __USER_ABI_UTSNAME_H

struct utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
};

#endif /* __USER_ABI_UTSNAME_H */
