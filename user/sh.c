// Enhanced Shell with line editing, history, tab completion, and job control
//
// Features:
// - Command history (up/down arrows, ring buffer)
// - Tab completion for commands (PATH) and files
// - Cursor movement (left/right) and mid-line editing
// - Ctrl+C/D/U/K/A/E/L key bindings
// - Raw terminal mode for character-by-character input
// - Environment variables ($VAR, ${VAR}, export/unset/env)
// - PATH-based command lookup
// - Foreground process group management (TIOCSPGRP)
// - Enhanced ls with permissions, types, sizes

#ifdef USE_NCURSES_SHELL
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/termios.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <curses.h>
#include <pwd.h>
extern int getdents(int fd, void *dirp, int count);
extern char **environ;
static int exec(const char *path, char **argv) { return execve(path, argv, environ); }
static inline void waitgdb(void) {
    asm volatile("li a0, 0\n\tebreak" ::: "a0", "memory");
}
static inline void waitgdb_stopentry(void) {
    asm volatile("li a0, 1\n\tebreak" ::: "a0", "memory");
}
#else
#include "user.h"
#include "kernel/inc/syscall.h"
#include "kernel/inc/vfs/fcntl.h"
#include "kernel/inc/vfs/stat.h"
#include "kernel/inc/tty/termios.h"
// Stubs — xv6 userlib has no real environ; the shell's internal
// env_vars[] table is the authoritative store.
static int setenv(const char *name, const char *value, int overwrite) {
    (void)name; (void)value; (void)overwrite;
    return 0;
}
static int unsetenv(const char *name) {
    (void)name;
    return 0;
}

static int exec_with_env(const char *path, char **argv, char **envp) {
#if defined(__x86_64__)
    long ret;
    __asm__ volatile("syscall"
                 : "=a"(ret)
                 : "a"((long)SYS_exec), "D"(path), "S"(argv), "d"(envp)
                 : "rcx", "r11", "memory");
    return (int)ret;
#elif defined(__riscv)
    register uint64 arg0 asm("a0") = (uint64)path;
    register uint64 arg1 asm("a1") = (uint64)argv;
    register uint64 arg2 asm("a2") = (uint64)envp;
    register uint64 syscall_num asm("a7") = SYS_exec;
    asm volatile("ecall"
                 : "+r"(arg0)
                 : "r"(arg1), "r"(arg2), "r"(syscall_num)
                 : "memory");
    return (int)arg0;
#endif
}
#endif

// Linux-compatible dirent structure for getdents
struct linux_dirent64 {
#ifdef USE_NCURSES_SHELL
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
#else
    uint64 d_ino;
    int64 d_off;
    uint16 d_reclen;
    uint8 d_type;
#endif
    char d_name[];
};

#define NAME_MAX 255

// =====================================================================
// Parsed command representation (same as original xv6 shell)
// =====================================================================
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

// Forward declarations
void panic(char *);
struct cmd *parsecmd(char *);
void runcmd(struct cmd *) __attribute__((noreturn));

// =====================================================================
// Utility helpers
// =====================================================================

static int strncmp_local(const char *a, const char *b, int n) {
    while (n > 0) {
        if (*a != *b)
            return (unsigned char)*a - (unsigned char)*b;
        if (*a == 0)
            return 0;
        a++;
        b++;
        n--;
    }
    return 0;
}

static char *strcat_local(char *dest, const char *src) {
    char *d = dest;
    while (*d)
        d++;
    while ((*d++ = *src++))
        ;
    return dest;
}

// Simple itoa into caller buffer, returns length written.
static int itoa_local(int val, char *buf, int bufsz) {
    if (bufsz < 2)
        return 0;
    if (val < 0) {
        buf[0] = '-';
        int n = itoa_local(-val, buf + 1, bufsz - 1);
        return n + 1;
    }
    if (val == 0) {
        buf[0] = '0';
        buf[1] = 0;
        return 1;
    }
    char tmp[16];
    int i = 0;
    while (val && i < 15) {
        tmp[i++] = '0' + (val % 10);
        val /= 10;
    }
    int len = i;
    if (len >= bufsz)
        len = bufsz - 1;
    for (int j = 0; j < len; j++)
        buf[j] = tmp[len - 1 - j];
    buf[len] = 0;
    return len;
}

// =====================================================================
// Line editing state
// =====================================================================

#define LINE_BUF_SIZE 256
#define HISTORY_SIZE 32

static char line_buf[LINE_BUF_SIZE];
static int line_len;
static int cursor_pos;

// History ring buffer
static char history[HISTORY_SIZE][LINE_BUF_SIZE];
static int history_count; // total entries stored
static int history_idx;   // browsing index
static int history_start; // oldest entry position in ring

// Terminal state
static struct termios orig_termios;
static int raw_mode;
#ifdef USE_NCURSES_SHELL
static int ncurses_active;
static int ncurses_initialized;
#endif

// Current working directory (for prompt)
static char cwd_path[512] = "/";

// Current user info (for prompt)
static char user_name[64] = "?";
static int  user_uid = -1;

#ifdef USE_NCURSES_SHELL
#define errprintf(...) fprintf(stderr, __VA_ARGS__)
#define ST_MODE(x) ((x).st_mode)
#define ST_MODE_P(x) ((x)->st_mode)
#define ST_NLINK_P(x) ((x)->st_nlink)
#define ST_SIZE_P(x) ((x)->st_size)
#else
#define errprintf(...) fprintf(2, __VA_ARGS__)
#define ST_MODE(x) ((x).st_mode)
#define ST_MODE_P(x) ((x)->st_mode)
#define ST_NLINK_P(x) ((x)->st_nlink)
#define ST_SIZE_P(x) ((x)->st_size)
#endif

// =====================================================================
// Environment variables
// =====================================================================

#define MAX_ENV_VARS 64
#define MAX_ENV_NAME 64
#define MAX_ENV_VALUE 256

struct env_var {
    char name[MAX_ENV_NAME];
    char value[MAX_ENV_VALUE];
    int used;
};

