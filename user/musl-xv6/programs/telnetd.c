/*
 * telnetd.c — Simple telnet daemon for xv6
 *
 * Listens on port 23 (or specified port), accepts connections, allocates
 * a PTY pair, and spawns login on each connection. Relays data between
 * the network socket and the PTY master.
 *
 * Usage: telnetd [-p port]
 *
 * Requires /dev/ptmx and /dev/pts/ support in the kernel.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef TIOCSCTTY
#define TIOCSCTTY 0x540E
#endif

#define TELNETD_PORT     23
#define BACKLOG          4
#define BUF_SIZE         1024

/* Telnet protocol bytes */
#define TEL_IAC   255
#define TEL_WILL  251
#define TEL_WONT  252
#define TEL_DO    253
#define TEL_DONT  254
#define TEL_SB    250
#define TEL_SE    240

/* Telnet options */
#define TELOPT_ECHO      1
#define TELOPT_SGA       3   /* Suppress Go Ahead */
#define TELOPT_LINEMODE  34

/* Send a telnet negotiation */
static void tel_send(int fd, unsigned char cmd, unsigned char opt)
{
    unsigned char buf[3] = { TEL_IAC, cmd, opt };
    write(fd, buf, 3);
}

/* Strip telnet IAC sequences from data, return cleaned length */
static int strip_telnet(const unsigned char *in, int inlen,
                        unsigned char *out)
{
    int j = 0;
    for (int i = 0; i < inlen; ) {
        if (in[i] == TEL_IAC && i + 1 < inlen) {
            unsigned char cmd = in[i + 1];
            if (cmd == TEL_WILL || cmd == TEL_WONT ||
                cmd == TEL_DO || cmd == TEL_DONT) {
                i += 3;  /* skip IAC + cmd + option */
                continue;
            }
            if (cmd == TEL_SB) {
                /* Skip until IAC SE */
                i += 2;
                while (i < inlen) {
                    if (in[i] == TEL_IAC && i + 1 < inlen &&
                        in[i + 1] == TEL_SE) {
                        i += 2;
                        break;
                    }
                    i++;
                }
                continue;
            }
            if (cmd == TEL_IAC) {
                /* Escaped IAC = literal 0xFF */
                out[j++] = TEL_IAC;
                i += 2;
                continue;
            }
            i += 2;
            continue;
        }
        /* Convert \r\n or \r\0 to just \n for Unix */
        if (in[i] == '\r') {
            out[j++] = '\n';
            i++;
            if (i < inlen && (in[i] == '\n' || in[i] == '\0'))
                i++;
            continue;
        }
        out[j++] = in[i++];
    }
    return j;
}

/* Open a PTY master via /dev/ptmx, return master fd and slave path */
static int open_pty(char *slave_path, int pathlen)
{
    int master = open("/dev/ptmx", O_RDWR);
    if (master < 0) {
        fprintf(stderr, "telnetd: open /dev/ptmx failed: %s (errno %d)\n",
                strerror(errno), errno);
        return -1;
    }

    /* Get the slave PTY number — try ptsname or construct manually */
    int pty_num = -1;
    if (ioctl(master, 0x80045430 /* TIOCGPTN */, &pty_num) < 0) {
        fprintf(stderr, "telnetd: TIOCGPTN ioctl failed: %s (errno %d)\n",
                strerror(errno), errno);
        close(master);
        return -1;
    }

    snprintf(slave_path, pathlen, "/dev/pts/%d", pty_num);
    fprintf(stderr, "telnetd: allocated PTY master fd=%d slave=%s\n",
            master, slave_path);

    /* Unlock the slave (TIOCSPTLCK) */
    int unlock = 0;
    ioctl(master, 0x40045431 /* TIOCSPTLCK */, &unlock);

    return master;
}

/* Reap zombie children */
static void sigchld_handler(int sig)
{
    (void)sig;
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;
}

