/*
 * Timer wrappers for unit tests
 * Provides mock timer/sleep functionality
 */

#include <stddef.h>
#include <time.h>
#include "types.h"

static uint64 g_mock_jiffies = 0;

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
