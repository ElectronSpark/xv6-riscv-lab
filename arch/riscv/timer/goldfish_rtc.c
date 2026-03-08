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
#include <mm/memlayout.h>
#include "riscv.h"
#include "trapframe.h"
#include "trap.h"
#include "printf.h"
#include "defs.h"
#include "dev/fdt.h"
#include "dev/plic.h"
#include "dev/x1_i2c.h"
#include "string.h"
#include "timer/goldfish_rtc.h"

uint64 __goldfish_rtc_mmio_base = 0x101000L;
uint64 __goldfish_rtc_irqno = 11;

// RTC base address (mapped in physical memory)
#define RTC_BASE GOLDFISH_RTC

// Counter for alarm interrupts
static volatile uint64 rtc_alarm_count = 0;

// Flag to track if RTC is initialized
static int rtc_initialized = 0;
static uint64 pmic_rtc_last_ns;
static uint64 pmic_rtc_anchor_ns;
static uint64 pmic_rtc_anchor_ticks;
static int pmic_rtc_anchor_valid;

#define SPM8821_RTC_REG_SECONDS 0x0d
#define SPM8821_RTC_REG_MINUTES 0x0e
#define SPM8821_RTC_REG_HOURS   0x0f
#define SPM8821_RTC_REG_DAY     0x10
#define SPM8821_RTC_REG_MONTH   0x11
#define SPM8821_RTC_REG_YEAR    0x12

static int goldfish_rtc_use_pmic(void) {
    return platform.has_pmic_rtc && x1_i2c_init() > 0 &&
           x1_i2c_is_ready((int)platform.pmic_rtc_bus);
}

static int64 rtc_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    int era = (year >= 0 ? year : year - 399) / 400;
    unsigned yoe = (unsigned)(year - era * 400);
    unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64)doe - 719468;
}

static uint64 rtc_ymdhms_to_ns(int year, int month, int day,
                               int hour, int minute, int second) {
    int64 days = rtc_days_from_civil(year, (unsigned)month, (unsigned)day);
    uint64 sec = (uint64)(days * 86400LL + hour * 3600 + minute * 60 + second);
    return sec * NS_PER_SEC;
}

static int pmic_rtc_read_regs(uint8 regs[6]) {
    static const uint8 rtc_regs[6] = {
        SPM8821_RTC_REG_SECONDS,
        SPM8821_RTC_REG_MINUTES,
        SPM8821_RTC_REG_HOURS,
        SPM8821_RTC_REG_DAY,
        SPM8821_RTC_REG_MONTH,
        SPM8821_RTC_REG_YEAR,
    };

    for (int i = 0; i < 6; i++) {
        if (x1_i2c_read_reg8((int)platform.pmic_rtc_bus,
                             (uint8)platform.pmic_rtc_addr,
                             rtc_regs[i], &regs[i]) != 0) {
            return -1;
        }
    }
    return 0;
}

static int pmic_rtc_read_ns(uint64 *ns_out) {
    uint8 prev[6], cur[6];

    if (pmic_rtc_read_regs(prev) != 0) {
        return -1;
    }

    for (int tries = 0; tries < 4; tries++) {
        if (pmic_rtc_read_regs(cur) != 0) {
            return -1;
        }
        if (memcmp(prev, cur, sizeof(cur)) == 0) {
            int sec = cur[0] & 0x3f;
            int min = cur[1] & 0x3f;
            int hour = cur[2] & 0x1f;
            int day = (cur[3] & 0x1f) + 1;
            int month = (cur[4] & 0x0f) + 1;
            int year = 2000 + (cur[5] & 0x3f);

            *ns_out = rtc_ymdhms_to_ns(year, month, day, hour, min, sec);
            pmic_rtc_last_ns = *ns_out;
            return 0;
        }
        memcpy(prev, cur, sizeof(cur));
    }

    return -1;
}

