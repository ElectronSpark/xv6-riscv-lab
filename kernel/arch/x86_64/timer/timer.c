/**
 * @file timer.c
 * @brief x86_64 timer subsystem — LAPIC / HPET / PIT selection.
 *
 * At boot, tries timer sources in priority order:
 *   1. LAPIC timer in TSC-deadline mode  (requires CPUID TSC-deadline)
 *   2. LAPIC timer in periodic mode      (calibrated via PIT channel 2)
 *   3. HPET in periodic mode             (legacy replacement → IRQ 0)
 *   4. PIT channel 0, mode 3             (legacy fallback)
 *
 * When TSC is available (CPUID), r_time() returns RDTSC-based timestamps
 * with __timebase_frequency set to the measured TSC frequency.
 * Otherwise, falls back to jiffies-based timestamps at a virtual 10 MHz
 * timebase.
 *
 * The red-black tree software timer infrastructure is also implemented
 * here (timer_init, timer_add, timer_remove, timer_tick, timer_node_init),
 * shared with the scheduler timer subsystem (sched_timer.c).
 */

#include "types.h"
#include "arch/timer.h"
#include "timer/timer.h"
#include "x86.h"
#include "printf.h"
#include "string.h"
#include "rbtree.h"
#include "list.h"
#include "lock/spinlock.h"
#include "proc/sched.h"
#include "defs.h"
#include "lapic.h"
#include "seg.h"

/* ══════════════════════════════════════════════════════════════
 *  Linker-compat globals (RISC-V legacy)
 * ══════════════════════════════════════════════════════════════ */
uint64 __clint_timer_irqno = 0;
uint64 __timebase_frequency = 10000000UL;   /* updated if TSC found */
uint64 __jiff_ticks = 10000UL;              /* updated if TSC found */
uint64 __goldfish_rtc_mmio_base = 0;
uint64 __goldfish_rtc_irqno = 0;
uint64 __plic_mmio_base = 0;

/* ══════════════════════════════════════════════════════════════
 *  I/O port helpers
 * ══════════════════════════════════════════════════════════════ */
static inline void outb(uint16 port, uint8 val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8 inb(uint16 port) {
    uint8 val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

/* ══════════════════════════════════════════════════════════════
 *  Timer source selection
 * ══════════════════════════════════════════════════════════════ */
enum timer_source {
    TIMER_PIT = 0,
    TIMER_HPET,
    TIMER_LAPIC_PERIODIC,
    TIMER_LAPIC_TSC_DEADLINE,
};

static enum timer_source active_timer = TIMER_PIT;

static const char *timer_source_names[] = {
    [TIMER_PIT]                = "PIT 8254",
    [TIMER_HPET]               = "HPET",
    [TIMER_LAPIC_PERIODIC]     = "LAPIC periodic",
    [TIMER_LAPIC_TSC_DEADLINE] = "LAPIC TSC-deadline",
};

/* ══════════════════════════════════════════════════════════════
 *  PIT constants
 * ══════════════════════════════════════════════════════════════ */
#define PIT_FREQ        1193182UL
#define PIT_HZ          100             /* PIT rate when used as tick source */
#define PIT_DIVISOR     (PIT_FREQ / PIT_HZ)

#define PIT_CH0_DATA    0x40
#define PIT_CMD         0x43

/* ══════════════════════════════════════════════════════════════
 *  HPET registers
 * ══════════════════════════════════════════════════════════════ */
#define HPET_BASE           0xFED00000ULL
#define HPET_REG_CAP_ID     0x000       /* General Capabilities and ID */
#define HPET_REG_CONFIG     0x010       /* General Configuration */
#define HPET_REG_STATUS     0x020       /* General Interrupt Status */
#define HPET_REG_COUNTER    0x0F0       /* Main Counter Value */
#define HPET_REG_T0_CONFIG  0x100       /* Timer 0 Config & Capability */
#define HPET_REG_T0_CMP     0x108       /* Timer 0 Comparator */

#define HPET_CFG_ENABLE     (1ULL << 0)
#define HPET_CFG_LEGACY     (1ULL << 1)

#define HPET_TN_ENABLE      (1ULL << 2)
#define HPET_TN_PERIODIC    (1ULL << 3)
#define HPET_TN_PER_CAP     (1ULL << 4) /* periodic capable (read-only) */
#define HPET_TN_SETVAL      (1ULL << 6) /* set accumulator for periodic */

static volatile uint64 *hpet_base_ptr;
static uint64 hpet_period_fs;          /* femtoseconds per tick */
static uint64 hpet_freq_hz;

/* ══════════════════════════════════════════════════════════════
 *  TSC
 * ══════════════════════════════════════════════════════════════ */
#define MSR_TSC_DEADLINE    0x6E0

static uint64 tsc_freq_hz;             /* measured TSC frequency */
static int    have_tsc;

static inline uint64 rdtsc(void) {
    uint32 lo, hi;
    asm volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64)hi << 32) | lo;
}