static struct env_var env_vars[MAX_ENV_VARS];

static int env_set(const char *name, const char *value);

static void env_init(void) {
    for (int i = 0; i < MAX_ENV_VARS; i++)
        env_vars[i].used = 0;
    env_set("PATH", "/:/bin");
    env_set("HOME", "/");
    env_set("TERM", "xterm");
    env_set("LANG", "C.UTF-8");
    env_set("LC_ALL", "C.UTF-8");
    env_set("PYTHONUTF8", "1");
    env_set("PYTHONIOENCODING", "utf-8");
    env_set("PYTHONHOME", "/usr/local");
    env_set("PYTHONPATH", "/usr/local/lib/python3.12");
    env_set("PYTHONDONTWRITEBYTECODE", "1");
}

static char *env_get(const char *name) {
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0)
            return env_vars[i].value;
    }
    return 0;
}

static int env_set(const char *name, const char *value) {
    // Keep standard environ in sync so exec'd children inherit env
    setenv(name, value, 1);
    // Update existing
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0) {
            int vlen = strlen(value);
            if (vlen >= MAX_ENV_VALUE)
                vlen = MAX_ENV_VALUE - 1;
            memcpy(env_vars[i].value, value, vlen);
            env_vars[i].value[vlen] = 0;
            return 0;
        }
    }
    // Find empty slot
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (!env_vars[i].used) {
            int nlen = strlen(name);
            int vlen = strlen(value);
            if (nlen >= MAX_ENV_NAME)
                nlen = MAX_ENV_NAME - 1;
            if (vlen >= MAX_ENV_VALUE)
                vlen = MAX_ENV_VALUE - 1;
            memcpy(env_vars[i].name, name, nlen);
            env_vars[i].name[nlen] = 0;
            memcpy(env_vars[i].value, value, vlen);
            env_vars[i].value[vlen] = 0;
            env_vars[i].used = 1;
            return 0;
        }
    }
    return -1;
}

static int env_unset(const char *name) {
    unsetenv(name);
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used && strcmp(env_vars[i].name, name) == 0) {
            env_vars[i].used = 0;
            return 0;
        }
    }
    return -1;
}

static void env_list(void) {
    for (int i = 0; i < MAX_ENV_VARS; i++) {
        if (env_vars[i].used)
            printf("%s=%s\n", env_vars[i].name, env_vars[i].value);
    }
}

// Expand $VAR, ${VAR}, $$, $? in a string.
// Returns pointer to a static buffer.
static char expand_buf[LINE_BUF_SIZE * 2];

static char *expand_env_vars(const char *input) {
    char *out = expand_buf;
    char *out_end = expand_buf + sizeof(expand_buf) - 1;
    const char *p = input;

    while (*p && out < out_end) {
        if (*p == '$') {
            p++;
            if (*p == '{') {
                // ${VAR}
                p++;
                const char *start = p;
                while (*p && *p != '}')
                    p++;
                int len = p - start;
                if (*p == '}')
                    p++;
                char varname[MAX_ENV_NAME];
                if (len >= MAX_ENV_NAME)
                    len = MAX_ENV_NAME - 1;
                memcpy(varname, start, len);
                varname[len] = 0;
                char *val = env_get(varname);
                if (val) {
                    while (*val && out < out_end)
                        *out++ = *val++;
                }
            } else if (*p == '$') {
                // $$ = PID
                p++;
                char pidbuf[16];
                int n = itoa_local(getpid(), pidbuf, sizeof(pidbuf));
                for (int j = 0; j < n && out < out_end; j++)
                    *out++ = pidbuf[j];
            } else if (*p == '?') {
                // $? – not tracked, just emit '0'
                p++;
                if (out < out_end)
                    *out++ = '0';
            } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       *p == '_') {
                // $VAR
                const char *start = p;
                while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
                       (*p >= '0' && *p <= '9') || *p == '_')
                    p++;
                int len = p - start;
                char varname[MAX_ENV_NAME];
                if (len >= MAX_ENV_NAME)
                    len = MAX_ENV_NAME - 1;
                memcpy(varname, start, len);
                varname[len] = 0;
                char *val = env_get(varname);
                if (val) {
                    while (*val && out < out_end)
                        *out++ = *val++;
                }
            } else {
                // Lone $
                *out++ = '$';
            }
        } else {
            *out++ = *p++;
        }
    }
    *out = 0;
    return expand_buf;
}

// =====================================================================
// Terminal control
// =====================================================================

static void enable_raw_mode(void) {
    if (raw_mode)
        return;
#ifdef USE_NCURSES_SHELL
    struct termios raw;
    if (tcgetattr(0, &orig_termios) < 0)
        return;
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ISIG);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &raw) < 0)
        return;
    raw_mode = 1;
    return;
#else
    struct termios raw;
    if (tcgetattr(0, &orig_termios) < 0)
        return;
    raw = orig_termios;
    raw.c_lflag &= ~(ICANON | ECHO | ECHOE | ECHOK | ISIG);
    raw.c_iflag &= ~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(0, TCSAFLUSH, &raw) < 0)
        return;
    raw_mode = 1;
#endif
}

static void disable_raw_mode(void) {
    if (!raw_mode)
        return;
#ifdef USE_NCURSES_SHELL
    tcsetattr(0, TCSAFLUSH, &orig_termios);
    raw_mode = 0;
    return;
#else
    tcsetattr(0, TCSAFLUSH, &orig_termios);
    raw_mode = 0;
#endif
}

static void term_write(const char *s, int n) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        for (int i = 0; i < n; i++) {
            char ch = s[i];
            if (ch == '\r') {
                int y, x;
                getyx(stdscr, y, x);
                move(y, 0);
            } else if (ch == '\b') {
                int y, x;
                getyx(stdscr, y, x);
                if (x > 0)
                    move(y, x - 1);
            } else {
                addch(ch);
            }
        }
        refresh();
        return;
    }
#endif
    write(1, s, n);
}

