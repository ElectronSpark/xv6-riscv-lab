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
#include "proc/tq.h"
#include "mm/vm.h"
#include "mm/slab.h"
#include "vfs/pipe.h"
#include "vfs/poll.h"
#include "tty/tty.h"
#include "tty/session.h"
#include "proc/pgroup.h"
#include "signal.h"
#include "signo.h"
#include "smp/percpu.h"
#include "kstats.h"

/* ------------------------------------------------------------------ */
/*  Slab cache                                                        */
/* ------------------------------------------------------------------ */

static slab_cache_t __tty_cache = {0};

void tty_init(void) {
    int ret = slab_cache_init(&__tty_cache, "tty_cache", sizeof(struct tty),
                              SLAB_FLAG_STATIC);
    assert(ret == 0, "tty_init: failed to init tty_cache slab, errno=%d", ret);
}

/* ------------------------------------------------------------------ */
/*  Allocation / reference counting                                   */
/* ------------------------------------------------------------------ */

struct tty *tty_alloc(const char *name, struct tty_ops *ops) {
    struct tty *tty = slab_alloc(&__tty_cache);
    if (tty == NULL)
        return ERR_PTR(-ENOMEM);

    /* Two blocking pipes: input (driver→user) and output (user→driver) */
    struct pipe *inp = pipe_alloc(0);
    if (IS_ERR(inp)) {
        slab_free(tty);
        return (struct tty *)inp; /* propagate error */
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
    tty->winsize.ws_row = DEFAULT_ROWS;
    tty->winsize.ws_col = DEFAULT_COLS;
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;
    tty->ops = ops;
    tty->ref_count = 1;
    tty->input_pipe = inp;
    tty->output_pipe = outp;
    tty->raw_r = 0;
    tty->raw_w = 0;
    tq_init(&tty->raw_wait, "tty_raw", NULL);
    tty->canon_len = 0;
    tty->driver_data = NULL;
    tty->session = NULL;
    safestrcpy(tty->name, name, sizeof(tty->name));

    return tty;
}

/*
 * Helper: close remaining open pipe ends safely.
 *
 * pty_slave_hangup() may have already closed one end of each pipe.
 * We check which ends are still open before closing, so the last
 * close frees the pipe struct and we never touch it afterwards.
 */
static void __tty_close_pipe(struct pipe *pi) {
    if (pi == NULL)
        return;
    int w = PIPE_WRITABLE(pi);
    int r = PIPE_READABLE(pi);
    if (w && r) {
        /* Both open: first close doesn't free, second does */
        pipe_close(pi, 1);
        pipe_close(pi, 0);
    } else if (w) {
        /* Only write end open: closing it frees the pipe */
        pipe_close(pi, 1);
    } else if (r) {
        /* Only read end open: closing it frees the pipe */
        pipe_close(pi, 0);
    }
    /* Neither open: pipe was already fully closed & freed */
}

void tty_free(struct tty *tty) {
    if (tty == NULL)
        return;

    __tty_close_pipe(tty->input_pipe);
    tty->input_pipe = NULL;
    __tty_close_pipe(tty->output_pipe);
    tty->output_pipe = NULL;

    slab_free(tty);
}

void tty_ref(struct tty *tty) {
    spin_lock(&tty->lock);
    tty->ref_count++;
    spin_unlock(&tty->lock);
}

void tty_unref(struct tty *tty) {
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

#define C(x) ((x) - '@') /* Control-x */
#define BACKSPACE 0x100

static inline int L_CANON(struct tty *tty) {
    return tty->termios.c_lflag & ICANON;
}

static inline int L_ECHO(struct tty *tty) {
    return tty->termios.c_lflag & ECHO;
}

static inline int L_ECHOE(struct tty *tty) {
    return tty->termios.c_lflag & ECHOE;
}

static inline int L_ECHOK(struct tty *tty) {
    return tty->termios.c_lflag & ECHOK;
}

static inline int L_ISIG(struct tty *tty) {
    return tty->termios.c_lflag & ISIG;
}

static inline int I_ICRNL(struct tty *tty) {
    return tty->termios.c_iflag & ICRNL;
}

static inline int I_IGNCR(struct tty *tty) {
    return tty->termios.c_iflag & IGNCR;
}

static inline int I_INLCR(struct tty *tty) {
    return tty->termios.c_iflag & INLCR;
}

static inline int I_ISTRIP(struct tty *tty) {
    return tty->termios.c_iflag & ISTRIP;
}

static inline int O_OPOST(struct tty *tty) {
    return tty->termios.c_oflag & OPOST;
}

static inline int O_ONLCR(struct tty *tty) {
    return tty->termios.c_oflag & ONLCR;
}

/* ------------------------------------------------------------------ */
/*  Echo a character to the output pipe                               */
/*                                                                    */
/*  IMPORTANT: tty->lock must be held on entry and will be held on    */
/*  return, but this function temporarily drops it while calling      */
/*  pipe_write() which may sleep when the pipe is full.               */
/* ------------------------------------------------------------------ */

static void tty_echo_char(struct tty *tty, int c) {
    char echobuf[4];
    int n = 0;

    if (c == BACKSPACE) {
        /* BS SPC BS */
        echobuf[n++] = '\b';
        echobuf[n++] = ' ';
        echobuf[n++] = '\b';
    } else {
        /* Output post-processing for echoed characters */
        if (O_OPOST(tty) && O_ONLCR(tty) && c == '\n')
            echobuf[n++] = '\r';
        echobuf[n++] = (char)c;
    }

    /* Drop the spinlock before calling pipe_write — it can sleep. */
    spin_unlock(&tty->lock);
    pipe_write(tty->output_pipe, echobuf, n, 0);
    spin_lock(&tty->lock);
}

/* ------------------------------------------------------------------ */
/*  Signal generation (foreground process group)                      */
/* ------------------------------------------------------------------ */

/*
 * tty_signal_fg_pgroup - send a signal to the foreground process group
 *
 * If the TTY has an owning session with a foreground process group,
 * the signal is sent to every process in that group.  Otherwise,
 * fall back to signalling the current process.
 */
static void tty_signal_fg_pgroup(struct tty *tty, int signum) {
    struct session *sess = tty->session;
    if (sess != NULL) {
        pid_t fg = session_get_fg_pgid(sess);
        if (fg > 0) {
            pgroup_kill(fg, signum);
            return;
        }
    }
    /* Fallback: no session or no fg group — signal current process */
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
ssize_t tty_input(struct tty *tty, const char *buf, size_t count) {
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
                tty_signal_fg_pgroup(tty, SIGINT);
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
                tty_signal_fg_pgroup(tty, SIGQUIT);
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
                tty_signal_fg_pgroup(tty, SIGTSTP);
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
            /* Erase (backspace / DEL) — remove last char from line buffer */
            if (c == tty->termios.c_cc[VERASE] || c == '\b') {
                if (tty->canon_len > 0) {
                    tty->canon_len--;
                    if (L_ECHOE(tty))
                        tty_echo_char(tty, BACKSPACE);
                }
                continue;
            }

            /* Kill line (^U) — clear entire line buffer */
            if (c == tty->termios.c_cc[VKILL]) {
                /* Echo backspaces for each character removed */
                if (L_ECHOE(tty)) {
                    while (tty->canon_len > 0) {
                        tty->canon_len--;
                        tty_echo_char(tty, BACKSPACE);
                    }
                } else {
                    tty->canon_len = 0;
                    if (L_ECHOK(tty))
                        tty_echo_char(tty, '\n');
                }
                continue;
            }

            /* EOF (^D) — flush current buffer (if any), deliver
             * zero-length read if buffer was empty */
            if (c == tty->termios.c_cc[VEOF]) {
                spin_unlock(&tty->lock);
                if (tty->canon_len > 0) {
                    pipe_write(tty->input_pipe, tty->canon_buf, tty->canon_len,
                               0);
                } else {
                    /* Zero-length write signals EOF to reader */
                    pipe_write(tty->input_pipe, "", 0, 0);
                }
                spin_lock(&tty->lock);
                tty->canon_len = 0;
                /* Wake tty_read which sleeps on raw_wait in all modes */
                tq_wakeup_all(&tty->raw_wait, 0, 0);
                continue;
            }
        }

        /* ---------- echo ---------- */

        if (L_ECHO(tty))
            tty_echo_char(tty, c);

        /* ---------- push character to consumer ---------- */

        if (!L_CANON(tty)) {
            /*
             * Raw mode: store in the direct ring buffer and wake
             * any reader immediately.  This avoids the pipe's
             * multi-lock wakeup path, giving single-character
             * latency for applications like readline.
             */
            uint next = (tty->raw_w + 1) & (TTY_RAW_BUF_SIZE - 1);
            if (next != (tty->raw_r & (TTY_RAW_BUF_SIZE - 1))) {
                tty->raw_buf[tty->raw_w & (TTY_RAW_BUF_SIZE - 1)] = (char)c;
                tty->raw_w++;
            }
            /* Wake readers — tq_wakeup_all is safe under spinlock */
            tq_wakeup_all(&tty->raw_wait, 0, 0);
        } else {
            /*
             * Canonical mode: accumulate characters in the line
             * buffer.  When a line terminator (newline or VEOL)
             * arrives, flush the complete line into the input pipe
             * so that read() returns the whole line at once.
             */
            if (tty->canon_len < TTY_CANON_BUF_SIZE)
                tty->canon_buf[tty->canon_len++] = (char)c;

            /* Flush on newline or EOL */
            if (c == '\n' || c == (unsigned char)tty->termios.c_cc[VEOL]) {
                spin_unlock(&tty->lock);
                pipe_write(tty->input_pipe, tty->canon_buf, tty->canon_len, 0);
                spin_lock(&tty->lock);
                tty->canon_len = 0;
                /* Wake tty_read which sleeps on raw_wait in all modes */
                tq_wakeup_all(&tty->raw_wait, 0, 0);
            }
        }
    }

    spin_unlock(&tty->lock);
    return (ssize_t)count;
}

/* ------------------------------------------------------------------ */
/*  Read (user → input pipe)                                          */
/* ------------------------------------------------------------------ */

static int tty_current_read_error(struct tty *tty) {
    struct session *sess = tty->session;
    pid_t fg;

    if (sess == NULL || current == NULL || current->session != sess)
        return (sess != NULL && current != NULL) ? -EIO : 0;

    fg = session_get_fg_pgid(sess);
    if (fg <= 0 || current->pgid == fg)
        return 0;

    /*
     * Linux sends SIGTTIN when a background process group attempts to read
     * from its controlling terminal.  This prevents daemons and desktop
     * helpers from draining the interactive console behind the shell.
     */
    pgroup_kill(current->pgid, SIGTTIN);
    return -EINTR;
}

/*
 * tty_read - read data from the terminal
 *
 * Both canonical and raw modes sleep on tty->raw_wait so that a
 * concurrent TCSETS (mode switch) can wake the reader and let it
 * re-check which buffer to drain.  Without this, a reader blocked
 * in the wrong path (pipe vs raw ring) after a mode switch would
 * never receive new input — the classic "keyboard goes dead" bug.
 *
 * Canonical mode: completed lines are read from the input pipe.
 *                 tty_input() wakes raw_wait after each pipe_write.
 * Raw mode:       characters are read from the direct ring buffer.
 *                 tty_input() wakes raw_wait after each store.
 */
ssize_t tty_read(struct tty *tty, char *buf, size_t count, bool user) {
    ssize_t total = 0;
    int err;

    err = tty_current_read_error(tty);
    if (err < 0)
        return err;

    spin_lock(&tty->lock);
    while ((size_t)total < count) {
        if (!L_CANON(tty)) {
            /* ---- Raw mode: drain direct ring buffer ---- */
            while ((size_t)total < count && tty->raw_r != tty->raw_w) {
                char ch = tty->raw_buf[tty->raw_r & (TTY_RAW_BUF_SIZE - 1)];
                tty->raw_r++;
                spin_unlock(&tty->lock);

                if (user) {
                    if (either_copyout(1, (uint64)buf + total, &ch, 1) < 0)
                        return total > 0 ? total : -EFAULT;
                } else {
                    buf[total] = ch;
                }
                total++;
                spin_lock(&tty->lock);
            }
            if (total > 0)
                break; /* short-read semantics */
        } else {
            /* ---- Canonical mode: read from input pipe ---- */
            struct pipe *pi = tty->input_pipe;
            uint nw = smp_load_acquire(&pi->nwrite);
            uint nr = smp_load_acquire(&pi->nread);
            if (nw != nr) {
                /*
                 * Data available.  Drop the tty lock and perform a
                 * pipe_read.  We are the sole reader of this pipe,
                 * so the data we observed will still be present.
                 */
                spin_unlock(&tty->lock);
                ssize_t n = pipe_read(pi, buf + total, count - total, user);
                if (n > 0)
                    return total + n;
                return total > 0 ? total : n;
            }
        }

        /*
         * No data in either buffer — sleep on raw_wait.
         * tty_input() and TCSETS both wake this queue, so we
         * will re-check the (possibly changed) mode on wakeup.
         */
        tq_wait_in_state(&tty->raw_wait, &tty->lock, NULL,
                         THREAD_INTERRUPTIBLE);
        if (signal_pending(current)) {
            spin_unlock(&tty->lock);
            return total > 0 ? total : -EINTR;
        }
    }
    spin_unlock(&tty->lock);

    return total;
}

/* ------------------------------------------------------------------ */
/*  Poll (check readiness without blocking)                           */
/* ------------------------------------------------------------------ */

/*
 * tty_poll - check whether the terminal has data available
 *
 * In canonical mode, checks the input pipe for buffered line data.
 * In raw mode, checks the direct ring buffer.
 * Output (POLLOUT) is always reported as ready since UART writes
 * never block the caller.
 *
 * Returns the subset of @events that are currently ready.
 */
int tty_poll(struct tty *tty, short events) {
    short revents = 0;

    spin_lock(&tty->lock);

    if (events & (POLLIN | POLLRDNORM | POLLRDBAND)) {
        if (L_CANON(tty)) {
            /* Canonical mode: data ready if input pipe has bytes */
            struct pipe *pi = tty->input_pipe;
            uint nwrite = smp_load_acquire(&pi->nwrite);
            uint nread = smp_load_acquire(&pi->nread);
            if ((nwrite - nread) > 0)
                revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        } else {
            /* Raw mode: data ready if ring buffer is non-empty */
            if (tty->raw_r != tty->raw_w)
                revents |= (events & (POLLIN | POLLRDNORM | POLLRDBAND));
        }
    }

    if (events & (POLLOUT | POLLWRNORM | POLLWRBAND)) {
        /* Output to UART is always accepted */
        revents |= (events & (POLLOUT | POLLWRNORM | POLLWRBAND));
    }

    spin_unlock(&tty->lock);
    return revents;
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
ssize_t tty_write(struct tty *tty, const char *buf, size_t count, bool user) {
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
        if (batch > sizeof(kbuf) >> 1) /* leave room for \r expansion */
            batch = sizeof(kbuf) >> 1;

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
ssize_t tty_output(struct tty *tty, char *buf, size_t count) {
    return pipe_read(tty->output_pipe, buf, count, 0);
}

/* ------------------------------------------------------------------ */
/*  Ioctl                                                             */
/* ------------------------------------------------------------------ */

int tty_ioctl(struct tty *tty, uint64 cmd, void *arg) {
    switch (cmd) {
    case TCGETS: {
        struct termios *tp = (struct termios *)arg;
        spin_lock(&tty->lock);
        *tp = tty->termios;
        spin_unlock(&tty->lock);
        return 0;
    }
    case TCSETS:
    case TCSETSW:
    case TCSETSF: {
        struct termios *tp = (struct termios *)arg;

        spin_lock(&tty->lock);
        /* Flush any partial canonical line buffer before changing modes.
         * If switching from canonical → raw, leftover chars would be lost. */
        if (tty->canon_len > 0) {
            char tmpbuf[TTY_CANON_BUF_SIZE];
            uint tmplen = tty->canon_len;
            __builtin_memcpy(tmpbuf, tty->canon_buf, tmplen);
            tty->canon_len = 0;
            spin_unlock(&tty->lock);
            pipe_write(tty->input_pipe, tmpbuf, tmplen, 0);
            spin_lock(&tty->lock);
        }
        tty->termios = *tp;
        /*
         * Wake any reader sleeping on raw_wait so it re-checks the
         * (possibly changed) ICANON mode.  Without this, a reader
         * blocked on the wrong wait channel after a mode switch
         * would never receive new input.
         */
        tq_wakeup_all(&tty->raw_wait, 0, 0);
        spin_unlock(&tty->lock);

        /* Notify driver if it cares */
        if (tty->ops && tty->ops->set_termios)
            tty->ops->set_termios(tty, tp);

        /* TCSETSF: also discard pending input */
        if (cmd == TCSETSF) {
            spin_lock(&tty->lock);
            tty->raw_r = tty->raw_w; /* flush raw ring buffer */
            tty->canon_len = 0;      /* flush canonical line buffer */
            spin_unlock(&tty->lock);
            if (tty->ops && tty->ops->discard_input)
                tty->ops->discard_input(tty);
        }

        return 0;
    }
    case TIOCGWINSZ: {
        struct winsize *wsp = (struct winsize *)arg;
        spin_lock(&tty->lock);
        *wsp = tty->winsize;
        spin_unlock(&tty->lock);
        return 0;
    }
    case TIOCSWINSZ: {
        struct winsize *wsp = (struct winsize *)arg;

        spin_lock(&tty->lock);
        tty->winsize = *wsp;
        spin_unlock(&tty->lock);

        if (tty->ops && tty->ops->set_winsize)
            tty->ops->set_winsize(tty, wsp);

        return 0;
    }
    case TIOCGPGRP: {
        /* Return the foreground process group from the session */
        pid_t *pgidp = (pid_t *)arg;
        *pgidp = 0;
        if (tty->session)
            *pgidp = session_get_fg_pgid(tty->session);
        return 0;
    }
    case TIOCSPGRP: {
        /* Set the foreground process group */
        pid_t pgid = *(pid_t *)arg;
        if (pgid <= 0)
            return -EINVAL;
        /* Caller must be in the same session as the tty */
        if (tty->session == NULL || tty->session->sid != current->sid)
            return -ENOTTY;
        session_set_fg_pgid(tty->session, pgid);
        return 0;
    }
    case TIOCSCTTY: {
        kstats_konsole_prepty_mark_pty();
        /* Set controlling terminal: the calling process must be a session
         * leader and must not already have a controlling terminal. */
        if (current->session == NULL)
            return -EPERM;
        session_set_ctrl_tty(current->session, tty);
        return 0;
    }
    case TIOCNOTTY: {
        /* Detach calling process from its controlling terminal.
         * If the caller is a session leader, disassociate the entire
         * session from the controlling tty (POSIX). */
        if (current->session != NULL)
            session_set_ctrl_tty(current->session, NULL);
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

int tty_open(struct tty *tty) {
    tty_ref(tty);
    if (tty->ops && tty->ops->open)
        return tty->ops->open(tty);
    return 0;
}

void tty_close(struct tty *tty) {
    if (tty->ops && tty->ops->close)
        tty->ops->close(tty);
    tty_unref(tty);
}

/* ------------------------------------------------------------------ */
/*  Hangup                                                            */
/* ------------------------------------------------------------------ */

void tty_hangup(struct tty *tty) {
    if (tty->ops && tty->ops->hangup)
        tty->ops->hangup(tty);
}
