/*
 * Process management wrappers for unit tests
 * Provides mock process/scheduler behavior
 */

#include <stddef.h>
#include <stdarg.h>
#include <setjmp.h>
#include <cmocka.h>
#include "types.h"

// Provide missing definitions before including kernel headers
#ifndef PGSIZE
#define PGSIZE 4096
#endif
#ifndef NCPU
#define NCPU 8
#endif
#ifndef NOFILE
#define NOFILE 64
#endif
// Note: pagetable_t is defined in riscv.h, don't redefine it here

#include "percpu_types.h"
#include "proc/proc.h"
#include "proc/proc_queue.h"
#include "spinlock.h"
#include "proc/sched.h"
#include "wrapper_tracking.h"

static struct cpu_local g_cpu_stub;
static struct proc g_proc_stub = {.pid = 1};

// Global tracking pointers (NULL if tracking disabled)
proc_queue_tracking_t *g_proc_queue_tracking = NULL;
proc_tracking_t *g_proc_tracking = NULL;

void wrapper_tracking_enable_proc_queue(proc_queue_tracking_t *tracking)
{
    g_proc_queue_tracking = tracking;
}

void wrapper_tracking_disable_proc_queue(void)
{
    g_proc_queue_tracking = NULL;
}

void wrapper_tracking_enable_proc(proc_tracking_t *tracking)
{
    g_proc_tracking = tracking;
}

void wrapper_tracking_disable_proc(void)
{
    g_proc_tracking = NULL;
}

static bool g_test_break_on_sleep = false;
static int g_test_sleep_call_count = 0;
static int g_test_max_sleep_calls = 1;

void pcache_test_set_break_on_sleep(bool enable)
{
    g_test_break_on_sleep = enable;
    g_test_sleep_call_count = 0;
}

void pcache_test_set_max_sleep_calls(int max_calls)
{
    g_test_max_sleep_calls = max_calls;
}

struct cpu_local *__wrap_mycpu(void)
{
    if (g_proc_tracking && g_proc_tracking->current_cpu) {
        return g_proc_tracking->current_cpu;
    }
    return &g_cpu_stub;
}

struct proc *__wrap_myproc(void)
{
    if (g_proc_tracking && g_proc_tracking->current_proc) {
        return g_proc_tracking->current_proc;
    }
    return &g_proc_stub;
}

int __wrap_cpuid(void)
{
    if (g_proc_tracking) {
        return g_proc_tracking->current_cpuid;
    }
    return 0;
}

/*
 * Note: When using --wrap=myproc etc., the linker automatically redirects
 * calls to myproc() in other object files to __wrap_myproc().
 * We don't define real myproc/mycpu/cpuid here - they're declared in percpu.h
 * for ON_HOST_OS and the wrapper mechanism handles the redirection.
 */

void __wrap_proc_lock(struct proc *p)
{
    (void)p;
}

void __wrap_proc_unlock(struct proc *p)
{
    (void)p;
}

void __wrap_proc_assert_holding(struct proc *p)
{
    (void)p;
}

void __wrap_sched_lock(void)
{
    /* Scheduler lock not needed in host tests */
}

void __wrap_sched_unlock(void)
{
    /* Scheduler unlock not needed in host tests */
}

void __wrap_scheduler_wakeup(struct proc *p)
{
    (void)p;
}

void __wrap_scheduler_sleep(struct spinlock *lk, enum procstate state)
{
    (void)lk;
    (void)state;
}

int __wrap_kernel_proc_create(const char *name, struct proc **retp, void *entry,
                              uint64 arg0, uint64 arg1, uint64 stack_order)
{
    // Don't use check_expected for parameters - just ignore them in tests
    (void)name;
    (void)entry;
    (void)arg0;
    (void)arg1;
    (void)stack_order;
    
    if (retp != NULL) {
        *retp = mock_ptr_type(struct proc *);
    }
    return mock_type(int);  // Return value controlled by will_return()
}

void __wrap_wakeup_proc(struct proc *p)
{
    (void)p;
}

void __wrap_wakeup_on_chan(void *chan)
{
    (void)chan;
}

void __wrap_sleep_on_chan(void *chan, struct spinlock *lk)
{
    (void)chan;
    
    g_test_sleep_call_count++;
    
    if (g_test_break_on_sleep && g_test_sleep_call_count >= g_test_max_sleep_calls) {
        // Break out of sleep loop for testing
        return;
    }
    
    // In a real implementation, we would release and re-acquire the lock
    // For mocking, just simulate the behavior without calling other wrappers
    if (lk != NULL) {
        lk->locked = 0;  // Simulate release
        lk->locked = 1;  // Simulate re-acquire
    }
}

// Proc queue wrappers
void __wrap_proc_queue_init(proc_queue_t *q, const char *name, spinlock_t *lock)
{
    if (g_proc_queue_tracking) {
        g_proc_queue_tracking->queue_init_count++;
        g_proc_queue_tracking->last_queue_init = q;
        g_proc_queue_tracking->last_queue_name = name;
        g_proc_queue_tracking->last_queue_lock = lock;
    }
    
    if (q == NULL) {
        return;
    }
    list_entry_init(&q->head);
    q->counter = 0;
    q->name = name;
    q->lock = lock;
    q->flags = 0;
}

int __wrap_proc_queue_size(proc_queue_t *q)
{
    if (q == NULL) {
        return -1;
    }
    return q->counter;
}

int __wrap_proc_queue_wait(proc_queue_t *q, struct spinlock *lock, uint64 *rdata)
{
    if (g_proc_queue_tracking) {
        g_proc_queue_tracking->queue_wait_count++;
        g_proc_queue_tracking->last_queue_wait = q;
        g_proc_queue_tracking->last_wait_lock = lock;
        
        // Call custom callback if provided
        if (g_proc_queue_tracking->wait_callback) {
            return g_proc_queue_tracking->wait_callback(q, lock, rdata, g_proc_queue_tracking->user_data);
        }
        
        // Increment counter to simulate waiting process
        if (q != NULL) {
            q->counter++;
        }
        
        return g_proc_queue_tracking->wait_return;
    }
    
    return mock_type(int);
}

int __wrap_proc_queue_wakeup(proc_queue_t *q, int error_no, uint64 rdata, struct proc **retp)
{
    if (g_proc_queue_tracking) {
        g_proc_queue_tracking->queue_wakeup_count++;
        g_proc_queue_tracking->last_queue_wakeup = q;
        g_proc_queue_tracking->last_wakeup_errno = error_no;
        g_proc_queue_tracking->last_wakeup_rdata = rdata;
        
        if (retp != NULL && g_proc_queue_tracking->next_wakeup_proc != NULL) {
            *retp = g_proc_queue_tracking->next_wakeup_proc;
        }
        
        // Decrement counter if queue tracking is enabled
        if (q != NULL && q->counter > 0) {
            q->counter--;
        }
        
        return g_proc_queue_tracking->wakeup_return;
    }
    
    return mock_type(int);
}

int __wrap_proc_queue_wakeup_all(proc_queue_t *q, int error_no, uint64 rdata)
{
    if (g_proc_queue_tracking) {
        g_proc_queue_tracking->queue_wakeup_all_count++;
        g_proc_queue_tracking->last_queue_wakeup_all = q;
        g_proc_queue_tracking->last_wakeup_all_errno = error_no;
        g_proc_queue_tracking->last_wakeup_all_rdata = rdata;
        
        // Reset counter when waking all
        if (q != NULL) {
            q->counter = 0;
        }
        return g_proc_queue_tracking->wakeup_all_return;
    }
    
    return mock_type(int);
}
