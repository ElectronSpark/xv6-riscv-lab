/*
 * sgtty.h — BSD compat shim for ncurses
 *
 * ncurses references sgtty.h in term.priv.h, but musl (correctly) does
 * not ship it. Provide the minimal structures and macros ncurses needs.
 */
#ifndef _SGTTY_H_
#define _SGTTY_H_

#include <sys/ioctl.h>

struct sgttyb {
    char sg_ispeed;
    char sg_ospeed;
    char sg_erase;
    char sg_kill;
    int sg_flags;
};

#ifndef RAW
#define RAW 0x0001
#endif

#ifndef CBREAK
#define CBREAK 0x0002
#endif

#ifndef XTABS
#define XTABS 0x0004
#endif

int gtty(int fd, struct sgttyb *buf);
int stty(int fd, const struct sgttyb *buf);

#ifndef TIOCGETP
#define TIOCGETP TCGETS
#endif

#ifndef TIOCSETP
#define TIOCSETP TCSETS
#endif

#ifndef TIOCSETN
#define TIOCSETN TCSETSW
#endif

#endif /* _SGTTY_H_ */
