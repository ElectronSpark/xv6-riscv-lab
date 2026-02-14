#ifndef __KERNEL_SESSION_H
#define __KERNEL_SESSION_H

#include "tty/session_types.h"

struct tty;
struct thread;

/* Lifecycle */
void session_init(void);
struct session *session_alloc(pid_t sid);
void session_ref(struct session *s);
void session_unref(struct session *s);

/* Controlling terminal */
void session_set_ctrl_tty(struct session *s, struct tty *tty);
struct tty *session_get_ctrl_tty(struct session *s);

/* Foreground process group */
void session_set_fg_pgid(struct session *s, pid_t pgid);
pid_t session_get_fg_pgid(struct session *s);

/*
 * setsid - create a new session (POSIX setsid)
 *
 * The calling process becomes the session leader of a new session.
 * Its pgid and sid are set to its pid.  There is no controlling
 * terminal initially.
 *
 * Must not be called by a process group leader (pgid == tgid).
 *
 * Returns the new session ID, or negative errno on failure.
 */
pid_t session_setsid(void);

/*
 * getsid - get the session ID of a process
 *
 * If @pid is 0, return the caller's sid.
 * Returns sid, or negative errno on failure.
 */
pid_t session_getsid(pid_t pid);

/*
 * session_init_first - create the initial session for init
 *
 * Called once during boot to give the init process (pid 1) a session.
 */
void session_init_first(struct thread *initproc);

#endif /* __KERNEL_SESSION_H */
