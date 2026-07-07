#include "types.h"
#include "string.h"
#include "cmdline.h"
#include "printf.h"
#include "defs.h"
#include "kstats.h"
#include "ksymbols.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/sched.h"
#include "clone_flags.h"
#include "vfs/file.h"
#include "vfs/poll.h"
#include "vfs/unix_socket.h"
#include "vfs/vfs_types.h"
#include "kqueue_types.h"
#include <smp/percpu.h>
#include "kde_ready_trace.h"

/* NOTE: each gate needs its OWN cache.  The old shared-static version
 * cached the FIRST queried key's answer for every key, silently
 * aliasing konsole_ready_trace and kde_wake_to_run_trace (whichever
 * ran first won — the wake-to-run trace was dead on boots where the
 * poll path queried the ready-trace gate first). */
static int kde_cmdline_flag_enabled(const char *key, int *initialized,
                                    int *enabled)
{
    char value[8];

    if (!*initialized) {
        *enabled = cmdline_get_param(key, value, sizeof(value)) == 0 &&
                   value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        *initialized = 1;
    }
    return *enabled;
}

int kde_ready_trace_enabled(void)
{
    static int initialized, enabled;

    return kde_cmdline_flag_enabled("konsole_ready_trace", &initialized,
                                    &enabled);
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
    static int initialized, enabled;

    return kde_cmdline_flag_enabled("kde_wake_to_run_trace", &initialized,
                                    &enabled);
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

/* wake_to_run_trace_all=1 widens the scope to EVERY thread — diagnostic
 * boots only, pair with a min-ms filter (kde_wake_to_run_trace=<N>) to
 * bound serial volume.  Used to test the systemic-scheduling-latency
 * hypothesis for desktop-wide slowness. */
static int kde_wake_to_run_trace_all(void)
{
    static int initialized, enabled;

    return kde_cmdline_flag_enabled("wake_to_run_trace_all", &initialized,
                                    &enabled);
}

/* Konsole-scoped by default: match on the thread group's exec path so
 * session startup (kwin/plasmashell Wayland and DBus threads) stays
 * quiet. */
static int kde_wake_to_run_thread_match(const struct thread *p)
{
    if (p == NULL || p->thread_group == NULL)
        return 0;
    if (kde_wake_to_run_trace_all())
        return 1;
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

#define KONSOLE_CHILD_LAUNCH_RECORD_CAP 16
#define KONSOLE_CHILD_SETUP_SLOW_CAP 4

struct konsole_child_launch_record {
    int used;
    pid_t pid;
    pid_t tgid;
    uint64 pid_seq;
    pid_t parent_pid;
    pid_t parent_tgid;
    uint64 flags;
    uint64 clone_begin_ms;
    uint64 clone_done_ms;
    uint64 child_woken_ms;
    uint64 child_run_ms;
    uint64 exec_begin_ms;
    uint64 exec_done_ms;
    uint64 setup_first_ms;
    uint64 setup_last_ms;
    uint64 setup_total_calls;
    uint64 setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_BUCKETS];
    uint64 setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_BUCKETS];
    uint64 setup_bucket_max_ms[KDE_KONSOLE_CHILD_SETUP_BUCKETS];
    uint64 setup_slow_count;
    uint64 setup_slow_logged;
    uint64 setup_slow_ms[KONSOLE_CHILD_SETUP_SLOW_CAP];
    int setup_slow_bucket[KONSOLE_CHILD_SETUP_SLOW_CAP];
    int setup_slow_arg0[KONSOLE_CHILD_SETUP_SLOW_CAP];
    int setup_slow_arg1[KONSOLE_CHILD_SETUP_SLOW_CAP];
    int setup_slow_ret[KONSOLE_CHILD_SETUP_SLOW_CAP];
    char parent_comm[16];
    char child_comm[16];
    char parent_exec[128];
    char child_exec[128];
};

struct konsole_child_parent_wait_record {
    int valid;
    pid_t pid;
    pid_t tgid;
    uint64 pid_seq;
    uint64 wait_start_ms;
    uint64 wait_end_ms;
    uint64 wait_ms;
    int nfds;
    int timeout_ms;
    int ret;
    uint32 role_mask;
    int primary_role;
    enum thread_state state;
    char phase[16];
    char comm[16];
};

static spinlock_t konsole_child_launch_lock =
    SPINLOCK_INITIALIZED("konsole_child_launch");
static struct konsole_child_launch_record
    konsole_child_launch_records[KONSOLE_CHILD_LAUNCH_RECORD_CAP];
static struct konsole_child_parent_wait_record konsole_child_parent_last_wait;
static uint64 konsole_child_launch_next_slot;
static uint64 konsole_child_launch_last_begin_ms;
static _Atomic uint64 konsole_child_launch_seq;

int kde_konsole_child_launch_trace_enabled(void)
{
    static int initialized, enabled;

    return kde_cmdline_flag_enabled("konsole_child_launch_trace",
                                    &initialized, &enabled);
}

int kde_konsole_child_launch_trace_current_child_setup(void)
{
    return kde_konsole_child_launch_trace_enabled() && current != NULL &&
           current->signal.kde_child_trace_active;
}

void kde_konsole_child_launch_trace_signal_note(struct thread *p, int signo,
                                                int source, int si_code,
                                                int si_pid)
{
    if (!kde_konsole_child_launch_trace_enabled() || p == NULL ||
        !p->signal.kde_child_trace_active)
        return;

    p->signal.kde_child_trace_last_signal = signo;
    p->signal.kde_child_trace_last_signal_source = source;
    p->signal.kde_child_trace_last_signal_code = si_code;
    p->signal.kde_child_trace_last_signal_pid = si_pid;
    p->signal.kde_child_trace_last_signal_ms = sched_timer_now_ms();
}

static const char *thread_exec_path(const struct thread *p)
{
    if (p == NULL || p->thread_group == NULL)
        return "";
    return p->thread_group->exec_path;
}

static int konsole_child_launch_parent_match(const struct thread *p)
{
    const char *path = thread_exec_path(p);

    if (path == NULL)
        return 0;
    return strcmp(path, "/usr/bin/konsole") == 0 ||
           strstr(path, "/usr/bin/konsole") != NULL;
}

static int konsole_child_launch_wrapper_match(const char *path)
{
    if (path == NULL)
        return 0;
    return strcmp(path, "/bin/kde-konsole-shell-wrapper") == 0 ||
           strstr(path, "kde-konsole-shell-wrapper") != NULL;
}

static struct konsole_child_launch_record *
konsole_child_launch_find_locked(const struct thread *p)
{
    if (p == NULL)
        return NULL;
    for (int i = 0; i < KONSOLE_CHILD_LAUNCH_RECORD_CAP; i++) {
        struct konsole_child_launch_record *rec =
            &konsole_child_launch_records[i];

        if (!rec->used)
            continue;
        if (rec->pid == p->pid && rec->pid_seq == p->pid_seq)
            return rec;
    }
    return NULL;
}

static uint64 konsole_child_launch_next_seq(void)
{
    return __atomic_add_fetch(&konsole_child_launch_seq, 1,
                              __ATOMIC_RELAXED);
}

