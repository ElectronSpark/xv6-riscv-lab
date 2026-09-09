#include "types.h"
#include "cmdline.h"
#include "errno.h"
#include "param.h"
#include "printf.h"
#include "proc/sched.h"
#include "proc/thread.h"
#include "proc/tq.h"
#include "proc/workqueue.h"

#define WQ_TEST_TIMEOUT_MS 2000ULL
#define WQ_TEST_MAX_POLLS 1000000U
#define WQ_TEST_BURST 4
#define WQ_TEST_ROUNDS 4

/* Opt-in only. There is no workqueue destroy API: the private queue remains
 * idle after the test. All callback state/items have static lifetime, including
 * when a failed bounded drain leaves a callback pending. Never reuse an item
 * until both workers are idle and no work is queued or executing. */
static struct {
    spinlock_t lock;
    tq_t parked;
    struct workqueue *wq;
    struct work_struct blocker;
    struct work_struct probes[WQ_TEST_BURST];
    bool release;
    bool entered;
    bool exited;
    int error;
    int completed;
    int completed_before_release;
    unsigned int completed_mask;
} wq_test;

struct wq_test_snapshot {
    int workers;
    int idle;
    int running;
    int pending;
};

static struct wq_test_snapshot wq_test_snapshot(void)
{
    struct workqueue *wq = wq_test.wq;
    struct wq_test_snapshot out;

    spin_lock(&wq->lock);
    out.workers = wq->nr_workers;
    out.idle = tq_size(&wq->idle_queue);
    out.running = wq->running_works;
    out.pending = wq->pending_works;
    spin_unlock(&wq->lock);
    return out;
}

static bool wq_test_state_is(int idle, int running, int pending)
{
    struct wq_test_snapshot s = wq_test_snapshot();

    return s.workers == 2 && s.idle == idle && s.running == running &&
           s.pending == pending;
}

static bool wq_test_expired(uint64 start, unsigned int polls)
{
    /* This clock refreshes from the architecture clock, independently of
     * workqueue-backed timer callbacks. The poll cap also bounds a stuck clock.
     * Yield rather than arm a sleep on another workqueue under test. */
    return sched_timer_now_ms() - start >= WQ_TEST_TIMEOUT_MS ||
           polls >= WQ_TEST_MAX_POLLS;
}

static bool wq_test_wait_idle(void)
{
    uint64 start = sched_timer_now_ms();

    for (unsigned int polls = 0; !wq_test_expired(start, polls); polls++) {
        if (wq_test_state_is(2, 0, 0))
            return true;
        scheduler_yield();
    }
    return false;
}

static void wq_test_blocker(struct work_struct *work)
{
    (void)work;
    spin_lock(&wq_test.lock);
    wq_test.entered = true;
    while (!wq_test.release) {
        int ret = tq_wait(&wq_test.parked, &wq_test.lock, NULL);

        if (ret != 0) {
            wq_test.error = ret;
            break;
        }
    }
    wq_test.exited = true;
    spin_unlock(&wq_test.lock);
}

static void wq_test_probe(struct work_struct *work)
{
    unsigned int bit = 1U << work->data;

    spin_lock(&wq_test.lock);
    if (wq_test.completed_mask & bit)
        wq_test.error = -EINVAL;
    wq_test.completed_mask |= bit;
    wq_test.completed++;
    if (!wq_test.release && !wq_test.exited)
        wq_test.completed_before_release++;
    spin_unlock(&wq_test.lock);
}

static bool wq_test_wait_parked(void)
{
    uint64 start = sched_timer_now_ms();

    for (unsigned int polls = 0; !wq_test_expired(start, polls); polls++) {
        spin_lock(&wq_test.lock);
        bool parked = wq_test.entered && !wq_test.exited &&
                      !wq_test.release && !wq_test.error &&
                      tq_size(&wq_test.parked) == 1;
        spin_unlock(&wq_test.lock);
        /* The blocker stays parked until this controller releases it. The
         * queue has no other producer, so these independently locked reads
         * establish one executing callback and one genuinely idle worker. */
        if (parked && wq_test_state_is(1, 1, 0))
            return true;
        scheduler_yield();
    }
    return false;
}

static bool wq_test_wait_probes(int expected)
{
    uint64 start = sched_timer_now_ms();
    unsigned int mask = (1U << expected) - 1U;

    for (unsigned int polls = 0; !wq_test_expired(start, polls); polls++) {
        spin_lock(&wq_test.lock);
        bool done = wq_test.completed == expected &&
                    wq_test.completed_before_release == expected &&
                    wq_test.completed_mask == mask && !wq_test.error &&
                    !wq_test.release && !wq_test.exited &&
                    tq_size(&wq_test.parked) == 1;
        spin_unlock(&wq_test.lock);
        if (done)
            return true;
        scheduler_yield();
    }
    return false;
}

