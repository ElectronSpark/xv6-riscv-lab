// Shell.

#include "kernel/inc/types.h"
#include "user/user.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/vfs/stat.h"

// Linux-compatible dirent structure for getdents (used by ls)
struct linux_dirent64 {
    uint64 d_ino;    // Inode number
    int64 d_off;     // Offset to next structure
    uint16 d_reclen; // Size of this dirent
    uint8 d_type;    // File type
    char d_name[];   // Filename (null-terminated)
};

#define NAME_MAX 255
#define LS_FMT_WIDTH 14 // Display width for formatting

// Parsed command representation
#define EXEC 1
#define REDIR 2
#define PIPE 3
#define LIST 4
#define BACK 5

#define MAXARGS 10

struct cmd {
    int type;
};

struct execcmd {
    int type;
    char *argv[MAXARGS];
    char *eargv[MAXARGS];
};

struct redircmd {
    int type;
    struct cmd *cmd;
    char *file;
    char *efile;
    int mode;
    int fd;
};

struct pipecmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct listcmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct backcmd {
    int type;
    struct cmd *cmd;
};

void panic(char *);
struct cmd *parsecmd(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

// Current working directory path buffer
static char cwd_path[512] = "/";

// Update cwd_path using getcwd syscall
static void update_cwd(void) {
    if (getcwd(cwd_path, sizeof(cwd_path)) == 0) {
        // On failure, keep previous value or set to "?"
        strcpy(cwd_path, "?");
    }
}

// Built-in ls command implementation
static char *ls_fmtname(char *path) {
    static char buf[LS_FMT_WIDTH + 1];
    char *p;

    // Find first character after last slash.
    for (p = path + strlen(path); p >= path && *p != '/'; p--)
        ;
    p++;

    // Return blank-padded name.
    int len = strlen(p);
    if (len >= LS_FMT_WIDTH)
        return p;
    memmove(buf, p, len);
    memset(buf + len, ' ', LS_FMT_WIDTH - len);
    buf[LS_FMT_WIDTH] = 0;
    return buf;
}

static void builtin_ls(char *path) {
    char buf[512], *p;
    int fd;
    struct stat st;
    char dirent_buf[1024]; // Buffer for getdents
    int nread;

    if ((fd = open(path, O_RDONLY | O_NOFOLLOW)) < 0) {
        fprintf(2, "ls: cannot open %s\n", path);
        return;
    }

    if (fstat(fd, &st) < 0) {
        fprintf(2, "ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    if (S_ISREG(st.mode) || S_ISCHR(st.mode) || S_ISBLK(st.mode)) {
        printf("%s %o %ld %ld\n", ls_fmtname(path), st.mode, st.ino, st.size);
    } else if (S_ISDIR(st.mode)) {
        if (strlen(path) + 1 + NAME_MAX + 1 > sizeof buf) {
            printf("ls: path too long\n");
            close(fd);
            return;
        }
        strcpy(buf, path);
        p = buf + strlen(buf);
        *p++ = '/';

        // Use getdents to read directory entries
        while ((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
            int pos = 0;
            while (pos < nread) {
                struct linux_dirent64 *de =
                    (struct linux_dirent64 *)(dirent_buf + pos);
                if (de->d_ino == 0) {
                    pos += de->d_reclen;
                    continue;
                }
                strcpy(p, de->d_name);
                if (stat(buf, &st) < 0) {
                    printf("ls: cannot stat %s\n", buf);
                    pos += de->d_reclen;
                    continue;
                }
                printf("%s %o %ld %ld\n", ls_fmtname(buf), st.mode, st.ino,
                       st.size);
                pos += de->d_reclen;
            }
        }
    }
    close(fd);
}

// Helper for pipe left side: redirect stdout to pipe write end, then run cmd
// Never returns - safe to call after vfork
static void run_pipe_left(struct cmd *cmd, int *p) __attribute__((noreturn));
static void run_pipe_left(struct cmd *cmd, int *p) {
    close(1);
    dup(p[1]);
    close(p[0]);
    close(p[1]);
    runcmd(cmd);
}

// Helper for pipe right side: redirect stdin to pipe read end, then run cmd
// Never returns - safe to call after vfork
static void run_pipe_right(struct cmd *cmd, int *p) __attribute__((noreturn));
static void run_pipe_right(struct cmd *cmd, int *p) {
    close(0);
    dup(p[0]);
    close(p[0]);
    close(p[1]);
    runcmd(cmd);
}

// Execute cmd.  Never returns.
void runcmd(struct cmd *cmd) {
    int p[2];
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;
    int pid;

    if (cmd == 0)
        exit(1);

    switch (cmd->type) {
    default:
        panic("runcmd");

    case EXEC:
        ecmd = (struct execcmd *)cmd;
        if (ecmd->argv[0] == 0)
            exit(1);
        exec(ecmd->argv[0], ecmd->argv);
        size_t len = strlen(ecmd->argv[0]);
        for (int i = 0; i < len; i++) {
            if (ecmd->argv[0][i] == '\x1b') {
                ecmd->argv[0][i] = '[';
            }
        }
        fprintf(2, "exec %s failed\n", ecmd->argv[0]);
        break;

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        close(rcmd->fd);
        if (open(rcmd->file, rcmd->mode) < 0) {
            fprintf(2, "open %s failed\n", rcmd->file);
            exit(1);
        }
        runcmd(rcmd->cmd);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        // vfork child calls runcmd which never returns - safe because
        // child uses stack space above parent's frame
        pid = vfork();
        if (pid < 0)
            panic("vfork");
        if (pid == 0)
            runcmd(lcmd->left);
        wait(0);
        runcmd(lcmd->right);
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        if (pipe(p) < 0)
            panic("pipe");

        // Left side of pipe: stdout -> pipe write end
        pid = vfork();
        if (pid < 0)
            panic("vfork");
        if (pid == 0)
            run_pipe_left(pcmd->left, p);

        // Right side of pipe: stdin <- pipe read end
        pid = vfork();
        if (pid < 0)
            panic("vfork");
        if (pid == 0)
            run_pipe_right(pcmd->right, p);

        close(p[0]);
        close(p[1]);
        wait(0);
        wait(0);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        pid = vfork();
        if (pid < 0)
            panic("vfork");
        if (pid == 0)
            runcmd(bcmd->cmd);
        // Don't wait - that's the point of &
        break;
    }
    exit(0);
}

int getcmd(char *buf, int nbuf) {
    fprintf(2, "%s $ ", cwd_path);
    memset(buf, 0, nbuf);
    gets(buf, nbuf);
    if (buf[0] == 0) // EOF
        return -1;
    return 0;
}

int main(void) {
    static char buf[100];
    int fd;

    // Ensure that three file descriptors are open.
    while ((fd = open("/dev/console", O_RDWR)) >= 0) {
        if (fd >= 3) {
            close(fd);
            break;
        }
    }

    // Initialize cwd path
    update_cwd();
    // Read and run input commands.
    while (getcmd(buf, sizeof(buf)) >= 0) {
        // Built-in: cd
        if (buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
            // Chdir must be called by the parent, not the child.
            buf[strlen(buf) - 1] = 0; // chop \n
            if (chdir(buf + 3) < 0)
                fprintf(2, "cannot cd %s\n", buf + 3);
            else
                update_cwd();
            continue;
        }
        // Built-in: ls
        if (buf[0] == 'l' && buf[1] == 's' &&
            (buf[2] == '\n' || buf[2] == ' ')) {
            buf[strlen(buf) - 1] = 0; // chop \n
            if (buf[2] == 0 || buf[3] == 0) {
                // ls with no arguments
                builtin_ls(".");
            } else {
                // ls with path argument
                builtin_ls(buf + 3);
            }
            continue;
        }

        // Parse command first (before vfork)
        struct cmd *cmd = parsecmd(buf);
        if (cmd == 0)
            continue; // Parse error, try next command
        int pid;

        // vfork + runcmd: child calls runcmd which never returns,
        // so parent's call frame stays intact
        pid = vfork();
        if (pid < 0)
            panic("vfork");
        if (pid == 0)
            runcmd(cmd);
        wait(0);
    }
    exit(0);
}

void panic(char *s) {
    fprintf(2, "%s\n", s);
    exit(1);
}

// PAGEBREAK!
//  Constructors

struct cmd *execcmd(void) {
    struct execcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;
    return (struct cmd *)cmd;
}

struct cmd *redircmd(struct cmd *subcmd, char *file, char *efile, int mode,
                     int fd) {
    struct redircmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd *)cmd;
}

struct cmd *pipecmd(struct cmd *left, struct cmd *right) {
    struct pipecmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

struct cmd *listcmd(struct cmd *left, struct cmd *right) {
    struct listcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

struct cmd *backcmd(struct cmd *subcmd) {
    struct backcmd *cmd;

    cmd = malloc(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;
    return (struct cmd *)cmd;
}
// PAGEBREAK!
//  Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int gettoken(char **ps, char *es, char **q, char **eq) {
    char *s;
    int ret;

    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    if (q)
        *q = s;
    ret = *s;
    switch (*s) {
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        s++;
        break;
    case '>':
        s++;
        if (*s == '>') {
            ret = '+';
            s++;
        }
        break;
    default:
        ret = 'a';
        while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
            s++;
        break;
    }
    if (eq)
        *eq = s;

    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return ret;
}

int peek(char **ps, char *es, char *toks) {
    char *s;

    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *parsecmd(char *s) {
    char *es;
    struct cmd *cmd;

    es = s + strlen(s);
    cmd = parseline(&s, es);
    peek(&s, es, "");
    if (s != es) {
        fprintf(2, "syntax error near: %s\n", s);
        return 0; // Return NULL instead of panicking
    }
    nulterminate(cmd);
    return cmd;
}

struct cmd *parseline(char **ps, char *es) {
    struct cmd *cmd;

    cmd = parsepipe(ps, es);
    while (peek(ps, es, "&")) {
        gettoken(ps, es, 0, 0);
        cmd = backcmd(cmd);
        // After &, if there's more input, parse it as a continuation
        // This allows "cmd1 & cmd2" syntax (implicit ; after &)
        if (*ps < es && !peek(ps, es, ";&")) {
            cmd = listcmd(cmd, parseline(ps, es));
            return cmd;
        }
    }
    if (peek(ps, es, ";")) {
        gettoken(ps, es, 0, 0);
        cmd = listcmd(cmd, parseline(ps, es));
    }
    return cmd;
}

struct cmd *parsepipe(char **ps, char *es) {
    struct cmd *cmd;

    cmd = parseexec(ps, es);
    if (peek(ps, es, "|")) {
        gettoken(ps, es, 0, 0);
        cmd = pipecmd(cmd, parsepipe(ps, es));
    }
    return cmd;
}

struct cmd *parseredirs(struct cmd *cmd, char **ps, char *es) {
    int tok;
    char *q, *eq;

    while (peek(ps, es, "<>")) {
        tok = gettoken(ps, es, 0, 0);
        if (gettoken(ps, es, &q, &eq) != 'a')
            panic("missing file for redirection");
        switch (tok) {
        case '<':
            cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
            break;
        case '>':
            cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREAT | O_TRUNC, 1);
            break;
        case '+': // >>
            cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREAT, 1);
            break;
        }
    }
    return cmd;
}

struct cmd *parseblock(char **ps, char *es) {
    struct cmd *cmd;

    if (!peek(ps, es, "("))
        panic("parseblock");
    gettoken(ps, es, 0, 0);
    cmd = parseline(ps, es);
    if (!peek(ps, es, ")"))
        panic("syntax - missing )");
    gettoken(ps, es, 0, 0);
    cmd = parseredirs(cmd, ps, es);
    return cmd;
}

struct cmd *parseexec(char **ps, char *es) {
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    if (peek(ps, es, "("))
        return parseblock(ps, es);

    ret = execcmd();
    cmd = (struct execcmd *)ret;

    argc = 0;
    ret = parseredirs(ret, ps, es);
    while (!peek(ps, es, "|)&;")) {
        if ((tok = gettoken(ps, es, &q, &eq)) == 0)
            break;
        if (tok != 'a')
            panic("syntax");
        cmd->argv[argc] = q;
        cmd->eargv[argc] = eq;
        argc++;
        if (argc >= MAXARGS)
            panic("too many args");
        ret = parseredirs(ret, ps, es);
    }
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
}

// NUL-terminate all the counted strings.
struct cmd *nulterminate(struct cmd *cmd) {
    int i;
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0)
        return 0;

    switch (cmd->type) {
    case EXEC:
        ecmd = (struct execcmd *)cmd;
        for (i = 0; ecmd->argv[i]; i++)
            *ecmd->eargv[i] = 0;
        break;

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        nulterminate(rcmd->cmd);
        *rcmd->efile = 0;
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        nulterminate(bcmd->cmd);
        break;
    }
    return cmd;
}
