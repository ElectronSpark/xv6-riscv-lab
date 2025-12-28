#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"
#include "kernel/inc/vfs/xv6fs/ondisk.h"
#include "kernel/inc/vfs/fcntl.h"

int find(char *path, char *name) {
    char buf[512], *p;
    int fd;
    int path_length; 
    struct dirent de;
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
    memcpy(buf, path, path_length);
    p = buf + path_length;
    *p++ = '/';
    *p = '\0';

    while(read(fd, &de, sizeof(de)) == sizeof(de)){
        if(de.inum == 0)
            continue;
        memmove(p, de.name, DIRSIZ);
        p[DIRSIZ] = 0;
        if(stat(buf, &st) < 0){
            printf("find: cannot stat %s\n", buf);
            continue;
        }
        if (strcmp(p, name) == 0) {
            printf("%s\n", buf);
        }
        if (S_ISDIR(st.mode) && strcmp(p, ".") && strcmp(p, "..")) {
            find(buf, name);
        }
    }

    return 0;
}

int
main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("usage: find [path] [name]\n");
        exit(1);
    }

    int ret = find(argv[1], argv[2]);

    exit(ret);
}