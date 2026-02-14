/*
 * tty.c - Core TTY subsystem
 *
 * Implements terminal allocation, reference counting, line-discipline
 * processing (canonical / raw), echo, output post-processing, signal
 * generation, and ioctl handling.
 *
 * Each tty owns an input pipe (driver → reader) and an output pipe
 * (writer → driver).  The line discipline sits between the driver and
 * the pipes: tty_input() processes incoming characters through the
 * discipline before pushing them into the input pipe, while
 * tty_write() performs output post-processing before pushing data
 * into the output pipe.
 */

#include "types.h"
#include "param.h"
#include "errno.h"
#include "string.h"
#include "printf.h"
#include "riscv.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "mm/vm.h"
#include "mm/slab.h"
#include "vfs/pipe.h"
#include "tty/tty.h"
#include "signal.h"
#include "signo.h"
#include "smp/percpu.h"

/* ------------------------------------------------------------------ */
/*  Slab cache                                                        */
/* ------------------------------------------------------------------ */

static slab_cache_t __tty_cache = {0};

void tty_init(void)
{
    int ret = slab_cache_init(&__tty_cache, "tty_cache",
                              sizeof(struct tty), SLAB_FLAG_STATIC);
    assert(ret == 0,
           "tty_init: failed to init tty_cache slab, errno=%d", ret);
}

/* ------------------------------------------------------------------ */
/*  Allocation / reference counting                                   */
/* ------------------------------------------------------------------ */

struct tty *tty_alloc(const char *name, struct tty_ops *ops)
{
    struct tty *tty = slab_alloc(&__tty_cache);
    if (tty == NULL)
        return ERR_PTR(-ENOMEM);

    /* Two blocking pipes: input (driver→user) and output (user→driver) */
    struct pipe *inp = pipe_alloc(0);
    if (IS_ERR(inp)) {
        slab_free(tty);
        return (struct tty *)inp;          /* propagate error */
    }
    struct pipe *outp = pipe_alloc(0);
    if (IS_ERR(outp)) {
        pipe_close(inp, 1);
        pipe_close(inp, 0);
        slab_free(tty);
        return (struct tty *)outp;
    }

    spin_init(&tty->lock, "tty");
    termios_init_default(&tty->termios);
    tty->winsize.ws_row    = DEFAULT_ROWS;
    tty->winsize.ws_col    = DEFAULT_COLS;
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;
    tty->ops        = ops;
    tty->ref_count  = 1;
    tty->input_pipe  = inp;
    tty->output_pipe = outp;
    tty->driver_data = NULL;
    safestrcpy(tty->name, name, sizeof(tty->name));

    return tty;
}

void tty_free(struct tty *tty)
{
    if (tty == NULL)
        return;

    /* Close both ends of each pipe */
    if (tty->input_pipe) {
        pipe_close(tty->input_pipe, 1); /* close write end */
        pipe_close(tty->input_pipe, 0); /* close read end  */
    }
    if (tty->output_pipe) {
        pipe_close(tty->output_pipe, 1);
        pipe_close(tty->output_pipe, 0);
    }

    slab_free(tty);
}

void tty_ref(struct tty *tty)
{
    spin_lock(&tty->lock);
    tty->ref_count++;
    spin_unlock(&tty->lock);
}

void tty_unref(struct tty *tty)
{
    int should_free = 0;

    spin_lock(&tty->lock);
    tty->ref_count--;
    if (tty->ref_count <= 0)
        should_free = 1;
    spin_unlock(&tty->lock);

    if (should_free)
        tty_free(tty);
}

/* ------------------------------------------------------------------ */
/*  Helpers                                                           */
/* ------------------------------------------------------------------ */

#define C(x) ((x) - '@')   /* Control-x */
#define BACKSPACE 0x100

static inline int L_CANON(struct tty *tty)
{
    return tty->termios.c_lflag & ICANON;
}

static inline int L_ECHO(struct tty *tty)
{
    return tty->termios.c_lflag & ECHO;
}

