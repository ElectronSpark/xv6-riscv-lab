// OOM (Out-Of-Memory) killer — select and terminate the worst process
//
// When the system is critically low on memory and reclamation has failed,
// the OOM killer selects the process with the highest "badness" score
// and sends it SIGKILL. This is a last resort — all shrinkers and
// reclaim paths must be exhausted first.
//
// Scoring is modeled after Linux's oom_badness():
//   - Base score = process RSS (live resident pages)
//   - Kernel threads and PID 1 (init) are exempt
//   - Zombie/exiting processes are skipped

#include "types.h"
#include "printf.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/sched.h"
#include "signo.h"
#include "string.h"
#include <mm/oom_kill.h>
#include <mm/mm_watermark.h>
#include "smp/atomic.h"
#include "smp/percpu.h"

// Protect against concurrent OOM kills
static spinlock_t __oom_lock = SPINLOCK_INITIALIZED("oom_lock");
static struct thread *__oom_reaper_thread = NULL;
static _Atomic int __oom_reaper_should_wake = 0;
static _Atomic uint64 __oom_kill_count = 0;
static _Atomic int __oom_in_progress = 0;
static _Atomic int __oom_victim_tgid = 0;
static _Atomic uint64 __oom_victim_tg_addr = 0;
static _Atomic uint64 __oom_victim_tg_ref = 0;
static _Atomic uint64 __oom_victim_deadline_ms = 0;
static _Atomic int __oom_victim_exited = 0;
static _Atomic int __oom_deferred_tgid = 0;

#define OOM_VICTIM_GRACE_MS 5000
#define OOM_EXIT_RECLAIM_GRACE_MS 250
#define OOM_REAPER_POLL_MS 50

static void oom_reaper_wake(void);

static uint64 oom_now_ms(void)
{
    return sched_timer_now_ms();
}

static void oom_clear_victim_state(void)
{
    struct thread_group *old_ref;

    old_ref = (struct thread_group *)
        __atomic_exchange_n(&__oom_victim_tg_ref, 0, __ATOMIC_ACQ_REL);
    __atomic_store_n(&__oom_victim_tgid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&__oom_victim_tg_addr, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&__oom_victim_deadline_ms, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&__oom_victim_exited, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&__oom_deferred_tgid, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&__oom_in_progress, 0, __ATOMIC_RELEASE);
    if (old_ref != NULL)
        thread_group_put(old_ref);
}

static int oom_expire_victim_if_needed(void)
{
    uint64 deadline = __atomic_load_n(&__oom_victim_deadline_ms,
                                      __ATOMIC_ACQUIRE);
    int tgid;
    struct thread_group *victim_ref;
    uint64 rss_pages;

    if (deadline == 0 || oom_now_ms() < deadline)
        return 0;

    tgid = __atomic_load_n(&__oom_victim_tgid, __ATOMIC_ACQUIRE);
    if (tgid <= 0)
        return 0;

    if (__atomic_load_n(&__oom_victim_exited, __ATOMIC_ACQUIRE)) {
        victim_ref = (struct thread_group *)
            __atomic_load_n(&__oom_victim_tg_ref, __ATOMIC_ACQUIRE);
        rss_pages = victim_ref != NULL ?
            __atomic_load_n(&victim_ref->acct.mm_rss_pages,
                            __ATOMIC_ACQUIRE) : 0;
        if (rss_pages != 0) {
            printf("OOM killer: victim %d exited but still owns %ld rss "
                   "pages after reclaim grace; clearing victim state\n",
                   tgid, rss_pages);
        } else {
            printf("OOM killer: victim %d exited and RSS is reclaimed; "
                   "clearing victim state\n", tgid);
        }
    } else {
        printf("OOM killer: victim %d reclaim grace expired\n", tgid);
    }
    oom_clear_victim_state();
    return 1;
}

