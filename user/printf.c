#include "kernel/inc/types.h"
#include "kernel/inc/vfs/stat.h"
#include "user/user.h"

#include <stdarg.h>

static char digits_lower[] = "0123456789abcdef";
static char digits_upper[] = "0123456789ABCDEF";

static void puts_n(int fd, const char *s, size_t n) {
    size_t idx = 0;
    while (idx < n) {
        int ret = write(fd, &s[idx], n - idx);
        if (ret <= 0) {
            break;
        }
        idx += ret;
    }
}

// Format flags
#define FLAG_LEFT 0x01  // '-' left justify
#define FLAG_PLUS 0x02  // '+' show plus sign
#define FLAG_SPACE 0x04 // ' ' space before positive number
#define FLAG_HASH 0x08  // '#' alternate form
#define FLAG_ZERO 0x10  // '0' zero padding
#define FLAG_UPPER 0x20 // uppercase hex

// Helper: format an integer into buffer, returns length
static int format_int(char *buf, long long val, int base, int is_signed,
                      int flags) {
    char tmp[32];
    int i = 0;
    int neg = 0;
    unsigned long long uval;
    char *dig = (flags & FLAG_UPPER) ? digits_upper : digits_lower;

    if (is_signed && val < 0) {
        neg = 1;
        uval = -val;
    } else {
        uval = val;
    }

    // Generate digits in reverse
    do {
        tmp[i++] = dig[uval % base];
        uval /= base;
    } while (uval != 0);

    // Copy to output buffer in correct order
    int len = 0;
    if (neg)
        buf[len++] = '-';
    else if (is_signed && (flags & FLAG_PLUS))
        buf[len++] = '+';
    else if (is_signed && (flags & FLAG_SPACE))
        buf[len++] = ' ';

    // Add 0x prefix for hash flag with hex
    if ((flags & FLAG_HASH) && base == 16) {
        buf[len++] = '0';
        buf[len++] = (flags & FLAG_UPPER) ? 'X' : 'x';
    } else if ((flags & FLAG_HASH) && base == 8 && tmp[i - 1] != '0') {
        buf[len++] = '0';
    }

    while (--i >= 0)
        buf[len++] = tmp[i];

    return len;
}

// Helper: format a pointer into buffer
static int format_ptr(char *buf, uint64 x) {
    int len = 0;
    buf[len++] = '0';
    buf[len++] = 'x';
    for (int i = 0; i < (int)(sizeof(uint64) * 2); i++, x <<= 4)
        buf[len++] = digits_lower[x >> (sizeof(uint64) * 8 - 4)];
    return len;
}

// Parse format flags
static const char *parse_flags(const char *fmt, int *flags) {
    *flags = 0;
    while (1) {
        switch (*fmt) {
        case '-':
            *flags |= FLAG_LEFT;
            fmt++;
            break;
        case '+':
            *flags |= FLAG_PLUS;
            fmt++;
            break;
        case ' ':
            *flags |= FLAG_SPACE;
            fmt++;
            break;
        case '#':
            *flags |= FLAG_HASH;
            fmt++;
            break;
        case '0':
            *flags |= FLAG_ZERO;
            fmt++;
            break;
        default:
            return fmt;
        }
    }
}

// Parse width/precision number
static const char *parse_number(const char *fmt, int *num, va_list ap) {
    *num = 0;
    if (*fmt == '*') {
        *num = va_arg(ap, int);
        return fmt + 1;
    }
    while (*fmt >= '0' && *fmt <= '9') {
        *num = *num * 10 + (*fmt - '0');
        fmt++;
    }
    return fmt;
}