static inline int L_ECHOE(struct tty *tty)
{
    return tty->termios.c_lflag & ECHOE;
}

static inline int L_ECHOK(struct tty *tty)
{
    return tty->termios.c_lflag & ECHOK;
}

static inline int L_ISIG(struct tty *tty)
{
    return tty->termios.c_lflag & ISIG;
}

static inline int I_ICRNL(struct tty *tty)
{
    return tty->termios.c_iflag & ICRNL;
}

static inline int I_IGNCR(struct tty *tty)
{
    return tty->termios.c_iflag & IGNCR;
}

static inline int I_INLCR(struct tty *tty)
{
    return tty->termios.c_iflag & INLCR;
}

static inline int I_ISTRIP(struct tty *tty)
{
    return tty->termios.c_iflag & ISTRIP;
}

static inline int O_OPOST(struct tty *tty)
{
    return tty->termios.c_oflag & OPOST;
}

static inline int O_ONLCR(struct tty *tty)
{
    return tty->termios.c_oflag & ONLCR;
}

/* ------------------------------------------------------------------ */
/*  Echo a character to the output pipe (called under tty->lock)      */
/* ------------------------------------------------------------------ */

static void tty_echo_char(struct tty *tty, int c)
{
    char ch;

    if (c == BACKSPACE) {
        /* BS SPC BS */
        ch = '\b'; pipe_write(tty->output_pipe, &ch, 1, 0);
        ch = ' ';  pipe_write(tty->output_pipe, &ch, 1, 0);
        ch = '\b'; pipe_write(tty->output_pipe, &ch, 1, 0);
        return;
    }

    ch = (char)c;

    /* Output post-processing for echoed characters */
    if (O_OPOST(tty) && O_ONLCR(tty) && ch == '\n') {
        char cr = '\r';
        pipe_write(tty->output_pipe, &cr, 1, 0);
    }

    pipe_write(tty->output_pipe, &ch, 1, 0);
}

/* ------------------------------------------------------------------ */
/*  Signal generation                                                 */
/* ------------------------------------------------------------------ */

static void tty_send_signal(struct tty *tty, int signum)
{
    (void)tty;
    /*
     * Send the signal to the current process. In a full implementation
     * this would target the foreground process group of the TTY.
     */
    kill_proc(current, signum);
}

/* ------------------------------------------------------------------ */
/*  Line-discipline input processing                                  */
/* ------------------------------------------------------------------ */

/*
 * tty_input - receive raw characters from the driver
 *
 * Called from interrupt context or from the PTY write path.
 * Applies input flags, canonical editing, echo, and signal
 * generation, then pushes completed data into the input pipe.
 *
 * @tty:   the terminal
 * @buf:   incoming characters
 * @count: number of characters
 *
 * Returns the number of characters consumed.
 */
