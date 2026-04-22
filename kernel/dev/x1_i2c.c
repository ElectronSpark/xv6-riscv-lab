#include "types.h"
#include "defs.h"
#include "printf.h"
#include "string.h"
#include "riscv.h"
#include "errno.h"
#include "dev/fdt.h"
#include "dev/x1_i2c.h"
#include "lock/spinlock.h"

enum {
    REG_CR = 0x00,
    REG_SR = 0x04,
    REG_DBR = 0x0c,
    REG_LCR = 0x10,
    REG_WCR = 0x14,
    REG_RST_CYC = 0x18,
    REG_BMR = 0x1c,
};

enum {
    CR_START = 1U << 0,
    CR_STOP = 1U << 1,
    CR_ACKNAK = 1U << 2,
    CR_TB = 1U << 3,
    CR_GPIOEN = 1U << 6,
    CR_MODE_FAST = 1U << 8,
    CR_MODE_HIGH = 1U << 9,
    CR_UR = 1U << 10,
    CR_RSTREQ = 1U << 11,
    CR_SCLE = 1U << 13,
    CR_IUE = 1U << 14,
    CR_ALDIE = 1U << 18,
    CR_DTEIE = 1U << 19,
    CR_DRFIE = 1U << 20,
    CR_GCD = 1U << 21,
    CR_BEIE = 1U << 22,
    CR_MSDIE = 1U << 25,
    CR_MSDE = 1U << 26,
};

enum {
    SR_RWM = 1U << 13,
    SR_ACKNAK = 1U << 14,
    SR_UB = 1U << 15,
    SR_IBB = 1U << 16,
    SR_EBB = 1U << 17,
    SR_ALD = 1U << 18,
    SR_ITE = 1U << 19,
    SR_IRF = 1U << 20,
    SR_BED = 1U << 22,
    SR_MSD = 1U << 26,
};

enum {
    BMR_SDA = 1U << 0,
    BMR_SCL = 1U << 1,
};

#define X1_I2C_INT_STATUS_MASK 0xfffc0000U

enum x1_i2c_phase {
    X1_I2C_PHASE_MASTER_CODE,
    X1_I2C_PHASE_SLAVE_ADDR,
    X1_I2C_PHASE_BODY,
    X1_I2C_PHASE_IDLE,
};

struct x1_i2c_bus {
    spinlock_t lock;
    uint64 base;
    uint64 size;
    uint32 irq;
    uint32 adapter_id;
    uint32 clk_rate;
    uint32 lcr;
    uint32 wcr;
    uint32 apb_clock;
    uint8 fast_mode;
    uint8 high_mode;
    uint8 present;
};

struct x1_i2c_xfer_state {
    struct x1_i2c_msg *msgs;
    int num;
    int msg_idx;
    struct x1_i2c_msg *cur_msg;
    uint8 *msg_buf;
    int rx_cnt;
    int tx_cnt;
    int is_rx;
    int is_xfer_start;
    uint32 slave_addr_rw;
    enum x1_i2c_phase phase;
    uint32 status;
    uint32 err;
};

static struct x1_i2c_bus x1_i2c_buses[X1_I2C_MAX];
static int x1_i2c_initialized;

static inline uint32 x1_i2c_readl(struct x1_i2c_bus *bus, uint32 reg)
{
    return *(volatile uint32 *)(bus->base + reg);
}

static inline void x1_i2c_writel(struct x1_i2c_bus *bus, uint32 reg, uint32 val)
{
    *(volatile uint32 *)(bus->base + reg) = val;
}

static void x1_i2c_udelay(uint64 usec)
{
    uint64 hz = platform.timebase_freq ? platform.timebase_freq : 24000000ULL;
    uint64 start = r_time();
    uint64 ticks = (hz * usec + 999999ULL) / 1000000ULL;
    if (ticks == 0) {
        ticks = 1;
    }
    while ((uint64)(r_time() - start) < ticks) {
        __asm__ volatile("nop" ::: "memory");
    }
}

static void x1_i2c_clear_status(struct x1_i2c_bus *bus, uint32 mask)
{
    x1_i2c_writel(bus, REG_SR, mask & X1_I2C_INT_STATUS_MASK);
}

static void x1_i2c_controller_reset(struct x1_i2c_bus *bus)
{
    x1_i2c_writel(bus, REG_CR, CR_UR);
    x1_i2c_udelay(5);
    x1_i2c_writel(bus, REG_CR, 0);

    if (bus->lcr) {
        x1_i2c_writel(bus, REG_LCR, bus->lcr);
    }
    if (bus->wcr) {
        x1_i2c_writel(bus, REG_WCR, bus->wcr);
    }
}