static bool wq_test_round(int round, int probes)
{
    const char *phase = "initial-idle";
    bool passed = false;

    if (!wq_test_wait_idle())
        goto release;

    spin_lock(&wq_test.lock);
    wq_test.release = false;
    wq_test.entered = false;
    wq_test.exited = false;
    wq_test.error = 0;
    wq_test.completed = 0;
    wq_test.completed_before_release = 0;
    wq_test.completed_mask = 0;
    spin_unlock(&wq_test.lock);
    init_work_struct(&wq_test.blocker, wq_test_blocker, 0);
    for (int i = 0; i < probes; i++)
        init_work_struct(&wq_test.probes[i], wq_test_probe, (uint64)i);

    phase = "enqueue-blocker";
    if (!queue_work(wq_test.wq, &wq_test.blocker))
        goto release;
    phase = "one-parked-one-idle";
    if (!wq_test_wait_parked())
        goto release;

    phase = "enqueue-probes";
    for (int i = 0; i < probes; i++) {
        if (!queue_work(wq_test.wq, &wq_test.probes[i]))
            goto release;
    }
    phase = "progress-before-release";
    passed = wq_test_wait_probes(probes);

release:;
    /* Capture the failure state before releasing A, but do not print or return
     * while it is parked. Even a blocker which has not started yet will see
     * release=true. Do not use the unbounded flush_workqueue() here. */
    struct wq_test_snapshot before = wq_test_snapshot();
    spin_lock(&wq_test.lock);
    int completed_before = wq_test.completed_before_release;
    wq_test.release = true;
    int released = tq_wakeup_all(&wq_test.parked, 0, 0);
    spin_unlock(&wq_test.lock);

    bool drained = wq_test_wait_idle();
    spin_lock(&wq_test.lock);
    bool callbacks_ok = !wq_test.error &&
                        wq_test.completed == probes && wq_test.exited;
    spin_unlock(&wq_test.lock);
    passed = passed && drained && callbacks_ok && released >= 0;
    printf("[workqueue-selftest] case=%d probes=%d result=%s phase=%s "
           "before_release=%d workers=%d idle=%d running=%d pending=%d "
           "drained=%d\n",
           round, probes, passed ? "PASS" : "FAIL", phase,
           completed_before, before.workers, before.idle, before.running,
           before.pending, drained);
    return passed;
}

static int wq_test_run(uint64 arg1, uint64 arg2)
{
    (void)arg1;
    (void)arg2;
    printf("[workqueue-selftest] BEGIN rounds=%d workers=2 deadline_ms=%lu\n",
           WQ_TEST_ROUNDS, (unsigned long)WQ_TEST_TIMEOUT_MS);
    spin_init(&wq_test.lock, "workqueue_selftest");
    tq_init(&wq_test.parked, "workqueue_test_parked", &wq_test.lock);
    wq_test.wq = workqueue_create("workqueue_selftest", 2);
    if (!wq_test.wq) {
        printf("[workqueue-selftest] END result=FAIL phase=create-queue\n");
        return -ENOMEM;
    }

    /* Repeated single-item trials hit the starvation boundary. The final
     * burst checks draining through the remaining worker. Every round also
     * requires the no-pending/both-idle state before reusing any work item. */
    for (int round = 1; round <= WQ_TEST_ROUNDS; round++) {
        int probes = round == WQ_TEST_ROUNDS ? WQ_TEST_BURST : 1;

        if (!wq_test_round(round, probes)) {
            printf("[workqueue-selftest] END result=FAIL case=%d "
                   "release_requested=1 retained_queue=1\n", round);
            return -ETIMEDOUT;
        }
    }
    printf("[workqueue-selftest] END result=PASS cases=%d retained_queue=1\n",
           WQ_TEST_ROUNDS);
    return 0;
}

void workqueue_selftest_run_if_enabled(void)
{
    static bool launched;
    char value[16];

    if (cmdline_get_param("workqueue_selftest", value, sizeof(value)) != 0 ||
        !cmdline_value_is_true(value))
        return;
    if (__atomic_exchange_n(&launched, true, __ATOMIC_ACQ_REL))
        return;

    struct thread *test = kthread_create("workqueue_test", wq_test_run, 0, 0,
                                         KERNEL_STACK_ORDER);
    if (IS_ERR_OR_NULL(test)) {
        printf("[workqueue-selftest] END result=FAIL phase=create-thread\n");
        return;
    }
    wakeup(test);
}
