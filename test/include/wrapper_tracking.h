#ifndef WRAPPER_TRACKING_H
#define WRAPPER_TRACKING_H

#include "types.h"

// Forward declarations to avoid pulling in all headers
struct spinlock;
struct tq;
struct proc;
struct cpu_local;

// Tracking structure for spinlock operations
typedef struct {
    int spin_init_count;
    struct spinlock *last_spin_init;
    const char *last_spin_name;
    int spin_lock_count;
    struct spinlock *last_spin_lock;
    int spin_unlock_count;
    struct spinlock *last_spin_unlock;
} spinlock_tracking_t;

// Tracking structure for proc/cpu operations
typedef struct {
    struct thread *current_proc;      // What current returns
    struct cpu_local *current_cpu;  // What mycpu() returns
    int current_cpuid;              // What cpuid() returns
} proc_tracking_t;

// Tracking structure for tq operations
typedef struct {
    int queue_init_count;
    struct tq *last_queue_init;
    const char *last_queue_name;
    struct spinlock *last_queue_lock;
    
    int queue_wait_count;
    struct tq *last_queue_wait;
    struct spinlock *last_wait_lock;
    
    int queue_wakeup_count;
    struct tq *last_queue_wakeup;
    int last_wakeup_errno;
    uint64 last_wakeup_rdata;
    
    int queue_wakeup_all_count;
    struct tq *last_queue_wakeup_all;
    int last_wakeup_all_errno;
    uint64 last_wakeup_all_rdata;
    
    int wait_return;
    int wakeup_return;
    int wakeup_all_return;
    
    // For custom behavior
    void *user_data;
    int (*wait_callback)(struct tq *q, struct spinlock *lock, uint64 *rdata, void *user_data);
    struct thread *next_wakeup;
} tq_tracking_t;

// Global tracking instances (can be NULL if tracking not needed)
extern spinlock_tracking_t *g_spinlock_tracking;
extern tq_tracking_t *g_tq_tracking;
extern proc_tracking_t *g_proc_tracking;

// Tracking control functions
void wrapper_tracking_enable_spinlock(spinlock_tracking_t *tracking);
void wrapper_tracking_enable_tq(tq_tracking_t *tracking);
void wrapper_tracking_enable_proc(proc_tracking_t *tracking);
void wrapper_tracking_disable_spinlock(void);
void wrapper_tracking_disable_tq(void);
void wrapper_tracking_disable_proc(void);

#endif // WRAPPER_TRACKING_H
