#include "uabi/stat.h"
#include "user/user.h"
#include "uabi/fcntl.h"
#include "uabi/statfs.h"

int main() {
    struct statfs fs;
    if (statfs("/", &fs) < 0) {
        printf("bigfile: statfs failed\n");
        exit(-1);
    }
    int bsize = fs.f_bsize;
    if (bsize == 0 || bsize > 4096) {
        printf("bigfile: bad block size %d\n", bsize);
        exit(-1);
    }

    int nindirect = bsize / sizeof(uint);
    int expected_blocks = 11 + nindirect + nindirect * nindirect;

    char buf[4096]; // large enough for any block size
    int fd, i, blocks, readblocks;

    fd = open("big.file", O_CREAT | O_WRONLY);
    if (fd < 0) {
        printf("bigfile: cannot open big.file for writing\n");
        exit(-1);
    }

    blocks = 0;
    while (1) {
        *(int *)buf = blocks;
        int cc = write(fd, buf, bsize);
        if (cc <= 0)
            break;
        blocks++;
        if (blocks % 100 == 0)
            printf(".");
    }

    printf("\nwrote %d blocks\n", blocks);
    if (blocks != expected_blocks) {
        printf("bigfile: file is too small (expected %d, got %d)\n",
               expected_blocks, blocks);
        exit(-1);
    }

    close(fd);
    fd = open("big.file", O_RDONLY);
    printf("reading bigfile\n");
    if (fd < 0) {
        printf("bigfile: cannot re-open big.file for reading\n");
        exit(-1);
    }
    readblocks = 0;
    for (i = 0; i < blocks; i++) {
        int cc = read(fd, buf, bsize);
        if (cc <= 0) {
            printf("bigfile: read error at block %d\n", i);
            exit(-1);
        }
        if (*(int *)buf != i) {
            printf("bigfile: read the wrong data (%d) for block %d\n",
                   *(int *)buf, i);
            exit(-1);
        }
        readblocks++;
        if (readblocks % 100 == 0)
            printf(".");
    }

    printf("\nbigfile done; ok\n");

    exit(0);
}
