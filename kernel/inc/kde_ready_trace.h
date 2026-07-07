#ifndef __KERNEL_KDE_READY_TRACE_H
#define __KERNEL_KDE_READY_TRACE_H

#include "types.h"

struct thread;
struct vfs_file;
struct konsole_prepty_wake_snapshot;

int kde_ready_trace_enabled(void);
int kde_ready_trace_current(void);
int kde_ready_trace_path_match(const char *path);
void kde_ready_trace_event(const char *phase, int fd, int arg0, int arg1,
                           int ret, uint64 wait_ms);

/* Opt-in kde_wake_to_run_trace=1: behavior-free wake-to-run latency trace
 * for Konsole-scoped threads. note_wake stamps the sleeping thread inside
 * the wakeup path; note_run prints and clears the stamp after the thread is
 * switched in (locks already released). */
int kde_wake_to_run_trace_enabled(void);
void kde_wake_to_run_trace_note_wake(struct thread *p);
void kde_wake_to_run_trace_note_run(struct thread *p);

/* Opt-in konsole_child_launch_trace=1: behavior-free child launch timeline
 * scoped to /usr/bin/konsole process clones and the
 * /bin/kde-konsole-shell-wrapper exec that follows. */
int kde_konsole_child_launch_trace_enabled(void);
int kde_konsole_child_launch_trace_current_child_setup(void);
void kde_konsole_child_launch_trace_signal_note(struct thread *p, int signo,
                                                int source, int si_code,
                                                int si_pid);

#define KDE_KONSOLE_CHILD_SIGNAL_THREAD_QUEUED 1
#define KDE_KONSOLE_CHILD_SIGNAL_GROUP_QUEUED  2
#define KDE_KONSOLE_CHILD_SIGNAL_THREAD_DELIV  3
#define KDE_KONSOLE_CHILD_SIGNAL_GROUP_DELIV   4

void kde_konsole_child_launch_trace_clone_begin(struct thread *parent,
                                                uint64 flags);
void kde_konsole_child_launch_trace_clone_done(struct thread *parent,
                                               struct thread *child,
                                               uint64 flags);
void kde_konsole_child_launch_trace_child_woken(struct thread *child);
void kde_konsole_child_launch_trace_child_run(struct thread *child);
void kde_konsole_child_launch_trace_exec_begin(const char *path);
void kde_konsole_child_launch_trace_exec_done(const char *path, int argc,
                                              int has_interp, int ret,
                                              uint64 elapsed_ms);
void kde_konsole_child_launch_trace_parent_wait(
    const char *phase, uint64 wait_start_ms, uint64 wait_end_ms, int nfds,
    int timeout_ms, int ret, uint32 role_mask, int primary_role);

enum kde_konsole_child_setup_bucket {
    KDE_KONSOLE_CHILD_SETUP_SETSID = 0,
    KDE_KONSOLE_CHILD_SETUP_SETPGID,
    KDE_KONSOLE_CHILD_SETUP_IOCTL_CTTY,
    KDE_KONSOLE_CHILD_SETUP_IOCTL_PGRP,
    KDE_KONSOLE_CHILD_SETUP_IOCTL_TCSETS,
    KDE_KONSOLE_CHILD_SETUP_IOCTL_OTHER,
    KDE_KONSOLE_CHILD_SETUP_DUP,
    KDE_KONSOLE_CHILD_SETUP_CLOSE,
    KDE_KONSOLE_CHILD_SETUP_CLOSE_RANGE,
    KDE_KONSOLE_CHILD_SETUP_FCNTL,
    KDE_KONSOLE_CHILD_SETUP_SIGMASK,
    KDE_KONSOLE_CHILD_SETUP_SIGACTION,
    KDE_KONSOLE_CHILD_SETUP_CHDIR,
    KDE_KONSOLE_CHILD_SETUP_WAIT,
    KDE_KONSOLE_CHILD_SETUP_POLL,
    KDE_KONSOLE_CHILD_SETUP_BUCKETS
};

uint64 kde_konsole_child_launch_trace_setup_begin(int bucket);
void kde_konsole_child_launch_trace_setup_end(int bucket, uint64 start_ms,
                                              int arg0, int arg1, int ret);

#define KDE_KONSOLE_PREPTY_RING_DISARM_NONE 0
#define KDE_KONSOLE_PREPTY_RING_DISARM_PTY 1
#define KDE_KONSOLE_PREPTY_RING_DISARM_LIMIT 2

void kde_konsole_prepty_ring_arm(const char *path);
void kde_konsole_prepty_ring_disarm(int reason);
int kde_konsole_prepty_ring_active(void);
void kde_konsole_prepty_ring_record_notify(
    struct vfs_file *file, int filter, int64 data, int matched,
    int enqueued_new, int already_queued, int propagated,
    uint64 first_ident, uint64 last_ident, uint64 first_udata,
    uint64 last_udata, void *first_kq, void *last_kq, void *origin);
void kde_konsole_prepty_ring_record_notify_eventfd(
    struct vfs_file *file, int filter, int64 data, int matched,
    int enqueued_new, int already_queued, int propagated,
    uint64 first_ident, uint64 last_ident, uint64 first_udata,
    uint64 last_udata, void *first_kq, void *last_kq, void *origin,
    int eventfd_op, uint64 counter_before, uint64 counter_after,
    uint64 value, uint64 read_value, int ret, void *eventfd_caller);
void kde_konsole_prepty_ring_record_pipe_write(
    void *pipe, struct vfs_file *write_file, struct vfs_file *read_file,
    ssize_t bytes, size_t count, uint32 nread, uint32 nwrite);
void kde_konsole_prepty_ring_record_poll_wait(
    uint64 wait_start_ms, uint64 waiter_run_ms, int nfds, int timeout_ms,
    int has_unnotified_fds, int ready, int nevents, uint32 role_mask,
    int primary_role, int poll_op);
void kde_konsole_prepty_ring_record_fd_lifecycle(
    int op, int oldfd, int newfd, struct vfs_file *file,
    struct vfs_file *replaced_file, int ret, int first_visible);
void kde_konsole_prepty_ring_record_eventfd_op(
    struct vfs_file *file, int op, uint64 counter_before,
    uint64 counter_after, uint64 value, uint64 read_value, int ret,
    void *caller);
void kde_konsole_prepty_ring_record_poll_fd(
    int fd, struct vfs_file *file, short events, short revents, int nfds,
    int scan_ready, int poll_op);
int kde_konsole_prepty_ring_snapshot(
    struct konsole_prepty_wake_snapshot *snap, uint64 size);

#endif /* __KERNEL_KDE_READY_TRACE_H */
