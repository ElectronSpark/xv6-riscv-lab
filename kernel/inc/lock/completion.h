#ifndef __KERNEL_COMPLETION_H
#define __KERNEL_COMPLETION_H

#include <lock/completion_types.h>

void completion_init(completion_t *c);
void completion_reinit(completion_t *c);
void wait_for_completion(completion_t *c);
bool try_wait_for_completion(completion_t *c);
void complete(completion_t *c);
void complete_all(completion_t *c);
bool completion_done(completion_t *c);

#endif // __KERNEL_COMPLETION_H