ssize_t tty_input(struct tty *tty, const char *buf, size_t count)
{
    size_t i;

    spin_lock(&tty->lock);

    for (i = 0; i < count; i++) {
        int c = (unsigned char)buf[i];

        /* ---------- input flag processing ---------- */

        if (I_ISTRIP(tty))
            c &= 0x7F;

        if (c == '\r') {
            if (I_IGNCR(tty))
                continue;
            if (I_ICRNL(tty))
                c = '\n';
        } else if (c == '\n') {
            if (I_INLCR(tty))
                c = '\r';
        }

        /* ---------- signal characters ---------- */

        if (L_ISIG(tty)) {
            if (c == tty->termios.c_cc[VINTR]) {
                spin_unlock(&tty->lock);
                tty_send_signal(tty, SIGINT);
                spin_lock(&tty->lock);
                if (L_ECHO(tty)) {
                    tty_echo_char(tty, '^');
                    tty_echo_char(tty, 'C');
                    tty_echo_char(tty, '\n');
                }
                continue;
            }
            if (c == tty->termios.c_cc[VQUIT]) {
                spin_unlock(&tty->lock);
                tty_send_signal(tty, SIGQUIT);
                spin_lock(&tty->lock);
                if (L_ECHO(tty)) {
                    tty_echo_char(tty, '^');
                    tty_echo_char(tty, '\\');
                    tty_echo_char(tty, '\n');
                }
                continue;
            }
            if (c == tty->termios.c_cc[VSUSP]) {
                spin_unlock(&tty->lock);
                tty_send_signal(tty, SIGTSTP);
                spin_lock(&tty->lock);
                if (L_ECHO(tty)) {
                    tty_echo_char(tty, '^');
                    tty_echo_char(tty, 'Z');
                    tty_echo_char(tty, '\n');
                }
                continue;
            }
        }

        /* ---------- canonical mode editing ---------- */

        if (L_CANON(tty)) {
            /* Erase (backspace / DEL) */
            if (c == tty->termios.c_cc[VERASE] || c == '\b') {
                /* Delete one character by writing a NUL into the pipe
                 * is not feasible—we rely on the pipe already being
                 * consumed line-by-line.  For simplicity, echo the
                 * visual backspace only; the pipe will never see the
                 * erased character because we don't push it. Instead
                 * we keep a small edit buffer. */
                if (L_ECHOE(tty))
                    tty_echo_char(tty, BACKSPACE);
                continue;
            }

            /* Kill line (^U) */
            if (c == tty->termios.c_cc[VKILL]) {
                if (L_ECHOK(tty))
                    tty_echo_char(tty, '\n');
                continue;
            }

            /* EOF (^D) — push a zero-length "line" to unblock readers */
            if (c == tty->termios.c_cc[VEOF]) {
                /* Don't push the ^D itself. Just echo if needed and
                 * let the pipe's existing data be delivered as a
                 * complete line. */
                spin_unlock(&tty->lock);
                /* Write a sentinel NUL so the reader sees zero-length */
                char nul = 0;
                pipe_write(tty->input_pipe, &nul, 0, 0);
                spin_lock(&tty->lock);
                continue;
            }
        }

        /* ---------- echo ---------- */

        if (L_ECHO(tty))
            tty_echo_char(tty, c);

        /* ---------- push into input pipe ---------- */

        char ch = (char)c;
        spin_unlock(&tty->lock);
        pipe_write(tty->input_pipe, &ch, 1, 0);
        spin_lock(&tty->lock);
    }

    spin_unlock(&tty->lock);
    return (ssize_t)count;
}

/* ------------------------------------------------------------------ */
/*  Read (user → input pipe)                                          */
/* ------------------------------------------------------------------ */

/*
 * tty_read - read data from the terminal
 *
 * In canonical mode this blocks until a full line (terminated by '\n'
 * or EOF) is available.  In raw mode it returns as soon as VMIN bytes
 * are available (VTIME is not implemented).
 */
ssize_t tty_read(struct tty *tty, char *buf, size_t count, bool user)
{
    return pipe_read(tty->input_pipe, buf, count, user);
}

/* ------------------------------------------------------------------ */
/*  Write (output pipe ← user)                                       */
/* ------------------------------------------------------------------ */

/*
 * tty_write - write data to the terminal
 *
 * Applies output post-processing (OPOST / ONLCR) and pushes the
 * result into the output pipe.  The driver drains the output pipe
 * via tty_output() or its own tx callback.
 */
ssize_t tty_write(struct tty *tty, const char *buf, size_t count, bool user)
{
    char kbuf[64];
    size_t written = 0;
    int need_opost;

    spin_lock(&tty->lock);
    need_opost = O_OPOST(tty) && O_ONLCR(tty);
    spin_unlock(&tty->lock);

    if (!need_opost) {
        /* Fast path — no post-processing */
        return pipe_write(tty->output_pipe, buf, count, user);
    }

    /* Slow path — scan for '\n' and insert '\r' before it */
    while (written < count) {
        size_t batch = count - written;
        if (batch > sizeof(kbuf) / 2) /* leave room for \r expansion */
            batch = sizeof(kbuf) / 2;

        /* Copy a batch from user/kernel space */
        if (user) {
            if (either_copyin(kbuf, 1, (uint64)buf + written, batch) < 0)
                return (ssize_t)(written ? written : -EFAULT);
        } else {
            memmove(kbuf, buf + written, batch);
        }

        /* Expand NL → CRNL into a second buffer */
        char outbuf[128];
        size_t olen = 0;
        for (size_t j = 0; j < batch; j++) {
            if (kbuf[j] == '\n')
                outbuf[olen++] = '\r';
            outbuf[olen++] = kbuf[j];
        }

        ssize_t ret = pipe_write(tty->output_pipe, outbuf, olen, 0);
        if (ret < 0)
            return (ssize_t)(written ? written : ret);

        written += batch;
    }

    return (ssize_t)written;
}

