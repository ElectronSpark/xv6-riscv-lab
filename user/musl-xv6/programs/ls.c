/*
 * ls.c — musl libc version for xv6
 *
 * Lists directory contents. Accepts optional directory arguments
 * (defaults to "."). With -l, shows size/type information.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static int show_long = 0;

static int do_ls(const char *path)
{
    DIR *d = opendir(path);
    if (!d) {
        fprintf(stderr, "ls: cannot open '%s'\n", path);
        return 1;
    }

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.' && de->d_name[1] == '\0')
            continue;
        if (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == '\0')
            continue;

        if (show_long) {
            char fullpath[256];
            int plen = strlen(path);
            if (plen + 1 + strlen(de->d_name) + 1 > sizeof(fullpath)) {
                printf("  ?  %s\n", de->d_name);
                continue;
            }
            memcpy(fullpath, path, plen);
            if (plen > 0 && path[plen - 1] != '/')
                fullpath[plen++] = '/';
            strcpy(fullpath + plen, de->d_name);

            struct stat st;
            if (stat(fullpath, &st) < 0) {
                printf("  ?  %s\n", de->d_name);
            } else {
                char type = '-';
                if (S_ISDIR(st.st_mode))  type = 'd';
                if (S_ISLNK(st.st_mode))  type = 'l';
                if (S_ISCHR(st.st_mode))  type = 'c';
                if (S_ISBLK(st.st_mode))  type = 'b';
                printf("%c %7ld  %s\n", type, (long)st.st_size, de->d_name);
            }
        } else {
            printf("%s\n", de->d_name);
        }
    }
    closedir(d);
    return 0;
}

int main(int argc, char *argv[])
{
    int i;
    int first_arg = 1;
    int ret = 0;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            char *p = argv[i] + 1;
            while (*p) {
                if (*p == 'l') show_long = 1;
                p++;
            }
            first_arg = i + 1;
        } else {
            break;
        }
    }

    if (first_arg >= argc) {
        ret = do_ls(".");
    } else {
        int multiple = (argc - first_arg > 1);
        for (i = first_arg; i < argc; i++) {
            if (multiple)
                printf("%s:\n", argv[i]);
            ret |= do_ls(argv[i]);
            if (multiple && i + 1 < argc)
                printf("\n");
        }
    }

    return ret;
}