static const char *konsole_prepty_role_name(int role)
{
    switch (role) {
    case KONSOLE_PREPTY_WAKE_ROLE_WAYLAND:
        return "wayland";
    case KONSOLE_PREPTY_WAKE_ROLE_QDBUS:
        return "qdbus";
    case KONSOLE_PREPTY_WAKE_ROLE_EVENTFD:
        return "eventfd";
    case KONSOLE_PREPTY_WAKE_ROLE_PIPE:
        return "pipe";
    case KONSOLE_PREPTY_WAKE_ROLE_KQUEUE:
        return "kqueue";
    case KONSOLE_PREPTY_WAKE_ROLE_UNIX_OTHER:
        return "unix_other";
    case KONSOLE_PREPTY_WAKE_ROLE_OTHER:
    default:
        return "other";
    }
}

static const char *konsole_child_setup_bucket_name(int bucket)
{
    switch (bucket) {
    case KDE_KONSOLE_CHILD_SETUP_SETSID:
        return "setsid";
    case KDE_KONSOLE_CHILD_SETUP_SETPGID:
        return "setpgid";
    case KDE_KONSOLE_CHILD_SETUP_IOCTL_CTTY:
        return "ioctl_ctty";
    case KDE_KONSOLE_CHILD_SETUP_IOCTL_PGRP:
        return "ioctl_pgrp";
    case KDE_KONSOLE_CHILD_SETUP_IOCTL_TCSETS:
        return "ioctl_tcsets";
    case KDE_KONSOLE_CHILD_SETUP_IOCTL_OTHER:
        return "ioctl_other";
    case KDE_KONSOLE_CHILD_SETUP_DUP:
        return "dup";
    case KDE_KONSOLE_CHILD_SETUP_CLOSE:
        return "close";
    case KDE_KONSOLE_CHILD_SETUP_CLOSE_RANGE:
        return "close_range";
    case KDE_KONSOLE_CHILD_SETUP_FCNTL:
        return "fcntl";
    case KDE_KONSOLE_CHILD_SETUP_SIGMASK:
        return "sigmask";
    case KDE_KONSOLE_CHILD_SETUP_SIGACTION:
        return "sigaction";
    case KDE_KONSOLE_CHILD_SETUP_CHDIR:
        return "chdir";
    case KDE_KONSOLE_CHILD_SETUP_WAIT:
        return "wait";
    case KDE_KONSOLE_CHILD_SETUP_POLL:
        return "poll";
    default:
        return "unknown";
    }
}

static int konsole_child_setup_bucket_valid(int bucket)
{
    return bucket >= 0 && bucket < KDE_KONSOLE_CHILD_SETUP_BUCKETS;
}

static void konsole_child_launch_print_setup_summary(
    const struct konsole_child_launch_record *rec, uint64 exec_begin_ms)
{
    uint64 seq = konsole_child_launch_next_seq();
    uint64 first_delta =
        rec->setup_first_ms != 0 && rec->child_run_ms != 0 &&
        rec->setup_first_ms >= rec->child_run_ms ?
            rec->setup_first_ms - rec->child_run_ms : 0;
    uint64 last_delta =
        rec->setup_last_ms != 0 && exec_begin_ms >= rec->setup_last_ms ?
            exec_begin_ms - rec->setup_last_ms : 0;

    printf("konsole-child-launch: seq=%lu ms=%lu phase=child_setup_summary "
           "pid=%d tgid=%d child_seq=%lu total_calls=%lu "
           "first_setup_ms=%lu child_run_to_first_setup_ms=%lu "
           "last_setup_ms=%lu last_setup_to_exec_begin_ms=%lu "
           "setsid_calls=%lu setsid_ms=%lu setpgid_calls=%lu setpgid_ms=%lu "
           "ioctl_ctty_calls=%lu ioctl_ctty_ms=%lu "
           "ioctl_pgrp_calls=%lu ioctl_pgrp_ms=%lu "
           "ioctl_tcsets_calls=%lu ioctl_tcsets_ms=%lu "
           "ioctl_other_calls=%lu ioctl_other_ms=%lu "
           "dup_calls=%lu dup_ms=%lu close_calls=%lu close_ms=%lu "
           "close_range_calls=%lu close_range_ms=%lu "
           "fcntl_calls=%lu fcntl_ms=%lu sigmask_calls=%lu sigmask_ms=%lu "
           "sigaction_calls=%lu sigaction_ms=%lu chdir_calls=%lu chdir_ms=%lu "
           "wait_calls=%lu wait_ms=%lu poll_calls=%lu poll_ms=%lu "
           "max_bucket=%s max_bucket_ms=%lu slow_count=%lu slow_logged=%lu\n",
           seq, exec_begin_ms, rec->pid, rec->tgid, rec->pid_seq,
           rec->setup_total_calls, rec->setup_first_ms, first_delta,
           rec->setup_last_ms, last_delta,
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_SETSID],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_SETSID],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_SETPGID],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_SETPGID],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_IOCTL_CTTY],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_IOCTL_CTTY],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_IOCTL_PGRP],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_IOCTL_PGRP],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_IOCTL_TCSETS],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_IOCTL_TCSETS],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_IOCTL_OTHER],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_IOCTL_OTHER],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_DUP],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_DUP],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_CLOSE],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_CLOSE],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_CLOSE_RANGE],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_CLOSE_RANGE],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_FCNTL],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_FCNTL],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_SIGMASK],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_SIGMASK],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_SIGACTION],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_SIGACTION],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_CHDIR],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_CHDIR],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_WAIT],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_WAIT],
           rec->setup_bucket_calls[KDE_KONSOLE_CHILD_SETUP_POLL],
           rec->setup_bucket_ms[KDE_KONSOLE_CHILD_SETUP_POLL],
           konsole_child_setup_bucket_name(rec->setup_slow_bucket[0]),
           rec->setup_slow_ms[0], rec->setup_slow_count,
           rec->setup_slow_logged);

    for (uint64 i = 0; i < rec->setup_slow_logged &&
                       i < KONSOLE_CHILD_SETUP_SLOW_CAP; i++) {
        seq = konsole_child_launch_next_seq();
        printf("konsole-child-launch: seq=%lu ms=%lu "
               "phase=child_setup_slow pid=%d bucket=%s elapsed_ms=%lu "
               "arg0=%d arg1=%d ret=%d\n",
               seq, exec_begin_ms, rec->pid,
               konsole_child_setup_bucket_name(rec->setup_slow_bucket[i]),
               rec->setup_slow_ms[i], rec->setup_slow_arg0[i],
               rec->setup_slow_arg1[i], rec->setup_slow_ret[i]);
    }
}