// Main vprintf implementation
void vprintf(int fd, const char *fmt, va_list ap) {
    char buf[256];
    int idx = 0;
    char numbuf[64];

    while (*fmt) {
        // Flush if buffer is nearly full
        if (idx >= 240) {
            puts_n(fd, buf, idx);
            idx = 0;
        }

        if (*fmt != '%') {
            buf[idx++] = *fmt++;
            continue;
        }

        fmt++; // skip '%'

        if (*fmt == '\0')
            break;

        // Parse flags
        int flags = 0;
        fmt = parse_flags(fmt, &flags);

        // Parse width
        int width = 0;
        fmt = parse_number(fmt, &width, ap);

        // Parse precision
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            fmt = parse_number(fmt, &precision, ap);
            if (precision < 0)
                precision = 0;
        }

        // Parse length modifier
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                is_long = 2;
                fmt++;
            }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h')
                fmt++;
        } else if (*fmt == 'z') {
            is_long = 1;
            fmt++;
        }

        // Flush buffer before handling format
        if (idx > 0) {
            puts_n(fd, buf, idx);
            idx = 0;
        }

        // Handle format specifier
        char c = *fmt++;
        int numlen = 0;
        char *s;
        int slen;
        int padlen;
        char padchar;

        switch (c) {
        case 'd':
        case 'i': {
            long long val;
            if (is_long >= 2)
                val = va_arg(ap, long long);
            else if (is_long == 1)
                val = va_arg(ap, long);
            else
                val = va_arg(ap, int);
            numlen = format_int(numbuf, val, 10, 1, flags);
            goto print_number;
        }

        case 'u': {
            unsigned long long val;
            if (is_long >= 2)
                val = va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = va_arg(ap, unsigned long);
            else
                val = va_arg(ap, unsigned int);
            numlen = format_int(numbuf, val, 10, 0, flags);
            goto print_number;
        }

        case 'x':
            flags &= ~FLAG_UPPER;
            goto hex_common;
        case 'X':
            flags |= FLAG_UPPER;
        hex_common: {
            unsigned long long val;
            if (is_long >= 2)
                val = va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = va_arg(ap, unsigned long);
            else
                val = va_arg(ap, unsigned int);
            numlen = format_int(numbuf, val, 16, 0, flags);
            goto print_number;
        }

        case 'o': {
            unsigned long long val;
            if (is_long >= 2)
                val = va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = va_arg(ap, unsigned long);
            else
                val = va_arg(ap, unsigned int);
            numlen = format_int(numbuf, val, 8, 0, flags);
            goto print_number;
        }

        print_number:
            padchar = (flags & FLAG_ZERO) && !(flags & FLAG_LEFT) ? '0' : ' ';
            padlen = width > numlen ? width - numlen : 0;

            // Handle sign/prefix with zero padding
            if (padchar == '0' && numlen > 0) {
                // Print sign first, then zeros
                int prefixlen = 0;
                if (numbuf[0] == '-' || numbuf[0] == '+' || numbuf[0] == ' ') {
                    puts_n(fd, numbuf, 1);
                    prefixlen = 1;
                } else if (numlen >= 2 && numbuf[0] == '0' &&
                           (numbuf[1] == 'x' || numbuf[1] == 'X')) {
                    puts_n(fd, numbuf, 2);
                    prefixlen = 2;
                }
                for (int p = 0; p < padlen; p++)
                    puts_n(fd, "0", 1);
                puts_n(fd, numbuf + prefixlen, numlen - prefixlen);
            } else {
                if (!(flags & FLAG_LEFT)) {
                    for (int p = 0; p < padlen; p++)
                        puts_n(fd, " ", 1);
                }
                puts_n(fd, numbuf, numlen);
                if (flags & FLAG_LEFT) {
                    for (int p = 0; p < padlen; p++)
                        puts_n(fd, " ", 1);
                }
            }
            break;

        case 'p':
            numlen = format_ptr(numbuf, va_arg(ap, uint64));
            padlen = width > numlen ? width - numlen : 0;
            if (!(flags & FLAG_LEFT)) {
                for (int p = 0; p < padlen; p++)
                    puts_n(fd, " ", 1);
            }
            puts_n(fd, numbuf, numlen);
            if (flags & FLAG_LEFT) {
                for (int p = 0; p < padlen; p++)
                    puts_n(fd, " ", 1);
            }
            break;

        case 's':
            s = va_arg(ap, char *);
            if (s == 0)
                s = "(null)";
            slen = strlen(s);
            if (precision >= 0 && precision < slen)
                slen = precision;
            padlen = width > slen ? width - slen : 0;
            if (!(flags & FLAG_LEFT)) {
                for (int p = 0; p < padlen; p++)
                    puts_n(fd, " ", 1);
            }
            puts_n(fd, s, slen);
            if (flags & FLAG_LEFT) {
                for (int p = 0; p < padlen; p++)
                    puts_n(fd, " ", 1);
            }
            break;

        case 'c':
            numbuf[0] = (char)va_arg(ap, int);
            padlen = width > 1 ? width - 1 : 0;
            if (!(flags & FLAG_LEFT)) {
                for (int p = 0; p < padlen; p++)
                    puts_n(fd, " ", 1);
            }
            puts_n(fd, numbuf, 1);
            if (flags & FLAG_LEFT) {
                for (int p = 0; p < padlen; p++)
                    puts_n(fd, " ", 1);
            }
            break;

        case '%':
            buf[idx++] = '%';
            break;

        case 'n':
            // %n is dangerous and not commonly needed, skip
            break;

        default:
            // Unknown format, print as-is
            buf[idx++] = '%';
            buf[idx++] = c;
            break;
        }
    }

    // Flush remaining buffer
    if (idx > 0) {
        puts_n(fd, buf, idx);
    }
}

