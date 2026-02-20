/*
 * histedit_shim.c — libedit → readline shim for dash on xv6.
 *
 * Provides the subset of the BSD editline (libedit) API that dash
 * actually calls, implemented on top of GNU readline + a small
 * internal history ring.
 *
 * Readline already provides:
 *   - line editing (emacs / vi modes)
 *   - filename tab-completion
 *   - terminal handling
 *
 * We maintain our own history list so the H_FIRST / H_NEXT / H_PREV /
 * H_NEXT_EVENT / H_PREV_STR operations used by dash's `fc` builtin
 * work correctly.
 */

#include "histedit.h"

#include <readline/readline.h>
#include <readline/history.h>

#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ================================================================== */
/* History                                                             */
/* ================================================================== */

/*
 * We keep history entries in a simple dynamic array so that the
 * sequential-number model expected by dash (he.num) is trivial.
 * Entry numbers start at 1 and increase monotonically.
 */

#define HIST_DEFAULT_SIZE 100

struct history {
    char **entries;   /* array of strdup'd strings */
    int    count;     /* number of entries stored   */
    int    capacity;  /* allocated slots            */
    int    maxsize;   /* user-requested limit       */
    int    cursor;    /* current position for iteration (0-based index) */
};

History *
history_init(void)
{
    History *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->maxsize  = HIST_DEFAULT_SIZE;
    h->capacity = HIST_DEFAULT_SIZE;
    h->entries  = calloc(h->capacity, sizeof(char *));
    if (!h->entries) { free(h); return NULL; }
    return h;
}

void
history_end(History *h)
{
    if (!h) return;
    for (int i = 0; i < h->count; i++)
        free(h->entries[i]);
    free(h->entries);
    free(h);
}

/* Ensure room for one more entry. */
static void
hist_grow(History *h)
{
    if (h->count < h->capacity)
        return;
    int newcap = h->capacity ? h->capacity * 2 : 64;
    char **p = realloc(h->entries, newcap * sizeof(char *));
    if (!p) return;
    h->entries  = p;
    h->capacity = newcap;
}

/* Trim oldest entries if we exceed maxsize. */
static void
hist_trim(History *h)
{
    while (h->count > h->maxsize && h->maxsize > 0) {
        free(h->entries[0]);
        memmove(h->entries, h->entries + 1,
                (h->count - 1) * sizeof(char *));
        h->count--;
    }
}

/* Event number for index i (1-based, monotonically increasing). */
static int
hist_num(History *h, int idx)
{
    (void)h;
    return idx + 1;
}

static int
hist_set(History *h, HistEvent *he, int idx)
{
    if (idx < 0 || idx >= h->count)
        return -1;
    he->num = hist_num(h, idx);
    he->str = h->entries[idx];
    h->cursor = idx;
    return 0;
}

int
history(History *h, HistEvent *he, int op, ...)
{
    va_list ap;
    va_start(ap, op);

    he->num = 0;
    he->str = "";

    int ret = 0;

    switch (op) {

    case H_SETSIZE: {
        int sz = va_arg(ap, int);
        if (sz > 0) h->maxsize = sz;
        hist_trim(h);
        break;
    }

    case H_ENTER: {
        const char *s = va_arg(ap, const char *);
        hist_grow(h);
        if (h->count < h->capacity) {
            h->entries[h->count] = strdup(s ? s : "");
            h->count++;
        }
        hist_trim(h);
        /* Also keep readline history in sync for completion etc. */
        add_history(s ? s : "");
        ret = hist_set(h, he, h->count - 1);
        break;
    }

    case H_APPEND: {
        const char *s = va_arg(ap, const char *);
        if (h->count > 0) {
            int last = h->count - 1;
            size_t olen = strlen(h->entries[last]);
            size_t alen = s ? strlen(s) : 0;
            char *newstr = realloc(h->entries[last], olen + alen + 1);
            if (newstr) {
                memcpy(newstr + olen, s ? s : "", alen);
                newstr[olen + alen] = '\0';
                h->entries[last] = newstr;
            }
            ret = hist_set(h, he, last);
        } else {
            /* nothing to append to — treat as ENTER */
            const char *s2 = s;
            hist_grow(h);
            if (h->count < h->capacity) {
                h->entries[h->count] = strdup(s2 ? s2 : "");
                h->count++;
            }
            ret = hist_set(h, he, h->count - 1);
        }
        break;
    }

    case H_FIRST:
        /* "first" in libedit means the most recent entry */
        ret = hist_set(h, he, h->count - 1);
        break;

    case H_LAST:
        /* "last" means the oldest entry */
        ret = hist_set(h, he, 0);
        break;

    case H_NEXT:
        /* towards older entries (decreasing index) */
        ret = hist_set(h, he, h->cursor - 1);
        break;

    case H_PREV:
        /* towards newer entries (increasing index) */
        ret = hist_set(h, he, h->cursor + 1);
        break;

    case H_NEXT_EVENT: {
        int target = va_arg(ap, int);
        /* find entry with event number == target */
        int idx = target - 1; /* event nums are 1-based */
        ret = hist_set(h, he, idx);
        break;
    }

    case H_PREV_STR: {
        const char *pat = va_arg(ap, const char *);
        size_t plen = pat ? strlen(pat) : 0;
        ret = -1;
        /* search backwards from current position */
        for (int i = h->cursor; i >= 0; i--) {
            if (plen == 0 || strncmp(h->entries[i], pat, plen) == 0) {
                ret = hist_set(h, he, i);
                break;
            }
        }
        break;
    }

    default:
        ret = -1;
        break;
    }

    va_end(ap);
    return ret;
}