static void term_putc(char c) { term_write(&c, 1); }

static void term_cursor_left(int n) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        int y, x;
        getyx(stdscr, y, x);
        x -= n;
        if (x < 0)
            x = 0;
        move(y, x);
        refresh();
        return;
    }
#endif
    while (n-- > 0)
        term_write("\x1b[D", 3);
}

static void term_cursor_right(int n) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        int y, x;
        getyx(stdscr, y, x);
        x += n;
        move(y, x);
        refresh();
        return;
    }
#endif
    while (n-- > 0)
        term_write("\x1b[C", 3);
}

static void term_clear_to_eol(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        clrtoeol();
        refresh();
        return;
    }
#endif
    term_write("\x1b[K", 3);
}

static void term_bell(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        beep();
        return;
    }
#endif
    term_putc('\x07');
}

static void term_clear_screen(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        clear();
        move(0, 0);
        refresh();
        return;
    }
#endif
    term_write("\x1b[2J\x1b[H", 7);
}

static void term_show_prompt(void) {
    term_write(user_name, strlen(user_name));
    term_write(":", 1);
    term_write(cwd_path, strlen(cwd_path));
    if (user_uid == 0)
        term_write("# ", 2);
    else
        term_write("$ ", 2);
}

// =====================================================================
// Line buffer operations
// =====================================================================

static void line_clear(void) {
    line_buf[0] = 0;
    line_len = 0;
    cursor_pos = 0;
}

static void line_insert_char(char c) {
    if (line_len >= LINE_BUF_SIZE - 1) {
        term_bell();
        return;
    }
    // Shift right
    for (int i = line_len; i > cursor_pos; i--)
        line_buf[i] = line_buf[i - 1];
    line_buf[cursor_pos] = c;
    line_len++;
    line_buf[line_len] = 0;

    // Write from cursor to end
    term_write(&line_buf[cursor_pos], line_len - cursor_pos);
    cursor_pos++;
    // Move cursor back
    if (cursor_pos < line_len)
        term_cursor_left(line_len - cursor_pos);
}

static void line_delete_char(void) {
    if (cursor_pos == 0) {
        term_bell();
        return;
    }
    cursor_pos--;
    for (int i = cursor_pos; i < line_len - 1; i++)
        line_buf[i] = line_buf[i + 1];
    line_len--;
    line_buf[line_len] = 0;

    term_write("\x08", 1);
    term_write(&line_buf[cursor_pos], line_len - cursor_pos);
    term_clear_to_eol();
    if (cursor_pos < line_len)
        term_cursor_left(line_len - cursor_pos);
}

static void line_delete_forward(void) {
    if (cursor_pos >= line_len) {
        term_bell();
        return;
    }
    for (int i = cursor_pos; i < line_len - 1; i++)
        line_buf[i] = line_buf[i + 1];
    line_len--;
    line_buf[line_len] = 0;

    term_write(&line_buf[cursor_pos], line_len - cursor_pos);
    term_clear_to_eol();
    if (cursor_pos < line_len)
        term_cursor_left(line_len - cursor_pos);
}

static void line_move_left(void) {
    if (cursor_pos > 0) {
        cursor_pos--;
        term_cursor_left(1);
    }
}

static void line_move_right(void) {
    if (cursor_pos < line_len) {
        cursor_pos++;
        term_cursor_right(1);
    }
}

static void line_move_home(void) {
    if (cursor_pos > 0) {
        term_cursor_left(cursor_pos);
        cursor_pos = 0;
    }
}

static void line_move_end(void) {
    if (cursor_pos < line_len) {
        term_cursor_right(line_len - cursor_pos);
        cursor_pos = line_len;
    }
}

static void line_kill_to_end(void) {
    term_clear_to_eol();
    line_len = cursor_pos;
    line_buf[line_len] = 0;
}

static void line_kill_all(void) {
    if (cursor_pos > 0)
        term_cursor_left(cursor_pos);
    term_clear_to_eol();
    line_clear();
}

static void line_set(const char *s) {
    if (cursor_pos > 0)
        term_cursor_left(cursor_pos);
    term_clear_to_eol();

    int n = strlen(s);
    if (n >= LINE_BUF_SIZE)
        n = LINE_BUF_SIZE - 1;
    memcpy(line_buf, s, n);
    line_buf[n] = 0;
    line_len = n;
    cursor_pos = n;
    term_write(line_buf, line_len);
}

// =====================================================================
// History management
// =====================================================================

static void history_add(const char *line) {
    if (line[0] == 0)
        return;
    // Skip duplicate of last entry
    if (history_count > 0) {
        int last = (history_start + history_count - 1) % HISTORY_SIZE;
        if (strcmp(history[last], line) == 0)
            return;
    }
    int idx;
    if (history_count < HISTORY_SIZE) {
        idx = history_count;
        history_count++;
    } else {
        idx = history_start;
        history_start = (history_start + 1) % HISTORY_SIZE;
    }
    int n = strlen(line);
    if (n >= LINE_BUF_SIZE)
        n = LINE_BUF_SIZE - 1;
    memcpy(history[idx], line, n);
    history[idx][n] = 0;
}

static void history_prev(void) {
    if (history_count == 0 || history_idx <= 0) {
        term_bell();
        return;
    }
    history_idx--;
    int idx = (history_start + history_idx) % HISTORY_SIZE;
    line_set(history[idx]);
}

static void history_next(void) {
    if (history_idx < history_count - 1) {
        history_idx++;
        int idx = (history_start + history_idx) % HISTORY_SIZE;
        line_set(history[idx]);
    } else if (history_idx == history_count - 1) {
        history_idx = history_count;
        line_set("");
    } else {
        term_bell();
    }
}

static void history_reset(void) { history_idx = history_count; }

// =====================================================================
// Tab completion
// =====================================================================

#define MAX_MATCHES 64
static char matches[MAX_MATCHES][NAME_MAX + 1];
static int match_count;

static int match_exists(const char *name) {
    for (int i = 0; i < match_count; i++) {
        if (strcmp(matches[i], name) == 0)
            return 1;
    }
    return 0;
}

