#ifndef __KERNEL_TTY_TYPES_H
#define __KERNEL_TTY_TYPES_H

#include "termios.h"
#include "lock/spinlock.h"
#include "proc/tq_type.h"

struct pipe;
struct tty;
struct session;

struct tty_ops {
    int (*open)(struct tty *tty);
    void (*close)(struct tty *tty);
    void (*hangup)(struct tty *tty);
    void (*throttle)(struct tty *tty);
    void (*unthrottle)(struct tty *tty);
    void (*stop)(struct tty *tty);
    void (*start)(struct tty *tty);
    void (*discard_input)(struct tty *tty);
    ssize_t (*read)(struct tty *tty, char *buf, size_t nr);
    ssize_t (*write)(struct tty *tty, const char *buf, size_t nr);
    ssize_t (*rx)(struct tty *tty, const char *buf, size_t nr);
    ssize_t (*tx)(struct tty *tty, char *buf, size_t nr);
    void (*set_termios)(struct tty *tty, struct termios *new_termios);
    void (*set_winsize)(struct tty *tty, struct winsize *new_winsize);
    int (*ioctl)(struct tty *tty, uint64 cmd, void *arg);
};

/*
 * Raw-mode ring buffer size.  Must be a power of two for fast
 * modulo via bitmask.
 */
#define TTY_RAW_BUF_SIZE 256

/*
 * Canonical-mode line editing buffer.  Characters are accumulated here
 * until a line-terminating character (newline, VEOL, or VEOF) arrives,
 * at which point the complete line is flushed into the input pipe.
 * This is how POSIX canonical mode is supposed to work: read() blocks
 * until a full line is available.
 *
 * MAX_CANON is the traditional limit (typically 255).  The buffer
 * holds up to MAX_CANON characters plus the terminating newline.
 */
#define TTY_CANON_BUF_SIZE 256

struct tty {
    spinlock_t lock; // Protects termios, winsize, and raw buffer

    struct termios termios; // Terminal I/O settings
    struct winsize winsize; // Terminal window size
    struct tty_ops *ops;    // Operations for this terminal
    int ref_count;          // Reference count for this terminal

    struct pipe *input_pipe;  // Pipe for incoming data (canonical mode)
    struct pipe *output_pipe; // Pipe for outgoing data (written by terminal)

    /*
     * Raw-mode (non-canonical) character buffer.
     *
     * When ICANON is off, incoming characters bypass the input pipe
     * and are stored here.  tty_read drains this buffer directly,
     * giving single-character latency without pipe overhead.
     */
    char   raw_buf[TTY_RAW_BUF_SIZE];
    uint   raw_r; // read index  (consumer: tty_read)
    uint   raw_w; // write index (producer: tty_input)
    tq_t   raw_wait; // readers sleep here when buffer empty

    /* Canonical-mode line editing buffer */
    char   canon_buf[TTY_CANON_BUF_SIZE];
    uint   canon_len; // number of characters in canon_buf

    void *driver_data; // Driver-specific data pointer

    struct session *session; // Owning session (NULL if no session attached)

    char name[64]; // Terminal name
};

#endif /* __KERNEL_TTY_TYPES_H */