/* ================================================================== */
/* SIGINT handling for readline                                        */
/* ================================================================== */

/*
 * readline's default SIGINT handler re-raises the signal with SIG_DFL,
 * which kills the process.  For an interactive shell this is wrong —
 * Ctrl-C should just abort the current input line and redisplay the
 * prompt.
 *
 * Strategy: disable readline's signal handling (rl_catch_signals = 0)
 * and install our own SIGINT handler that sets a flag.  We also replace
 * readline's character reader so it returns EOF on SIGINT, causing
 * readline to return NULL.  We then detect this and return an empty
 * line ("\n") to dash, which re-prompts without exiting.
 */

static volatile sig_atomic_t el_sigint_received;

static void
el_sigint_handler(int sig)
{
    (void)sig;
    el_sigint_received = 1;
}

/*
 * Custom readline character reader: on EINTR, check our SIGINT flag
 * and return EOF to break out of readline's input loop.
 */
static int
el_rl_getc(FILE *stream)
{
    unsigned char c;
    int fd = fileno(stream);
    for (;;) {
        ssize_t r = read(fd, &c, 1);
        if (r == 1)
            return (int)c;
        if (r == 0)
            continue;   /* xv6: spurious 0-byte reads — retry (matches CPython) */
        /* r < 0 */
        if (el_sigint_received)
            return EOF;
        if (errno == EINTR)
            continue;
        return EOF;
    }
}


/* ================================================================== */
/* EditLine                                                            */
/* ================================================================== */

struct editline {
    char *progname;
    FILE *fin;
    FILE *fout;
    FILE *ferr;
    /* prompt callback as installed by EL_PROMPT_ESC */
    const char *(*prompt_fn)(EditLine *);
    /* static buffer returned by el_gets */
    char *linebuf;
    size_t linesize;
};

/* readline's prompt is stored here; set before calling readline(). */
static const char *rl_prompt_str = "$ ";
static EditLine  *rl_el_ctx     = NULL;

/* Callback wrapper that dash passes via EL_PROMPT_ESC. */
static const char *
rl_get_prompt(void)
{
    if (rl_el_ctx && rl_el_ctx->prompt_fn) {
        const char *p = rl_el_ctx->prompt_fn(rl_el_ctx);
        return p ? p : "";
    }
    return rl_prompt_str;
}

