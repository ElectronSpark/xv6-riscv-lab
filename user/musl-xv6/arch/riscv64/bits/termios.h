/*
 * bits/termios.h — xv6 terminal I/O definitions for musl
 *
 * IMPORTANT: xv6 uses NCCS=16, musl's generic header uses NCCS=32.
 * We override to match xv6's kernel struct termios layout.
 */

/* c_cc character indices — match xv6 kernel (kernel/inc/tty/termios.h) */
#define VINTR    0
#define VQUIT    1
#define VERASE   2
#define VKILL    3
#define VEOF     4
#define VTIME    5
#define VMIN     6
#define VSWTC    7
#define VSTART   8
#define VSTOP    9
#define VSUSP    10
#define VEOL     11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE  14
#define VLNEXT   15
#define NCCS     16

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
    speed_t c_ispeed;
    speed_t c_ospeed;
};

/* Input mode flags (c_iflag) */
#define IGNBRK  0x001
#define BRKINT  0x002
#define IGNPAR  0x004
#define PARMRK  0x008
#define INPCK   0x010
#define ISTRIP  0x020
#define INLCR   0x040
#define IGNCR   0x080
#define ICRNL   0x100
#define IXON    0x200
#define IXANY   0x400
#define IXOFF   0x800

/* Output mode flags (c_oflag) */
#define OPOST   0x001
#define ONLCR   0x002

/* Control mode flags (c_cflag) */
#define CSIZE   0x0060
#define CS5     0x0000
#define CS6     0x0020
#define CS7     0x0040
#define CS8     0x0060
#define CSTOPB  0x0080
#define CREAD   0x0100
#define PARENB  0x0200
#define PARODD  0x0400
#define HUPCL   0x0800
#define CLOCAL  0x1000

/* Local mode flags (c_lflag) */
#define ISIG    0x001
#define ICANON  0x002
#define ECHO    0x004
#define ECHOE   0x008
#define ECHOK   0x010
#define ECHONL  0x020
#define NOFLSH  0x040
#define TOSTOP  0x080
#define IEXTEN  0x100
#define ECHOCTL 0x200
#define ECHOPRT 0x400
#define ECHOKE  0x800

/* Baud rates */
#define B0      0
#define B50     1
#define B75     2
#define B110    3
#define B134    4
#define B150    5
#define B200    6
#define B300    7
#define B600    8
#define B1200   9
#define B1800   10
#define B2400   11
#define B4800   12
#define B9600   13
#define B19200  14
#define B38400  15
#define B57600  16
#define B115200 17

/* tcsetattr actions */
#define TCSANOW    0
#define TCSADRAIN  1
#define TCSAFLUSH  2

/* tcflow actions */
#define TCOOFF 0
#define TCOON  1
#define TCIOFF 2
#define TCION  3

/* tcflush queue selectors */
#define TCIFLUSH  0
#define TCOFLUSH  1
#define TCIOFLUSH 2

/* ioctl requests for terminal */
#define TCGETS     0x5401
#define TCSETS     0x5402
#define TCSETSW    0x5403
#define TCSETSF    0x5404
#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCSCTTY  0x540E

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};