void kde_konsole_child_launch_trace_parent_wait(
    const char *phase, uint64 wait_start_ms, uint64 wait_end_ms, int nfds,
    int timeout_ms, int ret, uint32 role_mask, int primary_role)
{
    struct thread *p = current;

    if (!kde_konsole_child_launch_trace_enabled() || p == NULL ||
        !konsole_child_launch_parent_match(p))
        return;

    spin_lock(&konsole_child_launch_lock);
    memset(&konsole_child_parent_last_wait, 0,
           sizeof(konsole_child_parent_last_wait));
    konsole_child_parent_last_wait.valid = 1;
    konsole_child_parent_last_wait.pid = p->pid;
    konsole_child_parent_last_wait.tgid = p->tgid;
    konsole_child_parent_last_wait.pid_seq = p->pid_seq;
    konsole_child_parent_last_wait.wait_start_ms = wait_start_ms;
    konsole_child_parent_last_wait.wait_end_ms = wait_end_ms;
    konsole_child_parent_last_wait.wait_ms =
        wait_end_ms >= wait_start_ms ? wait_end_ms - wait_start_ms : 0;
    konsole_child_parent_last_wait.nfds = nfds;
    konsole_child_parent_last_wait.timeout_ms = timeout_ms;
    konsole_child_parent_last_wait.ret = ret;
    konsole_child_parent_last_wait.role_mask = role_mask;
    konsole_child_parent_last_wait.primary_role = primary_role;
    konsole_child_parent_last_wait.state = __thread_state_get(p);
    safestrcpy(konsole_child_parent_last_wait.phase,
               phase != NULL ? phase : "", sizeof(konsole_child_parent_last_wait.phase));
    safestrcpy(konsole_child_parent_last_wait.comm, p->name,
               sizeof(konsole_child_parent_last_wait.comm));
    spin_unlock(&konsole_child_launch_lock);
}

uint64 kde_konsole_child_launch_trace_setup_begin(int bucket)
{
    struct thread *p = current;
    uint64 now = 0;

    if (!kde_konsole_child_launch_trace_enabled() || p == NULL ||
        !konsole_child_setup_bucket_valid(bucket))
        return 0;

    spin_lock(&konsole_child_launch_lock);
    struct konsole_child_launch_record *rec =
        konsole_child_launch_find_locked(p);
    if (rec != NULL && rec->child_run_ms != 0 && rec->exec_begin_ms == 0)
        now = sched_timer_now_ms();
    spin_unlock(&konsole_child_launch_lock);
    return now;
}

void kde_konsole_child_launch_trace_setup_end(int bucket, uint64 start_ms,
                                              int arg0, int arg1, int ret)
{
    struct thread *p = current;
    uint64 now;
    uint64 elapsed;

    if (start_ms == 0 || !konsole_child_setup_bucket_valid(bucket) ||
        p == NULL)
        return;

    now = sched_timer_now_ms();
    elapsed = now >= start_ms ? now - start_ms : 0;

    spin_lock(&konsole_child_launch_lock);
    struct konsole_child_launch_record *rec =
        konsole_child_launch_find_locked(p);
    if (rec != NULL && rec->exec_begin_ms == 0) {
        rec->setup_total_calls++;
        if (rec->setup_first_ms == 0)
            rec->setup_first_ms = start_ms;
        rec->setup_last_ms = now != 0 ? now : start_ms;
        rec->setup_bucket_calls[bucket]++;
        rec->setup_bucket_ms[bucket] += elapsed;
        if (elapsed > rec->setup_bucket_max_ms[bucket])
            rec->setup_bucket_max_ms[bucket] = elapsed;
        if (elapsed >= 10) {
            uint64 slot = rec->setup_slow_count;
            rec->setup_slow_count++;
            if (slot < KONSOLE_CHILD_SETUP_SLOW_CAP) {
                rec->setup_slow_logged = slot + 1;
                rec->setup_slow_ms[slot] = elapsed;
                rec->setup_slow_bucket[slot] = bucket;
                rec->setup_slow_arg0[slot] = arg0;
                rec->setup_slow_arg1[slot] = arg1;
                rec->setup_slow_ret[slot] = ret;
            }
        }
        if (elapsed > rec->setup_slow_ms[0]) {
            rec->setup_slow_ms[0] = elapsed;
            rec->setup_slow_bucket[0] = bucket;
            rec->setup_slow_arg0[0] = arg0;
            rec->setup_slow_arg1[0] = arg1;
            rec->setup_slow_ret[0] = ret;
            if (rec->setup_slow_logged == 0)
                rec->setup_slow_logged = 1;
        }
    }
    spin_unlock(&konsole_child_launch_lock);
}

void kde_konsole_child_launch_trace_clone_begin(struct thread *parent,
                                                uint64 flags)
{
    uint64 now;
    uint64 seq;
    const char *exec_path;
    struct konsole_child_parent_wait_record last_wait;
    int have_last_wait = 0;

    if (!kde_konsole_child_launch_trace_enabled() ||
        parent == NULL || (flags & CLONE_THREAD) ||
        !konsole_child_launch_parent_match(parent))
        return;

    now = sched_timer_now_ms();
    spin_lock(&konsole_child_launch_lock);
    konsole_child_launch_last_begin_ms = now != 0 ? now : 1;
    if (konsole_child_parent_last_wait.valid &&
        konsole_child_parent_last_wait.tgid == parent->tgid) {
        last_wait = konsole_child_parent_last_wait;
        have_last_wait = 1;
    }
    spin_unlock(&konsole_child_launch_lock);

    exec_path = thread_exec_path(parent);
    seq = konsole_child_launch_next_seq();
    printf("konsole-child-launch: seq=%lu ms=%lu phase=clone_begin "
           "parent_pid=%d parent_tgid=%d parent_comm=%s parent_exec='%s' "
           "parent_state=%s flags=0x%lx thread=0 vfork=%d vm=%d "
           "last_wait_valid=%d last_wait_phase=%s last_wait_end_ms=%lu "
           "last_wait_age_ms=%lu last_wait_ms=%lu last_wait_nfds=%d "
           "last_wait_timeout_ms=%d last_wait_ret=%d "
           "last_wait_role=%s last_wait_role_mask=0x%x last_wait_comm=%s\n",
           seq, now, parent->pid, parent->tgid, parent->name,
           exec_path != NULL ? exec_path : "",
           thread_state_to_str(__thread_state_get(parent)), flags,
           !!(flags & CLONE_VFORK), !!(flags & CLONE_VM),
           have_last_wait, have_last_wait ? last_wait.phase : "",
           have_last_wait ? last_wait.wait_end_ms : 0,
           have_last_wait && now >= last_wait.wait_end_ms ?
               now - last_wait.wait_end_ms : 0,
           have_last_wait ? last_wait.wait_ms : 0,
           have_last_wait ? last_wait.nfds : 0,
           have_last_wait ? last_wait.timeout_ms : 0,
           have_last_wait ? last_wait.ret : 0,
           have_last_wait ?
               konsole_prepty_role_name(last_wait.primary_role) : "missing",
           have_last_wait ? last_wait.role_mask : 0,
           have_last_wait ? last_wait.comm : "");
}

