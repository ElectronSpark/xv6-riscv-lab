/*
 * useradd.c — Add a new user account for xv6
 *
 * Usage: useradd [-u uid] [-g gid] [-d homedir] [-s shell] username
 *
 * Creates entries in /etc/passwd, /etc/shadow, and /etc/group.
 * Creates the user's home directory.
 * Requires root privileges.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>

#define LINE_MAX_LEN 1024

/* Find the next available UID >= 1000 */
static uid_t next_uid(void)
{
    uid_t maxuid = 999;
    struct passwd *pw;
    setpwent();
    while ((pw = getpwent()) != NULL) {
        if (pw->pw_uid >= 1000 && pw->pw_uid > maxuid)
            maxuid = pw->pw_uid;
    }
    endpwent();
    return maxuid + 1;
}

/* Find the next available GID >= 1000 */
static gid_t next_gid(void)
{
    gid_t maxgid = 999;
    struct group *gr;
    setgrent();
    while ((gr = getgrent()) != NULL) {
        if (gr->gr_gid >= 1000 && gr->gr_gid > maxgid)
            maxgid = gr->gr_gid;
    }
    endgrent();
    return maxgid + 1;
}

/* Append a line to a file */
static int append_line(const char *path, const char *line)
{
    FILE *fp = fopen(path, "a");
    if (!fp) {
        perror(path);
        return -1;
    }
    fputs(line, fp);
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    if (getuid() != 0) {
        fprintf(stderr, "useradd: permission denied (must be root)\n");
        return 1;
    }

    uid_t uid = 0;
    gid_t gid = 0;
    const char *homedir = NULL;
    const char *shell = "/bin/sh";
    const char *username = NULL;
    int uid_set = 0, gid_set = 0;

    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "-u") == 0 && i + 1 < argc) {
            uid = atoi(argv[++i]);
            uid_set = 1;
        } else if (strcmp(argv[i], "-g") == 0 && i + 1 < argc) {
            gid = atoi(argv[++i]);
            gid_set = 1;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            homedir = argv[++i];
        } else if (strcmp(argv[i], "-s") == 0 && i + 1 < argc) {
            shell = argv[++i];
        } else if (argv[i][0] != '-') {
            username = argv[i];
        } else {
            fprintf(stderr, "useradd: unknown option: %s\n", argv[i]);
            return 1;
        }
        i++;
    }

    if (!username) {
        fprintf(stderr, "usage: useradd [-u uid] [-g gid] [-d home] [-s shell] username\n");
        return 1;
    }

    /* Check if user already exists */
    if (getpwnam(username) != NULL) {
        fprintf(stderr, "useradd: user '%s' already exists\n", username);
        return 1;
    }

    if (!uid_set) uid = next_uid();
    if (!gid_set) gid = (gid_t)uid;  /* default: gid = uid */

    char homebuf[256];
    if (!homedir) {
        snprintf(homebuf, sizeof(homebuf), "/home/%s", username);
        homedir = homebuf;
    }

    /* Add to /etc/passwd */
    char line[LINE_MAX_LEN];
    snprintf(line, sizeof(line), "%s:x:%d:%d:%s:%s:%s\n",
             username, uid, gid, username, homedir, shell);
    if (append_line("/etc/passwd", line) < 0)
        return 1;

    /* Add to /etc/shadow (locked, no password) */
    snprintf(line, sizeof(line), "%s:!:0:0:99999:7:::\n", username);
    if (append_line("/etc/shadow", line) < 0)
        return 1;

    /* Create user's group if gid == uid and group doesn't exist */
    if (!gid_set || gid == (gid_t)uid) {
        if (getgrgid(gid) == NULL) {
            snprintf(line, sizeof(line), "%s:x:%d:%s\n", username, gid, username);
            if (append_line("/etc/group", line) < 0)
                return 1;
        }
    }

    /* Create home directory */
    if (mkdir(homedir, 0755) < 0 && errno != EEXIST) {
        fprintf(stderr, "useradd: cannot create home directory %s: %s\n",
                homedir, strerror(errno));
        /* Non-fatal — user is created anyway */
    } else {
        /* chown home dir to user */
        chown(homedir, uid, gid);
    }

    printf("useradd: user '%s' created (uid=%d, gid=%d, home=%s)\n",
           username, uid, gid, homedir);
    return 0;
}
