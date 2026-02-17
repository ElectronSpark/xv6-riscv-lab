#include "uabi/stat.h"
#include "user/user.h"
#include "uabi/linux_dirent64.h"
#include "uabi/fcntl.h"

int find(char *path, char *name) {
    char buf[512], *p;
    int fd;
    int path_length;
    char dirent_buf[1024];
    int nread;
    struct stat st;

    if ((fd = open(path, O_RDONLY)) < 0) {
        fprintf(2, "find: cannot open %s\n", path);
        return -1;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "find: cannot stat %s\n", path);
        close(fd);
        return -1;
    }

    if (!S_ISDIR(st.mode)) {
        fprintf(2, "find: %s is not a directory\n", path);
        close(fd);
        return -1;
    }

    path_length = strlen(path);
    if (path_length + 1 + NAME_MAX + 1 > sizeof(buf)) {
        fprintf(2, "find: path too long\n");
        close(fd);
        return -1;
    }
    memcpy(buf, path, path_length);
    p = buf + path_length;
    *p++ = '/';
    *p = '\0';

    while ((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
        int pos = 0;
        while (pos < nread) {
            struct linux_dirent64 *de =
                (struct linux_dirent64 *)(dirent_buf + pos);
            if (de->d_ino == 0) {
                pos += de->d_reclen;
                continue;
            }
            strcpy(p, de->d_name);
            if (stat(buf, &st) < 0) {
                printf("find: cannot stat %s\n", buf);
                pos += de->d_reclen;
                continue;
            }
            if (strcmp(de->d_name, name) == 0) {
                printf("%s\n", buf);
            }
            if (S_ISDIR(st.mode) && strcmp(de->d_name, ".") != 0 &&
                strcmp(de->d_name, "..") != 0) {
                find(buf, name);
            }
            pos += de->d_reclen;
        }
    }

    close(fd);

    return 0;
}

int main(int argc, char *argv[]) {
    if (argc != 3) {
        printf("usage: find [path] [name]\n");
        exit(1);
    }

    int ret = find(argv[1], argv[2]);

    exit(ret);
}