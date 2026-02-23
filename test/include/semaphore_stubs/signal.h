#ifndef __TEST_STUB_SIGNAL_H
#define __TEST_STUB_SIGNAL_H

#include "types.h"

struct thread;

bool signal_pending(struct thread *p);

#endif