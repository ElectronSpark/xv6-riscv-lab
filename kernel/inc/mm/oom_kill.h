// OOM (Out-Of-Memory) killer — last resort memory reclamation
//
// Modeled after Linux's OOM killer. When all reclamation attempts fail
// and the system cannot satisfy a memory allocation, the OOM killer
// selects the "worst" process (highest OOM score) and terminates it
// to free memory.
//
// OOM scoring considers:
//   - Process RSS (resident set size) — primary factor
//   - Whether the process is a kernel thread (exempt)
//   - Process PID 1 (init) is protected
//   - OOM score adjustment per process (future extension)
//
// The OOM killer is invoked as a last resort after:
//   1. Shrinking all registered shrinkers
//   2. Direct reclaim of page caches
//   3. Flushing dirty pages
#ifndef __KERNEL_OOM_KILL_H
#define __KERNEL_OOM_KILL_H

#include "types.h"

// OOM killer constraints
#define OOM_SCORE_ADJ_MIN  (-1000) // Never kill (kernel, init)
#define OOM_SCORE_ADJ_MAX  1000    // Always kill first

// OOM killer return codes
#define OOM_SUCCESS     0   // Successfully killed a process
#define OOM_NO_VICTIM  -1   // No killable process found
#define OOM_ALREADY    -2   // OOM kill already in progress

// Initialize OOM killer subsystem
void oom_init(void);

// Invoke the OOM killer — select and kill the worst process.
// Returns OOM_SUCCESS if a process was killed, error otherwise.
// @order: allocation order that triggered OOM
// @gfp_flags: allocation flags
int oom_kill_process(uint64 order, uint64 gfp_flags);

// Get OOM score for a process (for /proc display)
// Higher score = more likely to be killed
int oom_score(int tgid);

// Print OOM killer statistics
void oom_dump_stats(void);

#endif /* __KERNEL_OOM_KILL_H */