/* ══════════════════════════════════════════════════════════════
 *  LAPIC timer calibration data
 * ══════════════════════════════════════════════════════════════ */
static uint64 lapic_bus_freq;           /* LAPIC timer bus frequency */
static uint32 lapic_periodic_icr;       /* initial count for periodic mode */

/* ══════════════════════════════════════════════════════════════
 *  Tick rate
 * ══════════════════════════════════════════════════════════════ */
#define TIMER_HZ    1000    /* tick rate for LAPIC/HPET (matches kernel HZ) */
static uint32 tick_hz = PIT_HZ;

/* ══════════════════════════════════════════════════════════════
 *  Jiffies
 * ══════════════════════════════════════════════════════════════ */
static volatile uint64 jiffies_count = 0;

uint64 get_jiffs(void) {
    return __atomic_load_n(&jiffies_count, __ATOMIC_RELAXED);
}

/* ══════════════════════════════════════════════════════════════
 *  Debugcon (port 0xE9)
 * ══════════════════════════════════════════════════════════════ */
static inline void dbg_putc_local(char c) {
    asm volatile("outb %0, %1" : : "a"((uint8)c), "Nd"((uint16)0xE9));
}
void dbg_putc(char c) {
    dbg_putc_local(c);
}
static void dbg_puts(const char *s) {
    while (*s) {
        asm volatile("outb %0, %1" : : "a"((uint8)*s), "Nd"((uint16)0xE9));
        s++;
    }
}
static void dbg_hex(uint64 v) {
    static const char h[] = "0123456789abcdef";
    dbg_puts("0x");
    for (int i = 15; i >= 0; i--) {
        char c = h[(v >> (i * 4)) & 0xF];
        asm volatile("outb %0, %1" : : "a"((uint8)c), "Nd"((uint16)0xE9));
    }
}

/* ══════════════════════════════════════════════════════════════
 *  Timer tick advance (called from trap handler)
 * ══════════════════════════════════════════════════════════════ */
void timer_tick_advance(void) {
    __atomic_fetch_add(&jiffies_count, 1, __ATOMIC_RELAXED);
    /* Drive the scheduler software-timer tree */
    sched_timer_tick();
}

/* Re-arm LAPIC timer for TSC-deadline mode (called from trap handler) */
void lapic_timer_rearm(void) {
    if (active_timer == TIMER_LAPIC_TSC_DEADLINE) {
        uint64 interval = tsc_freq_hz / TIMER_HZ;
        wrmsr(MSR_TSC_DEADLINE, rdtsc() + interval);
    }
}

/* ══════════════════════════════════════════════════════════════
 *  CPUID helpers
 * ══════════════════════════════════════════════════════════════ */
static void x86_cpuid(uint32 func, uint32 sub,
                       uint32 *eax, uint32 *ebx, uint32 *ecx, uint32 *edx) {
    asm volatile("cpuid"
                 : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
                 : "a"(func), "c"(sub));
}

static int cpu_has_tsc(void) {
    uint32 a, b, c, d;
    x86_cpuid(1, 0, &a, &b, &c, &d);
    return (d >> 4) & 1;            /* CPUID.01H:EDX.TSC[bit 4] */
}

