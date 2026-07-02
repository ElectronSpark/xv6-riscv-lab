#include "types.h"
#include "string.h"
#include "cmdline.h"
#include "printf.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include <smp/percpu.h>
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

int kde_wake_to_run_trace_enabled(void)
{
    return kde_ready_value_enabled("kde_wake_to_run_trace");
}

/* Optional min-latency filter: kde_wake_to_run_trace=<N> with N > 1 prints
 * only wake-to-run latencies of at least N ms. kde_wake_to_run_trace=1
 * prints every Konsole-scoped wake. */
static uint64 kde_wake_to_run_trace_min_ms(void)
{
    static int initialized;
    static uint64 min_ms;
    char value[16];

    if (!initialized) {
        if (cmdline_get_param("kde_wake_to_run_trace", value,
                              sizeof(value)) == 0) {
            uint64 n = 0;
            for (int i = 0; value[i] >= '0' && value[i] <= '9'; i++)
                n = n * 10 + (uint64)(value[i] - '0');
            if (n > 1)
                min_ms = n;
        }
        initialized = 1;
    }
    return min_ms;
}

/* Konsole-scoped only: match on the thread group's exec path so session
 * startup (kwin/plasmashell Wayland and DBus threads) stays quiet. */
static int kde_wake_to_run_thread_match(const struct thread *p)
{
    if (p == NULL || p->thread_group == NULL)
        return 0;
    return kde_ready_trace_path_match(p->thread_group->exec_path);
}

/* Called from the wakeup path with sched_entity->pi_lock held and the
 * target thread confirmed sleeping. Field writes only; no printing. */
void kde_wake_to_run_trace_note_wake(struct thread *p)
{
    if (!kde_wake_to_run_trace_enabled() || !kde_wake_to_run_thread_match(p))
        return;

    uint64 now = sched_timer_now_ms();

    p->kde_wake_trace_ms = now != 0 ? now : 1;
    p->kde_wake_trace_waker_irq = CPU_IN_ITR() ? 1 : 0;
    if (current != NULL) {
        p->kde_wake_trace_waker_pid = current->pid;
        safestrcpy(p->kde_wake_trace_waker, current->name,
                   sizeof(p->kde_wake_trace_waker));
    } else {
        p->kde_wake_trace_waker_pid = -1;
        p->kde_wake_trace_waker[0] = '\0';
    }
}

/* Called after context_switch_finish() has released the rq_lock, in the
 * context of the thread that just started running. */
void kde_wake_to_run_trace_note_run(struct thread *p)
{
    if (p == NULL || p->kde_wake_trace_ms == 0)
        return;

    uint64 wake_ms = p->kde_wake_trace_ms;

    p->kde_wake_trace_ms = 0;
    if (!kde_wake_to_run_trace_enabled())
        return;

    uint64 now = sched_timer_now_ms();
    uint64 delta = now >= wake_ms ? now - wake_ms : 0;

    if (delta < kde_wake_to_run_trace_min_ms())
        return;

    printf("kde-wake-to-run: ms=%lu pid=%d tgid=%d name=%s wake_ms=%lu "
           "wake_to_run_ms=%lu waker_pid=%d waker=%s waker_irq=%d\n",
           now, p->pid, p->tgid, p->name, wake_ms, delta,
           p->kde_wake_trace_waker_pid, p->kde_wake_trace_waker,
           p->kde_wake_trace_waker_irq);
}
