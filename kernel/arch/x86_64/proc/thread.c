#include "types.h"
#include "errno.h"
#include "proc/thread.h"

void tcb_lock(struct thread *p) {
	(void)p;
}

void tcb_unlock(struct thread *p) {
	(void)p;
}

void proc_assert_holding(struct thread *p) { (void)p; }

void attach_child(struct thread *parent, struct thread *child) {
	(void)parent;
	(void)child;
}

void detach_child(struct thread *parent, struct thread *child) {
	(void)parent;
	(void)child;
}

struct thread *kthread_create(const char *name, void *entry, uint64 arg1,
							  uint64 arg2, int stack_order) {
	(void)name;
	(void)entry;
	(void)arg1;
	(void)arg2;
	(void)stack_order;
	return NULL;
}

void thread_destroy(struct thread *p) { (void)p; }

int thread_clone(struct clone_args *args) {
	(void)args;
	return -ENOSYS;
}

void thread_init(void) {}

void idle_thread_init(void) {}

void userinit(void) {}

void install_user_root(void) {}