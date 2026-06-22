/*
 * termios.h - Terminal I/O interface definitions
 *
 * Simplified termios implementation for xv6.
 * Supports raw/canonical mode switching for shell line editing.
 */

#ifndef __USER_ABI_TERMIOS_H
#define __USER_ABI_TERMIOS_H

#include "types.h"

/* c_cc characters — must match Linux generic and musl indices. */
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16
#define NCCS 32

/* c_iflag bits — Linux generic values. */
#define IGNBRK  0x0001
#define BRKINT  0x0002
#define IGNPAR  0x0004
#define PARMRK  0x0008
#define INPCK   0x0010
#define ISTRIP  0x0020
#define INLCR   0x0040
#define IGNCR   0x0080
#define ICRNL   0x0100
#define IUCLC   0x0200
#define IXON    0x0400
#define IXANY   0x0800
#define IXOFF   0x1000
#define IMAXBEL 0x2000
#define IUTF8   0x4000

/* c_oflag bits — Linux generic values. */
#define OPOST  0x0001
#define OLCUC  0x0002
#define ONLCR  0x0004
#define OCRNL  0x0008
#define ONOCR  0x0010
#define ONLRET 0x0020

/* c_cflag bits — Linux generic values. */
#define CSIZE  0x0030
#define CS5    0x0000
#define CS6    0x0010
#define CS7    0x0020
#define CS8    0x0030
#define CSTOPB 0x0040
#define CREAD  0x0080
#define PARENB 0x0100
#define PARODD 0x0200
#define HUPCL  0x0400
#define CLOCAL 0x0800

/* c_lflag bits — Linux generic values. */
#define ISIG    0x0001
#define ICANON  0x0002
#define XCASE   0x0004
#define ECHO    0x0008
#define ECHOE   0x0010
#define ECHOK   0x0020
#define ECHONL  0x0040
#define NOFLSH  0x0080
#define TOSTOP  0x0100
#define ECHOCTL 0x0200
#define ECHOPRT 0x0400
#define ECHOKE  0x0800
#define IEXTEN  0x8000

/* tcsetattr optional_actions */
#define TCSANOW 0   /* Change immediately */
#define TCSADRAIN 1 /* Change after all output written */
#define TCSAFLUSH 2 /* Change after output written, discard input */

/* tcflush queue_selector */
#define TCIFLUSH 0  /* Flush input queue */
#define TCOFLUSH 1  /* Flush output queue */
#define TCIOFLUSH 2 /* Flush both queues */

/* tcflow action */
#define TCOOFF 0 /* Suspend output */
#define TCOON 1  /* Resume output */
#define TCIOFF 2 /* Transmit STOP character */
#define TCION 3  /* Transmit START character */

/* Speed values — Linux generic enumeration. */
#define B0      0x0000
#define B50     0x0001
#define B75     0x0002
#define B110    0x0003
#define B134    0x0004
#define B150    0x0005
#define B200    0x0006
#define B300    0x0007
#define B600    0x0008
#define B1200   0x0009
#define B1800   0x000A
#define B2400   0x000B
#define B4800   0x000C
#define B9600   0x000D
#define B19200  0x000E
#define B38400  0x000F
#define B57600  0x1001
#define B115200 0x1002

typedef uint32 tcflag_t;
typedef uint8 cc_t;
typedef uint32 speed_t;

struct termios {
    tcflag_t c_iflag; /* Input mode flags */
    tcflag_t c_oflag; /* Output mode flags */
    tcflag_t c_cflag; /* Control mode flags */
    tcflag_t c_lflag; /* Local mode flags */
    cc_t c_line;      /* Line discipline */
    cc_t c_cc[NCCS];  /* Control characters */
    speed_t c_ispeed; /* Input speed */
    speed_t c_ospeed; /* Output speed */
};

/* ioctl requests for terminal */
#define TCGETS 0x5401     /* Get termios struct */
#define TCSETS 0x5402     /* Set termios struct (TCSANOW) */
#define TCSETSW 0x5403    /* Set termios struct (TCSADRAIN) */
#define TCSETSF 0x5404    /* Set termios struct (TCSAFLUSH) */
#define TIOCGWINSZ 0x5413 /* Get window size */
#define TIOCSWINSZ 0x5414 /* Set window size */
#define TIOCGPGRP 0x540F  /* Get foreground process group */
#define TIOCSPGRP 0x5410  /* Set foreground process group */
#define TIOCSCTTY 0x540E  /* Set controlling terminal */
#define TIOCGPTN   0x80045430 /* Get PTY slave number */
#define TIOCSPTLCK 0x40045431 /* Lock/unlock PTY slave */
#define TIOCGPTPEER 0x5441    /* Safely open PTY slave */
#define TIOCNOTTY  0x5422     /* Detach from controlling terminal */

/* Window size structure */
struct winsize {
    uint16 ws_row;    /* Rows, in characters */
    uint16 ws_col;    /* Columns, in characters */
    uint16 ws_xpixel; /* Horizontal size, in pixels */
    uint16 ws_ypixel; /* Vertical size, in pixels */
};

/* Default terminal size */
#define DEFAULT_ROWS 24
#define DEFAULT_COLS 80

/* cfmakeraw helper - sets raw mode flags */
static inline void cfmakeraw(struct termios *t) {
    t->c_iflag &=
        ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t->c_oflag &= ~OPOST;
    t->c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);
    t->c_cflag &= ~(CSIZE);
    t->c_cflag |= CS8;
    t->c_cc[VMIN] = 1;
    t->c_cc[VTIME] = 0;
}

#endif /* __USER_ABI_TERMIOS_H */