static void x1_i2c_bus_reset(struct x1_i2c_bus *bus)
{
    int clk_cnt = 0;
    uint32 bmr = x1_i2c_readl(bus, REG_BMR);

    if (!(bmr & BMR_SDA) || !(bmr & BMR_SCL)) {
        x1_i2c_controller_reset(bus);
        x1_i2c_udelay(20);
    }

    while (clk_cnt < 9) {
        bmr = x1_i2c_readl(bus, REG_BMR);
        if (bmr & BMR_SDA) {
            break;
        }
        x1_i2c_writel(bus, REG_RST_CYC, 1);
        x1_i2c_writel(bus, REG_CR, CR_RSTREQ);
        x1_i2c_udelay(30);
        clk_cnt++;
    }
}

static void x1_i2c_unit_init(struct x1_i2c_bus *bus)
{
    uint32 cr = CR_BEIE | CR_ALDIE | CR_DRFIE | CR_GCD | CR_SCLE |
                CR_MSDE | CR_MSDIE;

    if (bus->fast_mode) {
        cr |= CR_MODE_FAST;
    }
    if (bus->high_mode) {
        cr |= CR_MODE_HIGH | CR_GPIOEN;
    }
    x1_i2c_writel(bus, REG_CR, cr);
}

static void x1_i2c_enable(struct x1_i2c_bus *bus)
{
    x1_i2c_writel(bus, REG_CR, x1_i2c_readl(bus, REG_CR) | CR_IUE);
}

static void x1_i2c_disable(struct x1_i2c_bus *bus)
{
    x1_i2c_writel(bus, REG_CR, x1_i2c_readl(bus, REG_CR) & ~CR_IUE);
}

static void x1_i2c_mark_rw(struct x1_i2c_xfer_state *st)
{
    if (st->cur_msg->flags & X1_I2C_M_RD) {
        st->is_rx = 1;
        st->slave_addr_rw = ((st->cur_msg->addr & 0x7f) << 1) | 1;
    } else {
        st->is_rx = 0;
        st->slave_addr_rw = (st->cur_msg->addr & 0x7f) << 1;
    }
}

static int x1_i2c_last_byte_to_send(struct x1_i2c_xfer_state *st)
{
    return st->tx_cnt == st->cur_msg->len && st->msg_idx == st->num - 1;
}

static int x1_i2c_last_byte_to_receive(struct x1_i2c_xfer_state *st)
{
    return st->rx_cnt == st->cur_msg->len - 1 && st->msg_idx == st->num - 1;
}

static void x1_i2c_trigger_byte_xfer(struct x1_i2c_bus *bus)
{
    uint32 cr = x1_i2c_readl(bus, REG_CR);
    cr &= ~CR_STOP;
    cr |= CR_START | CR_TB | CR_DTEIE;
    x1_i2c_writel(bus, REG_CR, cr);
}

static void x1_i2c_send_slave_addr(struct x1_i2c_bus *bus,
                                   struct x1_i2c_xfer_state *st)
{
    st->phase = X1_I2C_PHASE_SLAVE_ADDR;
    x1_i2c_writel(bus, REG_DBR, st->slave_addr_rw);
    x1_i2c_trigger_byte_xfer(bus);
}

static int x1_i2c_byte_xfer(struct x1_i2c_bus *bus,
                            struct x1_i2c_xfer_state *st);

static int x1_i2c_next_msg(struct x1_i2c_bus *bus,
                           struct x1_i2c_xfer_state *st)
{
    if (st->msg_idx >= st->num - 1) {
        return 0;
    }

    st->msg_idx++;
    st->cur_msg = &st->msgs[st->msg_idx];
    st->msg_buf = st->cur_msg->buf;
    st->rx_cnt = 0;
    st->tx_cnt = 0;
    st->err = 0;
    st->status = 0;
    st->phase = X1_I2C_PHASE_IDLE;
    x1_i2c_mark_rw(st);
    return x1_i2c_byte_xfer(bus, st);
}

static int x1_i2c_byte_xfer_body(struct x1_i2c_bus *bus,
                                 struct x1_i2c_xfer_state *st)
{
    uint32 cr = x1_i2c_readl(bus, REG_CR);
    cr &= ~(CR_TB | CR_ACKNAK | CR_STOP | CR_START);
    st->phase = X1_I2C_PHASE_BODY;