static int cpu_has_apic(void) {
    uint32 a, b, c, d;
    x86_cpuid(1, 0, &a, &b, &c, &d);
    return (d >> 9) & 1;            /* CPUID.01H:EDX.APIC[bit 9] */
}

static int cpu_has_tsc_deadline(void) {
    uint32 a, b, c, d;
    x86_cpuid(1, 0, &a, &b, &c, &d);
    return (c >> 24) & 1;           /* CPUID.01H:ECX.TSC-Deadline[bit 24] */
}

static int cpu_has_invariant_tsc(void) {
    uint32 a, b, c, d;
    x86_cpuid(0x80000000, 0, &a, &b, &c, &d);
    if (a < 0x80000007)
        return 0;
    x86_cpuid(0x80000007, 0, &a, &b, &c, &d);
    return (d >> 8) & 1;            /* CPUID.80000007H:EDX.InvTSC[bit 8] */
}

/* ══════════════════════════════════════════════════════════════
 *  HPET-based calibration.
 *
 *  The HPET's frequency is self-describing: the capabilities
 *  register contains the period in femtoseconds.  We use the
 *  HPET main counter as the reference clock to measure TSC and
 *  LAPIC bus frequencies — no PIT needed at all.
 *
 *  Calibration window: ~50 ms worth of HPET ticks.
 * ══════════════════════════════════════════════════════════════ */
#define CAL_MS  50      /* calibration window in milliseconds */

static inline uint64 hpet_read_counter(void) {
    return hpet_base_ptr[HPET_REG_COUNTER / 8];
}

/* Ensure HPET main counter is running (free-run, no legacy mode yet) */
static void hpet_start_freerun(void) {
    /* Disable legacy replacement, just enable the main counter */
    hpet_base_ptr[HPET_REG_CONFIG / 8] = HPET_CFG_ENABLE;
}

/* ── Calibrate TSC frequency using HPET ── */
static uint64 calibrate_tsc_freq(void) {
    dbg_puts("[CAL] TSC...");
    hpet_start_freerun();

    uint64 cal_ticks = (hpet_freq_hz * CAL_MS) / 1000;

    uint64 hpet_start = hpet_read_counter();
    uint64 tsc_start  = rdtsc();

    /* Spin until HPET has advanced by cal_ticks */
    while ((hpet_read_counter() - hpet_start) < cal_ticks)
        ;

    uint64 tsc_end = rdtsc();
    uint64 tsc_delta = tsc_end - tsc_start;

    dbg_puts("done\n");
    /* TSC_freq = tsc_delta * hpet_freq / cal_ticks */
    return tsc_delta * hpet_freq_hz / cal_ticks;
}

/* ── Calibrate LAPIC bus frequency using HPET ── */
static uint64 calibrate_lapic_freq(void) {
    dbg_puts("[CAL] LAPIC...");
    hpet_start_freerun();

    /* Set LAPIC timer: divide-by-1, masked, max initial count */
    lapic_write(LAPIC_TIMER_DCR, LAPIC_TDCR_1);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    lapic_write(LAPIC_TIMER_ICR, 0xFFFFFFFF);

    uint64 cal_ticks = (hpet_freq_hz * CAL_MS) / 1000;
    uint64 hpet_start = hpet_read_counter();

    /* Spin until HPET has advanced by cal_ticks */
    while ((hpet_read_counter() - hpet_start) < cal_ticks)
        ;

    /* Read remaining LAPIC count (it counts DOWN) */
    uint32 remaining = lapic_read(LAPIC_TIMER_CCR);
    uint32 elapsed   = 0xFFFFFFFF - remaining;

    /* Stop LAPIC timer */
    lapic_write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);

    dbg_puts("done\n");
    /* bus_freq = elapsed_lapic_ticks * hpet_freq / cal_ticks */
    return (uint64)elapsed * hpet_freq_hz / cal_ticks;
}

/* ══════════════════════════════════════════════════════════════
 *  HPET probe
 * ══════════════════════════════════════════════════════════════ */
