/*
 * chown.c — Change file owner and group for xv6
 *
 * Usage: chown owner[:group] file...
 *        chown :group file...
 *
 * Only root can change file ownership.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <sys/stat.h>

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: chown owner[:group] file...\n");
        return 1;
    }

    const char *spec = argv[1];
    uid_t uid = (uid_t)-1;
    gid_t gid = (gid_t)-1;

    /* Parse owner:group */
    char *colon = strchr(spec, ':');
    if (colon) {
        /* owner:group or :group */
        if (colon != spec) {
            /* Has owner part */
            char owner[64];
            int olen = colon - spec;
            if (olen >= (int)sizeof(owner)) olen = sizeof(owner) - 1;
            memcpy(owner, spec, olen);
            owner[olen] = '\0';

            /* Try numeric first */
            char *end;
            long val = strtol(owner, &end, 10);
            if (*end == '\0')
                uid = (uid_t)val;
            else {
                struct passwd *pw = getpwnam(owner);
                if (!pw) {
                    fprintf(stderr, "chown: invalid user '%s'\n", owner);
                    return 1;
                }
                uid = pw->pw_uid;
            }
        }

        /* Group part */
        const char *grpstr = colon + 1;
        if (grpstr[0] != '\0') {
            char *end;
            long val = strtol(grpstr, &end, 10);
            if (*end == '\0')
                gid = (gid_t)val;
            else {
                struct group *gr = getgrnam(grpstr);
                if (!gr) {
                    fprintf(stderr, "chown: invalid group '%s'\n", grpstr);
                    return 1;
                }
                gid = gr->gr_gid;
            }
        }
    } else {
        /* Owner only */
        char *end;
        long val = strtol(spec, &end, 10);
        if (*end == '\0')
            uid = (uid_t)val;
        else {
            struct passwd *pw = getpwnam(spec);
            if (!pw) {
                fprintf(stderr, "chown: invalid user '%s'\n", spec);
                return 1;
            }
            uid = pw->pw_uid;
        }
    }

    int ret = 0;
    for (int i = 2; i < argc; i++) {
        if (chown(argv[i], uid, gid) < 0) {
            perror(argv[i]);
            ret = 1;
        }
    }
    return ret;
}