    if (st->status & SR_IRF) {
        if (!st->is_rx) {
            return 0;
        }

        if (st->rx_cnt < st->cur_msg->len) {
            *st->msg_buf++ = (uint8)x1_i2c_readl(bus, REG_DBR);
            st->rx_cnt++;
        }

        if (st->status & (SR_MSD | SR_ACKNAK)) {
            return 0;
        }

        if (st->rx_cnt < st->cur_msg->len) {
            if (x1_i2c_last_byte_to_receive(st)) {
                cr |= CR_STOP | CR_ACKNAK;
            }
            cr |= CR_ALDIE | CR_TB;
            x1_i2c_writel(bus, REG_CR, cr);
            return 0;
        }

        if (st->msg_idx < st->num - 1) {
            return x1_i2c_next_msg(bus, st);
        }
        return 0;
    }

    if (st->status & SR_ITE) {
        if (st->status & SR_MSD) {
            return 0;
        }

        if (st->status & SR_RWM) {
            if (!st->is_rx) {
                return 0;
            }

            if (x1_i2c_last_byte_to_receive(st)) {
                cr |= CR_STOP | CR_ACKNAK;
            }
            cr |= CR_ALDIE | CR_TB;
            cr &= ~CR_DTEIE;
            x1_i2c_writel(bus, REG_CR, cr);
            return 0;
        }

        if (st->is_rx) {
            return 0;
        }

        if (st->tx_cnt < st->cur_msg->len) {
            x1_i2c_writel(bus, REG_DBR, *st->msg_buf++);
            st->tx_cnt++;
            if (x1_i2c_last_byte_to_send(st)) {
                cr |= CR_STOP;
            }
            cr |= CR_ALDIE | CR_TB;
            x1_i2c_writel(bus, REG_CR, cr);
            return 0;
        }

        if (st->msg_idx < st->num - 1) {
            return x1_i2c_next_msg(bus, st);
        }
    }

    return 0;
}

static int x1_i2c_byte_xfer(struct x1_i2c_bus *bus,
                            struct x1_i2c_xfer_state *st)
{
    if (st->err) {
        return -1;
    }

    if (st->phase == X1_I2C_PHASE_IDLE) {
        x1_i2c_send_slave_addr(bus, st);
        st->is_xfer_start = 0;
        return 0;
    }

    if (st->phase == X1_I2C_PHASE_MASTER_CODE) {
        x1_i2c_send_slave_addr(bus, st);
        return 0;
    }

    return x1_i2c_byte_xfer_body(bus, st);
}

static int x1_i2c_timeout_us(struct x1_i2c_bus *bus,
                             struct x1_i2c_msg *msgs, int num)
{
    int cnt = 0;
    int freq = bus->high_mode ? 1500000 : (bus->fast_mode ? 400000 : 100000);

    for (int i = 0; i < num; i++) {
        cnt += msgs[i].len + 1;
    }

    if (cnt <= 0) {
        cnt = 1;
    }

    return (cnt * 9 * 1000000) / freq + (cnt * 220) + 500000;
}

static int x1_i2c_wait_not_busy(struct x1_i2c_bus *bus)
{
    int timeout = bus->high_mode ? 1000 : 1500;
    int loops = 100000 / timeout;

    while (x1_i2c_readl(bus, REG_SR) & (SR_UB | SR_IBB)) {
        if (loops-- <= 0) {
            x1_i2c_controller_reset(bus);
            return -EAGAIN;
        }
        x1_i2c_udelay(timeout);
    }
    return 0;
}

static int x1_i2c_pio_xfer_locked(struct x1_i2c_bus *bus,
                                  struct x1_i2c_msg *msgs, int num)
{
    struct x1_i2c_xfer_state st;
    int ret;
    int timeout;

    memset(&st, 0, sizeof(st));
    st.msgs = msgs;
    st.num = num;
    st.cur_msg = &msgs[0];
    st.msg_buf = st.cur_msg->buf;
    st.phase = X1_I2C_PHASE_IDLE;
    st.is_xfer_start = 1;
    x1_i2c_mark_rw(&st);

    ret = x1_i2c_wait_not_busy(bus);
    if (ret < 0) {
        x1_i2c_bus_reset(bus);
    }

    x1_i2c_controller_reset(bus);
    x1_i2c_udelay(2);
    x1_i2c_unit_init(bus);
    x1_i2c_clear_status(bus, X1_I2C_INT_STATUS_MASK);
    x1_i2c_enable(bus);

    ret = x1_i2c_byte_xfer(bus, &st);
    if (ret < 0) {
        x1_i2c_disable(bus);
        return -EINVAL;
    }

