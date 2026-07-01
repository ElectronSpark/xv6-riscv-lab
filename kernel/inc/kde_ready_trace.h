#ifndef __KERNEL_KDE_READY_TRACE_H
#define __KERNEL_KDE_READY_TRACE_H

#include "types.h"

int kde_ready_trace_enabled(void);
int kde_ready_trace_current(void);
int kde_ready_trace_path_match(const char *path);
void kde_ready_trace_event(const char *phase, int fd, int arg0, int arg1,
                           int ret, uint64 wait_ms);

#endif /* __KERNEL_KDE_READY_TRACE_H */
