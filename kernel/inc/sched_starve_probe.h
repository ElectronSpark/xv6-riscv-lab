#ifndef __KERNEL_SCHED_STARVE_PROBE_H
#define __KERNEL_SCHED_STARVE_PROBE_H

#include "types.h"

int sched_starve_probe_is_enabled(void);
void sched_starve_probe_tick(void);
void sched_starve_probe_note_timer_tick(int cpu);
void sched_starve_probe_note_wake_ipi_send(int target_cpu);
void sched_starve_probe_note_wake_ipi_recv(int cpu);
uint64 sched_starve_probe_idle_needs_resched_samples(void);
uint64 sched_starve_probe_snapshots(void);

#endif /* __KERNEL_SCHED_STARVE_PROBE_H */
