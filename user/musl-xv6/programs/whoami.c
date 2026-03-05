/*
 * whoami.c — Print the current effective user name for xv6
 */
#include <stdio.h>
#include <unistd.h>
#include <pwd.h>

int main(void)
{
    struct passwd *pw = getpwuid(geteuid());
    if (pw)
        printf("%s\n", pw->pw_name);
    else
        printf("%d\n", geteuid());
    return 0;
}