// Return non-zero if c is a shell word-boundary character.
static int is_word_boundary(char c) {
    return c == ' ' || c == '\t' || c == '|' || c == ';' || c == '&' ||
           c == '>' || c == '<' || c == '(' || c == ')';
}

// Determine word at cursor and whether it is a command position.
static int get_completion_word(char *word, int maxlen, int *is_first_word) {
    int start = cursor_pos;
    while (start > 0 && !is_word_boundary(line_buf[start - 1]))
        start--;

    // Walk everything before start to decide whether this is a command
    // (first-word) position.  Pipe, semicolon, and ampersand reset to
    // command position; redirection operators and normal characters
    // indicate argument position.
    int first = 1;
    for (int i = 0; i < start; i++) {
        char c = line_buf[i];
        if (c == ' ' || c == '\t')
            continue;
        if (c == '|' || c == ';' || c == '&')
            first = 1; // next word is a command
        else
            first = 0; // next word is an argument / filename
    }
    if (is_first_word)
        *is_first_word = first;

    int len = cursor_pos - start;
    if (len >= maxlen)
        len = maxlen - 1;
    memcpy(word, &line_buf[start], len);
    word[len] = 0;
    return start;
}

static void get_completion_dir(const char *word, char *dir, char *prefix,
                               int maxdir) {
    const char *slash = 0;
    for (const char *p = word; *p; p++) {
        if (*p == '/')
            slash = p;
    }
    if (slash) {
        int dlen = slash - word + 1;
        if (dlen >= maxdir)
            dlen = maxdir - 1;
        memcpy(dir, word, dlen);
        dir[dlen] = 0;
        strcpy(prefix, slash + 1);
    } else {
        strcpy(dir, ".");
        strcpy(prefix, word);
    }
}

static void add_matches_from_dir(const char *dir, const char *prefix,
                                 int executables_only) {
    int prefix_len = strlen(prefix);
    int fd = open(dir, O_RDONLY);
    if (fd < 0)
        return;

    char dirent_buf[1024];
    int nread;

    while ((nread = getdents(fd, dirent_buf, sizeof(dirent_buf))) > 0) {
        int pos = 0;
        while (pos < nread && match_count < MAX_MATCHES) {
            struct linux_dirent64 *de =
                (struct linux_dirent64 *)(dirent_buf + pos);
            if (de->d_ino != 0) {
                if (prefix_len == 0 ||
                    strncmp_local(de->d_name, prefix, prefix_len) == 0) {
                    if (strcmp(de->d_name, ".") != 0 &&
                        strcmp(de->d_name, "..") != 0 &&
                        !match_exists(de->d_name)) {
                        if (executables_only) {
                            char path[512];
                            int dlen = strlen(dir);
                            memcpy(path, dir, dlen);
                            if (dlen > 0 && dir[dlen - 1] != '/')
                                path[dlen++] = '/';
                            strcpy(path + dlen, de->d_name);
                            struct stat st;
                            if (stat(path, &st) == 0 && S_ISREG(ST_MODE(st)))
                                strcpy(matches[match_count++], de->d_name);
                        } else {
                            strcpy(matches[match_count++], de->d_name);
                        }
                    }
                }
            }
            pos += de->d_reclen;
        }
    }
    close(fd);
}

static void collect_path_matches(const char *prefix) {
    match_count = 0;

    // Files in current directory
    add_matches_from_dir(".", prefix, 0);

    // Executables from PATH
    char *path = env_get("PATH");
    if (path) {
        char pathcopy[MAX_ENV_VALUE];
        int plen = strlen(path);
        if (plen >= MAX_ENV_VALUE)
            plen = MAX_ENV_VALUE - 1;
        memcpy(pathcopy, path, plen);
        pathcopy[plen] = 0;

        char *p = pathcopy;
        while (*p && match_count < MAX_MATCHES) {
            char *start = p;
            while (*p && *p != ':')
                p++;
            int dlen = p - start;
            if (dlen > 0 && dlen < 256) {
                char dir[256];
                memcpy(dir, start, dlen);
                dir[dlen] = 0;
                add_matches_from_dir(dir, prefix, 1);
            }
            if (*p == ':')
                p++;
        }
    }

    // Built-in commands
    static const char *builtins[] = {
        "cd", "ls", "echo", "exit", "export", "unset", "env", "history",
        "waitgdb", 0};
    int prefix_len = strlen(prefix);
    for (int i = 0; builtins[i] && match_count < MAX_MATCHES; i++) {
        if (prefix_len == 0 ||
            strncmp_local(builtins[i], prefix, prefix_len) == 0) {
            if (!match_exists(builtins[i]))
                strcpy(matches[match_count++], builtins[i]);
        }
    }
}

static int common_prefix_len(void) {
    if (match_count <= 1)
        return match_count ? (int)strlen(matches[0]) : 0;
    int len = 0;
    while (1) {
        char c = matches[0][len];
        if (c == 0)
            break;
        int ok = 1;
        for (int i = 1; i < match_count; i++) {
            if (matches[i][len] != c) {
                ok = 0;
                break;
            }
        }
        if (!ok)
            break;
        len++;
    }
    return len;
}