static void oom_reaper_main(void)
{
    for (;;) {
        sleep_ms(OOM_REAPER_POLL_MS);
        __atomic_store_n(&__oom_reaper_should_wake, 0, __ATOMIC_RELEASE);
        oom_process_deferred_kill();
    }
}

static void oom_reaper_wake(void)
{
    if (__oom_reaper_thread == NULL)
        return;
    __atomic_store_n(&__oom_reaper_should_wake, 1, __ATOMIC_RELEASE);
    wakeup(__oom_reaper_thread);
}

void oom_init(void) {
    __oom_reaper_thread = kthread_create("oom_reaper", (void *)oom_reaper_main,
                                         0, 0, 0);
    if (IS_ERR_OR_NULL(__oom_reaper_thread)) {
        printf("WARNING: failed to create OOM reaper thread\n");
        __oom_reaper_thread = NULL;
    } else {
        printf("OOM reaper started (tid=%d)\n", __oom_reaper_thread->pid);
    }
    printf("OOM killer initialized\n");
}

static int oom_try_begin(void)
{
    int expected = 0;

    if (__atomic_compare_exchange_n(&__oom_in_progress, &expected, 1, 0,
                                    __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE))
        return 1;

    if (oom_expire_victim_if_needed()) {
        expected = 0;
        if (__atomic_compare_exchange_n(&__oom_in_progress, &expected, 1, 0,
                                        __ATOMIC_ACQ_REL,
                                        __ATOMIC_ACQUIRE))
            return 1;
    }

    return 0;
}

static void oom_track_victim(struct thread_group *tg)
{
    struct thread_group *old_ref;

    thread_group_get(tg);
    old_ref = (struct thread_group *)
        __atomic_exchange_n(&__oom_victim_tg_ref, (uint64)tg,
                            __ATOMIC_ACQ_REL);
    __atomic_store_n(&__oom_victim_tgid, tg->tgid, __ATOMIC_RELAXED);
    __atomic_store_n(&__oom_victim_tg_addr, (uint64)tg, __ATOMIC_RELEASE);
    __atomic_store_n(&__oom_victim_deadline_ms,
                     oom_now_ms() + OOM_VICTIM_GRACE_MS,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&__oom_victim_exited, 0, __ATOMIC_RELEASE);
    if (old_ref != NULL)
        thread_group_put(old_ref);
}

static struct thread_group *oom_get_thread_group_ref(int tgid) {
    struct thread_group *tg;

    pid_rlock();
    tg = get_thread_group(tgid);
    if (tg != NULL)
        thread_group_get(tg);
    pid_runlock();
    return tg;
}

static int oom_mark_victim(struct thread_group *victim_tg)
{
    return tg_signal_force_sigkill(victim_tg, false);
}

static int oom_wake_victim(struct thread_group *victim_tg)
{
    return tg_signal_force_sigkill(victim_tg, true);
}

static int oom_can_wake_deferred_victim(void)
{
    struct cpu_local *c = mycpu();

    return !CPU_IN_ITR() && !sched_holding() &&
           c->noff == 0 && c->spin_depth == 0;
}

// Compute badness score for a thread group.
// Higher = more likely to be killed. Returns 0 for unkillable processes.
static uint64 __oom_badness(struct thread_group *tg) {
    // Never kill kernel threads
    if (thread_group_is_kernel(tg))
        return 0;

    // Never kill init (PID 1)
    if (tg->tgid <= 1)
        return 0;

    // Skip groups already exiting
    if (thread_group_exiting(tg))
        return 0;
    if (tg->group_leader == NULL || THREAD_KILLED(tg->group_leader))
        return 0;

    // Lock-free live RSS accounting.  OOM may run from allocation-failure
    // paths that cannot safely acquire vm_rlock().
    uint64 score = __atomic_load_n(&tg->acct.mm_rss_pages, __ATOMIC_RELAXED);

    // Minimum score of 1 for any killable process (so we always have a victim)
    if (score == 0)
        score = 1;

    return score;
}