void kde_konsole_child_launch_trace_clone_done(struct thread *parent,
                                               struct thread *child,
                                               uint64 flags)
{
    struct konsole_child_launch_record rec;
    uint64 now;
    uint64 begin_ms;
    uint64 seq;
    uint64 slot;

    if (!kde_konsole_child_launch_trace_enabled() || parent == NULL ||
        child == NULL || (flags & CLONE_THREAD) ||
        !konsole_child_launch_parent_match(parent))
        return;

    now = sched_timer_now_ms();
    memset(&rec, 0, sizeof(rec));
    rec.used = 1;
    rec.pid = child->pid;
    rec.tgid = child->tgid;
    rec.pid_seq = child->pid_seq;
    rec.parent_pid = parent->pid;
    rec.parent_tgid = parent->tgid;
    rec.flags = flags;
    rec.clone_done_ms = now;
    safestrcpy(rec.parent_comm, parent->name, sizeof(rec.parent_comm));
    safestrcpy(rec.child_comm, child->name, sizeof(rec.child_comm));
    safestrcpy(rec.parent_exec, thread_exec_path(parent),
               sizeof(rec.parent_exec));
    safestrcpy(rec.child_exec, thread_exec_path(child), sizeof(rec.child_exec));

    spin_lock(&konsole_child_launch_lock);
    begin_ms = konsole_child_launch_last_begin_ms;
    rec.clone_begin_ms = begin_ms;
    slot = konsole_child_launch_next_slot++ % KONSOLE_CHILD_LAUNCH_RECORD_CAP;
    konsole_child_launch_records[slot] = rec;
    spin_unlock(&konsole_child_launch_lock);

    seq = konsole_child_launch_next_seq();
    printf("konsole-child-launch: seq=%lu ms=%lu phase=clone_done "
           "parent_pid=%d parent_tgid=%d parent_comm=%s parent_exec='%s' "
           "child_pid=%d child_tgid=%d child_seq=%lu child_comm=%s "
           "child_exec='%s' flags=0x%lx clone_begin_ms=%lu "
           "clone_done_ms=%lu clone_elapsed_ms=%lu thread=0 vfork=%d vm=%d\n",
           seq, now, parent->pid, parent->tgid, parent->name,
           rec.parent_exec, child->pid, child->tgid, child->pid_seq,
           child->name, rec.child_exec, flags, begin_ms, now,
           now >= begin_ms ? now - begin_ms : 0,
           !!(flags & CLONE_VFORK), !!(flags & CLONE_VM));
}

void kde_konsole_child_launch_trace_child_woken(struct thread *child)
{
    struct konsole_child_launch_record rec;
    uint64 now;
    uint64 seq;
    int found = 0;

    if (!kde_konsole_child_launch_trace_enabled() || child == NULL)
        return;

    now = sched_timer_now_ms();
    spin_lock(&konsole_child_launch_lock);
    struct konsole_child_launch_record *slot =
        konsole_child_launch_find_locked(child);
    if (slot != NULL) {
        if (slot->child_woken_ms == 0)
            slot->child_woken_ms = now != 0 ? now : 1;
        rec = *slot;
        found = 1;
    }
    spin_unlock(&konsole_child_launch_lock);
    if (!found)
        return;

    seq = konsole_child_launch_next_seq();
    printf("konsole-child-launch: seq=%lu ms=%lu phase=child_woken "
           "child_pid=%d child_tgid=%d child_seq=%lu child_comm=%s "
           "clone_begin_ms=%lu clone_done_ms=%lu child_woken_ms=%lu "
           "clone_done_to_woken_ms=%lu\n",
           seq, now, rec.pid, rec.tgid, rec.pid_seq, child->name,
           rec.clone_begin_ms, rec.clone_done_ms, rec.child_woken_ms,
           rec.child_woken_ms >= rec.clone_done_ms ?
               rec.child_woken_ms - rec.clone_done_ms : 0);
}

void kde_konsole_child_launch_trace_child_run(struct thread *child)
{
    struct konsole_child_launch_record rec;
    uint64 now;
    uint64 seq;
    int found = 0;

    if (!kde_konsole_child_launch_trace_enabled() || child == NULL)
        return;

    now = sched_timer_now_ms();
    spin_lock(&konsole_child_launch_lock);
    struct konsole_child_launch_record *slot =
        konsole_child_launch_find_locked(child);
    if (slot != NULL && slot->child_run_ms == 0) {
        slot->child_run_ms = now != 0 ? now : 1;
        child->signal.kde_child_trace_active = 1;
        rec = *slot;
        found = 1;
    }
    spin_unlock(&konsole_child_launch_lock);
    if (!found)
        return;

    seq = konsole_child_launch_next_seq();
    printf("konsole-child-launch: seq=%lu ms=%lu phase=child_run "
           "child_pid=%d child_tgid=%d child_seq=%lu child_comm=%s "
           "clone_begin_ms=%lu clone_done_ms=%lu child_woken_ms=%lu "
           "child_run_ms=%lu clone_done_to_run_ms=%lu "
           "child_woken_to_run_ms=%lu\n",
           seq, now, rec.pid, rec.tgid, rec.pid_seq, child->name,
           rec.clone_begin_ms, rec.clone_done_ms, rec.child_woken_ms,
           rec.child_run_ms,
           rec.child_run_ms >= rec.clone_done_ms ?
               rec.child_run_ms - rec.clone_done_ms : 0,
           rec.child_woken_ms != 0 && rec.child_run_ms >= rec.child_woken_ms ?
               rec.child_run_ms - rec.child_woken_ms : 0);
}

void kde_konsole_child_launch_trace_exec_begin(const char *path)
{
    struct konsole_child_launch_record rec;
    struct thread *p = current;
    uint64 now;
    uint64 seq;
    int found = 0;

    if (!kde_konsole_child_launch_trace_enabled() || p == NULL ||
        !konsole_child_launch_wrapper_match(path))
        return;

    now = sched_timer_now_ms();
    spin_lock(&konsole_child_launch_lock);
    struct konsole_child_launch_record *slot =
        konsole_child_launch_find_locked(p);
    if (slot != NULL) {
        if (slot->exec_begin_ms == 0)
            slot->exec_begin_ms = now != 0 ? now : 1;
        p->signal.kde_child_trace_active = 0;
        rec = *slot;
        found = 1;
    }
    spin_unlock(&konsole_child_launch_lock);

    if (found)
        konsole_child_launch_print_setup_summary(&rec, now);

    seq = konsole_child_launch_next_seq();
    printf("konsole-child-launch: seq=%lu ms=%lu phase=exec_begin "
           "pid=%d tgid=%d child_seq=%lu comm=%s path='%s' "
           "matched_clone=%d clone_begin_ms=%lu clone_done_ms=%lu "
           "child_run_ms=%lu exec_begin_ms=%lu child_run_to_exec_begin_ms=%lu "
           "clone_done_to_exec_begin_ms=%lu\n",
           seq, now, p->pid, p->tgid, p->pid_seq, p->name,
           path != NULL ? path : "", found, found ? rec.clone_begin_ms : 0,
           found ? rec.clone_done_ms : 0, found ? rec.child_run_ms : 0,
           now,
           found && rec.child_run_ms != 0 && now >= rec.child_run_ms ?
               now - rec.child_run_ms : 0,
           found && rec.clone_done_ms != 0 && now >= rec.clone_done_ms ?
               now - rec.clone_done_ms : 0);
}