static void do_tab_completion(void) {
    char word[NAME_MAX + 1];
    char dir[256];
    char prefix[NAME_MAX + 1];
    int is_first_word = 0;

    (void)get_completion_word(word, sizeof(word), &is_first_word);
    get_completion_dir(word, dir, prefix, sizeof(dir));

    if (is_first_word && strcmp(dir, ".") == 0)
        collect_path_matches(prefix);
    else {
        match_count = 0;
        add_matches_from_dir(dir, prefix, 0);
    }

    if (match_count == 0) {
        term_bell();
        return;
    }

    int prefix_len = strlen(prefix);
    int common = common_prefix_len();

    if (common > prefix_len) {
        for (int i = prefix_len; i < common; i++)
            line_insert_char(matches[0][i]);

        if (match_count == 1) {
            // Append / for dirs, space otherwise
            char path[512];
            if (strcmp(dir, ".") == 0)
                strcpy(path, matches[0]);
            else {
                strcpy(path, dir);
                strcat_local(path, matches[0]);
            }
            struct stat st;
            if (stat(path, &st) == 0 && S_ISDIR(ST_MODE(st)))
                line_insert_char('/');
            else
                line_insert_char(' ');
        }
    } else if (match_count > 1) {
        // Show candidates
        term_write("\r\n", 2);

        int maxlen = 0;
        for (int i = 0; i < match_count; i++) {
            int len = strlen(matches[i]);
            if (len > maxlen)
                maxlen = len;
        }
        int colwidth = maxlen + 2;
        if (colwidth < 8)
            colwidth = 8;
        int cols = 80 / colwidth;
        if (cols < 1)
            cols = 1;

        for (int i = 0; i < match_count; i++) {
            int len = strlen(matches[i]);
            term_write(matches[i], len);
            if ((i + 1) % cols == 0 || i == match_count - 1) {
                term_write("\r\n", 2);
            } else {
                for (int j = len; j < colwidth; j++)
                    term_putc(' ');
            }
        }

        // Re-display prompt + current line
        term_show_prompt();
        term_write(line_buf, line_len);
        if (cursor_pos < line_len)
            term_cursor_left(line_len - cursor_pos);
    }
}

// =====================================================================
// Escape sequence parsing
// =====================================================================

static int read_char(void) {
    char c;
    for (;;) {
        int n = read(0, &c, 1);
        if (n == 1)
            return (unsigned char)c;
        // On xv6 console, raw reads can transiently return 0 (or <0) during
        // mode transitions; keep waiting instead of treating as EOF.
    }
}

#define KEY_EXT_BASE 0x100
#define KEY_EXT_UP (KEY_EXT_BASE + 1)
#define KEY_EXT_DOWN (KEY_EXT_BASE + 2)
#define KEY_EXT_LEFT (KEY_EXT_BASE + 3)
#define KEY_EXT_RIGHT (KEY_EXT_BASE + 4)
#define KEY_EXT_HOME (KEY_EXT_BASE + 5)
#define KEY_EXT_END (KEY_EXT_BASE + 6)
#define KEY_EXT_DELETE (KEY_EXT_BASE + 7)

static int read_key(void) {
#ifdef USE_NCURSES_SHELL
    if (ncurses_active) {
        int c = getch();
        switch (c) {
        case KEY_UP:
            return KEY_EXT_UP;
        case KEY_DOWN:
            return KEY_EXT_DOWN;
        case KEY_LEFT:
            return KEY_EXT_LEFT;
        case KEY_RIGHT:
            return KEY_EXT_RIGHT;
        case KEY_HOME:
            return KEY_EXT_HOME;
        case KEY_END:
            return KEY_EXT_END;
        case KEY_DC:
            return KEY_EXT_DELETE;
        case KEY_BACKSPACE:
            return 0x7f;
        case KEY_ENTER:
            return '\n';
        default:
            return c;
        }
    }
#endif

    int c = read_char();
    if (c != 0x1b)
        return c;

    int c1 = read_char();
    if (c1 < 0)
        return 0x1b;
    if (c1 == '[') {
        int c2 = read_char();
        if (c2 < 0)
            return 0x1b;
        switch (c2) {
        case 'A':
            return KEY_EXT_UP;
        case 'B':
            return KEY_EXT_DOWN;
        case 'C':
            return KEY_EXT_RIGHT;
        case 'D':
            return KEY_EXT_LEFT;
        case 'H':
            return KEY_EXT_HOME;
        case 'F':
            return KEY_EXT_END;
        case '3':
            if (read_char() == '~')
                return KEY_EXT_DELETE;
            break;
        case '1':
        case '7':
            if (read_char() == '~')
                return KEY_EXT_HOME;
            break;
        case '4':
        case '8':
            if (read_char() == '~')
                return KEY_EXT_END;
            break;
        }
        return 0x1b;
    }
    if (c1 == 'O') {
        int c2 = read_char();
        if (c2 == 'A')
            return KEY_EXT_UP;
        if (c2 == 'B')
            return KEY_EXT_DOWN;
        if (c2 == 'C')
            return KEY_EXT_RIGHT;
        if (c2 == 'D')
            return KEY_EXT_LEFT;
        if (c2 == 'H')
            return KEY_EXT_HOME;
        if (c2 == 'F')
            return KEY_EXT_END;
    }
    return 0x1b;
}

// =====================================================================
// readline – main line reading loop
// =====================================================================

static int do_readline(char *buf, int nbuf) {
    line_clear();
    history_reset();
    enable_raw_mode();
    term_show_prompt();

    while (1) {
        int c = read_key();
        if (c < 0) {
            disable_raw_mode();
            return -1;
        }

        switch (c) {
        case '\r':
        case '\n':
            term_write("\r\n", 2);
            disable_raw_mode();
            if (line_len > 0)
                history_add(line_buf);
            if (line_len >= nbuf)
                line_len = nbuf - 1;
            memcpy(buf, line_buf, line_len);
            buf[line_len] = '\n';
            buf[line_len + 1] = 0;
            return line_len + 1;

        case 0x04: // Ctrl+D
            if (line_len == 0) {
                term_write("\r\n", 2);
                disable_raw_mode();
                return -1;
            }
            line_delete_forward();
            break;

        case 0x03: // Ctrl+C
            term_write("^C\r\n", 4);
            line_clear();
            disable_raw_mode();
            enable_raw_mode();
            term_show_prompt();
            break;

        case 0x15: // Ctrl+U
            line_kill_all();
            break;

        case 0x0b: // Ctrl+K
            line_kill_to_end();
            break;

        case 0x01: // Ctrl+A
            line_move_home();
            break;

        case 0x05: // Ctrl+E
            line_move_end();
            break;

        case 0x08: // Ctrl+H / backspace
        case 0x7f: // DEL
            line_delete_char();
            break;

        case '\t':
            do_tab_completion();
            break;

        case KEY_EXT_UP:
            history_prev();
            break;

        case KEY_EXT_DOWN:
            history_next();
            break;

        case KEY_EXT_LEFT:
            line_move_left();
            break;

        case KEY_EXT_RIGHT:
            line_move_right();
            break;

        case KEY_EXT_HOME:
            line_move_home();
            break;

        case KEY_EXT_END:
            line_move_end();
            break;

        case KEY_EXT_DELETE:
            line_delete_forward();
            break;

        case 0x0c: // Ctrl+L – clear screen
            term_clear_screen();
            term_show_prompt();
            term_write(line_buf, line_len);
            if (cursor_pos < line_len)
                term_cursor_left(line_len - cursor_pos);
            break;

        default:
            if (c >= 32 && c < 127)
                line_insert_char(c);
            break;
        }
    }
}

