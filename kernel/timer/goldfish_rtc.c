/**
 * Goldfish RTC Driver
 * 
 * This driver provides access to the Goldfish RTC device emulated by QEMU.
 * The Goldfish RTC provides:
 *   - Wall-clock time (nanoseconds since Unix epoch)
 *   - Alarm functionality with interrupt support
 * 
 * The driver registers an IRQ handler and sets up a periodic 1-second alarm.
 */

#include "types.h"
#include "memlayout.h"
#include "riscv.h"
#include "trapframe.h"
#include "trap.h"
#include "printf.h"
#include "defs.h"
#include "plic.h"
#include "timer/goldfish_rtc.h"

uint64 __goldfish_rtc_mmio_base = 0x101000L;
uint64 __goldfish_rtc_irqno = 11;

// RTC base address (mapped in physical memory)
#define RTC_BASE    GOLDFISH_RTC

// Counter for alarm interrupts
static volatile uint64 rtc_alarm_count = 0;

// Flag to track if RTC is initialized
static int rtc_initialized = 0;

/**
 * Read a 32-bit register from the RTC
 */
static inline uint32 rtc_read_reg(uint64 offset)
{
    return *(volatile uint32 *)(RTC_BASE + offset);
}

/**
 * Write a 32-bit value to an RTC register
 */
static inline void rtc_write_reg(uint64 offset, uint32 value)
{
    *(volatile uint32 *)(RTC_BASE + offset) = value;
}

/**
 * Read current time in nanoseconds since Unix epoch
 * Uses high-low-high read pattern to handle wrap-around
 */
uint64 goldfish_rtc_read_ns(void)
{
    uint32 low, high, high2;
    
    // Read high-low-high to handle the case where low wraps
    // while we're reading
    do {
        high = rtc_read_reg(GOLDFISH_RTC_TIME_HIGH);
        low = rtc_read_reg(GOLDFISH_RTC_TIME_LOW);
        high2 = rtc_read_reg(GOLDFISH_RTC_TIME_HIGH);
    } while (high != high2);
    
    return ((uint64)high << 32) | low;
}

/**
 * Read current time in seconds since Unix epoch
 */
uint64 goldfish_rtc_read_sec(void)
{
    return goldfish_rtc_read_ns() / NS_PER_SEC;
}

/**
 * Set alarm time (absolute, in nanoseconds since epoch)
 */
static void rtc_set_alarm_absolute(uint64 alarm_ns)
{
    // Write high first, then low (hardware latches on low write)
    rtc_write_reg(GOLDFISH_RTC_ALARM_HIGH, (uint32)(alarm_ns >> 32));
    rtc_write_reg(GOLDFISH_RTC_ALARM_LOW, (uint32)(alarm_ns & 0xFFFFFFFF));
}

/**
 * Set an alarm to fire after 'ns' nanoseconds from now
 */
void goldfish_rtc_set_alarm_ns(uint64 ns)
{
    uint64 now = goldfish_rtc_read_ns();
    uint64 alarm_time = now + ns;
    rtc_set_alarm_absolute(alarm_time);
}

/**
 * Set an alarm to fire after 'sec' seconds from now
 */
void goldfish_rtc_set_alarm_sec(uint64 sec)
{
    goldfish_rtc_set_alarm_ns(sec * NS_PER_SEC);
}

/**
 * Clear pending alarm
 */
void goldfish_rtc_clear_alarm(void)
{
    rtc_write_reg(GOLDFISH_RTC_ALARM_CLEAR, 1);
}

/**
 * Enable or disable RTC alarm interrupts
 */
void goldfish_rtc_irq_enable(int enable)
{
    rtc_write_reg(GOLDFISH_RTC_IRQ_ENABLED, enable ? 1 : 0);
}

/**
 * Clear the RTC interrupt
 */
static void rtc_clear_interrupt(void)
{
    rtc_write_reg(GOLDFISH_RTC_IRQ_CLEAR, 1);
}

/**
 * Get the number of RTC alarm interrupts received
 */
uint64 goldfish_rtc_get_alarm_count(void)
{
    return __atomic_load_n(&rtc_alarm_count, __ATOMIC_SEQ_CST);
}

/**
 * RTC interrupt handler
 * Called when the alarm fires. Sets up the next 1-second alarm.
 */
static void goldfish_rtc_intr(int irq, void *data, device_t *dev)
{
    (void)irq;
    (void)data;
    (void)dev;
    
    // Increment alarm counter
    __atomic_fetch_add(&rtc_alarm_count, 1, __ATOMIC_SEQ_CST);
    
    // Clear the interrupt
    rtc_clear_interrupt();
    
    // Set next alarm for 1 second from now
    goldfish_rtc_set_alarm_sec(1);
    
    // Optional: Print a message every 10 seconds for debugging
    uint64 count = goldfish_rtc_get_alarm_count();
    if (count % 10 == 0) {
        uint64 now_sec = goldfish_rtc_read_sec();
        printf("goldfish_rtc: alarm #%lu, unix time: %lu\n", count, now_sec);
    }
}

/**
 * Initialize the Goldfish RTC driver
 * Registers IRQ handler and sets up periodic 1-second alarm
 */
void goldfish_rtc_init(void)
{
    if (rtc_initialized) {
        return;
    }
    
    // Read initial time
    uint64 now_ns = goldfish_rtc_read_ns();
    uint64 now_sec = now_ns / NS_PER_SEC;
    
    printf("goldfish_rtc: initializing, current unix time: %lu\n", now_sec);
    
    // Register IRQ handler for RTC
    struct irq_desc rtc_irq_desc = {
        .handler = goldfish_rtc_intr,
        .data = NULL,
        .dev = NULL,
    };
    
    int ret = register_irq_handler(PLIC_IRQ(GOLDFISH_RTC_IRQ), &rtc_irq_desc);
    if (ret != 0) {
        printf("goldfish_rtc: failed to register IRQ handler: %d\n", ret);
        return;
    }
    
    // Clear any pending interrupts
    rtc_clear_interrupt();
    goldfish_rtc_clear_alarm();
    
    // Enable RTC interrupts
    goldfish_rtc_irq_enable(1);
    
    // Set first alarm for 1 second from now
    goldfish_rtc_set_alarm_sec(1);
    
    rtc_initialized = 1;
    printf("goldfish_rtc: initialized, alarm set for 1 second intervals\n");
}
