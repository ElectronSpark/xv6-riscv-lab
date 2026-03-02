/**
 * @file dd.c
 * @brief dd - copy and convert data
 *
 * Usage: dd [if=FILE] [of=FILE] [bs=N] [count=N] [skip=N] [seek=N]
 *
 * Copies data from input to output with optional block size,
 * count, skip (input offset), and seek (output offset).
 * Defaults: stdin/stdout, bs=512, count=unlimited.
 */

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define DEFAULT_BS 512
#define MAX_BS 65536

static char buf[MAX_BS];

/**
 * Parse a numeric value with optional K/M suffix.
 * Returns -1 on error.
 */
static int64 parse_num(const char *s)
{
    int64 n = 0;
    int i = 0;

    if (s[0] == '\0')
        return -1;

    while (s[i] >= '0' && s[i] <= '9') {
        n = n * 10 + (s[i] - '0');
        i++;
    }

    if (s[i] == 'K' || s[i] == 'k') {
        n *= 1024;
        i++;
    } else if (s[i] == 'M' || s[i] == 'm') {
        n *= 1024 * 1024;
        i++;
    }

    if (s[i] != '\0')
        return -1;

    return n;
}

/**
 * Check if string starts with prefix, return pointer past '=' or NULL.
 */
static const char *match_opt(const char *arg, const char *prefix)
{
    while (*prefix) {
        if (*arg != *prefix)
            return 0;
        arg++;
        prefix++;
    }
    if (*arg != '=')
        return 0;
    return arg + 1;
}

int main(int argc, char *argv[])
{
    const char *infile = 0;
    const char *outfile = 0;
    int64 bs = DEFAULT_BS;
    int64 count = -1; /* unlimited */
    int64 skip = 0;
    int64 seek = 0;
    int ifd = 0;  /* stdin */
    int ofd = 1;  /* stdout */
    int64 blocks_in = 0, blocks_out = 0;
    int64 partial_in = 0, partial_out = 0;
    const char *val;

    for (int i = 1; i < argc; i++) {
        if ((val = match_opt(argv[i], "if"))) {
            infile = val;
        } else if ((val = match_opt(argv[i], "of"))) {
            outfile = val;
        } else if ((val = match_opt(argv[i], "bs"))) {
            bs = parse_num(val);
            if (bs <= 0 || bs > MAX_BS) {
                fprintf(2, "dd: invalid bs=%s (max %d)\n", val, MAX_BS);
                exit(1);
            }
        } else if ((val = match_opt(argv[i], "count"))) {
            count = parse_num(val);
            if (count < 0) {
                fprintf(2, "dd: invalid count=%s\n", val);
                exit(1);
            }
        } else if ((val = match_opt(argv[i], "skip"))) {
            skip = parse_num(val);
            if (skip < 0) {
                fprintf(2, "dd: invalid skip=%s\n", val);
                exit(1);
            }
        } else if ((val = match_opt(argv[i], "seek"))) {
            seek = parse_num(val);
            if (seek < 0) {
                fprintf(2, "dd: invalid seek=%s\n", val);
                exit(1);
            }
        } else {
            fprintf(2, "dd: unknown option: %s\n", argv[i]);
            fprintf(2, "Usage: dd [if=FILE] [of=FILE] [bs=N] [count=N] [skip=N] [seek=N]\n");
            exit(1);
        }
    }

    /* Open input */
    if (infile) {
        ifd = open(infile, O_RDONLY);
        if (ifd < 0) {
            fprintf(2, "dd: cannot open %s\n", infile);
            exit(1);
        }
    }

    /* Open output */
    if (outfile) {
        ofd = open(outfile, O_WRONLY | O_CREAT | O_TRUNC);
        if (ofd < 0) {
            fprintf(2, "dd: cannot open %s\n", outfile);
            exit(1);
        }
    }

    /* Apply skip (input offset) */
    if (skip > 0) {
        if (lseek(ifd, skip * bs, SEEK_SET) < 0) {
            /* If lseek fails, try reading and discarding */
            for (int64 i = 0; i < skip; i++) {
                int n = read(ifd, buf, bs);
                if (n <= 0)
                    break;
            }
        }
    }

    /* Apply seek (output offset) */
    if (seek > 0) {
        if (lseek(ofd, seek * bs, SEEK_SET) < 0) {
            fprintf(2, "dd: cannot seek on output\n");
            exit(1);
        }
    }

    /* Copy loop */
    int64 done = 0;
    while (count < 0 || done < count) {
        int n = read(ifd, buf, bs);
        if (n < 0) {
            fprintf(2, "dd: read error\n");
            exit(1);
        }
        if (n == 0)
            break;

        if (n == bs)
            blocks_in++;
        else
            partial_in++;

        int w = write(ofd, buf, n);
        if (w != n) {
            fprintf(2, "dd: write error\n");
            exit(1);
        }

        if (w == bs)
            blocks_out++;
        else
            partial_out++;

        done++;
    }

    if (infile)
        close(ifd);
    if (outfile)
        close(ofd);

    fprintf(2, "%d+%d records in\n", (int)blocks_in, (int)partial_in);
    fprintf(2, "%d+%d records out\n", (int)blocks_out, (int)partial_out);

    exit(0);
}