void kde_konsole_child_launch_trace_exec_done(const char *path, int argc,
                                              int has_interp, int ret,
                                              uint64 elapsed_ms)
{
    struct konsole_child_launch_record rec;
    struct thread *p = current;
    uint64 now;
    uint64 seq;
    int found = 0;

    if (!kde_konsole_child_launch_trace_enabled() || p == NULL ||
        !konsole_child_launch_wrapper_match(path))
        return;

    now = sched_timer_now_ms();
    spin_lock(&konsole_child_launch_lock);
    struct konsole_child_launch_record *slot =
        konsole_child_launch_find_locked(p);
    if (slot != NULL) {
        slot->exec_done_ms = now != 0 ? now : 1;
        safestrcpy(slot->child_exec, path, sizeof(slot->child_exec));
        rec = *slot;
        found = 1;
    }
    spin_unlock(&konsole_child_launch_lock);

    seq = konsole_child_launch_next_seq();
    printf("konsole-child-launch: seq=%lu ms=%lu phase=exec_done "
           "pid=%d tgid=%d child_seq=%lu comm=%s path='%s' "
           "matched_clone=%d clone_begin_ms=%lu clone_done_ms=%lu "
           "child_run_ms=%lu exec_begin_ms=%lu exec_done_ms=%lu "
           "exec_elapsed_ms=%lu argc=%d has_interp=%d ret=%d "
           "child_run_to_exec_begin_ms=%lu exec_begin_to_done_ms=%lu "
           "clone_done_to_exec_done_ms=%lu\n",
           seq, now, p->pid, p->tgid, p->pid_seq, p->name,
           path != NULL ? path : "", found, found ? rec.clone_begin_ms : 0,
           found ? rec.clone_done_ms : 0, found ? rec.child_run_ms : 0,
           found ? rec.exec_begin_ms : 0, now, elapsed_ms, argc, has_interp,
           ret,
           found && rec.child_run_ms != 0 && rec.exec_begin_ms >= rec.child_run_ms ?
               rec.exec_begin_ms - rec.child_run_ms : 0,
           found && rec.exec_begin_ms != 0 && now >= rec.exec_begin_ms ?
               now - rec.exec_begin_ms : 0,
           found && rec.clone_done_ms != 0 && now >= rec.clone_done_ms ?
               now - rec.clone_done_ms : 0);
}

static spinlock_t konsole_prepty_ring_lock =
    SPINLOCK_INITIALIZED("konsole_prepty_ring");
static struct konsole_prepty_wake_record
    konsole_prepty_ring[KONSOLE_PREPTY_WAKE_RING_CAP];
static uint64 konsole_prepty_ring_count;
static uint64 konsole_prepty_ring_start;
static uint64 konsole_prepty_ring_next_seq;
static uint64 konsole_prepty_ring_total_seen;
static uint64 konsole_prepty_ring_overwritten;
static uint64 konsole_prepty_ring_dropped;
static int konsole_prepty_ring_armed;
static int konsole_prepty_ring_disarm_reason;

static int konsole_prepty_ring_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[16];

    if (!initialized) {
        enabled =
            (cmdline_get_param("konsole_prepty_wake_source_ring", value,
                               sizeof(value)) == 0 &&
             value[0] != '0' && value[0] != 'n' && value[0] != 'N') ||
            (cmdline_get_param("konsole_prepty_wake_source_trace", value,
                               sizeof(value)) == 0 &&
             value[0] != '0' && value[0] != 'n' && value[0] != 'N') ||
            (cmdline_get_param("konsole_prepty_role_trace", value,
                               sizeof(value)) == 0 &&
             value[0] != '0' && value[0] != 'n' && value[0] != 'N');
        initialized = 1;
    }
    return enabled;
}

static int konsole_prepty_role_trace_enabled(void)
{
    static int initialized;
    static int enabled;
    char value[16];

    if (!initialized) {
        enabled = cmdline_get_param("konsole_prepty_role_trace", value,
                                    sizeof(value)) == 0 &&
                  value[0] != '0' && value[0] != 'n' && value[0] != 'N';
        initialized = 1;
    }
    return enabled;
}

static uint64 konsole_prepty_ring_limit(void)
{
    static int initialized;
    static uint64 limit;
    char value[32];

    if (!initialized) {
        limit = KONSOLE_PREPTY_WAKE_RING_CAP;
        if (cmdline_get_param("konsole_prepty_wake_source_trace_limit",
                              value, sizeof(value)) == 0 &&
            value[0] != '\0') {
            uint64 parsed = strtoul(value, NULL, 10);
            if (parsed > 0)
                limit = parsed;
        }
        if (cmdline_get_param("konsole_prepty_role_trace_limit",
                              value, sizeof(value)) == 0 &&
            value[0] != '\0') {
            uint64 parsed = strtoul(value, NULL, 10);
            if (parsed > 0)
                limit = parsed;
        }
        if (limit > KONSOLE_PREPTY_WAKE_RING_CAP)
            limit = KONSOLE_PREPTY_WAKE_RING_CAP;
        initialized = 1;
    }
    return limit;
}

void kde_konsole_prepty_ring_arm(const char *path)
{
    if (!konsole_prepty_ring_enabled() || !kde_ready_trace_path_match(path))
        return;

    spin_lock(&konsole_prepty_ring_lock);
    memset(konsole_prepty_ring, 0, sizeof(konsole_prepty_ring));
    konsole_prepty_ring_count = 0;
    konsole_prepty_ring_start = 0;
    konsole_prepty_ring_next_seq = 1;
    konsole_prepty_ring_total_seen = 0;
    konsole_prepty_ring_overwritten = 0;
    konsole_prepty_ring_dropped = 0;
    konsole_prepty_ring_armed = 1;
    konsole_prepty_ring_disarm_reason =
        KDE_KONSOLE_PREPTY_RING_DISARM_NONE;
    spin_unlock(&konsole_prepty_ring_lock);
}

void kde_konsole_prepty_ring_disarm(int reason)
{
    if (!konsole_prepty_ring_enabled())
        return;

    spin_lock(&konsole_prepty_ring_lock);
    if (konsole_prepty_ring_armed) {
        konsole_prepty_ring_armed = 0;
        konsole_prepty_ring_disarm_reason = reason;
    }
    spin_unlock(&konsole_prepty_ring_lock);
}

int kde_konsole_prepty_ring_active(void)
{
    if (!konsole_prepty_ring_enabled())
        return 0;
    return __atomic_load_n(&konsole_prepty_ring_armed,
                           __ATOMIC_RELAXED) != 0;
}

static void konsole_prepty_ring_append(
    struct konsole_prepty_wake_record *rec)
{
    uint64 limit;
    uint64 idx;

    if (!konsole_prepty_ring_enabled())
        return;

    limit = konsole_prepty_ring_limit();
    spin_lock(&konsole_prepty_ring_lock);
    if (!konsole_prepty_ring_armed) {
        spin_unlock(&konsole_prepty_ring_lock);
        return;
    }
    rec->seq = konsole_prepty_ring_next_seq++;
    konsole_prepty_ring_total_seen++;
    if (konsole_prepty_ring_count < limit &&
        konsole_prepty_ring_count < KONSOLE_PREPTY_WAKE_RING_CAP) {
        idx = (konsole_prepty_ring_start + konsole_prepty_ring_count) %
              KONSOLE_PREPTY_WAKE_RING_CAP;
        konsole_prepty_ring_count++;
    } else {
        idx = konsole_prepty_ring_start;
        konsole_prepty_ring_start =
            (konsole_prepty_ring_start + 1) % KONSOLE_PREPTY_WAKE_RING_CAP;
        konsole_prepty_ring_overwritten++;
    }
    konsole_prepty_ring[idx] = *rec;
    spin_unlock(&konsole_prepty_ring_lock);
}

