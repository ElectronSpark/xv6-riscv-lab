/**
 * @file dh.c
 * @brief dh - disk/file hexdump
 *
 * Usage: dh [-s offset] [-n length] [-w width] <file>
 *
 * Displays raw bytes from a file or block device in hex + ASCII format.
 * Options:
 *   -s offset   start offset in bytes (supports K/M suffix)
 *   -n length   number of bytes to dump (supports K/M suffix, default: 256)
 *   -w width    bytes per line (default: 16)
 */

#include "kernel/inc/types.h"
#include "kernel/inc/vfs/fcntl.h"
#include "user/user.h"

#define DEFAULT_LENGTH 256
#define DEFAULT_WIDTH 16
#define MAX_WIDTH 64
#define READ_BUF 4096

static char rbuf[READ_BUF];

/**
 * Parse a numeric value with optional K/M suffix.
 */
static int64 parse_num(const char *s)
{
    int64 n = 0;
    int i = 0;

    if (s[0] == '\0')
        return -1;

    /* Support 0x prefix for hex */
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        i = 2;
        while (1) {
            char c = s[i];
            if (c >= '0' && c <= '9')
                n = n * 16 + (c - '0');
            else if (c >= 'a' && c <= 'f')
                n = n * 16 + (c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                n = n * 16 + (c - 'A' + 10);
            else
                break;
            i++;
        }
    } else {
        while (s[i] >= '0' && s[i] <= '9') {
            n = n * 10 + (s[i] - '0');
            i++;
        }
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

static const char hex[] = "0123456789abcdef";

/**
 * Print a hex line: offset, hex bytes, ASCII.
 */
static void hexline(uint64 addr, const uchar *data, int len, int width)
{
    char line[256];
    int pos = 0;

    /* Address (8 hex digits) */
    for (int i = 28; i >= 0; i -= 4)
        line[pos++] = hex[(addr >> i) & 0xf];
    line[pos++] = ':';
    line[pos++] = ' ';

    /* Hex bytes */
    for (int i = 0; i < width; i++) {
        if (i == width >> 1) {
            line[pos++] = ' ';
        }
        if (i < len) {
            line[pos++] = hex[data[i] >> 4];
            line[pos++] = hex[data[i] & 0xf];
        } else {
            line[pos++] = ' ';
            line[pos++] = ' ';
        }
        line[pos++] = ' ';
    }

    /* ASCII */
    line[pos++] = '|';
    for (int i = 0; i < len; i++) {
        if (data[i] >= 0x20 && data[i] <= 0x7e)
            line[pos++] = data[i];
        else
            line[pos++] = '.';
    }
    for (int i = len; i < width; i++)
        line[pos++] = ' ';
    line[pos++] = '|';
    line[pos++] = '\n';

    write(1, line, pos);
}

int main(int argc, char *argv[])
{
    int64 offset = 0;
    int64 length = DEFAULT_LENGTH;
    int width = DEFAULT_WIDTH;
    const char *file = 0;
    int i;

    /* Parse arguments */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            if (++i >= argc) {
                fprintf(2, "dh: -s requires an argument\n");
                exit(1);
            }
            offset = parse_num(argv[i]);
            if (offset < 0) {
                fprintf(2, "dh: invalid offset: %s\n", argv[i]);
                exit(1);
            }
        } else if (strcmp(argv[i], "-n") == 0) {
            if (++i >= argc) {
                fprintf(2, "dh: -n requires an argument\n");
                exit(1);
            }
            length = parse_num(argv[i]);
            if (length <= 0) {
                fprintf(2, "dh: invalid length: %s\n", argv[i]);
                exit(1);
            }
        } else if (strcmp(argv[i], "-w") == 0) {
            if (++i >= argc) {
                fprintf(2, "dh: -w requires an argument\n");
                exit(1);
            }
            width = atoi(argv[i]);
            if (width < 1 || width > MAX_WIDTH) {
                fprintf(2, "dh: width must be 1-%d\n", MAX_WIDTH);
                exit(1);
            }
        } else if (argv[i][0] == '-') {
            fprintf(2, "dh: unknown option: %s\n", argv[i]);
            fprintf(2, "Usage: dh [-s offset] [-n length] [-w width] <file>\n");
            exit(1);
        } else {
            file = argv[i];
        }
    }

    if (!file) {
        fprintf(2, "Usage: dh [-s offset] [-n length] [-w width] <file>\n");
        fprintf(2, "  -s offset   byte offset (supports 0x hex, K/M suffix)\n");
        fprintf(2, "  -n length   bytes to dump (default: %d)\n", DEFAULT_LENGTH);
        fprintf(2, "  -w width    bytes per line (default: %d)\n", DEFAULT_WIDTH);
        exit(1);
    }

    int fd = open(file, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "dh: cannot open %s\n", file);
        exit(1);
    }

    /* Seek to start offset */
    if (offset > 0) {
        if (lseek(fd, offset, SEEK_SET) < 0) {
            fprintf(2, "dh: cannot seek to offset\n");
            exit(1);
        }
    }

    /* Read and dump */
    int64 remaining = length;
    uint64 addr = offset;

    while (remaining > 0) {
        int toread = READ_BUF;
        if (remaining < toread)
            toread = remaining;

        int n = read(fd, rbuf, toread);
        if (n < 0) {
            fprintf(2, "dh: read error\n");
            exit(1);
        }
        if (n == 0)
            break;

        /* Print hex lines */
        int off = 0;
        while (off < n) {
            int linelen = n - off;
            if (linelen > width)
                linelen = width;
            hexline(addr, (uchar *)(rbuf + off), linelen, width);
            off += linelen;
            addr += linelen;
        }

        remaining -= n;
    }

    close(fd);
    exit(0);
}
