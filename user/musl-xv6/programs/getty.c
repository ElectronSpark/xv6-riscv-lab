/*
 * getty.c — Open a TTY and spawn login for xv6
 *
 * Usage: getty <tty_device>
 *        getty /dev/console
 *        getty /dev/pts/0
 *
 * Opens the specified TTY, sets it as stdin/stdout/stderr and
 * controlling terminal, prints a banner, then exec's /bin/login.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* xv6 ioctl for setting controlling terminal */
#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

int main(int argc, char **argv)
{
    const char *tty = "/dev/console";

    if (argc >= 2)
        tty = argv[1];

    /* Create a new session (detach from any controlling terminal) */
    setsid();

    /* Close inherited fds */
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    /* Open the TTY as stdin */
    int fd = open(tty, O_RDWR);
    if (fd < 0) {
        /* Can't print error — no stdout */
        _exit(1);
    }

    /* Ensure fd 0 */
    if (fd != STDIN_FILENO) {
        dup2(fd, STDIN_FILENO);
        close(fd);
    }

    /* stdout and stderr */
    dup2(STDIN_FILENO, STDOUT_FILENO);
    dup2(STDIN_FILENO, STDERR_FILENO);

    /* Set controlling terminal */
    ioctl(STDIN_FILENO, TIOCSCTTY, (void *)0);

    /* Print banner */
    printf("\nxv6 operating system (%s)\n\n", tty);
    fflush(stdout);

    /* Exec login — auto-login as root on UART console */
    char *login_argv[] = { "login", "-f", "root", NULL };
    execv("/bin/login", login_argv);

    /* If login is not available, fall back to shell */
    fprintf(stderr, "getty: exec /bin/login failed, falling back to /bin/sh\n");
    char *sh_argv[] = { "sh", NULL };
    execv("/bin/sh", sh_argv);

    _exit(1);
}