static uint64 pmic_rtc_interpolated_ns(void) {
    uint64 coarse_ns = 0;
    uint64 ticks_now;
    uint64 hz;
    uint64 delta_ticks;
    uint64 delta_ns;
    uint64 ns;

    if (pmic_rtc_read_ns(&coarse_ns) != 0) {
        return pmic_rtc_last_ns;
    }

    ticks_now = r_time();
    hz = platform.timebase_freq ? platform.timebase_freq : 24000000ULL;

    if (!pmic_rtc_anchor_valid || coarse_ns != pmic_rtc_anchor_ns) {
        pmic_rtc_anchor_ns = coarse_ns;
        pmic_rtc_anchor_ticks = ticks_now;
        pmic_rtc_anchor_valid = 1;
        if (coarse_ns > pmic_rtc_last_ns) {
            pmic_rtc_last_ns = coarse_ns;
        }
        return pmic_rtc_last_ns;
    }

    delta_ticks = ticks_now - pmic_rtc_anchor_ticks;
    delta_ns = (delta_ticks * NS_PER_SEC) / hz;
    if (delta_ns >= NS_PER_SEC) {
        delta_ns = NS_PER_SEC - 1;
    }

    ns = pmic_rtc_anchor_ns + delta_ns;
    if (ns < pmic_rtc_last_ns) {
        ns = pmic_rtc_last_ns;
    } else {
        pmic_rtc_last_ns = ns;
    }

    return ns;
}

/**
 * Read a 32-bit register from the RTC
 */
static inline uint32 rtc_read_reg(uint64 offset) {
    return *(volatile uint32 *)(RTC_BASE + offset);
}

/**
 * Write a 32-bit value to an RTC register
 */
static inline void rtc_write_reg(uint64 offset, uint32 value) {
    *(volatile uint32 *)(RTC_BASE + offset) = value;
}

/**
 * Read current time in nanoseconds since Unix epoch
 * Uses high-low-high read pattern to handle wrap-around
 */
uint64 goldfish_rtc_read_ns(void) {
    if (goldfish_rtc_use_pmic()) {
        return pmic_rtc_interpolated_ns();
    }

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
uint64 goldfish_rtc_read_sec(void) {
    return goldfish_rtc_read_ns() / NS_PER_SEC;
}

/**
 * Set alarm time (absolute, in nanoseconds since epoch)
 */
static void rtc_set_alarm_absolute(uint64 alarm_ns) {
    // Write high first, then low (hardware latches on low write)
    rtc_write_reg(GOLDFISH_RTC_ALARM_HIGH, (uint32)(alarm_ns >> 32));
    rtc_write_reg(GOLDFISH_RTC_ALARM_LOW, (uint32)(alarm_ns & 0xFFFFFFFF));
}

/**
 * Set an alarm to fire after 'ns' nanoseconds from now
 */
void goldfish_rtc_set_alarm_ns(uint64 ns) {
    if (goldfish_rtc_use_pmic()) {
        (void)ns;
        return;
    }

    uint64 now = goldfish_rtc_read_ns();
    uint64 alarm_time = now + ns;
    rtc_set_alarm_absolute(alarm_time);
}

/**
 * Set an alarm to fire after 'sec' seconds from now
 */
void goldfish_rtc_set_alarm_sec(uint64 sec) {
    goldfish_rtc_set_alarm_ns(sec * NS_PER_SEC);
}

/**
 * Clear pending alarm
 */
void goldfish_rtc_clear_alarm(void) {
    if (goldfish_rtc_use_pmic()) {
        return;
    }

    rtc_write_reg(GOLDFISH_RTC_ALARM_CLEAR, 1);
}

/**
 * Enable or disable RTC alarm interrupts
 */
void goldfish_rtc_irq_enable(int enable) {
    if (goldfish_rtc_use_pmic()) {
        (void)enable;
        return;
    }

    rtc_write_reg(GOLDFISH_RTC_IRQ_ENABLED, enable ? 1 : 0);
}

/**
 * Clear the RTC interrupt
 */
static void rtc_clear_interrupt(void) {
    if (goldfish_rtc_use_pmic()) {
        return;
    }

    rtc_write_reg(GOLDFISH_RTC_IRQ_CLEAR, 1);
}

/**
 * Get the number of RTC alarm interrupts received
 */
uint64 goldfish_rtc_get_alarm_count(void) {
    return __atomic_load_n(&rtc_alarm_count, __ATOMIC_SEQ_CST);
}

/**
 * RTC interrupt handler
 * Called when the alarm fires. Sets up the next 1-second alarm.
 */
static void goldfish_rtc_intr(int irq, void *data, device_t *dev) {
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
void goldfish_rtc_init(void) {
    if (rtc_initialized) {
        return;
    }

    if (goldfish_rtc_use_pmic()) {
        uint64 now_ns = goldfish_rtc_read_ns();
        printf("pmic_rtc: current unix time: %llu\n",
               (unsigned long long)(now_ns / NS_PER_SEC));
        rtc_initialized = 1;
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
