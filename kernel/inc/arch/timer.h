#ifndef _ARCH_TIMER_H_
#define _ARCH_TIMER_H_

// Initialize architecture-specific timer
void arch_timer_init(void);

// Initialize timer for the current CPU/hart
void arch_timer_init_hart(void);

#endif // _ARCH_TIMER_H_