// =====================================================================
// Built-in commands
// =====================================================================

static void update_cwd(void) {
    if (getcwd(cwd_path, sizeof(cwd_path)) == 0)
        strcpy(cwd_path, "?");
}

// Resolve username from /etc/passwd for the current uid.
static void update_user_info(void) {
#ifdef USE_NCURSES_SHELL
    user_uid = (int)getuid();
    struct passwd *pw = getpwuid(user_uid);
    if (pw && pw->pw_name) {
        int n = strlen(pw->pw_name);
        if (n >= (int)sizeof(user_name))
            n = sizeof(user_name) - 1;
        memcpy(user_name, pw->pw_name, n);
        user_name[n] = 0;
    } else {
        itoa_local(user_uid, user_name, sizeof(user_name));
    }
#else
    user_uid = getuid();
    // Parse /etc/passwd: each line is  name:x:uid:gid:...
    int fd = open("/etc/passwd", O_RDONLY);
    if (fd < 0) {
        itoa_local(user_uid, user_name, sizeof(user_name));
        return;
    }
    char pbuf[1024];
    int total = 0, n;
    while ((n = read(fd, pbuf + total, sizeof(pbuf) - total - 1)) > 0)
        total += n;
    close(fd);
    pbuf[total] = 0;

    char *p = pbuf;
    while (*p) {
        // Parse one line: name:x:uid:...
        char *line_start = p;
        // Find first colon (end of name)
        char *c1 = 0;
        for (char *q = p; *q && *q != '\n'; q++) {
            if (*q == ':' && !c1) { c1 = q; break; }
        }
        if (!c1) { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        // Skip password field → find second colon
        char *c2 = 0;
        for (char *q = c1 + 1; *q && *q != '\n'; q++) {
            if (*q == ':') { c2 = q; break; }
        }
        if (!c2) { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        // Parse uid number after second colon
        int pw_uid = 0;
        for (char *q = c2 + 1; *q >= '0' && *q <= '9'; q++)
            pw_uid = pw_uid * 10 + (*q - '0');
        if (pw_uid == user_uid) {
            int namelen = c1 - line_start;
            if (namelen >= (int)sizeof(user_name))
                namelen = sizeof(user_name) - 1;
            memcpy(user_name, line_start, namelen);
            user_name[namelen] = 0;
            return;
        }
        while (*p && *p != '\n') p++;
        if (*p) p++;
    }
    // Not found — use numeric uid
    itoa_local(user_uid, user_name, sizeof(user_name));
#endif
}

// ---- Enhanced ls ----

static char mode_type_char(mode_t m) {
    if (S_ISDIR(m))
        return 'd';
    if (S_ISLNK(m))
        return 'l';
    if (S_ISCHR(m))
        return 'c';
    if (S_ISBLK(m))
        return 'b';
    if (S_ISFIFO(m))
        return 'p';
    if (S_ISSOCK(m))
        return 's';
    return '-';
}

static void format_permissions(mode_t m, char *buf) {
    buf[0] = (m & S_IRUSR) ? 'r' : '-';
    buf[1] = (m & S_IWUSR) ? 'w' : '-';
    buf[2] = (m & S_IXUSR) ? 'x' : '-';
    buf[3] = (m & S_IRGRP) ? 'r' : '-';
    buf[4] = (m & S_IWGRP) ? 'w' : '-';
    buf[5] = (m & S_IXGRP) ? 'x' : '-';
    buf[6] = (m & S_IROTH) ? 'r' : '-';
    buf[7] = (m & S_IWOTH) ? 'w' : '-';
    buf[8] = (m & S_IXOTH) ? 'x' : '-';
    buf[9] = 0;
}

static char name_indicator(mode_t m) {
    if (S_ISDIR(m))
        return '/';
    if (S_ISLNK(m))
        return '@';
    if (S_ISSOCK(m))
        return '=';
    if (S_ISFIFO(m))
        return '|';
    if (m & (S_IXUSR | S_IXGRP | S_IXOTH))
        return '*';
    return 0;
}

static void ls_print_entry(char *path, struct stat *st) {
    char perms[10];
    format_permissions(ST_MODE_P(st), perms);

    // Extract basename
    char *name;
    for (name = path + strlen(path); name >= path && *name != '/'; name--)
        ;
    name++;

    char ind = name_indicator(ST_MODE_P(st));

    if (ind)
         printf("%c%s %3u %7lu %s%c\n", mode_type_char(ST_MODE_P(st)), perms,
             ST_NLINK_P(st), ST_SIZE_P(st), name, ind);
    else
         printf("%c%s %3u %7lu %s\n", mode_type_char(ST_MODE_P(st)), perms,
             ST_NLINK_P(st), ST_SIZE_P(st), name);
}

static void builtin_ls(char *path) {
    int fd;
    struct stat st;

    if ((fd = open(path, O_RDONLY)) < 0) {
        errprintf("ls: cannot open %s\n", path);
        return;
    }
    if (fstat(fd, &st) < 0) {
        errprintf("ls: cannot stat %s\n", path);
        close(fd);
        return;
    }

    if (!S_ISDIR(ST_MODE(st))) {
        // Single file
        ls_print_entry(path, &st);
        close(fd);
        return;
    }

    // Directory listing
    char buf[512], *p;
    if (strlen(path) + 1 + NAME_MAX + 1 > sizeof(buf)) {
        printf("ls: path too long\n");
        close(fd);
        return;
    }
    strcpy(buf, path);
    p = buf + strlen(buf);
    *p++ = '/';

    char dirent_buf[1024];
    int nread;

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
            ls_print_entry(buf, &st);
            pos += de->d_reclen;
        }
    }
    close(fd);
}