// Per-tgid callback used during OOM victim selection
struct oom_scan_ctx {
    int best_tgid;
    uint64 best_score;
};

static void __oom_scan_tgid(int tgid, void *arg) {
    struct oom_scan_ctx *ctx = (struct oom_scan_ctx *)arg;
    struct thread_group *tg = oom_get_thread_group_ref(tgid);
    if (tg == NULL)
        return;

    uint64 score = __oom_badness(tg);
    thread_group_put(tg);

    if (score > ctx->best_score) {
        ctx->best_score = score;
        ctx->best_tgid = tgid;
    }
}

int oom_kill_process(uint64 order, uint64 gfp_flags) {
    // Prevent concurrent OOM kills while a previous victim is reclaiming.
    if (!oom_try_begin())
        return OOM_ALREADY;

    spin_lock(&__oom_lock);

    printf("\n=== OOM KILLER INVOKED ===\n");
    printf("Allocation order: %ld, flags: 0x%lx\n", order, gfp_flags);
    mm_watermark_dump();

    // Scan all processes to find the worst offender
    struct oom_scan_ctx ctx = {
        .best_tgid = -1,
        .best_score = 0,
    };

    proctab_for_each_tgid(__oom_scan_tgid, &ctx);

    if (ctx.best_tgid < 0) {
        printf("OOM killer: no killable process found!\n");
        spin_unlock(&__oom_lock);
        oom_clear_victim_state();
        return OOM_NO_VICTIM;
    }

    // Kill the victim
    struct thread_group *victim_tg = oom_get_thread_group_ref(ctx.best_tgid);
    if (victim_tg == NULL) {
        printf("OOM killer: victim tgid %d vanished\n", ctx.best_tgid);
        spin_unlock(&__oom_lock);
        oom_clear_victim_state();
        return OOM_NO_VICTIM;
    }

    char victim_name[16] = "?";
    if (victim_tg->group_leader != NULL)
        safestrcpy(victim_name, victim_tg->group_leader->name,
                   sizeof(victim_name));
    printf("OOM killer: killing process %d name=%s "
           "(score=%ld rss_pages=%ld)\n",
           ctx.best_tgid, victim_name, ctx.best_score, ctx.best_score);

    oom_track_victim(victim_tg);
    __atomic_store_n(&__oom_deferred_tgid, victim_tg->tgid,
                     __ATOMIC_RELEASE);
    oom_reaper_wake();

    printf("=== OOM KILLER DONE ===\n\n");

    spin_unlock(&__oom_lock);

    if (oom_mark_victim(victim_tg) == 0) {
        __atomic_fetch_add(&__oom_kill_count, 1, __ATOMIC_RELAXED);
        printf("OOM killer: marked SIGKILL for process %d; wake deferred "
               "until safe point\n", victim_tg->tgid);
    } else {
        oom_clear_victim_state();
        thread_group_put(victim_tg);
        return OOM_NO_VICTIM;
    }

    thread_group_put(victim_tg);
    return OOM_SUCCESS;
}

int oom_process_deferred_kill(void)
{
    int tgid = __atomic_load_n(&__oom_deferred_tgid, __ATOMIC_ACQUIRE);
    struct thread_group *victim_tg;

    if (tgid <= 0) {
        oom_expire_victim_if_needed();
        return 0;
    }
    if (!oom_can_wake_deferred_victim()) {
        oom_expire_victim_if_needed();
        return OOM_ALREADY;
    }
    tgid = __atomic_exchange_n(&__oom_deferred_tgid, 0, __ATOMIC_ACQ_REL);
    if (tgid <= 0) {
        oom_expire_victim_if_needed();
        return 0;
    }

    victim_tg = oom_get_thread_group_ref(tgid);
    if (victim_tg == NULL) {
        printf("OOM killer: deferred victim %d vanished\n", tgid);
        oom_clear_victim_state();
        return OOM_NO_VICTIM;
    }

    printf("OOM killer: waking deferred SIGKILL victim process %d\n", tgid);
    int ret = oom_wake_victim(victim_tg);
    if (ret == -ESRCH) {
        printf("OOM killer: deferred victim %d has no live threads; "
               "clearing victim state\n", tgid);
        oom_clear_victim_state();
    } else if (ret < 0) {
        oom_clear_victim_state();
    }
    thread_group_put(victim_tg);
    return ret;
}