static void konsole_prepty_record_base(
    struct konsole_prepty_wake_record *rec, int event)
{
    memset(rec, 0, sizeof(*rec));
    rec->event = event;
    rec->role = KONSOLE_PREPTY_WAKE_ROLE_OTHER;
    rec->role_mask =
        KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_OTHER);
    rec->notify_ms = sched_timer_now_ms();
    rec->pid = current != NULL ? current->pid : -1;
    rec->tgid = current != NULL ? current->tgid : -1;
    rec->fd = -1;
    if (current != NULL)
        safestrcpy(rec->comm, current->name, sizeof(rec->comm));
}

static void konsole_prepty_readlink_target(struct vfs_file *file,
                                           char *buf, size_t buflen)
{
    ssize_t n;

    if (buflen == 0)
        return;
    buf[0] = '\0';
    if (file == NULL || file->ops == NULL || file->ops->readlink == NULL)
        return;
    n = file->ops->readlink(file, buf, buflen);
    if (n < 0) {
        buf[0] = '\0';
        return;
    }
    if ((size_t)n >= buflen)
        n = (ssize_t)buflen - 1;
    buf[n] = '\0';
}

static int konsole_prepty_current_qdbus_thread(void)
{
    return current != NULL && strstr(current->name, "QDBus") != NULL;
}

static int konsole_prepty_path_contains(const char *path, const char *needle)
{
    return path != NULL && path[0] != '\0' && strstr(path, needle) != NULL;
}

static int konsole_prepty_role_from_unix_paths(const char *path,
                                               const char *peer_path)
{
    if (konsole_prepty_path_contains(path, "wayland-") ||
        konsole_prepty_path_contains(peer_path, "wayland-"))
        return KONSOLE_PREPTY_WAKE_ROLE_WAYLAND;
    if (konsole_prepty_current_qdbus_thread() ||
        konsole_prepty_path_contains(path, "/bus") ||
        konsole_prepty_path_contains(peer_path, "/bus") ||
        konsole_prepty_path_contains(path, "dbus") ||
        konsole_prepty_path_contains(peer_path, "dbus"))
        return KONSOLE_PREPTY_WAKE_ROLE_QDBUS;
    return KONSOLE_PREPTY_WAKE_ROLE_UNIX_OTHER;
}

static void konsole_prepty_set_role(struct konsole_prepty_wake_record *rec,
                                    int role)
{
    rec->role = role;
    rec->role_mask = KONSOLE_PREPTY_WAKE_ROLE_BIT(role);
}

static void konsole_prepty_trace_pc(uint64 pc, char *sym_buf,
                                    size_t sym_len, int32 *off,
                                    char *file_buf, size_t file_len,
                                    uint32 *line)
{
    ksymbols_t *sym;
    int offset;

    if (pc == 0)
        return;
    if (sym_buf != NULL && sym_len != 0 &&
        ksym_lookup(pc, sym_buf, sym_len, NULL) < 0)
        sym_buf[0] = '\0';
    sym = ksym_search(pc);
    if (sym != NULL) {
        if (file_buf != NULL && file_len != 0)
            ksym_get_location(sym, file_buf, file_len, line);
        offset = ksym_get_offset(sym, pc);
        if (off != NULL)
            *off = offset;
    }
}

static void konsole_prepty_trace_origin(
    struct konsole_prepty_wake_record *rec, void *origin)
{
    if (origin == NULL)
        return;
    rec->origin = (uint64)origin;
    konsole_prepty_trace_pc((uint64)origin, rec->origin_sym,
                            sizeof(rec->origin_sym), &rec->origin_off,
                            rec->origin_file, sizeof(rec->origin_file),
                            &rec->origin_line);
}

static void konsole_prepty_trace_eventfd_caller(
    struct konsole_prepty_wake_record *rec, void *caller)
{
    if (caller == NULL)
        return;
    rec->eventfd_caller = (uint64)caller;
    konsole_prepty_trace_pc((uint64)caller, rec->eventfd_caller_sym,
                            sizeof(rec->eventfd_caller_sym),
                            &rec->eventfd_caller_off,
                            rec->eventfd_caller_file,
                            sizeof(rec->eventfd_caller_file),
                            &rec->eventfd_caller_line);
}

static int konsole_prepty_current_fd_for_file(struct vfs_file *file)
{
    if (current == NULL || current->fdtable == NULL || file == NULL)
        return -1;
    return vfs_fdtable_find_file(current->fdtable, file);
}

static void konsole_prepty_classify_file(
    struct konsole_prepty_wake_record *rec, struct vfs_file *file)
{
    char target[KONSOLE_PREPTY_WAKE_TARGET_LEN];
    uint64 eventfd_count;
    uint64 eventfd_id;
    void *eventfd_object = NULL;

    rec->file = (uint64)file;
    rec->fd = konsole_prepty_current_fd_for_file(file);
    if (file == NULL) {
        safestrcpy(rec->kind, "none", sizeof(rec->kind));
        return;
    }
    rec->fd_visible_refs =
        __atomic_load_n(&file->visible_fd_refs, __ATOMIC_RELAXED);
    rec->file_ref_count =
        __atomic_load_n(&file->ref_count, __ATOMIC_RELAXED);
    rec->file_ops = (uint64)file->ops;
    rec->file_poll =
        (uint64)((file->ops != NULL) ? file->ops->poll : NULL);
    if (file->opened_path != NULL)
        safestrcpy(rec->path, file->opened_path, sizeof(rec->path));

    if (eventfd_file_info(file, &eventfd_count, &eventfd_id,
                          &eventfd_object) == 0) {
        rec->eventfd_count = eventfd_count;
        rec->eventfd_id = eventfd_id;
        rec->eventfd_object = (uint64)eventfd_object;
        rec->eventfd_readable = eventfd_count > 0;
        rec->eventfd_writable = eventfd_count < ((uint64)~0ULL - 1);
    }

    if (file->ops == &unix_socket_file_ops) {
        struct unix_sock *sk = (struct unix_sock *)file->private_data;
        struct unix_sock *peer = NULL;

        safestrcpy(rec->kind, "unix", sizeof(rec->kind));
        if (sk != NULL) {
            spin_lock(&sk->lock);
            rec->unix_ino = sk->proc_ino;
            rec->unix_type = sk->type;
            rec->unix_state = sk->state;
            rec->unix_shutdown = sk->shutdown_flags;
            if (sk->bound && sk->bind_len > 0)
                safestrcpy(rec->path, sk->bind_path, sizeof(rec->path));
            peer = sk->peer;
            if (peer != NULL)
                unix_sock_get_ref(peer);
            spin_unlock(&sk->lock);

            if (peer != NULL) {
                spin_lock(&peer->lock);
                rec->unix_peer_ino = peer->proc_ino;
                if (peer->bound && peer->bind_len > 0)
                    safestrcpy(rec->peer_path, peer->bind_path,
                               sizeof(rec->peer_path));
                spin_unlock(&peer->lock);
                unix_sock_put_ref(peer);
            }
        }
        konsole_prepty_set_role(
            rec, konsole_prepty_role_from_unix_paths(rec->path,
                                                     rec->peer_path));
        return;
    }

