/*
 * reset.c — Reset terminal to sane defaults for xv6
 *
 * Equivalent to "stty sane" — restores canonical mode, echo, ONLCR, etc.
 */

#include <stdio.h>
#include <termios.h>
#include <unistd.h>

int main(void) {
    struct termios t;
    int fd = STDIN_FILENO;

    if (tcgetattr(fd, &t) < 0) {
        /* Terminal might be very messed up, write directly */
        const char msg[] = "reset: tcgetattr failed\r\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        return 1;
    }

    t.c_iflag = ICRNL | IXON;
    t.c_oflag = OPOST | ONLCR;
    t.c_cflag = CS8 | CREAD;
    t.c_lflag = ICANON | ECHO | ECHOE | ECHOK | ISIG;
    t.c_cc[VEOF]   = 4;   /* ^D */
    t.c_cc[VEOL]   = 0;
    t.c_cc[VERASE] = 127; /* DEL */
    t.c_cc[VINTR]  = 3;   /* ^C */
    t.c_cc[VKILL]  = 21;  /* ^U */
    t.c_cc[VMIN]   = 1;
    t.c_cc[VTIME]  = 0;
    t.c_cc[VQUIT]  = 28;  /* ^\ */
    t.c_cc[VSUSP]  = 26;  /* ^Z */
    t.c_cc[VSTART] = 17;  /* ^Q */
    t.c_cc[VSTOP]  = 19;  /* ^S */

    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        const char msg[] = "reset: tcsetattr failed\r\n";
        write(STDERR_FILENO, msg, sizeof(msg) - 1);
        return 1;
    }

    return 0;
}
