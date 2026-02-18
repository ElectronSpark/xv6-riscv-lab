/*
 * termios.h - Terminal I/O interface definitions
 *
 * Simplified termios implementation for xv6.
 * Supports raw/canonical mode switching for shell line editing.
 */

#ifndef __KERNEL_TERMIOS_H
#define __KERNEL_TERMIOS_H

#include "types.h"

/* c_cc characters */
#define VINTR 0  /* Interrupt character (^C) - ASCII: 0x03 */
#define VQUIT 1  /* Quit character (^\) - ASCII: 0x1C */
#define VERASE 2 /* Erase character (^H/DEL) - ASCII: 0x08/0x7F */
#define VKILL 3  /* Kill line character (^U) - ASCII: 0x15 */
#define VEOF 4   /* EOF character (^D) - ASCII: 0x04 */
#define VTIME 5  /* Timeout for non-canonical read */
#define VMIN 6   /* Minimum chars for non-canonical read */
#define VSTART 7 /* Start character (^Q) - ASCII: 0x11 */
#define VSTOP 8  /* Stop character (^S) - ASCII: 0x13 */
#define VSUSP 9  /* Suspend character (^Z) - ASCII: 0x1A */
#define VEOL 10  /* End of line - ASCII: 0x00 */
#define NCCS 16  /* Size of c_cc array */

/* c_iflag bits */
#define IGNBRK 0x0001 /* Ignore break condition */
#define BRKINT 0x0002 /* Signal interrupt on break */
#define IGNPAR 0x0004 /* Ignore parity errors */
#define PARMRK 0x0008 /* Mark parity errors */
#define INPCK 0x0010  /* Enable input parity check */
#define ISTRIP 0x0020 /* Strip 8th bit */
#define INLCR 0x0040  /* Translate NL to CR */
#define IGNCR 0x0080  /* Ignore CR */
#define ICRNL 0x0100  /* Translate CR to NL */
#define IXON 0x0200   /* Enable start/stop output control */
#define IXOFF 0x0400  /* Enable start/stop input control */

/* c_oflag bits */
#define OPOST 0x0001 /* Post-process output */
#define ONLCR 0x0002 /* Map NL to CR-NL */

/* c_cflag bits (simplified) */
#define CSIZE 0x0030  /* Character size mask */
#define CS5 0x0000    /* 5 bits */
#define CS6 0x0010    /* 6 bits */
#define CS7 0x0020    /* 7 bits */
#define CS8 0x0030    /* 8 bits */
#define CREAD 0x0080  /* Enable receiver */
#define CLOCAL 0x0800 /* Ignore modem status lines */

/* c_lflag bits */
#define ISIG 0x0001   /* Enable signals (INTR, QUIT, SUSP) */
#define ICANON 0x0002 /* Canonical mode (line buffering) */
#define ECHO 0x0008   /* Echo input characters */
#define ECHOE 0x0010  /* Echo ERASE as backspace-space-backspace */
#define ECHOK 0x0020  /* Echo NL after KILL */
#define ECHONL 0x0040 /* Echo NL even if ECHO is off */
#define IEXTEN 0x0100 /* Enable extended input processing */

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

/* Speed values (baud rates) - simplified */
#define B0 0
#define B9600 9600
#define B19200 19200
#define B38400 38400
#define B57600 57600
#define B115200 115200

typedef uint32 tcflag_t;
typedef uint8 cc_t;
typedef uint32 speed_t;

struct termios {
    tcflag_t c_iflag; /* Input mode flags */
    tcflag_t c_oflag; /* Output mode flags */
    tcflag_t c_cflag; /* Control mode flags */
    tcflag_t c_lflag; /* Local mode flags */
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
#define TIOCGPTN  0x80045430 /* Get PTY slave number */

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

#endif /* __KERNEL_TERMIOS_H */