void oom_note_victim_reclaim(struct thread_group *tg)
{
    if (tg == NULL)
        return;

    uint64 victim_addr = __atomic_load_n(&__oom_victim_tg_addr,
                                         __ATOMIC_ACQUIRE);
    if (victim_addr != (uint64)tg)
        return;
    if (__atomic_load_n(&__oom_victim_tgid, __ATOMIC_RELAXED) != tg->tgid)
        return;

    printf("OOM killer: victim %d reclaim observed\n", tg->tgid);
    oom_clear_victim_state();
}

void oom_note_victim_exit(struct thread_group *tg)
{
    uint64 victim_addr;
    uint64 rss_pages;

    if (tg == NULL)
        return;
    if (__atomic_load_n(&tg->live_threads, __ATOMIC_ACQUIRE) > 0)
        return;

    victim_addr = __atomic_load_n(&__oom_victim_tg_addr, __ATOMIC_ACQUIRE);
    if (victim_addr != (uint64)tg)
        return;
    if (__atomic_load_n(&__oom_victim_tgid, __ATOMIC_RELAXED) != tg->tgid)
        return;

    rss_pages = __atomic_load_n(&tg->acct.mm_rss_pages, __ATOMIC_ACQUIRE);
    if (rss_pages == 0) {
        printf("OOM killer: victim %d exit observed after VM reclaim; "
               "clearing victim state\n", tg->tgid);
        oom_clear_victim_state();
        return;
    }

    /*
     * Task exit and address-space teardown are distinct observations.  A dying
     * victim may still have temporary VM references, so do not open the OOM
     * gate just because the thread group is gone.  Give VM teardown a short
     * chance to report reclaim, then let the next OOM pass rescan instead of
     * waiting the full kill grace behind a dead victim.
     */
    __atomic_store_n(&__oom_victim_exited, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&__oom_victim_deadline_ms,
                     oom_now_ms() + OOM_EXIT_RECLAIM_GRACE_MS,
                     __ATOMIC_RELEASE);
    printf("OOM killer: victim %d fully exited with %ld rss pages still "
           "accounted; waiting briefly for VM reclaim\n", tg->tgid,
           rss_pages);
}

int oom_thread_group_is_victim(struct thread_group *tg)
{
    if (tg == NULL)
        return 0;
    if (__atomic_load_n(&__oom_victim_tg_addr, __ATOMIC_ACQUIRE) !=
        (uint64)tg)
        return 0;
    return __atomic_load_n(&__oom_victim_tgid, __ATOMIC_ACQUIRE) == tg->tgid;
}

int oom_score(int tgid) {
    struct thread_group *tg = oom_get_thread_group_ref(tgid);
    if (tg == NULL)
        return 0;
    uint64 score = __oom_badness(tg);
    thread_group_put(tg);
    // Normalize to 0-1000 range (like Linux's /proc/PID/oom_score)
    if (score > 1000)
        score = 1000;
    return (int)score;
}

void oom_dump_stats(void) {
    printf("OOM stats: kills=%ld, in_progress=%d victim=%d exited=%d deadline=%ld\n",
           __atomic_load_n(&__oom_kill_count, __ATOMIC_RELAXED),
           __atomic_load_n(&__oom_in_progress, __ATOMIC_RELAXED),
           __atomic_load_n(&__oom_victim_tgid, __ATOMIC_RELAXED),
           __atomic_load_n(&__oom_victim_exited, __ATOMIC_RELAXED),
           __atomic_load_n(&__oom_victim_deadline_ms, __ATOMIC_RELAXED));
}
