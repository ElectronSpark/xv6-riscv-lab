/*
 * Timer wrappers for unit tests
 * Provides mock timer/sleep functionality
 */

#include <stddef.h>
#include <time.h>
#include "types.h"
#include "timer/timer_types.h"

static uint64 g_mock_jiffies = 0;
uint64 __timebase_frequency = 1000000;

uint64 r_time(void)
{
    return g_mock_jiffies++;
}

uint64 __wrap_get_jiffs(void)
{
    return g_mock_jiffies++;
}

void __wrap_sleep_ms(uint64 ms)
{
    (void)ms;
    /* Don't actually sleep in unit tests */
    g_mock_jiffies += ms;
}

void sleep_ms(uint64 ms)
{
    __wrap_sleep_ms(ms);
}

int sched_timer_set(struct timer_node *tn, uint64 ticks)
{
    (void)tn;
    (void)ticks;
    return 0;
}

void sched_timer_done(struct timer_node *tn)
{
    (void)tn;
}
