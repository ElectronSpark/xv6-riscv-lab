/*
 * echo.c — musl libc version for xv6
 */
#include <stdio.h>

int main(int argc, char *argv[])
{
    for (int i = 1; i < argc; i++) {
        fputs(argv[i], stdout);
        if (i + 1 < argc)
            putchar(' ');
    }
    putchar('\n');
    return 0;
}
