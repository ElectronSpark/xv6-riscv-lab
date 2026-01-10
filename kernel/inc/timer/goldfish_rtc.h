#ifndef __KERNEL_GOLDFISH_RTC_H
#define __KERNEL_GOLDFISH_RTC_H

#include "types.h"

// Goldfish RTC Register Offsets
// The RTC returns time in nanoseconds since Unix epoch
#define GOLDFISH_RTC_TIME_LOW       0x00    // Low 32 bits of time (ns)
#define GOLDFISH_RTC_TIME_HIGH      0x04    // High 32 bits of time (ns)
#define GOLDFISH_RTC_ALARM_LOW      0x08    // Low 32 bits of alarm time (ns)
#define GOLDFISH_RTC_ALARM_HIGH     0x0C    // High 32 bits of alarm time (ns)
#define GOLDFISH_RTC_IRQ_ENABLED    0x10    // IRQ enable register
#define GOLDFISH_RTC_ALARM_CLEAR    0x14    // Clear alarm (write any value)
#define GOLDFISH_RTC_ALARM_STATUS   0x18    // Alarm status
#define GOLDFISH_RTC_IRQ_CLEAR      0x1C    // Clear interrupt (write any value)

// Time conversion constants
#define NS_PER_SEC      1000000000ULL
#define NS_PER_MS       1000000ULL
#define NS_PER_US       1000ULL

// Initialize the Goldfish RTC driver
void goldfish_rtc_init(void);

// Read current time in nanoseconds since Unix epoch
uint64 goldfish_rtc_read_ns(void);

// Read current time in seconds since Unix epoch
uint64 goldfish_rtc_read_sec(void);

// Set an alarm to fire after 'ns' nanoseconds from now
void goldfish_rtc_set_alarm_ns(uint64 ns);

// Set an alarm to fire after 'sec' seconds from now
void goldfish_rtc_set_alarm_sec(uint64 sec);

// Clear pending alarm
void goldfish_rtc_clear_alarm(void);

// Enable/disable RTC alarm interrupts
void goldfish_rtc_irq_enable(int enable);

// Get the number of RTC alarm interrupts received
uint64 goldfish_rtc_get_alarm_count(void);

#endif // __KERNEL_GOLDFISH_RTC_H
