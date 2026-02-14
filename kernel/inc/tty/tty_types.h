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
    int (*ioctl)(struct tty *tty, uint64 cmd, uint64 arg);
};

struct tty {
    spinlock_t lock; // Protects termios and winsize

    struct termios termios; // Terminal I/O settings
    struct winsize winsize; // Terminal window size
    struct tty_ops *ops;    // Operations for this terminal
    int ref_count;          // Reference count for this terminal

    struct pipe *input_pipe;  // Pipe for incoming data (read by terminal)
    struct pipe *output_pipe; // Pipe for outgoing data (written by terminal)

    void *driver_data; // Driver-specific data pointer

    struct session *session; // Owning session (NULL if no session attached)

    char name[64]; // Terminal name
};

#endif /* __KERNEL_TTY_TYPES_H */
