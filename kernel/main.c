#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "defs.h"
#include "printf.h"
#include "sched.h"
#include "signal.h"
#include "workqueue.h"
#include "kobject.h"
#include "dev.h"
#include "pcache.h"

// start() jumps here in supervisor mode on all CPUs.
void
main()
{
  start_kernel();    
}
