/*
 * userdel.c — Delete a user account for xv6
 *
 * Usage: userdel [-r] username
 *
 * -r: remove home directory
 *
 * Removes the user from /etc/passwd, /etc/shadow, and /etc/group.
 * Requires root privileges.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>

#define LINE_MAX_LEN 1024
#define MAX_LINES    64

/* Remove lines starting with "username:" from a file */
static int remove_user_from_file(const char *path, const char *username)
{
    FILE *fp = fopen(path, "r");
    if (!fp)
        return -1;

    char lines[MAX_LINES][LINE_MAX_LEN];
    int nlines = 0;
    while (nlines < MAX_LINES && fgets(lines[nlines], LINE_MAX_LEN, fp))
        nlines++;
    fclose(fp);

    int ulen = strlen(username);
    fp = fopen(path, "w");
    if (!fp) {
        perror(path);
        return -1;
    }

    for (int i = 0; i < nlines; i++) {
        if (strncmp(lines[i], username, ulen) == 0 && lines[i][ulen] == ':')
            continue;  /* skip this user's line */
        fputs(lines[i], fp);
    }
    fclose(fp);
    return 0;
}

int main(int argc, char **argv)
{
    if (getuid() != 0) {
        fprintf(stderr, "userdel: permission denied (must be root)\n");
        return 1;
    }

    int remove_home = 0;
    const char *username = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0)
            remove_home = 1;
        else if (argv[i][0] != '-')
            username = argv[i];
        else {
            fprintf(stderr, "userdel: unknown option: %s\n", argv[i]);
            return 1;
        }
    }

    if (!username) {
        fprintf(stderr, "usage: userdel [-r] username\n");
        return 1;
    }

    /* Prevent deleting root */
    if (strcmp(username, "root") == 0) {
        fprintf(stderr, "userdel: cannot delete root\n");
        return 1;
    }

    struct passwd *pw = getpwnam(username);
    if (!pw) {
        fprintf(stderr, "userdel: user '%s' not found\n", username);
        return 1;
    }

    char homedir[256];
    strncpy(homedir, pw->pw_dir, sizeof(homedir) - 1);
    homedir[sizeof(homedir) - 1] = '\0';

    /* Remove from /etc/passwd */
    remove_user_from_file("/etc/passwd", username);
    /* Remove from /etc/shadow */
    remove_user_from_file("/etc/shadow", username);
    /* Remove from /etc/group (their own group) */
    remove_user_from_file("/etc/group", username);

    if (remove_home && homedir[0] != '\0' && strcmp(homedir, "/") != 0) {
        /* Simple rmdir — won't remove non-empty dirs */
        if (rmdir(homedir) < 0)
            fprintf(stderr, "userdel: could not remove %s (not empty?)\n", homedir);
        else
            printf("userdel: removed home directory %s\n", homedir);
    }

    printf("userdel: user '%s' deleted\n", username);
    return 0;
}