    if (file->f_kind == VFS_FILE_KIND_PIPE) {
        safestrcpy(rec->kind, "pipe", sizeof(rec->kind));
        safestrcpy(rec->target, "pipe", sizeof(rec->target));
        konsole_prepty_set_role(rec, KONSOLE_PREPTY_WAKE_ROLE_PIPE);
        return;
    }

    if (eventfd_file_is_eventfd(file)) {
        safestrcpy(rec->kind, "eventfd", sizeof(rec->kind));
        konsole_prepty_readlink_target(file, rec->target,
                                       sizeof(rec->target));
        konsole_prepty_set_role(rec, KONSOLE_PREPTY_WAKE_ROLE_EVENTFD);
        return;
    }

    target[0] = '\0';
    konsole_prepty_readlink_target(file, target, sizeof(target));
    if (target[0] != '\0')
        safestrcpy(rec->target, target, sizeof(rec->target));
    if (strstr(target, "kqueue") != NULL) {
        safestrcpy(rec->kind, "kqueue", sizeof(rec->kind));
        konsole_prepty_set_role(rec, KONSOLE_PREPTY_WAKE_ROLE_KQUEUE);
    } else if (strstr(target, "eventfd") != NULL) {
        safestrcpy(rec->kind, "eventfd", sizeof(rec->kind));
        konsole_prepty_set_role(rec, KONSOLE_PREPTY_WAKE_ROLE_EVENTFD);
    } else {
        safestrcpy(rec->kind, target[0] != '\0' ? target : "file",
                   sizeof(rec->kind));
    }
}

static int konsole_prepty_record_fd_match(int fd, struct vfs_file *file)
{
    if (!kde_konsole_prepty_ring_active())
        return 0;
    if (konsole_prepty_role_trace_enabled() &&
        !konsole_child_launch_parent_match(current))
        return 0;
    if (fd == 4)
        return 1;
    if (eventfd_file_is_eventfd(file))
        return 1;
    if (file != NULL && konsole_prepty_current_fd_for_file(file) == 4)
        return 1;
    return 0;
}

static int konsole_prepty_record_poll_fd_match(int fd, struct vfs_file *file)
{
    if (!kde_konsole_prepty_ring_active())
        return 0;
    if (konsole_prepty_role_trace_enabled() &&
        konsole_child_launch_parent_match(current))
        return 1;
    return konsole_prepty_record_fd_match(fd, file);
}

void kde_konsole_prepty_ring_record_notify(
    struct vfs_file *file, int filter, int64 data, int matched,
    int enqueued_new, int already_queued, int propagated,
    uint64 first_ident, uint64 last_ident, uint64 first_udata,
    uint64 last_udata, void *first_kq, void *last_kq, void *origin)
{
    kde_konsole_prepty_ring_record_notify_eventfd(
        file, filter, data, matched, enqueued_new, already_queued,
        propagated, first_ident, last_ident, first_udata, last_udata,
        first_kq, last_kq, origin, KONSOLE_PREPTY_EVENTFD_OP_NONE,
        0, 0, 0, 0, 0, NULL);
}

void kde_konsole_prepty_ring_record_notify_eventfd(
    struct vfs_file *file, int filter, int64 data, int matched,
    int enqueued_new, int already_queued, int propagated,
    uint64 first_ident, uint64 last_ident, uint64 first_udata,
    uint64 last_udata, void *first_kq, void *last_kq, void *origin,
    int eventfd_op, uint64 counter_before, uint64 counter_after,
    uint64 value, uint64 read_value, int ret, void *eventfd_caller)
{
    struct konsole_prepty_wake_record rec;

    if (!kde_konsole_prepty_ring_active())
        return;

    konsole_prepty_record_base(&rec, KONSOLE_PREPTY_WAKE_EVENT_NOTIFY);
    rec.filter = filter;
    rec.data = (int32)data;
    rec.matched = matched;
    rec.enqueued_new = enqueued_new;
    rec.already_queued = already_queued;
    rec.propagated = propagated;
    rec.first_ident = first_ident;
    rec.last_ident = last_ident;
    rec.first_udata = first_udata;
    rec.last_udata = last_udata;
    rec.first_kq = (uint64)first_kq;
    rec.last_kq = (uint64)last_kq;
    rec.first_kq_waiters = first_kq != NULL ?
        ((struct kqueue *)first_kq)->waiters : 0;
    rec.last_kq_waiters = last_kq != NULL ?
        ((struct kqueue *)last_kq)->waiters : 0;
    rec.eventfd_op = eventfd_op;
    rec.eventfd_counter_before = counter_before;
    rec.eventfd_counter_after = counter_after;
    rec.eventfd_value = value;
    rec.eventfd_read_value = read_value;
    rec.eventfd_ret = ret;
    konsole_prepty_classify_file(&rec, file);
    konsole_prepty_trace_origin(&rec, origin);
    konsole_prepty_trace_eventfd_caller(&rec, eventfd_caller);
    konsole_prepty_ring_append(&rec);
}

void kde_konsole_prepty_ring_record_pipe_write(
    void *pipe, struct vfs_file *write_file, struct vfs_file *read_file,
    ssize_t bytes, size_t count, uint32 nread, uint32 nwrite)
{
    struct konsole_prepty_wake_record rec;
    uint32 readable_after;

    if (!kde_konsole_prepty_ring_active())
        return;

    konsole_prepty_record_base(&rec, KONSOLE_PREPTY_WAKE_EVENT_PIPE_WRITE);
    rec.file = (uint64)pipe;
    rec.first_kq = (uint64)write_file;
    rec.last_kq = (uint64)read_file;
    rec.bytes = (int32)bytes;
    rec.data = (int32)count;
    rec.nread = nread;
    rec.nwrite = nwrite;
    readable_after = nwrite - nread;
    rec.readable_after = readable_after;
    rec.readable_before =
        readable_after > (uint32)bytes ? readable_after - (uint32)bytes : 0;
    konsole_prepty_classify_file(&rec, read_file != NULL ? read_file :
                                 write_file);
    safestrcpy(rec.kind, "pipe-write", sizeof(rec.kind));
    konsole_prepty_set_role(&rec, KONSOLE_PREPTY_WAKE_ROLE_PIPE);
    konsole_prepty_ring_append(&rec);
}

