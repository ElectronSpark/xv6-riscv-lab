#ifndef __KERNEL_X1_I2C_H
#define __KERNEL_X1_I2C_H

#include "types.h"

#ifndef X1_I2C_MAX
#define X1_I2C_MAX 10
#endif

#define X1_I2C_M_RD 0x0001

struct x1_i2c_msg {
    uint16 addr;
    uint16 flags;
    uint8 *buf;
    uint16 len;
};

int x1_i2c_init(void);
int x1_i2c_is_ready(int bus_id);
int x1_i2c_xfer(int bus_id, struct x1_i2c_msg *msgs, int num);
int x1_i2c_read_reg8(int bus_id, uint8 addr, uint8 reg, uint8 *val);
int x1_i2c_write_reg8(int bus_id, uint8 addr, uint8 reg, uint8 val);

#endif /* __KERNEL_X1_I2C_H */