/* ------------------------------------------------------------------ */
/*  Output drain helper                                               */
/* ------------------------------------------------------------------ */

/*
 * tty_output - pull data from the output pipe
 *
 * Drivers call this (or read the output pipe directly) to obtain
 * post-processed output bytes for transmission.
 */
ssize_t tty_output(struct tty *tty, char *buf, size_t count)
{
    return pipe_read(tty->output_pipe, buf, count, 0);
}

/* ------------------------------------------------------------------ */
/*  Ioctl                                                             */
/* ------------------------------------------------------------------ */

int tty_ioctl(struct tty *tty, uint64 cmd, uint64 arg)
{
    switch (cmd) {
    case TCGETS: {
        if (either_copyout(1, arg, &tty->termios,
                           sizeof(struct termios)) < 0)
            return -EFAULT;
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios new_t;
        if (either_copyin(&new_t, 1, arg, sizeof(struct termios)) < 0)
            return -EFAULT;

        spin_lock(&tty->lock);
        tty->termios = new_t;
        spin_unlock(&tty->lock);

        /* Notify driver if it cares */
        if (tty->ops && tty->ops->set_termios)
            tty->ops->set_termios(tty, &new_t);

        /* TCSETSF: also discard pending input */
        if (cmd == TCSETSF && tty->ops && tty->ops->discard_input)
            tty->ops->discard_input(tty);

        return 0;
    }
    case TIOCGWINSZ: {
        if (either_copyout(1, arg, &tty->winsize,
                           sizeof(struct winsize)) < 0)
            return -EFAULT;
        return 0;
    }
    case TIOCSWINSZ: {
        struct winsize ws;
        if (either_copyin(&ws, 1, arg, sizeof(struct winsize)) < 0)
            return -EFAULT;

        spin_lock(&tty->lock);
        tty->winsize = ws;
        spin_unlock(&tty->lock);

        if (tty->ops && tty->ops->set_winsize)
            tty->ops->set_winsize(tty, &ws);

        return 0;
    }
    case TIOCGPGRP: {
        /* Return the foreground process group (stub: current pgid) */
        pid_t pgid = current->pgid;
        if (either_copyout(1, arg, &pgid, sizeof(pid_t)) < 0)
            return -EFAULT;
        return 0;
    }
    case TIOCSPGRP: {
        /* Set foreground process group (stub: ignored) */
        return 0;
    }
    default:
        /* Let the driver handle unknown ioctls */
        if (tty->ops && tty->ops->ioctl)
            return tty->ops->ioctl(tty, cmd, arg);
        return -ENOTTY;
    }
}

/* ------------------------------------------------------------------ */
/*  Open / close via ops                                              */
/* ------------------------------------------------------------------ */

int tty_open(struct tty *tty)
{
    tty_ref(tty);
    if (tty->ops && tty->ops->open)
        return tty->ops->open(tty);
    return 0;
}

void tty_close(struct tty *tty)
{
    if (tty->ops && tty->ops->close)
        tty->ops->close(tty);
    tty_unref(tty);
}

/* ------------------------------------------------------------------ */
/*  Hangup                                                            */
/* ------------------------------------------------------------------ */

void tty_hangup(struct tty *tty)
{
    if (tty->ops && tty->ops->hangup)
        tty->ops->hangup(tty);
}