EditLine *
el_init(const char *prog, FILE *fin, FILE *fout, FILE *ferr)
{
    (void)ferr;
    EditLine *el = calloc(1, sizeof(*el));
    if (!el) return NULL;
    el->progname = strdup(prog ? prog : "dash");
    el->fin  = fin;
    el->fout = fout;
    el->ferr = ferr;
    rl_el_ctx = el;

    /* Initialise readline with defaults. */
    rl_readline_name = el->progname;
    /* Auto-complete file names by default. */
    rl_attempted_completion_function = NULL;
    /* Don't let readline install signal handlers — we handle SIGINT
     * ourselves via el_rl_getc + el_sigint_handler. */
    rl_catch_signals = 0;
    rl_catch_sigwinch = 0;
    rl_getc_function = el_rl_getc;

    /* Tell readline which streams to use for I/O. */
    rl_instream  = fin;
    rl_outstream = fout;

    /* xv6 UART console: stdio buffering prevents readline's character-
     * by-character echo from reaching the screen.  Make stdout and
     * stderr fully unbuffered so prompts and error messages appear
     * immediately.  (Same fix applied in CPython's readline module —
     * see user/v6-cpython/Modules/readline.c, setvbuf call.)          */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    return el;
}

void
el_end(EditLine *el)
{
    if (!el) return;
    if (rl_el_ctx == el) rl_el_ctx = NULL;
    free(el->progname);
    free(el->linebuf);
    free(el);
}

int
el_set(EditLine *el, int op, ...)
{
    va_list ap;
    va_start(ap, op);

    switch (op) {

    case EL_HIST:
        /* el_set(el, EL_HIST, history, hist) — we ignore this;
         * our el_gets feeds history via the History API. */
        (void)va_arg(ap, void *);   /* history func ptr */
        (void)va_arg(ap, void *);   /* History *        */
        break;

    case EL_PROMPT_ESC: {
        const char *(*fn)(EditLine *) =
            va_arg(ap, const char *(*)(EditLine *));
        (void)va_arg(ap, int);  /* escape char */
        el->prompt_fn = fn;
        break;
    }

    case EL_EDITOR: {
        const char *mode = va_arg(ap, const char *);
        if (mode && strcmp(mode, "vi") == 0)
            rl_variable_bind("editing-mode", "vi");
        else
            rl_variable_bind("editing-mode", "emacs");
        break;
    }

    case EL_TERMINAL:
        /* Readline handles the terminal itself. */
        (void)va_arg(ap, const char *);
        break;

    default:
        va_end(ap);
        return -1;
    }

    va_end(ap);
    return 0;
}

int
el_source(EditLine *el, const char *file)
{
    (void)el;
    /* Read inputrc if file is NULL (readline's default behaviour). */
    if (file == NULL)
        rl_read_init_file(NULL);
    else
        rl_read_init_file(file);
    return 0;
}

const char *
el_gets(EditLine *el, int *count)
{
    const char *prompt = "";
    if (el->prompt_fn)
        prompt = el->prompt_fn(el);
    if (!prompt)
        prompt = "";

    /* Install our SIGINT handler; save the previous one. */
    struct sigaction sa, old_sa;
    sa.sa_handler = el_sigint_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    el_sigint_received = 0;
    sigaction(SIGINT, &sa, &old_sa);

    char *line = readline(prompt);

    /* Restore the previous SIGINT handler. */
    sigaction(SIGINT, &old_sa, NULL);

    if (el_sigint_received) {
        /* Ctrl-C pressed — discard any partial input, return an empty
         * line so dash re-prompts without interpreting EOF. */
        free(line);
        write(STDOUT_FILENO, "\n", 1);

        size_t need = 2; /* '\n' + '\0' */
        if (need > el->linesize) {
            char *nb = realloc(el->linebuf, need);
            if (!nb) {
                if (count) *count = 0;
                return NULL;
            }
            el->linebuf  = nb;
            el->linesize = need;
        }
        el->linebuf[0] = '\n';
        el->linebuf[1] = '\0';
        if (count) *count = 1;
        return el->linebuf;
    }

    if (line == NULL) {
        /* EOF */
        if (count) *count = 0;
        return NULL;
    }

    /*
     * readline() strips the newline; dash expects a trailing '\n'.
     * Build "line\n\0" in our buffer.
     */
    size_t len = strlen(line);
    size_t need = len + 2; /* line + '\n' + '\0' */
    if (need > el->linesize) {
        char *nb = realloc(el->linebuf, need);
        if (!nb) {
            free(line);
            if (count) *count = 0;
            return NULL;
        }
        el->linebuf  = nb;
        el->linesize = need;
    }

    memcpy(el->linebuf, line, len);
    el->linebuf[len]     = '\n';
    el->linebuf[len + 1] = '\0';

    free(line);

    if (count) *count = (int)(len + 1);
    return el->linebuf;
}