static int hpet_probe(void) {
    hpet_base_ptr = (volatile uint64 *)HPET_BASE;

    uint64 cap = hpet_base_ptr[HPET_REG_CAP_ID / 8];
    if (cap == 0 || cap == ~0ULL)
        return 0;   /* no HPET */

    hpet_period_fs = cap >> 32;         /* bits 63:32 = CLK_PERIOD in fs */
    if (hpet_period_fs == 0 || hpet_period_fs > 100000000UL)
        return 0;   /* unreasonable period (> 100 ns) */

    int has_legacy  = (cap >> 15) & 1;
    int num_timers  = ((cap >> 8) & 0x1F) + 1;

    hpet_freq_hz = 1000000000000000ULL / hpet_period_fs;

    printf("[x86] HPET: period=%ld fs  freq=%ld Hz  timers=%d  legacy=%d\n",
           hpet_period_fs, hpet_freq_hz, num_timers, has_legacy);

    return has_legacy;  /* only use HPET if legacy-replacement capable */
}

/* ══════════════════════════════════════════════════════════════
 *  Timer setup — one function per source
 * ══════════════════════════════════════════════════════════════ */

/* ── 1. LAPIC TSC-deadline mode ── */
static void setup_lapic_tsc_deadline(void) {
    lapic_write(LAPIC_LVT_TIMER,
                LAPIC_TIMER_TSCDEADLINE | LAPIC_TIMER_VEC);

    /* Arm first deadline: now + one period */
    uint64 interval = tsc_freq_hz / TIMER_HZ;
    wrmsr(MSR_TSC_DEADLINE, rdtsc() + interval);

    active_timer = TIMER_LAPIC_TSC_DEADLINE;
    tick_hz = TIMER_HZ;
}

/* ── 2. LAPIC periodic mode ── */
static void setup_lapic_periodic(void) {
    lapic_periodic_icr = (uint32)(lapic_bus_freq / TIMER_HZ);

    lapic_write(LAPIC_TIMER_DCR, LAPIC_TDCR_1);
    lapic_write(LAPIC_LVT_TIMER, LAPIC_TIMER_PERIODIC | LAPIC_TIMER_VEC);
    lapic_write(LAPIC_TIMER_ICR, lapic_periodic_icr);

    dbg_puts("[LAPIC] bus_freq=");
    dbg_hex(lapic_bus_freq);
    dbg_puts(" ICR=");
    dbg_hex((uint64)lapic_periodic_icr);
    dbg_puts("\n");

    /* Read-back to verify */
    dbg_puts("[LAPIC] readback LVT=");
    dbg_hex((uint64)lapic_read(LAPIC_LVT_TIMER));
    dbg_puts(" CCR=");
    dbg_hex((uint64)lapic_read(LAPIC_TIMER_CCR));
    dbg_puts(" DCR=");
    dbg_hex((uint64)lapic_read(LAPIC_TIMER_DCR));
    dbg_puts(" SVR=");
    dbg_hex((uint64)lapic_read(LAPIC_SVR));
    dbg_puts(" TPR=");
    dbg_hex((uint64)lapic_read(LAPIC_TPR));
    dbg_puts("\n");

    active_timer = TIMER_LAPIC_PERIODIC;
    tick_hz = TIMER_HZ;
}

/* ── 3. HPET (legacy replacement → IRQ 0) ── */
static void setup_hpet(void) {
    /* Stop HPET */
    hpet_base_ptr[HPET_REG_CONFIG / 8] = 0;

    /* Reset main counter */
    hpet_base_ptr[HPET_REG_COUNTER / 8] = 0;

    /* Check Timer 0 supports periodic mode */
    uint64 t0_cap = hpet_base_ptr[HPET_REG_T0_CONFIG / 8];
    if (!(t0_cap & HPET_TN_PER_CAP)) {
        printf("[x86] HPET Timer 0 lacks periodic capability, skipping\n");
        return;
    }

    /* Timer 0: periodic, enable interrupt, set accumulator */
    uint64 period_ticks = hpet_freq_hz / TIMER_HZ;
    hpet_base_ptr[HPET_REG_T0_CONFIG / 8] = HPET_TN_ENABLE
                                           | HPET_TN_PERIODIC
                                           | HPET_TN_SETVAL;
    hpet_base_ptr[HPET_REG_T0_CMP / 8] = period_ticks;

    /* Enable HPET + legacy replacement (Timer 0 → IRQ 0) */
    hpet_base_ptr[HPET_REG_CONFIG / 8] = HPET_CFG_ENABLE | HPET_CFG_LEGACY;

    active_timer = TIMER_HPET;
    tick_hz = TIMER_HZ;
}