static void builtin_history(void) {
    for (int i = 0; i < history_count; i++) {
        int idx = (history_start + i) % HISTORY_SIZE;
        printf("%3d  %s\n", i + 1, history[idx]);
    }
}

static char **build_exec_envp(void) {
#ifdef USE_NCURSES_SHELL
    return environ;
#else
    static char env_storage[MAX_ENV_VARS][MAX_ENV_NAME + MAX_ENV_VALUE + 2];
    static char *envp[MAX_ENV_VARS + 1];
    int out = 0;

    for (int i = 0; i < MAX_ENV_VARS && out < MAX_ENV_VARS; i++) {
        if (!env_vars[i].used)
            continue;
        int nlen = strlen(env_vars[i].name);
        int vlen = strlen(env_vars[i].value);
        if (nlen >= MAX_ENV_NAME)
            nlen = MAX_ENV_NAME - 1;
        if (vlen >= MAX_ENV_VALUE)
            vlen = MAX_ENV_VALUE - 1;

        memcpy(env_storage[out], env_vars[i].name, nlen);
        env_storage[out][nlen] = '=';
        memcpy(env_storage[out] + nlen + 1, env_vars[i].value, vlen);
        env_storage[out][nlen + 1 + vlen] = 0;
        envp[out] = env_storage[out];
        out++;
    }
    envp[out] = 0;
    return envp;
#endif
}

static int shell_exec(char *path, char **argv) {
#ifdef USE_NCURSES_SHELL
    return exec(path, argv);
#else
    return exec_with_env(path, argv, build_exec_envp());
#endif
}

// =====================================================================
// PATH-based exec
// =====================================================================

static void exec_with_path(char *cmd, char **argv) {
    // If contains '/', use directly
    for (char *p = cmd; *p; p++) {
        if (*p == '/') {
            shell_exec(cmd, argv);
            return;
        }
    }

    // Try as-is (current dir)
    shell_exec(cmd, argv);

    // Search PATH
    char *path = env_get("PATH");
    if (!path)
        return;

    char pathcopy[MAX_ENV_VALUE];
    int plen = strlen(path);
    if (plen >= MAX_ENV_VALUE)
        plen = MAX_ENV_VALUE - 1;
    memcpy(pathcopy, path, plen);
    pathcopy[plen] = 0;

    char *p = pathcopy;
    while (*p) {
        char *start = p;
        while (*p && *p != ':')
            p++;
        int dlen = p - start;
        if (dlen > 0) {
            char fullpath[512];
            memcpy(fullpath, start, dlen);
            if (fullpath[dlen - 1] != '/')
                fullpath[dlen++] = '/';
            strcpy(fullpath + dlen, cmd);
            shell_exec(fullpath, argv);
        }
        if (*p == ':')
            p++;
    }
}

// =====================================================================
// Pipe helpers (vfork-safe wrappers)
// =====================================================================

static void run_pipe_left(struct cmd *cmd, int *p) __attribute__((noreturn));
static void run_pipe_left(struct cmd *cmd, int *p) {
    close(1);
    dup(p[1]);
    close(p[0]);
    close(p[1]);
    runcmd(cmd);
}

static void run_pipe_right(struct cmd *cmd, int *p) __attribute__((noreturn));
static void run_pipe_right(struct cmd *cmd, int *p) {
    close(0);
    dup(p[0]);
    close(p[0]);
    close(p[1]);
    runcmd(cmd);
}

// =====================================================================
// Command execution (recursive walk of the parse tree)
// =====================================================================

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
        // waitgdb: pause for debugger, then exec the real command
        // waitgdb -e <cmd>: also stop at entry point after exec
        if (strcmp(ecmd->argv[0], "waitgdb") == 0) {
            int stop_entry = 0;
            int cmd_idx = 1;
            if (ecmd->argv[1] && strcmp(ecmd->argv[1], "-e") == 0) {
                stop_entry = 1;
                cmd_idx = 2;
            }
            if (ecmd->argv[cmd_idx] == 0) {
                errprintf("usage: waitgdb [-e] <command> [args...]\n");
                exit(1);
            }
            if (stop_entry)
                waitgdb_stopentry();
            else
                waitgdb();
            exec_with_path(ecmd->argv[cmd_idx], &ecmd->argv[cmd_idx]);
            errprintf("waitgdb: exec %s failed\n", ecmd->argv[cmd_idx]);
            exit(127);
        }
        exec_with_path(ecmd->argv[0], ecmd->argv);
        errprintf("exec %s failed\n", ecmd->argv[0]);
        exit(127);

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        close(rcmd->fd);
        if (open(rcmd->file, rcmd->mode) < 0) {
            errprintf("open %s failed\n", rcmd->file);
            exit(1);
        }
        runcmd(rcmd->cmd);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
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
        pid = vfork();
        if (pid < 0)
            panic("vfork");
        if (pid == 0)
            run_pipe_left(pcmd->left, p);
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
        break;
    }
    exit(0);
}

// =====================================================================
// getcmd – prompt + readline
// =====================================================================

int getcmd(char *buf, int nbuf) {
    memset(buf, 0, nbuf);
    int n = do_readline(buf, nbuf);
    if (n < 0)
        return -1;
    return 0;
}

// =====================================================================
// main
// =====================================================================

