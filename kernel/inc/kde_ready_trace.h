#ifndef __KERNEL_KDE_READY_TRACE_H
#define __KERNEL_KDE_READY_TRACE_H

#include "types.h"

struct thread;

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

#endif /* __KERNEL_KDE_READY_TRACE_H */
