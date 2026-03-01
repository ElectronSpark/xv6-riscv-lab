/*
 * nslookup — simple DNS lookup utility for xv6
 *
 * Usage: nslookup <hostname> [server]
 *
 * Resolves a hostname to its IP address(es) using getaddrinfo().
 * The optional [server] argument is accepted for compatibility but
 * ignored — resolution always uses /etc/resolv.conf.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/in.h>

static void usage(const char *prog)
{
    fprintf(stderr, "Usage: %s <hostname> [server]\n", prog);
    exit(1);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
        usage(argv[0]);

    const char *hostname = argv[1];
    const char *server = (argc >= 3) ? argv[2] : NULL;

    if (server)
        printf("Server:\t\t%s\n\n", server);

    struct addrinfo hints, *res, *rp;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;     /* IPv4 or IPv6 */
    hints.ai_socktype = SOCK_STREAM; /* any type */

    int err = getaddrinfo(hostname, NULL, &hints, &res);
    if (err != 0) {
        fprintf(stderr, "** nslookup: can't resolve '%s': %s\n",
                hostname, gai_strerror(err));
        return 1;
    }

    printf("Name:\t%s\n", hostname);

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        char buf[INET6_ADDRSTRLEN];
        void *addr;

        if (rp->ai_family == AF_INET) {
            struct sockaddr_in *sin = (struct sockaddr_in *)rp->ai_addr;
            addr = &sin->sin_addr;
        } else if (rp->ai_family == AF_INET6) {
            struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *)rp->ai_addr;
            addr = &sin6->sin6_addr;
        } else {
            continue;
        }

        if (inet_ntop(rp->ai_family, addr, buf, sizeof(buf)) != NULL)
            printf("Address:\t%s\n", buf);
    }

    freeaddrinfo(res);
    return 0;
}
