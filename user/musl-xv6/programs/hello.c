/*
 * hello.c — Simple musl libc test program for xv6
 *
 * This is the first program to test the musl libc integration.
 * If this prints "Hello from musl libc on xv6!" then musl is working.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[])
{
    printf("Hello from musl libc on xv6!\n");

    /* Test a few libc functions */
    char buf[64];
    snprintf(buf, sizeof(buf), "argc = %d", argc);
    printf("%s\n", buf);

    for (int i = 0; i < argc; i++)
        printf("argv[%d] = %s\n", i, argv[i]);

    /* Test strlen/strcmp */
    const char *s = "musl works";
    printf("strlen(\"%s\") = %zu\n", s, strlen(s));
    printf("strcmp(\"abc\", \"abc\") = %d\n", strcmp("abc", "abc"));

    /* Test malloc/free */
    char *p = malloc(128);
    if (p) {
        strcpy(p, "heap allocation works");
        printf("%s\n", p);
        free(p);
    } else {
        printf("malloc failed!\n");
        return 1;
    }

    printf("All musl libc tests passed.\n");
    return 0;
}
