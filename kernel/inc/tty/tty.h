#ifndef __KERNEL_TTY_H
#define __KERNEL_TTY_H

#include "tty_types.h"

/* ---- termios.c ---- */
void termios_init_default(struct termios *t);

/* ---- tty.c ---- */
void tty_init(void);
struct tty *tty_alloc(const char *name, struct tty_ops *ops);
void tty_free(struct tty *tty);
void tty_ref(struct tty *tty);
void tty_unref(struct tty *tty);

ssize_t tty_input(struct tty *tty, const char *buf, size_t count);
ssize_t tty_read(struct tty *tty, char *buf, size_t count, bool user);
ssize_t tty_write(struct tty *tty, const char *buf, size_t count, bool user);
ssize_t tty_output(struct tty *tty, char *buf, size_t count);
int tty_ioctl(struct tty *tty, uint64 cmd, uint64 arg);
int tty_open(struct tty *tty);
void tty_close(struct tty *tty);
void tty_hangup(struct tty *tty);

/* ---- pty.c ---- */
int pty_alloc(struct tty **slave_out, const char *name);
ssize_t pty_master_read(struct tty *slave, char *buf, size_t count, bool user);
ssize_t pty_master_write(struct tty *slave, const char *buf, size_t count, bool user);

#endif /* __KERNEL_TTY_H */
