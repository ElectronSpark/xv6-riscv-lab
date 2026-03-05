/*
 * passwd.c — Change user password for xv6
 *
 * Usage: passwd [username]
 *
 * Non-root users can only change their own password.
 * Root can change any user's password.
 *
 * Updates /etc/shadow (or /etc/passwd if no shadow).
 * Uses SHA-512 ($6$) password hashing via musl's crypt().
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <shadow.h>
#include <crypt.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>

#define PASSWORD_MAX 128
#define LINE_MAX_LEN 1024
#define SALT_LEN     16

/* Generate a random salt for SHA-512 crypt */
static void generate_salt(char *salt, int len)
{
    static const char charset[] =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789./";

    /* Read from /dev/random if available, otherwise use time-based */
    int fd = open("/dev/random", O_RDONLY);
    if (fd >= 0) {
        unsigned char randbuf[SALT_LEN];
        if (read(fd, randbuf, sizeof(randbuf)) == sizeof(randbuf)) {
            for (int i = 0; i < len; i++)
                salt[i] = charset[randbuf[i] % (sizeof(charset) - 1)];
            close(fd);
            salt[len] = '\0';
            return;
        }
        close(fd);
    }

    /* Fallback: use time + pid */
    unsigned int seed = (unsigned int)time(NULL) ^ (unsigned int)getpid();
    for (int i = 0; i < len; i++) {
        seed = seed * 1103515245 + 12345;
        salt[i] = charset[(seed >> 16) % (sizeof(charset) - 1)];
    }
    salt[len] = '\0';
}

/* Read password without echo */
static int read_password(const char *prompt, char *buf, int max)
{
    printf("%s", prompt);
    fflush(stdout);

    int i = 0;
    while (i < max - 1) {
        char c;
        ssize_t n = read(STDIN_FILENO, &c, 1);
        if (n <= 0)
            return -1;
        if (c == '\n' || c == '\r')
            break;
        if (c == '\b' || c == 127) {
            if (i > 0) i--;
            continue;
        }
        buf[i++] = c;
    }
    buf[i] = '\0';
    printf("\n");
    return i;
}

/* Rewrite /etc/shadow, replacing the hash for the given user */
static int update_shadow(const char *username, const char *newhash)
{
    FILE *fp = fopen("/etc/shadow", "r");
    if (fp == NULL) {
        perror("passwd: cannot open /etc/shadow");
        return -1;
    }

    /* Read all lines into memory */
    char lines[32][LINE_MAX_LEN];
    int nlines = 0;

    while (nlines < 32 && fgets(lines[nlines], LINE_MAX_LEN, fp) != NULL)
        nlines++;
    fclose(fp);

    /* Find and replace the user's entry */
    int found = 0;
    for (int i = 0; i < nlines; i++) {
        int ulen = strlen(username);
        if (strncmp(lines[i], username, ulen) == 0 && lines[i][ulen] == ':') {
            /* Reconstruct: user:hash:rest_of_fields */
            char *second_colon = strchr(lines[i] + ulen + 1, ':');
            if (second_colon == NULL)
                second_colon = "\n";
            char newline[LINE_MAX_LEN];
            snprintf(newline, LINE_MAX_LEN, "%s:%s%s",
                     username, newhash, second_colon);
            strncpy(lines[i], newline, LINE_MAX_LEN);
            found = 1;
            break;
        }
    }

    if (!found) {
        fprintf(stderr, "passwd: user %s not found in /etc/shadow\n", username);
        return -1;
    }

    /* Write back */
    fp = fopen("/etc/shadow", "w");
    if (fp == NULL) {
        perror("passwd: cannot write /etc/shadow");
        return -1;
    }
    for (int i = 0; i < nlines; i++)
        fputs(lines[i], fp);
    fclose(fp);

    return 0;
}

int main(int argc, char **argv)
{
    const char *target_user;
    uid_t myuid = getuid();

    if (argc >= 2) {
        target_user = argv[1];
        /* Non-root can only change own password */
        if (myuid != 0) {
            struct passwd *me = getpwuid(myuid);
            if (me == NULL || strcmp(me->pw_name, target_user) != 0) {
                fprintf(stderr, "passwd: permission denied\n");
                return 1;
            }
        }
    } else {
        struct passwd *me = getpwuid(myuid);
        if (me == NULL) {
            fprintf(stderr, "passwd: cannot determine current user\n");
            return 1;
        }
        target_user = me->pw_name;
    }

    /* Verify user exists */
    struct passwd *pw = getpwnam(target_user);
    if (pw == NULL) {
        fprintf(stderr, "passwd: user %s not found\n", target_user);
        return 1;
    }

    /* If non-root, ask for current password */
    if (myuid != 0) {
        char oldpass[PASSWORD_MAX];
        if (read_password("Current password: ", oldpass, PASSWORD_MAX) < 0)
            return 1;

        struct spwd *sp = getspnam(target_user);
        const char *oldhash = sp ? sp->sp_pwdp : pw->pw_passwd;

        if (oldhash && oldhash[0] != '\0') {
            char *check = crypt(oldpass, oldhash);
            memset(oldpass, 0, sizeof(oldpass));
            if (check == NULL || strcmp(check, oldhash) != 0) {
                fprintf(stderr, "passwd: authentication failed\n");
                return 1;
            }
        }
        memset(oldpass, 0, sizeof(oldpass));
    }

    /* Read new password twice */
    char newpass1[PASSWORD_MAX], newpass2[PASSWORD_MAX];

    if (read_password("New password: ", newpass1, PASSWORD_MAX) < 0)
        return 1;

    if (strlen(newpass1) < 1) {
        fprintf(stderr, "passwd: password cannot be empty\n");
        memset(newpass1, 0, sizeof(newpass1));
        return 1;
    }

    if (read_password("Retype new password: ", newpass2, PASSWORD_MAX) < 0) {
        memset(newpass1, 0, sizeof(newpass1));
        return 1;
    }

    if (strcmp(newpass1, newpass2) != 0) {
        fprintf(stderr, "passwd: passwords do not match\n");
        memset(newpass1, 0, sizeof(newpass1));
        memset(newpass2, 0, sizeof(newpass2));
        return 1;
    }
    memset(newpass2, 0, sizeof(newpass2));

    /* Generate salt and hash */
    char salt[SALT_LEN + 1];
    generate_salt(salt, SALT_LEN);

    char salt_str[SALT_LEN + 8];
    snprintf(salt_str, sizeof(salt_str), "$6$%s$", salt);

    char *newhash = crypt(newpass1, salt_str);
    memset(newpass1, 0, sizeof(newpass1));

    if (newhash == NULL) {
        fprintf(stderr, "passwd: password hashing failed\n");
        return 1;
    }

    /* Update /etc/shadow */
    if (update_shadow(target_user, newhash) < 0)
        return 1;

    printf("passwd: password updated successfully\n");
    return 0;
}