void fprintf(int fd, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fd, fmt, ap);
    va_end(ap);
}

void printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(1, fmt, ap);
    va_end(ap);
}

// snprintf implementation
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap) {
    if (size == 0)
        return 0;

    char *out = buf;
    char *end = buf + size - 1;
    char numbuf[64];

    while (*fmt && out < end) {
        if (*fmt != '%') {
            *out++ = *fmt++;
            continue;
        }

        fmt++; // skip '%'
        if (*fmt == '\0')
            break;

        // Parse flags
        int flags = 0;
        fmt = parse_flags(fmt, &flags);

        // Parse width
        int width = 0;
        fmt = parse_number(fmt, &width, ap);

        // Parse precision
        int precision = -1;
        if (*fmt == '.') {
            fmt++;
            fmt = parse_number(fmt, &precision, ap);
            if (precision < 0)
                precision = 0;
        }

        // Parse length modifier
        int is_long = 0;
        if (*fmt == 'l') {
            is_long = 1;
            fmt++;
            if (*fmt == 'l') {
                is_long = 2;
                fmt++;
            }
        } else if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h')
                fmt++;
        } else if (*fmt == 'z') {
            is_long = 1;
            fmt++;
        }

        char c = *fmt++;
        int numlen = 0;
        char *s;
        int slen;
        int padlen;
        char padchar;

        switch (c) {
        case 'd':
        case 'i': {
            long long val;
            if (is_long >= 2)
                val = va_arg(ap, long long);
            else if (is_long == 1)
                val = va_arg(ap, long);
            else
                val = va_arg(ap, int);
            numlen = format_int(numbuf, val, 10, 1, flags);
            goto snprint_number;
        }

        case 'u': {
            unsigned long long val;
            if (is_long >= 2)
                val = va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = va_arg(ap, unsigned long);
            else
                val = va_arg(ap, unsigned int);
            numlen = format_int(numbuf, val, 10, 0, flags);
            goto snprint_number;
        }

        case 'x':
            flags &= ~FLAG_UPPER;
            goto snhex_common;
        case 'X':
            flags |= FLAG_UPPER;
        snhex_common: {
            unsigned long long val;
            if (is_long >= 2)
                val = va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = va_arg(ap, unsigned long);
            else
                val = va_arg(ap, unsigned int);
            numlen = format_int(numbuf, val, 16, 0, flags);
            goto snprint_number;
        }

        case 'o': {
            unsigned long long val;
            if (is_long >= 2)
                val = va_arg(ap, unsigned long long);
            else if (is_long == 1)
                val = va_arg(ap, unsigned long);
            else
                val = va_arg(ap, unsigned int);
            numlen = format_int(numbuf, val, 8, 0, flags);
            goto snprint_number;
        }

        snprint_number:
            padchar = (flags & FLAG_ZERO) && !(flags & FLAG_LEFT) ? '0' : ' ';
            padlen = width > numlen ? width - numlen : 0;

            if (padchar == '0' && numlen > 0) {
                int prefixlen = 0;
                if (numbuf[0] == '-' || numbuf[0] == '+' || numbuf[0] == ' ') {
                    if (out < end)
                        *out++ = numbuf[0];
                    prefixlen = 1;
                } else if (numlen >= 2 && numbuf[0] == '0' &&
                           (numbuf[1] == 'x' || numbuf[1] == 'X')) {
                    if (out < end)
                        *out++ = numbuf[0];
                    if (out < end)
                        *out++ = numbuf[1];
                    prefixlen = 2;
                }
                for (int p = 0; p < padlen && out < end; p++)
                    *out++ = '0';
                for (int p = prefixlen; p < numlen && out < end; p++)
                    *out++ = numbuf[p];
            } else {
                if (!(flags & FLAG_LEFT)) {
                    for (int p = 0; p < padlen && out < end; p++)
                        *out++ = ' ';
                }
                for (int p = 0; p < numlen && out < end; p++)
                    *out++ = numbuf[p];
                if (flags & FLAG_LEFT) {
                    for (int p = 0; p < padlen && out < end; p++)
                        *out++ = ' ';
                }
            }
            break;

        case 'p':
            numlen = format_ptr(numbuf, va_arg(ap, uint64));
            padlen = width > numlen ? width - numlen : 0;
            if (!(flags & FLAG_LEFT)) {
                for (int p = 0; p < padlen && out < end; p++)
                    *out++ = ' ';
            }
            for (int p = 0; p < numlen && out < end; p++)
                *out++ = numbuf[p];
            if (flags & FLAG_LEFT) {
                for (int p = 0; p < padlen && out < end; p++)
                    *out++ = ' ';
            }
            break;

        case 's':
            s = va_arg(ap, char *);
            if (s == 0)
                s = "(null)";
            slen = strlen(s);
            if (precision >= 0 && precision < slen)
                slen = precision;
            padlen = width > slen ? width - slen : 0;
            if (!(flags & FLAG_LEFT)) {
                for (int p = 0; p < padlen && out < end; p++)
                    *out++ = ' ';
            }
            for (int p = 0; p < slen && out < end; p++)
                *out++ = s[p];
            if (flags & FLAG_LEFT) {
                for (int p = 0; p < padlen && out < end; p++)
                    *out++ = ' ';
            }
            break;

        case 'c':
            padlen = width > 1 ? width - 1 : 0;
            if (!(flags & FLAG_LEFT)) {
                for (int p = 0; p < padlen && out < end; p++)
                    *out++ = ' ';
            }
            if (out < end)
                *out++ = (char)va_arg(ap, int);
            if (flags & FLAG_LEFT) {
                for (int p = 0; p < padlen && out < end; p++)
                    *out++ = ' ';
            }
            break;

        case '%':
            if (out < end)
                *out++ = '%';
            break;

        default:
            if (out < end)
                *out++ = '%';
            if (out < end)
                *out++ = c;
            break;
        }
    }

    *out = '\0';
    return out - buf;
}

int snprintf(char *buf, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *buf, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, 0x7fffffff, fmt, ap);
    va_end(ap);
    return ret;
}
