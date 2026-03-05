/*
 * id.c — Print user and group IDs for xv6
 *
 * Usage: id [username]
 *
 * Without arguments, prints the current user's uid, gid, and groups.
 * With a username, prints that user's information.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>

int main(int argc, char **argv)
{
    uid_t uid;
    gid_t gid;
    struct passwd *pw;

    if (argc >= 2) {
        pw = getpwnam(argv[1]);
        if (!pw) {
            fprintf(stderr, "id: '%s': no such user\n", argv[1]);
            return 1;
        }
        uid = pw->pw_uid;
        gid = pw->pw_gid;
    } else {
        uid = getuid();
        gid = getgid();
        pw = getpwuid(uid);
    }

    /* Print uid */
    printf("uid=%d", uid);
    if (pw)
        printf("(%s)", pw->pw_name);

    /* Print gid */
    struct group *gr = getgrgid(gid);
    printf(" gid=%d", gid);
    if (gr)
        printf("(%s)", gr->gr_name);

    /* Print effective uid if different */
    uid_t euid = geteuid();
    if (euid != uid) {
        struct passwd *epw = getpwuid(euid);
        printf(" euid=%d", euid);
        if (epw)
            printf("(%s)", epw->pw_name);
    }

    /* Print effective gid if different */
    gid_t egid = getegid();
    if (egid != gid) {
        struct group *egr = getgrgid(egid);
        printf(" egid=%d", egid);
        if (egr)
            printf("(%s)", egr->gr_name);
    }

    /* Print supplementary groups */
    gid_t groups[32];
    int ngroups = getgroups(32, groups);
    if (ngroups > 0) {
        printf(" groups=");
        for (int i = 0; i < ngroups; i++) {
            if (i > 0) printf(",");
            struct group *ggr = getgrgid(groups[i]);
            printf("%d", groups[i]);
            if (ggr)
                printf("(%s)", ggr->gr_name);
        }
    }

    printf("\n");
    return 0;
}
