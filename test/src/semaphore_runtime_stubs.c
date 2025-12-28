#include "types.h"
#include "proc.h"
#include "spinlock.h"
#include "sched.h"

void proc_lock(struct proc *p) {
    (void)p;
}

void proc_unlock(struct proc *p) {
    (void)p;
}

void proc_assert_holding(struct proc *p) {
    (void)p;
}

struct cpu *mycpu(void) {
    static struct cpu cpu_stub;
    return &cpu_stub;
}

struct proc *myproc(void) {
    static struct proc proc_stub = {.pid = 1};
    return &proc_stub;
}

void sched_lock(void) {}

void sched_unlock(void) {}

void scheduler_wakeup(struct proc *p) {
    (void)p;
}

void scheduler_sleep(struct spinlock *lk, enum procstate state) {
    (void)lk;
    (void)state;
}