/* ── 4. PIT 8254 (fallback) ── */
static void setup_pit(void) {
    /* Channel 0, lo/hi access, mode 3 (square wave), binary */
    outb(PIT_CMD, 0x36);
    outb(PIT_CH0_DATA, (uint8)(PIT_DIVISOR & 0xFF));
    outb(PIT_CH0_DATA, (uint8)((PIT_DIVISOR >> 8) & 0xFF));

    active_timer = TIMER_PIT;
    tick_hz = PIT_HZ;
}



/* ══════════════════════════════════════════════════════════════
 *  r_time() — monotonic time (TSC-based or jiffies-based)
 * ══════════════════════════════════════════════════════════════ */
uint64 x86_r_time(void) {
    if (have_tsc)
        return rdtsc();
    /* Fallback: jiffies at virtual 10 MHz timebase */
    return get_jiffs() * (__timebase_frequency / tick_hz);
}

/* ══════════════════════════════════════════════════════════════
 *  Query functions (used by trap.c / arch_irq_init)
 * ══════════════════════════════════════════════════════════════ */

/* Returns non-zero if the tick interrupt uses IOAPIC IRQ 0 */
int arch_timer_needs_ioapic_irq0(void) {
    return active_timer == TIMER_PIT || active_timer == TIMER_HPET;
}

/* Returns non-zero if the tick interrupt uses LAPIC timer vector */
int arch_timer_is_lapic(void) {
    return active_timer == TIMER_LAPIC_PERIODIC
        || active_timer == TIMER_LAPIC_TSC_DEADLINE;
}

/* ══════════════════════════════════════════════════════════════
 *  arch_timer_init — timer selection and setup
 *
 *  Called from arch_irq_init() after LAPIC and IOAPIC are up.
 *  Interrupts are still disabled at this point (boot-time).
 * ══════════════════════════════════════════════════════════════ */