int main(void) {
    static char buf[256];
    static char expanded_buf[512];
    int fd;

    // Ensure three file descriptors are open.
    while ((fd = open("/dev/console", O_RDWR)) >= 0) {
        if (fd >= 3) {
            close(fd);
            break;
        }
    }

    env_init();
    update_cwd();
    update_user_info();

#ifdef USE_NCURSES_SHELL
    setenv("TERMINFO", "/usr/share/terminfo", 1);
    if (getenv("TERM") == 0)
        setenv("TERM", "xterm", 1);
#endif

    for (;;) {
        if (getcmd(buf, sizeof(buf)) < 0)
            continue;
        // ---- Built-in: cd ----
        if (buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
            buf[strlen(buf) - 1] = 0; // chop \n
            char *path = buf + 3;
            char *expanded = expand_env_vars(path);
            if (chdir(expanded) < 0)
                errprintf("cannot cd %s\n", expanded);
            else
                update_cwd();
            continue;
        }

        // ---- Built-in: ls ----
        if (buf[0] == 'l' && buf[1] == 's' &&
            (buf[2] == '\n' || buf[2] == ' ')) {
            buf[strlen(buf) - 1] = 0;
            if (buf[2] == 0 || buf[3] == 0)
                builtin_ls(".");
            else
                builtin_ls(buf + 3);
            continue;
        }

        // ---- Built-in: history ----
        if (strncmp_local(buf, "history", 7) == 0 &&
            (buf[7] == '\n' || buf[7] == 0)) {
            builtin_history();
            continue;
        }

        // ---- Built-in: env ----
        if (strncmp_local(buf, "env", 3) == 0 &&
            (buf[3] == '\n' || buf[3] == 0)) {
            env_list();
            continue;
        }

        // ---- Built-in: export VAR=value ----
        if (strncmp_local(buf, "export ", 7) == 0) {
            buf[strlen(buf) - 1] = 0;
            char *arg = buf + 7;
            while (*arg == ' ')
                arg++;
            char *eq = strchr(arg, '=');
            if (eq) {
                *eq = 0;
                char *name = arg;
                char *value = eq + 1;
                int vlen = strlen(value);
                if (vlen >= 2 &&
                    ((value[0] == '"' && value[vlen - 1] == '"') ||
                     (value[0] == '\'' && value[vlen - 1] == '\''))) {
                    value[vlen - 1] = 0;
                    value++;
                }
                if (env_set(name, value) < 0)
                    errprintf("export: too many variables\n");
            } else {
                errprintf("export: usage: export VAR=value\n");
            }
            continue;
        }

        // ---- Built-in: unset ----
        if (strncmp_local(buf, "unset ", 6) == 0) {
            buf[strlen(buf) - 1] = 0;
            char *name = buf + 6;
            while (*name == ' ')
                name++;
            env_unset(name);
            continue;
        }

        // ---- Built-in: echo (with expansion) ----
        if (strncmp_local(buf, "echo ", 5) == 0) {
            buf[strlen(buf) - 1] = 0;
            char *expanded = expand_env_vars(buf + 5);
            printf("%s\n", expanded);
            continue;
        }

        // ---- Built-in: exit ----
        if (strncmp_local(buf, "exit", 4) == 0 &&
            (buf[4] == '\n' || buf[4] == 0))
            break;

        // ---- External command ----
        // Expand env vars
        char *expanded = expand_env_vars(buf);
        int elen = strlen(expanded);
        if (elen >= (int)sizeof(expanded_buf))
            elen = sizeof(expanded_buf) - 1;
        memcpy(expanded_buf, expanded, elen);
        expanded_buf[elen] = 0;

        struct cmd *cmd = parsecmd(expanded_buf);
        if (cmd == 0)
            continue;

        // Skip empty commands (e.g. bare Enter)
        if (cmd->type == EXEC && ((struct execcmd *)cmd)->argv[0] == 0)
            continue;

        int pid;

        // Snapshot shell terminal settings before child can mutate tty state.
        struct termios shell_termios;
        tcgetattr(0, &shell_termios);
    #ifdef USE_NCURSES_SHELL
        pid = vfork();
    #else
        pid = fork();
    #endif
        if (pid < 0)
            panic("fork");
        if (pid == 0) {
            setpgid(0, 0); // New process group (pgid = own pid)

            // Child side: ensure we own foreground tty and restore sane mode
            // before launching interactive programs like python.
            int child_pgid = getpgid(0);
            ioctl(0, TIOCSPGRP, &child_pgid);

            struct termios child_termios;
            if (tcgetattr(0, &child_termios) == 0) {
                child_termios.c_lflag |= (ICANON | ECHO | ISIG | ECHOE | ECHOK);
                child_termios.c_iflag |= (ICRNL | IXON);
                child_termios.c_cc[VMIN] = 1;
                child_termios.c_cc[VTIME] = 0;
                tcsetattr(0, TCSANOW, &child_termios);
            }

            runcmd(cmd);
        }

        // Also set from parent side to avoid race. This may fail if child
        // already changed groups, which is fine.
        (void)setpgid(pid, pid);

        // Always hand terminal foreground to child's process group.
        // Gating this on setpgid() success can lose input due to races.
        int child_pgid = pid;
        ioctl(0, TIOCSPGRP, &child_pgid);

        int status = 0;
        waitpid(pid, &status, WUNTRACED);

        // Restore shell as foreground
        {
            int shell_pgid = getpgid(0);
            ioctl(0, TIOCSPGRP, &shell_pgid);
        }

        // Restore shell's terminal settings (child may have changed them)
        tcsetattr(0, TCSANOW, &shell_termios);

        if (WIFSTOPPED(status)) {
            printf("[suspended] pid %d\n", pid);
        }
    }

    disable_raw_mode();
    exit(0);
}

void panic(char *s) {
    errprintf("%s\n", s);
    exit(1);
}

// =====================================================================
// Command constructors
// =====================================================================

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

// =====================================================================
// Tokeniser & parser (standard xv6 parser)
// =====================================================================

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
        errprintf("syntax error near: %s\n", s);
        return 0;
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
        case '+':
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
