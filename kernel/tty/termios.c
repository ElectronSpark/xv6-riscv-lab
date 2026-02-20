/*
 * termios.c - Terminal I/O settings management
 *
 * Provides default termios initialization and helpers for applying
 * termios changes to a TTY.
 */

#include "types.h"
#include "tty/termios.h"
#include "tty/tty_types.h"

/*
 * termios_init_default - fill a termios struct with sane defaults
 *
 * Sets up canonical mode with echo, CR-to-NL input mapping,
 * NL-to-CRNL output mapping, standard control characters, and
 * 115200 baud.  This matches the typical Linux console defaults.
 */
void termios_init_default(struct termios *t) {
    /* Input: map CR→NL, enable XON/XOFF */
    t->c_iflag = ICRNL | IXON;

    /* Output: post-process, map NL→CRNL */
    t->c_oflag = OPOST | ONLCR;

    /* Control: 8-bit chars, receiver on, local line */
    t->c_cflag = CS8 | CREAD | CLOCAL;

    /* Local: canonical, echo, signals, erase echo */
    t->c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;

    /* Control characters */
    t->c_cc[VINTR] = 0x03;  /* ^C  */
    t->c_cc[VQUIT] = 0x1C;  /* ^\  */
    t->c_cc[VERASE] = 0x7F; /* DEL */
    t->c_cc[VKILL] = 0x15;  /* ^U  */
    t->c_cc[VEOF] = 0x04;   /* ^D  */
    t->c_cc[VTIME] = 0;
    t->c_cc[VMIN] = 1;
    t->c_cc[VSWTC] = 0x00;  /* disabled */
    t->c_cc[VSTART] = 0x11; /* ^Q  */
    t->c_cc[VSTOP] = 0x13;  /* ^S  */
    t->c_cc[VSUSP] = 0x1A;  /* ^Z  */
    t->c_cc[VEOL] = 0x00;
    t->c_cc[VREPRINT] = 0x12; /* ^R */
    t->c_cc[VDISCARD] = 0x0F; /* ^O */
    t->c_cc[VWERASE] = 0x17;  /* ^W */
    t->c_cc[VLNEXT] = 0x16;   /* ^V */

    /* Default baud rate */
    t->c_ispeed = B115200;
    t->c_ospeed = B115200;
}
