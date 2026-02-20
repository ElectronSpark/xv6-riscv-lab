/*
 * bits/ioctl.h — xv6 ioctl request codes for musl
 *
 * Terminal ioctls match the kernel's definitions.
 */

#define TCGETS       0x5401
#define TCSETS       0x5402
#define TCSETSW      0x5403
#define TCSETSF      0x5404
#define TIOCGWINSZ   0x5413
#define TIOCSWINSZ   0x5414
#define TIOCGPGRP    0x540F
#define TIOCSPGRP    0x5410
#define TIOCSCTTY    0x540E
#define TIOCNOTTY    0x5422
#define TIOCGPTN     0x80045430
#define TIOCSPTLCK   0x40045431
#define FIOCLEX      0x5451
#define FIONCLEX     0x5450
#define FIONREAD     0x541B
#define FIONBIO      0x5421
#define FIOASYNC     0x5452