void arch_timer_init(void) {
    int feat_tsc       = cpu_has_tsc();
    int feat_apic      = cpu_has_apic();
    int feat_deadline  = cpu_has_tsc_deadline();
    int feat_inv_tsc   = cpu_has_invariant_tsc();

    dbg_puts("[TIMER] CPUID: tsc=");
    dbg_putc(feat_tsc ? '1' : '0');
    dbg_puts(" apic=");
    dbg_putc(feat_apic ? '1' : '0');
    dbg_puts(" deadline=");
    dbg_putc(feat_deadline ? '1' : '0');
    dbg_puts(" inv_tsc=");
    dbg_putc(feat_inv_tsc ? '1' : '0');
    dbg_puts("\n");

    /* Probe HPET first — it serves as our calibration reference */
    int feat_hpet = hpet_probe();

    printf("[x86] CPUID: TSC=%d  APIC=%d  TSC-deadline=%d  InvariantTSC=%d  HPET=%d\n",
           feat_tsc, feat_apic, feat_deadline, feat_inv_tsc, feat_hpet);

    /* We need HPET to calibrate TSC and LAPIC.  If no HPET, fall back
     * to PIT as tick source (no calibration needed for PIT). */
    if (!feat_hpet) {
        dbg_puts("[TIMER] no HPET, falling back to PIT\n");
        setup_pit();
        goto done;
    }

    /* ── Calibrate TSC if present ── */
    if (feat_tsc) {
        have_tsc = 1;
        tsc_freq_hz = calibrate_tsc_freq();
        printf("[x86] TSC: %ld Hz  (%ld MHz)\n",
               tsc_freq_hz, tsc_freq_hz / 1000000);

        /* Switch timebase to TSC */
        __timebase_frequency = tsc_freq_hz;
        __jiff_ticks = tsc_freq_hz / HZ;
    }

    /* ── 1. Try LAPIC timer in TSC-deadline mode ── */
    if (feat_deadline && have_tsc) {
        if (feat_apic) {
            lapic_bus_freq = calibrate_lapic_freq();
            printf("[x86] LAPIC bus: %ld Hz  (%ld MHz)\n",
                   lapic_bus_freq, lapic_bus_freq / 1000000);
        }
        setup_lapic_tsc_deadline();
        goto done;
    }

    /* ── 2. Try LAPIC timer in periodic mode ── */
    if (feat_apic) {
        lapic_bus_freq = calibrate_lapic_freq();
        if (lapic_bus_freq > 0) {
            printf("[x86] LAPIC bus: %ld Hz  (%ld MHz)\n",
                   lapic_bus_freq, lapic_bus_freq / 1000000);
            setup_lapic_periodic();
            goto done;
        }
    }

    /* ── 3. Try HPET as tick source ── */
    setup_hpet();
    if (active_timer == TIMER_HPET)
        goto done;

    /* ── 4. PIT (last resort) ── */
    setup_pit();

done:
    printf("[x86] Timer: %s @ %d Hz\n",
           timer_source_names[active_timer], tick_hz);
}

void arch_timer_init_hart(void) {
    /* Per-CPU timer setup for secondary CPUs */
    if (active_timer == TIMER_LAPIC_PERIODIC)
        setup_lapic_periodic();
    else if (active_timer == TIMER_LAPIC_TSC_DEADLINE)
        setup_lapic_tsc_deadline();
    /* HPET / PIT are global — no per-CPU setup needed */
}

/* ══════════════════════════════════════════════════════════════
 *  Red-black-tree based software timer infrastructure
 *  (unchanged from the original — used by sched_timer.c)
 * ══════════════════════════════════════════════════════════════ */

static int __timer_root_keys_cmp_fun(uint64 key1, uint64 key2) {
    struct timer_node *node1 = (struct timer_node *)key1;
    struct timer_node *node2 = (struct timer_node *)key2;
    if (node1->expires < node2->expires)
        return -1;
    else if (node1->expires > node2->expires)
        return 1;
    else if (key1 < key2)
        return -1;
    else if (key1 > key2)
        return 1;
    else
        return 0;
}

static uint64 __timer_root_get_key_fun(struct rb_node *node) {
    assert(node != NULL, "node is NULL");
    struct timer_node *timer_node = container_of(node, struct timer_node, rb);
    return (uint64)timer_node;
}

static struct rb_root_opts __timer_root_opts = {
    .keys_cmp_fun = __timer_root_keys_cmp_fun,
    .get_key_fun = __timer_root_get_key_fun,
};

static void __timer_update_next_tick(struct timer_root *timer) {
    struct timer_node *next =
        LIST_FIRST_NODE(&timer->list_head, struct timer_node, list_entry);
    if (next != NULL)
        timer->next_tick = next->expires;
    else
        timer->next_tick = 0;
}

void timer_init(struct timer_root *timer) {
    if (timer == NULL)
        return;
    memset(timer, 0, sizeof(struct timer_root));
    rb_root_init(&timer->root, &__timer_root_opts);
    list_entry_init(&timer->list_head);
    timer->next_tick = 0;
    timer->current_tick = 0;
    timer->valid = 1;
    spin_init(&timer->lock, "timer_lock");
}

void timer_node_init(struct timer_node *node, uint64 expires,
                     void (*callback)(struct timer_node *), void *data,
                     int retry_limit) {
    if (node == NULL)
        return;
    memset(node, 0, sizeof(struct timer_node));
    rb_node_init(&node->rb);
    list_entry_init(&node->list_entry);
    node->expires = expires;
    node->callback = callback;
    node->data = data;
    node->retry_limit = retry_limit;
}

