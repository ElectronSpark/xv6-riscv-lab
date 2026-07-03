#include "types.h"
#include "param.h"
#include "cmdline.h"
#include "printf.h"
#include "proc/rq.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "sched_starve_probe.h"
#include "smp/percpu.h"
#include "string.h"
#include "timer/timer.h"

struct runnable_user_count {
    uint64 count;
};

struct sched_starve_probe_cpu_state {
    uint64 wake_ipi_send_ms;
    uint64 wake_ipi_send_seq;
    uint64 wake_ipi_send_from;
    uint64 wake_ipi_recv_ms;
    uint64 wake_ipi_recv_seq;
    uint64 timer_tick_ms;
    uint64 timer_tick_seq;
} __ALIGNED_CACHELINE;

static struct sched_starve_probe_cpu_state cpu_state[MAX_CPUS]
    __ALIGNED_CACHELINE;
static uint64 idle_needs_resched_samples;
static uint64 starvation_snapshots;

int sched_starve_probe_is_enabled(void) {
    static int cached = -1;
    char value[16];

    if (cached >= 0)
        return cached;

    cached = 0;
    if (cmdline_get_param("sched_starve_probe", value, sizeof(value)) == 0 &&
        cmdline_value_is_true(value)) {
        cached = 1;
    }
    return cached;
}

static void sched_starve_probe_count_thread(struct thread *p, void *arg) {
    struct runnable_user_count *count = arg;
    enum thread_state state;

    if (p == NULL || count == NULL || !THREAD_USER_SPACE(p))
        return;

    state = __thread_state_get(p);
    if (state == THREAD_RUNNING || state == THREAD_WAKENING)
        count->count++;
}

static uint64 probe_age(uint64 now_ms, uint64 then_ms) {
    if (then_ms == 0 || now_ms < then_ms)
        return 0;
    return now_ms - then_ms;
}

void sched_starve_probe_note_wake_ipi_send(int target_cpu) {
    if (!sched_starve_probe_is_enabled())
        return;
    if (target_cpu < 0 || target_cpu >= MAX_CPUS)
        return;

    struct sched_starve_probe_cpu_state *st = &cpu_state[target_cpu];
    __atomic_store_n(&st->wake_ipi_send_from, (uint64)cpuid(),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&st->wake_ipi_send_ms, get_jiffs(),
                     __ATOMIC_RELEASE);
    __atomic_fetch_add(&st->wake_ipi_send_seq, 1, __ATOMIC_RELAXED);
}

void sched_starve_probe_note_wake_ipi_recv(int cpu) {
    if (!sched_starve_probe_is_enabled())
        return;
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;

    struct sched_starve_probe_cpu_state *st = &cpu_state[cpu];
    __atomic_store_n(&st->wake_ipi_recv_ms, get_jiffs(), __ATOMIC_RELEASE);
    __atomic_fetch_add(&st->wake_ipi_recv_seq, 1, __ATOMIC_RELAXED);
}

void sched_starve_probe_note_timer_tick(int cpu) {
    if (!sched_starve_probe_is_enabled())
        return;
    if (cpu < 0 || cpu >= MAX_CPUS)
        return;

    struct sched_starve_probe_cpu_state *st = &cpu_state[cpu];
    __atomic_store_n(&st->timer_tick_ms, get_jiffs(), __ATOMIC_RELEASE);
    __atomic_fetch_add(&st->timer_tick_seq, 1, __ATOMIC_RELAXED);
}

uint64 sched_starve_probe_idle_needs_resched_samples(void) {
    return __atomic_load_n(&idle_needs_resched_samples, __ATOMIC_ACQUIRE);
}

uint64 sched_starve_probe_snapshots(void) {
    return __atomic_load_n(&starvation_snapshots, __ATOMIC_ACQUIRE);
}