void kde_konsole_prepty_ring_record_poll_wait(
    uint64 wait_start_ms, uint64 waiter_run_ms, int nfds, int timeout_ms,
    int has_unnotified_fds, int ready, int nevents, uint32 role_mask,
    int primary_role, int poll_op)
{
    struct konsole_prepty_wake_record rec;

    if (!kde_konsole_prepty_ring_active())
        return;

    konsole_prepty_record_base(&rec, KONSOLE_PREPTY_WAKE_EVENT_POLL_WAIT);
    rec.notify_ms = waiter_run_ms;
    rec.wait_start_ms = wait_start_ms;
    rec.waiter_run_ms = waiter_run_ms;
    rec.nfds = nfds;
    rec.timeout_ms = timeout_ms;
    rec.has_unnotified_fds = has_unnotified_fds;
    rec.ready = ready;
    rec.nevents = nevents;
    rec.poll_op = poll_op;
    rec.role = primary_role;
    rec.role_mask = role_mask != 0 ? role_mask :
        KONSOLE_PREPTY_WAKE_ROLE_BIT(KONSOLE_PREPTY_WAKE_ROLE_OTHER);
    safestrcpy(rec.kind, "poll-wait", sizeof(rec.kind));
    konsole_prepty_ring_append(&rec);
}

void kde_konsole_prepty_ring_record_fd_lifecycle(
    int op, int oldfd, int newfd, struct vfs_file *file,
    struct vfs_file *replaced_file, int ret, int first_visible)
{
    struct konsole_prepty_wake_record rec;

    if (!konsole_prepty_record_fd_match(newfd, file) &&
        !konsole_prepty_record_fd_match(oldfd, file) &&
        !konsole_prepty_record_fd_match(newfd, replaced_file))
        return;

    konsole_prepty_record_base(&rec,
                               KONSOLE_PREPTY_WAKE_EVENT_FD_LIFECYCLE);
    rec.fd_lifecycle_op = op;
    rec.oldfd = oldfd;
    rec.newfd = newfd;
    rec.fd = newfd >= 0 ? newfd : oldfd;
    rec.replaced_file = (uint64)replaced_file;
    rec.eventfd_ret = ret;
    rec.first_visible = first_visible;
    rec.fdtable = current != NULL ? (uint64)current->fdtable : 0;
    konsole_prepty_classify_file(&rec, file != NULL ? file : replaced_file);
    rec.fd = newfd >= 0 ? newfd : oldfd;
    konsole_prepty_ring_append(&rec);
}

void kde_konsole_prepty_ring_record_eventfd_op(
    struct vfs_file *file, int op, uint64 counter_before,
    uint64 counter_after, uint64 value, uint64 read_value, int ret,
    void *caller)
{
    struct konsole_prepty_wake_record rec;

    if (!konsole_prepty_record_fd_match(-1, file))
        return;

    konsole_prepty_record_base(&rec, KONSOLE_PREPTY_WAKE_EVENT_EVENTFD_OP);
    rec.eventfd_op = op;
    rec.eventfd_counter_before = counter_before;
    rec.eventfd_counter_after = counter_after;
    rec.eventfd_value = value;
    rec.eventfd_read_value = read_value;
    rec.eventfd_ret = ret;
    konsole_prepty_classify_file(&rec, file);
    konsole_prepty_trace_eventfd_caller(&rec, caller);
    konsole_prepty_ring_append(&rec);
}

void kde_konsole_prepty_ring_record_poll_fd(
    int fd, struct vfs_file *file, short events, short revents, int nfds,
    int scan_ready, int poll_op)
{
    struct konsole_prepty_wake_record rec;
    uint64 count = 0;
    uint64 eventfd_id = 0;
    void *eventfd_object = NULL;

    if (!konsole_prepty_record_poll_fd_match(fd, file))
        return;

    konsole_prepty_record_base(&rec, KONSOLE_PREPTY_WAKE_EVENT_POLL_FD);
    rec.fd = fd;
    rec.nfds = nfds;
    rec.ready = scan_ready;
    rec.poll_events = events;
    rec.poll_revents = revents;
    rec.poll_op = poll_op;
    konsole_prepty_classify_file(&rec, file);
    rec.fd = fd;
    if (eventfd_file_info(file, &count, &eventfd_id,
                          &eventfd_object) == 0) {
        int requested_read = (events & (POLLIN | POLLRDNORM |
                              POLLRDBAND | POLLRDHUP)) != 0;
        int returned_read = (revents & (POLLIN | POLLRDNORM |
                              POLLRDBAND | POLLRDHUP)) != 0;

        rec.poll_file_count = count;
        rec.eventfd_count = count;
        rec.eventfd_id = eventfd_id;
        rec.eventfd_object = (uint64)eventfd_object;
        rec.eventfd_readable = count > 0;
        rec.eventfd_writable = count < ((uint64)~0ULL - 1);
        rec.poll_readable_mismatch =
            requested_read && count > 0 && !returned_read;
    }
    safestrcpy(rec.kind, "poll-fd", sizeof(rec.kind));
    konsole_prepty_ring_append(&rec);
}

static uint64 konsole_prepty_record_start_ms(
    const struct konsole_prepty_wake_record *rec)
{
    if (rec->wait_start_ms != 0)
        return rec->wait_start_ms;
    return rec->notify_ms;
}

static uint64 konsole_prepty_record_end_ms(
    const struct konsole_prepty_wake_record *rec)
{
    uint64 end = rec->notify_ms;

    if (rec->waiter_run_ms > end)
        end = rec->waiter_run_ms;
    return end;
}

int kde_konsole_prepty_ring_snapshot(
    struct konsole_prepty_wake_snapshot *snap, uint64 size)
{
    uint64 limit;
    uint64 first_ms = 0;
    uint64 last_ms = 0;

    if (snap == NULL || size < sizeof(*snap))
        return -EINVAL;

    limit = konsole_prepty_ring_limit();
    memset(snap, 0, sizeof(*snap));
    spin_lock(&konsole_prepty_ring_lock);
    snap->abi_version = KONSOLE_PREPTY_WAKE_RING_ABI_VERSION;
    snap->record_size = sizeof(struct konsole_prepty_wake_record);
    snap->capacity = KONSOLE_PREPTY_WAKE_RING_CAP;
    snap->count = konsole_prepty_ring_count;
    snap->next_seq = konsole_prepty_ring_next_seq;
    snap->dropped = konsole_prepty_ring_dropped;
    snap->armed = konsole_prepty_ring_armed;
    snap->disarm_reason = konsole_prepty_ring_disarm_reason;
    snap->total_seen = konsole_prepty_ring_total_seen;
    snap->stored = konsole_prepty_ring_count;
    snap->overwritten = konsole_prepty_ring_overwritten;
    snap->configured_limit = limit;
    for (uint64 i = 0; i < konsole_prepty_ring_count; i++) {
        const struct konsole_prepty_wake_record *rec;
        uint64 rec_start;
        uint64 rec_end;

        rec = &konsole_prepty_ring[
            (konsole_prepty_ring_start + i) % KONSOLE_PREPTY_WAKE_RING_CAP];
        snap->records[i] = *rec;
        if (i == 0)
            snap->first_seq = rec->seq;
        snap->last_seq = rec->seq;
        rec_start = konsole_prepty_record_start_ms(rec);
        rec_end = konsole_prepty_record_end_ms(rec);
        if (rec_start != 0 && (first_ms == 0 || rec_start < first_ms))
            first_ms = rec_start;
        if (rec_end > last_ms)
            last_ms = rec_end;
    }
    snap->first_ms = first_ms;
    snap->last_ms = last_ms;
    snap->span_ms = last_ms >= first_ms ? last_ms - first_ms : 0;
    spin_unlock(&konsole_prepty_ring_lock);
    return 0;
}
