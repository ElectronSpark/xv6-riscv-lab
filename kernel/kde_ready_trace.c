#include "types.h"
#include "string.h"
#include "cmdline.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "kde_ready_trace.h"

static int kde_ready_value_enabled(const char *key)
{
    static int initialized;
    static int enabled;
    char value[8];

    if (!initialized) {
        enabled = cmdline_get_param(key, value, sizeof(value)) == 0 &&
                  value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

int kde_ready_trace_enabled(void)
{
    return kde_ready_value_enabled("konsole_ready_trace");
}

int kde_ready_trace_path_match(const char *path)
{
    if (path == NULL)
        return 0;
    return strstr(path, "/konsole") != NULL ||
           strstr(path, "kde-konsole-shell-wrapper") != NULL;
}

static int kde_ready_trace_name_match(const char *name)
{
    if (name == NULL)
        return 0;
    return strncmp(name, "konsole", 7) == 0 ||
           strncmp(name, "QDBusConnection", 15) == 0 ||
           strncmp(name, "WaylandEventThr", 15) == 0 ||
           strncmp(name, "kde-konsole-she", 15) == 0;
}

int kde_ready_trace_current(void)
{
    if (!kde_ready_trace_enabled() || current == NULL)
        return 0;
    if (current->thread_group == NULL ||
        !kde_ready_trace_path_match(current->thread_group->exec_path))
        return 0;
    if (kde_ready_trace_name_match(current->name))
        return 1;
    return 0;
}

void kde_ready_trace_event(const char *phase, int fd, int arg0, int arg1,
                           int ret, uint64 wait_ms)
{
    static _Atomic uint64 seq;
    struct thread *p = current;
    const char *exec_path = "";

    if (!kde_ready_trace_enabled() || p == NULL)
        return;

    if (p->thread_group != NULL)
        exec_path = p->thread_group->exec_path;

    uint64 n = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED);
    printf("kde-ready-trace: seq=%lu ms=%lu pid=%d tgid=%d name=%s "
           "exec='%s' phase=%s fd=%d arg0=%d arg1=%d ret=%d wait_ms=%lu "
           "sid=%d pgid=%d\n",
           n, sched_timer_now_ms(), p->pid, p->tgid, p->name,
           exec_path != NULL ? exec_path : "", phase != NULL ? phase : "",
           fd, arg0, arg1, ret, wait_ms, p->sid, p->pgid);
}
