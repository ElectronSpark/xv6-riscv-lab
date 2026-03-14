/*
 * stty.c — Minimal stty for xv6
 *
 * Usage:
 *   stty           Print current terminal settings
 *   stty sane      Restore sane terminal defaults
 *   stty raw       Set raw mode
 *   stty cooked    Set cooked (canonical) mode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

static void print_flag(const char *name, int val) {
    if (!val)
        printf("-%s ", name);
    else
        printf("%s ", name);
}

static void print_settings(struct termios *t) {
    printf("iflag: ");
    print_flag("icrnl",  t->c_iflag & ICRNL);
    print_flag("inlcr",  t->c_iflag & INLCR);
    print_flag("igncr",  t->c_iflag & IGNCR);
    print_flag("ixon",   t->c_iflag & IXON);
    print_flag("ixoff",  t->c_iflag & IXOFF);
    printf("\n");

    printf("oflag: ");
    print_flag("opost",  t->c_oflag & OPOST);
    print_flag("onlcr",  t->c_oflag & ONLCR);
    printf("\n");

    printf("cflag: ");
    print_flag("cs8", (t->c_cflag & CSIZE) == CS8);
    print_flag("cread",  t->c_cflag & CREAD);
    printf("\n");

    printf("lflag: ");
    print_flag("icanon", t->c_lflag & ICANON);
    print_flag("echo",   t->c_lflag & ECHO);
    print_flag("echoe",  t->c_lflag & ECHOE);
    print_flag("echok",  t->c_lflag & ECHOK);
    print_flag("isig",   t->c_lflag & ISIG);
    printf("\n");

    printf("cc: VMIN=%d VTIME=%d\n", t->c_cc[VMIN], t->c_cc[VTIME]);
}

static void set_sane(struct termios *t) {
    t->c_iflag = ICRNL | IXON;
    t->c_oflag = OPOST | ONLCR;
    t->c_cflag = CS8 | CREAD;
    t->c_lflag = ICANON | ECHO | ECHOE | ECHOK | ISIG;
    t->c_cc[VEOF]   = 4;   /* ^D */
    t->c_cc[VEOL]   = 0;
    t->c_cc[VERASE] = 127; /* DEL */
    t->c_cc[VINTR]  = 3;   /* ^C */
    t->c_cc[VKILL]  = 21;  /* ^U */
    t->c_cc[VMIN]   = 1;
    t->c_cc[VTIME]  = 0;
    t->c_cc[VQUIT]  = 28;  /* ^\ */
    t->c_cc[VSUSP]  = 26;  /* ^Z */
    t->c_cc[VSTART] = 17;  /* ^Q */
    t->c_cc[VSTOP]  = 19;  /* ^S */
}

int main(int argc, char *argv[]) {
    struct termios t;
    int fd = STDIN_FILENO;

    if (!isatty(fd)) {
        fprintf(stderr, "stty: stdin is not a terminal\n");
        return 1;
    }

    if (tcgetattr(fd, &t) < 0) {
        perror("tcgetattr");
        return 1;
    }

    if (argc == 1) {
        print_settings(&t);
        return 0;
    }

    if (strcmp(argv[1], "sane") == 0) {
        set_sane(&t);
    } else if (strcmp(argv[1], "raw") == 0) {
        cfmakeraw(&t);
    } else if (strcmp(argv[1], "cooked") == 0) {
        t.c_iflag |= ICRNL | IXON;
        t.c_oflag |= OPOST | ONLCR;
        t.c_lflag |= ICANON | ECHO | ECHOE | ECHOK | ISIG;
    } else {
        fprintf(stderr, "usage: stty [sane|raw|cooked]\n");
        return 1;
    }

    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        perror("tcsetattr");
        return 1;
    }

    return 0;
}