void sched_starve_probe_tick(void) {
    static uint64 last_sample_ms;
    static struct rq_starve_probe_snapshot rq[MAX_CPUS];
    struct sched_timer_starve_probe_snapshot timer;
    struct runnable_user_count runnable = {0};
    uint64 now_ms;
    int halted = 0;
    int ncpu;

    if (!sched_starve_probe_is_enabled())
        return;

    now_ms = get_jiffs();
    if (last_sample_ms != 0 && now_ms - last_sample_ms < 2 * HZ)
        return;
    last_sample_ms = now_ms;

    ncpu = cpu_possible_count();
    if (ncpu > MAX_CPUS)
        ncpu = MAX_CPUS;

    proctab_for_each_rcu(sched_starve_probe_count_thread, &runnable);
    for (int cpu = 0; cpu < ncpu; cpu++) {
        rq_starve_probe_snapshot_cpu(cpu, &rq[cpu], now_ms);
        if (rq[cpu].halted)
            halted++;
    }

    if (runnable.count <= 16 || halted <= 1)
        return;

    sched_timer_starve_probe_snapshot(&timer, now_ms);
    __atomic_fetch_add(&starvation_snapshots, 1, __ATOMIC_RELAXED);

    printf("sched_starve_probe: ms=%lu runnable_user=%lu halted=%d ncpu=%d\n",
           now_ms, runnable.count, halted, ncpu);
    for (int cpu = 0; cpu < ncpu; cpu++) {
        struct sched_starve_probe_cpu_state *st = &cpu_state[cpu];
        uint64 send_ms =
            __atomic_load_n(&st->wake_ipi_send_ms, __ATOMIC_ACQUIRE);
        uint64 recv_ms =
            __atomic_load_n(&st->wake_ipi_recv_ms, __ATOMIC_ACQUIRE);
        uint64 send_seq =
            __atomic_load_n(&st->wake_ipi_send_seq, __ATOMIC_ACQUIRE);
        uint64 recv_seq =
            __atomic_load_n(&st->wake_ipi_recv_seq, __ATOMIC_ACQUIRE);
        uint64 send_from =
            __atomic_load_n(&st->wake_ipi_send_from, __ATOMIC_ACQUIRE);
        uint64 tick_ms =
            __atomic_load_n(&st->timer_tick_ms, __ATOMIC_ACQUIRE);
        uint64 tick_seq =
            __atomic_load_n(&st->timer_tick_seq, __ATOMIC_ACQUIRE);

        if (rq[cpu].halted && rq[cpu].needs_resched)
            __atomic_fetch_add(&idle_needs_resched_samples, 1,
                               __ATOMIC_RELAXED);

        printf("sched_starve_probe: cpu=%d active=%d halted=%d needs_resched=%d "
               "cur_pid=%d cur_state=%s cur_name=%s rq_queued=%lu rq_owner=%d "
               "rq_hold_ms=%lu wake_depth=%lu rq_try_fail_seq=%lu "
               "rq_try_fail_age_ms=%lu wake_ipi_send_seq=%lu "
               "wake_ipi_send_from=%lu wake_ipi_send_age_ms=%lu "
               "wake_ipi_recv_seq=%lu wake_ipi_recv_age_ms=%lu "
               "timer_tick_seq=%lu timer_tick_age_ms=%lu\n",
               cpu, rq[cpu].active, rq[cpu].halted, rq[cpu].needs_resched,
               rq[cpu].current_pid,
               thread_state_short((enum thread_state)rq[cpu].current_state),
               rq[cpu].current_name[0] ? rq[cpu].current_name : "-",
               rq[cpu].nr_queued, rq[cpu].lock_owner_cpu, rq[cpu].lock_hold_ms,
               rq[cpu].wake_list_depth, rq[cpu].try_fail_seq,
               rq[cpu].try_fail_age_ms, send_seq, send_from,
               probe_age(now_ms, send_ms), recv_seq, probe_age(now_ms, recv_ms),
               tick_seq, probe_age(now_ms, tick_ms));
    }
    printf("sched_starve_probe: sched_timer_owner=%d sched_timer_hold_ms=%lu "
           "sched_timer_current=%lu sched_timer_next=%lu\n",
           timer.lock_owner_cpu, timer.lock_hold_ms, timer.current_tick,
           timer.next_tick);
}