int timer_add(struct timer_root *timer, struct timer_node *node) {
    if (timer == NULL || node == NULL)
        return -1;
    if (node->callback == NULL)
        return -1;
    spin_lock(&timer->lock);
    if (!timer->valid) {
        spin_unlock(&timer->lock);
        return -1;
    }
    if (timer->current_tick >= node->expires) {
        spin_unlock(&timer->lock);
        return -1;
    }
    struct rb_node *inserted = rb_insert_color(&timer->root, &node->rb);
    if (inserted == NULL) {
        spin_unlock(&timer->lock);
        return -1;
    }
    if (inserted != &node->rb) {
        spin_unlock(&timer->lock);
        return -1;
    }
    struct rb_node *prev = rb_prev_node(&node->rb);
    if (prev == NULL) {
        list_node_push_back(&timer->list_head, node, list_entry);
        timer->next_tick = node->expires;
    } else {
        struct timer_node *prev_node =
            container_of(prev, struct timer_node, rb);
        list_node_insert(prev_node, node, list_entry);
    }
    node->timer = timer;
    spin_unlock(&timer->lock);
    return 0;
}

static void __timer_remove_unlocked(struct timer_root *timer,
                                    struct timer_node *node) {
    rb_delete_node_color(&timer->root, &node->rb);
    list_node_detach(node, list_entry);
    node->timer = NULL;
    __timer_update_next_tick(timer);
}

void timer_remove(struct timer_node *node) {
    struct timer_root *timer = node->timer;
    if (timer == NULL)
        return;
    spin_lock(&timer->lock);
    __timer_remove_unlocked(timer, node);
    spin_unlock(&timer->lock);
}

void timer_tick(struct timer_root *timer, uint64 ticks) {
    if (timer == NULL || ticks == 0)
        return;
    if (timer->valid == 0)
        return;
    spin_lock(&timer->lock);
    if (timer->next_tick == 0) {
        spin_unlock(&timer->lock);
        return;
    }
    if (timer->current_tick >= ticks) {
        spin_unlock(&timer->lock);
        return;
    }
    timer->current_tick = ticks;
    if (timer->next_tick > ticks) {
        spin_unlock(&timer->lock);
        return;
    }

    struct timer_node *node, *next;
    list_foreach_node_safe(&timer->list_head, node, next, list_entry) {
        if (node->expires > ticks)
            break;
        if (node->callback == NULL) {
            printf("Warning: Timer expired without callback\n");
            __timer_remove_unlocked(node->timer, node);
            continue;
        }
        node->retry++;
        if (node->retry >= node->retry_limit) {
            __timer_remove_unlocked(node->timer, node);
        }
        node->callback(node);
    }

    spin_unlock(&timer->lock);
}

/* ══════════════════════════════════════════════════════════════
 *  Wall-clock time via CMOS RTC + r_time()
 *
 *  goldfish_rtc_read_ns/sec are called by sys_clock_gettime
 *  (CLOCK_REALTIME and CLOCK_MONOTONIC) and sys_gettimeofday.
 *  On x86 we read the CMOS RTC at boot to seed the wall clock,
 *  then track elapsed time via r_time() (TSC or jiffies-based).
 * ══════════════════════════════════════════════════════════════ */

/* CMOS RTC I/O ports */
#define CMOS_ADDR   0x70
#define CMOS_DATA   0x71

/* CMOS register indices */
#define CMOS_SEC    0x00
#define CMOS_MIN    0x02
#define CMOS_HOUR   0x04
#define CMOS_DAY    0x07
#define CMOS_MON    0x08
#define CMOS_YEAR   0x09
#define CMOS_STATUS_A   0x0A
#define CMOS_STATUS_B   0x0B

static uint64 boot_epoch_ns;           /* CMOS time at boot (nanoseconds) */
static uint64 boot_ticks;              /* r_time() snapshot at boot time  */
static int    rtc_seeded;

