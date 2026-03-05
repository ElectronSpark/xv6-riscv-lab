/*
 * su.c — Switch user for xv6
 *
 * Usage: su [username]     (default: root)
 *        su -c command [username]
 *
 * Authenticates with the target user's password, then starts a new
 * shell (or runs the specified command) as that user.
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
#include <errno.h>

#define PASSWORD_MAX 128

static int read_password(const char *prompt, char *buf, int max)
{
    printf("%s", prompt);
    fflush(stdout);
    int i = 0;
    while (i < max - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0) return -1;
        if (c == '\n' || c == '\r') break;
        if (c == '\b' || c == 127) { if (i > 0) i--; continue; }
        buf[i++] = c;
    }
    buf[i] = '\0';
    printf("\n");
    return i;
}

static int authenticate(const char *username, const char *password)
{
    struct spwd *sp = getspnam(username);
    const char *hash;
    if (sp)
        hash = sp->sp_pwdp;
    else {
        struct passwd *pw = getpwnam(username);
        if (!pw) return -1;
        hash = pw->pw_passwd;
    }
    if (!hash || hash[0] == '\0') return 0;
    if (hash[0] == '!' || hash[0] == '*') return -1;
    if (strcmp(hash, "x") == 0) return -1;
    char *result = crypt(password, hash);
    if (!result) return -1;
    return strcmp(result, hash) == 0 ? 0 : -1;
}

int main(int argc, char **argv)
{
    const char *target = "root";
    const char *command = NULL;
    int i = 1;

    /* Parse options */
    while (i < argc) {
        if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command = argv[++i];
            i++;
        } else if (argv[i][0] != '-') {
            target = argv[i];
            i++;
        } else {
            fprintf(stderr, "su: unknown option %s\n", argv[i]);
            fprintf(stderr, "usage: su [-c command] [username]\n");
            return 1;
        }
    }

    struct passwd *pw = getpwnam(target);
    if (!pw) {
        fprintf(stderr, "su: user %s not found\n", target);
        return 1;
    }

    /* Root doesn't need to authenticate */
    if (getuid() != 0) {
        char password[PASSWORD_MAX];
        if (read_password("Password: ", password, PASSWORD_MAX) < 0)
            return 1;
        if (authenticate(target, password) < 0) {
            memset(password, 0, sizeof(password));
            fprintf(stderr, "su: authentication failure\n");
            return 1;
        }
        memset(password, 0, sizeof(password));
    }

    /* Set supplementary groups */
    initgroups(pw->pw_name, pw->pw_gid);

    /* Switch credentials */
    if (setgid(pw->pw_gid) < 0) { perror("su: setgid"); return 1; }
    if (setuid(pw->pw_uid) < 0) { perror("su: setuid"); return 1; }

    /* Set environment */
    setenv("HOME", pw->pw_dir, 1);
    setenv("USER", pw->pw_name, 1);
    setenv("LOGNAME", pw->pw_name, 1);
    setenv("SHELL", pw->pw_shell, 1);

    if (chdir(pw->pw_dir) < 0)
        chdir("/");

    if (command) {
        /* Run single command */
        char *cmd_argv[] = { "sh", "-c", (char *)command, NULL };
        const char *shell = pw->pw_shell;
        if (!shell || shell[0] == '\0') shell = "/bin/sh";
        execv(shell, cmd_argv);
        perror("su: exec");
        return 1;
    }

    /* Start interactive shell */
    const char *shell = pw->pw_shell;
    if (!shell || shell[0] == '\0') shell = "/bin/sh";

    const char *base = strrchr(shell, '/');
    base = base ? base + 1 : shell;

    char argv0[64];
    snprintf(argv0, sizeof(argv0), "-%s", base);

    char *shell_argv[] = { argv0, NULL };
    execv(shell, shell_argv);
    perror("su: exec");
    return 1;
}
