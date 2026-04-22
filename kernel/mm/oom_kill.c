// OOM (Out-Of-Memory) killer — select and terminate the worst process
//
// When the system is critically low on memory and reclamation has failed,
// the OOM killer selects the process with the highest "badness" score
// and sends it SIGKILL. This is a last resort — all shrinkers and
// reclaim paths must be exhausted first.
//
// Scoring is modeled after Linux's oom_badness():
//   - Base score = process RSS (resident pages via anon page count)
//   - Kernel threads and PID 1 (init) are exempt
//   - Zombie/exiting processes are skipped

#include "types.h"
#include "printf.h"
#include "defs.h"
#include "lock/spinlock.h"
#include "proc/thread.h"
#include "proc/thread_group.h"
#include "proc/sched.h"
#include "signal.h"
#include "signo.h"
#include <mm/oom_kill.h>
#include <mm/mm_watermark.h>
#include "smp/atomic.h"

// Protect against concurrent OOM kills
static spinlock_t __oom_lock = SPINLOCK_INITIALIZED("oom_lock");
static _Atomic uint64 __oom_kill_count = 0;
static _Atomic int __oom_in_progress = 0;

void oom_init(void) {
    printf("OOM killer initialized\n");
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

    // Score = RSS estimate from memory accounting
    // Use mm_peak_vm as a proxy for memory consumption since we don't
    // have per-process RSS tracking. Processes with large address spaces
    // are the prime OOM candidates.
    uint64 score = __atomic_load_n(&tg->acct.mm_peak_vm, __ATOMIC_RELAXED);

    // Normalize to pages
    score = score / PAGE_SIZE;

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
    struct thread_group *tg = get_thread_group(tgid);
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
    // Prevent concurrent OOM kills
    if (__atomic_exchange_n(&__oom_in_progress, 1, __ATOMIC_ACQ_REL))
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
        __atomic_store_n(&__oom_in_progress, 0, __ATOMIC_RELEASE);
        return OOM_NO_VICTIM;
    }

    // Kill the victim
    struct thread_group *victim_tg = get_thread_group(ctx.best_tgid);
    if (victim_tg == NULL) {
        printf("OOM killer: victim tgid %d vanished\n", ctx.best_tgid);
        spin_unlock(&__oom_lock);
        __atomic_store_n(&__oom_in_progress, 0, __ATOMIC_RELEASE);
        return OOM_NO_VICTIM;
    }

    printf("OOM killer: killing process %d (score=%ld)\n",
           ctx.best_tgid, ctx.best_score);

    // Send SIGKILL to the thread group
    struct ksiginfo info = {0};
    info.signo = SIGKILL;
    tg_signal_send(victim_tg, &info);
    thread_group_put(victim_tg);

    __atomic_fetch_add(&__oom_kill_count, 1, __ATOMIC_RELAXED);

    printf("=== OOM KILLER DONE ===\n\n");

    spin_unlock(&__oom_lock);
    __atomic_store_n(&__oom_in_progress, 0, __ATOMIC_RELEASE);
    return OOM_SUCCESS;
}

int oom_score(int tgid) {
    struct thread_group *tg = get_thread_group(tgid);
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
    printf("OOM stats: kills=%ld, in_progress=%d\n",
           __atomic_load_n(&__oom_kill_count, __ATOMIC_RELAXED),
           __atomic_load_n(&__oom_in_progress, __ATOMIC_RELAXED));
}
