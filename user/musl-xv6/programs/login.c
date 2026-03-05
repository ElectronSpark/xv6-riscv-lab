/*
 * login.c — Multi-user login program for xv6
 *
 * Prompts for username and password, authenticates against /etc/passwd
 * and /etc/shadow, then drops privileges and exec's the user's shell.
 *
 * Usage: login [-f username]   (called by getty/telnetd)
 *        login                 (interactive prompt)
 *
 * -f username: skip authentication (auto-login), only root can use this.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <shadow.h>
#include <crypt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define MAX_LOGIN_ATTEMPTS 3
#define USERNAME_MAX       64
#define PASSWORD_MAX       128

/* Read a line from stdin, strip trailing newline. Returns length or -1. */
static int read_line(char *buf, int max)
{
    int i = 0;
    while (i < max - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0)
            return -1;
        if (c == '\n' || c == '\r')
            break;
        if (c == '\b' || c == 127) {  /* backspace/DEL */
            if (i > 0) {
                i--;
                write(STDOUT_FILENO, "\b \b", 3);
            }
            continue;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    return i;
}

/* Read password with echo disabled (best-effort, no termios available
 * in all xv6 builds — we just avoid echoing by not reading char-by-char). */
static int read_password(char *buf, int max)
{
    /* Disable echo — write escape sequence to hide input */
    /* Since we may not have full termios, just read without echoing */
    int i = 0;
    while (i < max - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0)
            return -1;
        if (c == '\n' || c == '\r')
            break;
        if (c == '\b' || c == 127) {
            if (i > 0)
                i--;
            continue;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    write(STDOUT_FILENO, "\n", 1);
    return i;
}

/* Authenticate user against /etc/shadow (or /etc/passwd if no shadow). */
static int authenticate(const char *username, const char *password)
{
    struct spwd *sp = getspnam(username);
    const char *hash;

    if (sp != NULL) {
        hash = sp->sp_pwdp;
    } else {
        /* Fall back to /etc/passwd password field */
        struct passwd *pw = getpwnam(username);
        if (pw == NULL)
            return -1;
        hash = pw->pw_passwd;
    }

    /* Empty hash = no password required */
    if (hash == NULL || hash[0] == '\0')
        return 0;

    /* Locked account (starts with ! or *) */
    if (hash[0] == '!' || hash[0] == '*')
        return -1;

    /* 'x' means password is in shadow — but we already tried shadow */
    if (strcmp(hash, "x") == 0) {
        /* No shadow entry and passwd says 'x' = locked */
        return -1;
    }

    char *result = crypt(password, hash);
    if (result == NULL)
        return -1;

    return strcmp(result, hash) == 0 ? 0 : -1;
}

/* Set up environment for the user */
static void setup_env(const struct passwd *pw)
{
    clearenv();
    setenv("HOME", pw->pw_dir, 1);
    setenv("SHELL", pw->pw_shell, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("PATH", "/bin:/usr/bin", 1);
    setenv("TERM", "vt100", 1);
}

int main(int argc, char **argv)
{
    char username[USERNAME_MAX];
    char password[PASSWORD_MAX];
    int autologin = 0;

    /* Parse -f flag (auto-login, root only) */
    if (argc >= 3 && strcmp(argv[1], "-f") == 0) {
        if (getuid() != 0) {
            fprintf(stderr, "login: -f requires root\n");
            return 1;
        }
        strncpy(username, argv[2], USERNAME_MAX - 1);
        username[USERNAME_MAX - 1] = '\0';
        autologin = 1;
    }

    int attempts = 0;

    while (attempts < MAX_LOGIN_ATTEMPTS) {
        if (!autologin) {
            printf("xv6 login: ");
            fflush(stdout);
            if (read_line(username, USERNAME_MAX) < 0)
                return 1;

            if (username[0] == '\0')
                continue;

            printf("Password: ");
            fflush(stdout);
            if (read_password(password, PASSWORD_MAX) < 0)
                return 1;
        }

        /* Look up user */
        struct passwd *pw = getpwnam(username);
        if (pw == NULL) {
            fprintf(stderr, "Login incorrect\n");
            attempts++;
            memset(password, 0, sizeof(password));
            continue;
        }

        /* Authenticate (skip if autologin) */
        if (!autologin) {
            if (authenticate(username, password) < 0) {
                fprintf(stderr, "Login incorrect\n");
                memset(password, 0, sizeof(password));
                attempts++;
                continue;
            }
        }

        /* Clear password from memory */
        memset(password, 0, sizeof(password));

        /* Set supplementary groups */
        initgroups(pw->pw_name, pw->pw_gid);

        /* Drop privileges: set gid first, then uid */
        if (setgid(pw->pw_gid) < 0) {
            perror("login: setgid");
            return 1;
        }
        if (setuid(pw->pw_uid) < 0) {
            perror("login: setuid");
            return 1;
        }

        /* Change to home directory */
        if (chdir(pw->pw_dir) < 0) {
            /* Fall back to / */
            chdir("/");
        }

        /* Set up environment */
        setup_env(pw);

        /* Print login message */
        printf("\nWelcome to xv6!\n");
        printf("Logged in as %s (uid=%d)\n\n", pw->pw_name, pw->pw_uid);

        /* Exec user's shell */
        const char *shell = pw->pw_shell;
        if (shell == NULL || shell[0] == '\0')
            shell = "/bin/sh";

        /* Use -shell as argv[0] to indicate login shell */
        const char *shell_basename = strrchr(shell, '/');
        if (shell_basename)
            shell_basename++;
        else
            shell_basename = shell;

        char login_argv0[64];
        snprintf(login_argv0, sizeof(login_argv0), "-%s", shell_basename);

        char *shell_argv[] = { login_argv0, NULL };
        execv(shell, shell_argv);

        /* If exec fails, try /bin/sh */
        fprintf(stderr, "login: exec %s failed\n", shell);
        shell_argv[0] = "-sh";
        execv("/bin/sh", shell_argv);
        return 1;
    }

    fprintf(stderr, "Too many login failures\n");
    return 1;
}
