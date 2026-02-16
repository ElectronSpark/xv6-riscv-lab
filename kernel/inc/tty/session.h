#ifndef __KERNEL_SESSION_H
#define __KERNEL_SESSION_H

#include "tty/session_types.h"

/* Lifecycle */
void session_init(struct thread *initproc);
struct session *session_alloc(pid_t sid);
void session_ref(struct session *s);
void session_unref(struct session *s);

int session_add_thread(struct session *s, struct thread *t);
int session_remove_thread(struct session *s, struct thread *t);
int session_add_pg(struct session *s, struct pgroup *pg);
int session_remove_pg(struct session *s, struct pgroup *pg);

/* Controlling terminal */
void session_set_ctrl_tty(struct session *s, struct tty *tty);
struct tty *session_get_ctrl_tty(struct session *s);

/* Foreground process group */
void session_set_fg_pgid(struct session *s, pid_t pgid);
pid_t session_get_fg_pgid(struct session *s);

/*
 * session_hangup - mark session as exited and signal all members
 *
 * Sends SIGHUP + SIGCONT to the foreground process group (or all
 * pgroups if no foreground group exists), marks the session as
 * exited, and disassociates the controlling terminal.
 *
 * Called when the session leader exits or the controlling terminal
 * is disconnected.
 *
 * Caller must hold pid_wlock.
 */
void session_hangup(struct session *s);

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
 * Look up a session by SID.
 *
 * Caller must be inside rcu_read_lock() or hold pid_lock.
 * Returns session pointer, or ERR_PTR(-ESRCH) if not found.
 */
struct session *get_session(pid_t sid);

/*
 * Global session list — protected by pid_lock.
 * Use session_for_each / session_for_each_safe to iterate.
 */
extern list_node_t session_list;

#define session_for_each(pos, tmp) \
    list_foreach_node_safe(&session_list, pos, tmp, global_entry)

#endif /* __KERNEL_SESSION_H */
