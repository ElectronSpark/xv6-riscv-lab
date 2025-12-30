#ifndef WRAPPER_TRACKING_H
#define WRAPPER_TRACKING_H

#include "types.h"

// Forward declarations to avoid pulling in all headers
struct spinlock;
struct proc_queue;

// Tracking structure for spinlock operations
typedef struct {
    int spin_init_count;
    struct spinlock *last_spin_init;
    const char *last_spin_name;
    int spin_acquire_count;
    struct spinlock *last_spin_acquire;
    int spin_release_count;
    struct spinlock *last_spin_release;
} spinlock_tracking_t;

// Tracking structure for proc_queue operations
typedef struct {
    int queue_init_count;
    struct proc_queue *last_queue_init;
    const char *last_queue_name;
    struct spinlock *last_queue_lock;
    
    int queue_wait_count;
    struct proc_queue *last_queue_wait;
    struct spinlock *last_wait_lock;
    
    int queue_wakeup_count;
    struct proc_queue *last_queue_wakeup;
    int last_wakeup_errno;
    uint64 last_wakeup_rdata;
    
    int queue_wakeup_all_count;
    struct proc_queue *last_queue_wakeup_all;
    int last_wakeup_all_errno;
    uint64 last_wakeup_all_rdata;
    
    int wait_return;
    int wakeup_return;
    int wakeup_all_return;
    
    // For custom behavior
    void *user_data;
    int (*wait_callback)(struct proc_queue *q, struct spinlock *lock, uint64 *rdata, void *user_data);
    struct proc *next_wakeup_proc;
} proc_queue_tracking_t;

// Global tracking instances (can be NULL if tracking not needed)
extern spinlock_tracking_t *g_spinlock_tracking;
extern proc_queue_tracking_t *g_proc_queue_tracking;

// Tracking control functions
void wrapper_tracking_enable_spinlock(spinlock_tracking_t *tracking);
void wrapper_tracking_enable_proc_queue(proc_queue_tracking_t *tracking);
void wrapper_tracking_disable_spinlock(void);
void wrapper_tracking_disable_proc_queue(void);

#endif // WRAPPER_TRACKING_H