static uint8 cmos_read(uint8 reg)
{
    outb(CMOS_ADDR, reg);
    return inb(CMOS_DATA);
}

static uint8 bcd_to_bin(uint8 val)
{
    return (val & 0x0F) + ((val >> 4) * 10);
}

/* Days in each month (non-leap year) */
static int days_in_month(int month, int year)
{
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (month == 2 && ((year % 4 == 0 && year % 100 != 0) || year % 400 == 0))
        return 29;
    return dm[month - 1];
}

/* Convert broken-down UTC time to seconds since Unix epoch */
static uint64 mktime_utc(int year, int mon, int day, int hour, int min, int sec)
{
    uint64 days = 0;
    for (int y = 1970; y < year; y++) {
        days += 365;
        if ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)
            days++;
    }
    for (int m = 1; m < mon; m++)
        days += days_in_month(m, year);
    days += day - 1;
    return days * 86400ULL + hour * 3600ULL + min * 60ULL + sec;
}

/*
 * cmos_read_epoch_ns — read the CMOS RTC and convert to nanoseconds
 * since Unix epoch.  Waits for the UIP (Update In Progress) bit to
 * clear, then reads a consistent set of date/time registers.
 */
static uint64 cmos_read_epoch_ns(void)
{
    /* Wait until RTC is not updating */
    while (cmos_read(CMOS_STATUS_A) & 0x80)
        ;

    uint8 sec  = cmos_read(CMOS_SEC);
    uint8 min  = cmos_read(CMOS_MIN);
    uint8 hour = cmos_read(CMOS_HOUR);
    uint8 day  = cmos_read(CMOS_DAY);
    uint8 mon  = cmos_read(CMOS_MON);
    uint8 year = cmos_read(CMOS_YEAR);
    uint8 status_b = cmos_read(CMOS_STATUS_B);

    /* Convert BCD → binary if needed (bit 2 of status B = binary mode) */
    if (!(status_b & 0x04)) {
        sec  = bcd_to_bin(sec);
        min  = bcd_to_bin(min);
        hour = bcd_to_bin(hour);
        day  = bcd_to_bin(day);
        mon  = bcd_to_bin(mon);
        year = bcd_to_bin(year);
    }

    /* CMOS only stores 2-digit year; assume 2000+ */
    int full_year = 2000 + year;

    return mktime_utc(full_year, mon, day, hour, min, sec) * 1000000000ULL;
}

/*
 * ticks_to_ns — convert r_time() delta to nanoseconds.
 *
 * r_time() returns ticks at __timebase_frequency Hz.
 * To avoid overflow on large tick counts we split the conversion:
 *   ns = ticks / freq * 1e9  +  (ticks % freq) * 1e9 / freq
 */
static uint64 ticks_to_ns(uint64 ticks)
{
    uint64 freq = __timebase_frequency;
    uint64 whole_sec = ticks / freq;
    uint64 frac_ticks = ticks % freq;
    return whole_sec * 1000000000ULL + frac_ticks * 1000000000ULL / freq;
}

static void rtc_seed(void)
{
    if (rtc_seeded)
        return;
    boot_epoch_ns = cmos_read_epoch_ns();
    boot_ticks = x86_r_time();
    rtc_seeded = 1;
}

void goldfish_rtc_init(void)
{
    rtc_seed();
}

uint64 goldfish_rtc_read_ns(void)
{
    if (!rtc_seeded)
        rtc_seed();
    uint64 elapsed = x86_r_time() - boot_ticks;
    return boot_epoch_ns + ticks_to_ns(elapsed);
}

uint64 goldfish_rtc_read_sec(void)
{
    return goldfish_rtc_read_ns() / 1000000000ULL;
}

void   goldfish_rtc_set_alarm_ns(uint64 ns)    { (void)ns; }
void   goldfish_rtc_set_alarm_sec(uint64 sec)   { (void)sec; }
void   goldfish_rtc_clear_alarm(void) {}
void   goldfish_rtc_irq_enable(int enable)     { (void)enable; }
uint64 goldfish_rtc_get_alarm_count(void) { return 0; }
