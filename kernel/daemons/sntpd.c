/*
 * sntpd — Kernel-resident SNTP time synchronisation daemon
 *
 * Uses lwIP's built-in SNTP client to periodically synchronise the
 * system clock from an NTP server.
 *
 * The Goldfish RTC already provides wall-clock time from the QEMU host,
 * but this daemon can detect (and log) any drift between the RTC and
 * real NTP time.  The computed offset is exported so that other kernel
 * subsystems can apply it if higher accuracy is needed.
 *
 * SNTP uses the raw UDP API and runs entirely within the tcpip thread,
 * so no separate kthread is needed.
 *
 * By default, queries pool.ntp.org via DNS.
 */

#include "types.h"
#include "param.h"
#include "printf.h"
#include "timer/goldfish_rtc.h"

#include "lwip/arch.h"
#include "lwip/apps/sntp.h"

/* ──────────────────────────────────────────────────────────────────────────── */
/* NTP ↔ RTC offset tracking                                                   */
/*                                                                             */
/* offset_ns = NTP_time − RTC_time                                             */
/* Corrected time = goldfish_rtc_read_ns() + sntp_offset_ns                    */
/* ──────────────────────────────────────────────────────────────────────────── */

/* Exported offset (ns) — add to goldfish_rtc_read_ns() for NTP-corrected time */
volatile int64 sntp_offset_ns;
volatile int   sntp_synced;
static   int   sntp_sync_count;

void sntp_set_system_time_us(unsigned int sec, unsigned int us)
{
    /* NTP time in nanoseconds */
    uint64 ntp_ns = (uint64)sec * NS_PER_SEC + (uint64)us * NS_PER_US;

    /* Current hardware RTC time */
    uint64 rtc_ns = goldfish_rtc_read_ns();

    /* Compute signed offset: positive = RTC behind NTP, negative = RTC ahead */
    int64 offset = (int64)(ntp_ns - rtc_ns);
    sntp_offset_ns = offset;
    sntp_synced = 1;
    sntp_sync_count++;

    int64 drift_ms = offset / (int64)NS_PER_MS;

    printf("sntpd: sync #%d — NTP %u.%06u  RTC drift %s%d ms\n",
           sntp_sync_count, sec, us,
           drift_ms >= 0 ? "+" : "", (int)drift_ms);
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Corrected time accessors (for other kernel subsystems)                      */
/* ──────────────────────────────────────────────────────────────────────────── */

/**
 * Return NTP-corrected wall-clock time in nanoseconds since Unix epoch.
 * Falls back to raw RTC if SNTP has not synchronised yet.
 */
uint64 sntp_corrected_time_ns(void)
{
    uint64 rtc = goldfish_rtc_read_ns();
    if (sntp_synced)
        return (uint64)((int64)rtc + sntp_offset_ns);
    return rtc;
}

/**
 * Return NTP-corrected wall-clock time in seconds since Unix epoch.
 */
uint64 sntp_corrected_time_sec(void)
{
    return sntp_corrected_time_ns() / NS_PER_SEC;
}

/* ──────────────────────────────────────────────────────────────────────────── */
/* Module init                                                                 */
/* ──────────────────────────────────────────────────────────────────────────── */

void sntpd_init(void)
{
    /* Log current RTC time at startup */
    uint64 rtc_sec = goldfish_rtc_read_sec();
    printf("sntpd: RTC says %lu seconds since epoch\n", (unsigned long)rtc_sec);

    sntp_setoperatingmode(SNTP_OPMODE_POLL);

#if SNTP_SERVER_DNS
    sntp_setservername(0, "pool.ntp.org");
#else
    /* Fallback: use a well-known NTP IP (Google Public NTP: 216.239.35.0) */
    ip_addr_t ntp_server;
    IP4_ADDR(ip_2_ip4(&ntp_server), 216, 239, 35, 0);
    sntp_setserver(0, &ntp_server);
#endif

    sntp_init();
    printf("sntpd: client started (polling pool.ntp.org)\n");
}