/* Handle a single telnet connection */
static void handle_connection(int sockfd)
{
    char slave_path[64];
    int master = open_pty(slave_path, sizeof(slave_path));
    if (master < 0) {
        const char *msg = "telnetd: cannot allocate PTY\r\n";
        write(sockfd, msg, strlen(msg));
        close(sockfd);
        return;
    }

    /* Send initial telnet negotiations:
     * Server WILL ECHO (we handle echo via PTY)
     * Server WILL SGA (suppress go-ahead)
     * Server DO SGA (ask client to suppress too)
     */
    tel_send(sockfd, TEL_WILL, TELOPT_ECHO);
    tel_send(sockfd, TEL_WILL, TELOPT_SGA);
    tel_send(sockfd, TEL_DO, TELOPT_SGA);

    pid_t child = fork();
    if (child < 0) {
        close(master);
        close(sockfd);
        return;
    }

    if (child == 0) {
        /* Child: set up slave PTY and exec login */
        close(master);
        close(sockfd);

        /* New session */
        setsid();

        /* Open slave as stdin/stdout/stderr */
        int slave = open(slave_path, O_RDWR);
        if (slave < 0)
            _exit(1);

        /* Set controlling terminal */
        ioctl(slave, TIOCSCTTY, (void *)0);

        dup2(slave, STDIN_FILENO);
        dup2(slave, STDOUT_FILENO);
        dup2(slave, STDERR_FILENO);
        if (slave > STDERR_FILENO)
            close(slave);

        /* Exec login */
        char *login_argv[] = { "login", NULL };
        execv("/bin/login", login_argv);
        _exit(1);
    }

    /* Parent: relay data between socket and PTY master */
    unsigned char buf[BUF_SIZE];
    unsigned char cleaned[BUF_SIZE];

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(sockfd, &rfds);
        FD_SET(master, &rfds);
        int maxfd = sockfd > master ? sockfd : master;

        int ret = select(maxfd + 1, &rfds, NULL, NULL, NULL);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        /* Data from network -> PTY */
        if (FD_ISSET(sockfd, &rfds)) {
            ssize_t n = read(sockfd, buf, BUF_SIZE);
            if (n <= 0)
                break;
            int clen = strip_telnet(buf, n, cleaned);
            if (clen > 0)
                write(master, cleaned, clen);
        }

        /* Data from PTY -> network */
        if (FD_ISSET(master, &rfds)) {
            ssize_t n = read(master, buf, BUF_SIZE);
            if (n <= 0)
                break;
            /* Convert \n to \r\n for telnet */
            unsigned char outbuf[BUF_SIZE * 2];
            int olen = 0;
            for (int i = 0; i < n && olen < (int)sizeof(outbuf) - 1; i++) {
                if (buf[i] == '\n')
                    outbuf[olen++] = '\r';
                outbuf[olen++] = buf[i];
            }
            write(sockfd, outbuf, olen);
        }
    }

    /* Connection ended — clean up */
    close(master);
    close(sockfd);
    kill(child, SIGHUP);
    waitpid(child, NULL, 0);
}

int main(int argc, char **argv)
{
    int port = TELNETD_PORT;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
            port = atoi(argv[++i]);
    }

    /* Ignore SIGPIPE */
    signal(SIGPIPE, SIG_IGN);
    /* Reap children */
    signal(SIGCHLD, sigchld_handler);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        perror("telnetd: socket");
        return 1;
    }

    int opt = 1;
    setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(listenfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("telnetd: bind");
        close(listenfd);
        return 1;
    }

    if (listen(listenfd, BACKLOG) < 0) {
        perror("telnetd: listen");
        close(listenfd);
        return 1;
    }

    printf("telnetd: listening on port %d\n", port);

    for (;;) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int connfd = accept(listenfd, (struct sockaddr *)&client, &clen);
        if (connfd < 0) {
            if (errno == EINTR)
                continue;
            perror("telnetd: accept");
            continue;
        }

        unsigned char *ip = (unsigned char *)&client.sin_addr.s_addr;
        printf("telnetd: connection from %d.%d.%d.%d:%d\n",
               ip[0], ip[1], ip[2], ip[3], ntohs(client.sin_port));

        pid_t pid = fork();
        if (pid < 0) {
            close(connfd);
            continue;
        }

        if (pid == 0) {
            /* Child handles this connection */
            close(listenfd);
            handle_connection(connfd);
            _exit(0);
        }

        /* Parent continues accepting */
        close(connfd);
    }
}