    timeout = x1_i2c_timeout_us(bus, msgs, num);
    while (timeout > 0) {
        uint32 status = x1_i2c_readl(bus, REG_SR);
        x1_i2c_clear_status(bus, status);
        st.status = status;
        st.err = status & (SR_BED | SR_ALD);

        if (st.err) {
            break;
        }

        if (status & SR_IRF) {
            ret = x1_i2c_byte_xfer(bus, &st);
            if (ret < 0) {
                break;
            }
        }
        if (status & SR_ITE) {
            ret = x1_i2c_byte_xfer(bus, &st);
            if (ret < 0) {
                break;
            }
        }
        if (status & SR_MSD) {
            break;
        }

        x1_i2c_udelay(10);
        timeout -= 10;
    }

    x1_i2c_disable(bus);

    if (timeout <= 0) {
        x1_i2c_bus_reset(bus);
        return -ETIMEDOUT;
    }

    if (st.err) {
        if (st.err & SR_ALD) {
            return -EAGAIN;
        }
        return (st.status & SR_ACKNAK) ? -ENXIO : -EIO;
    }

    return num;
}

static struct x1_i2c_bus *x1_i2c_get_bus(int bus_id)
{
    if (bus_id < 0 || bus_id >= X1_I2C_MAX) {
        return NULL;
    }
    if (!x1_i2c_initialized) {
        x1_i2c_init();
    }
    return x1_i2c_buses[bus_id].present ? &x1_i2c_buses[bus_id] : NULL;
}

int x1_i2c_init(void)
{
    int found = 0;

    if (x1_i2c_initialized) {
        for (int i = 0; i < X1_I2C_MAX; i++) {
            if (x1_i2c_buses[i].present) {
                found++;
            }
        }
        return found;
    }

    memset(x1_i2c_buses, 0, sizeof(x1_i2c_buses));
    for (int i = 0; i < platform.i2c_count && i < X1_I2C_MAX; i++) {
        int bus_id = (int)platform.i2c[i].adapter_id;
        if (bus_id < 0 || bus_id >= X1_I2C_MAX || platform.i2c[i].base == 0) {
            continue;
        }

        x1_i2c_buses[bus_id].base = platform.i2c[i].base;
        x1_i2c_buses[bus_id].size = platform.i2c[i].size;
        x1_i2c_buses[bus_id].irq = platform.i2c[i].irq;
        x1_i2c_buses[bus_id].adapter_id = platform.i2c[i].adapter_id;
        x1_i2c_buses[bus_id].clk_rate = platform.i2c[i].clk_rate;
        x1_i2c_buses[bus_id].lcr = platform.i2c[i].lcr;
        x1_i2c_buses[bus_id].wcr = platform.i2c[i].wcr;
        x1_i2c_buses[bus_id].apb_clock = platform.i2c[i].apb_clock;
        x1_i2c_buses[bus_id].fast_mode = platform.i2c[i].fast_mode;
        x1_i2c_buses[bus_id].high_mode = platform.i2c[i].high_mode;
        x1_i2c_buses[bus_id].present = 1;
        spin_init(&x1_i2c_buses[bus_id].lock, "x1_i2c");
        found++;
    }

    x1_i2c_initialized = 1;
    if (found > 0) {
        printf("x1_i2c: initialized %d bus(es)\n", found);
    }
    return found;
}

int x1_i2c_is_ready(int bus_id)
{
    return x1_i2c_get_bus(bus_id) != NULL;
}

int x1_i2c_xfer(int bus_id, struct x1_i2c_msg *msgs, int num)
{
    struct x1_i2c_bus *bus = x1_i2c_get_bus(bus_id);
    int ret;

    if (bus == NULL || msgs == NULL || num <= 0) {
        return -EINVAL;
    }

    spin_lock(&bus->lock);
    ret = x1_i2c_pio_xfer_locked(bus, msgs, num);
    spin_unlock(&bus->lock);
    return ret;
}

int x1_i2c_read_reg8(int bus_id, uint8 addr, uint8 reg, uint8 *val)
{
    uint8 regbuf = reg;
    int ret;
    struct x1_i2c_msg msgs[2] = {
        {.addr = addr, .flags = 0, .buf = &regbuf, .len = 1},
        {.addr = addr, .flags = X1_I2C_M_RD, .buf = val, .len = 1},
    };

    if (val == NULL) {
        return -EINVAL;
    }
    ret = x1_i2c_xfer(bus_id, msgs, 2);
    return ret < 0 ? ret : 0;
}

int x1_i2c_write_reg8(int bus_id, uint8 addr, uint8 reg, uint8 val)
{
    uint8 buf[2] = {reg, val};
    int ret;
    struct x1_i2c_msg msg = {
        .addr = addr,
        .flags = 0,
        .buf = buf,
        .len = 2,
    };

    ret = x1_i2c_xfer(bus_id, &msg, 1);
    return ret < 0 ? ret : 0;
}