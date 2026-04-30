#ifndef __KERNEL_DEV_PS2MOUSE_H
#define __KERNEL_DEV_PS2MOUSE_H

#include <types.h>

/*
 * PS/2 mouse driver — exposes /dev/mouse as a character device.
 *
 * Reads return 3-byte packets: [buttons, dx, dy]
 * Each packet is a struct mouse_event.
 */

#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

/* PS/2 controller commands */
#define PS2_CMD_READ_CONFIG     0x20
#define PS2_CMD_WRITE_CONFIG    0x60
#define PS2_CMD_DISABLE_PORT2   0xA7
#define PS2_CMD_ENABLE_PORT2    0xA8
#define PS2_CMD_WRITE_PORT2     0xD4

/* Mouse commands (sent via port 2) */
#define MOUSE_CMD_SET_DEFAULTS  0xF6
#define MOUSE_CMD_ENABLE        0xF4
#define MOUSE_CMD_RESET         0xFF
#define MOUSE_CMD_SET_SAMPLE    0xF3

/* Mouse IRQ (ISA IRQ 12) */
#define MOUSE_IRQ  12

/* Device numbers */
#define MOUSE_MAJOR  13
#define MOUSE_MINOR   0

/* Mouse event structure (read from /dev/mouse) */
struct mouse_event {
    int16  dx;      /* relative x movement, or absolute x (0-65535) */
    int16  dy;      /* relative y movement, or absolute y (0-65535) */
    uint8  buttons; /* button state: bit0=left, bit1=right, bit2=middle */
    uint8  flags;   /* bit0: 1 = absolute coordinates */
    int8   dz;      /* scroll wheel delta (negative=up, positive=down) */
    uint8  pad[1];  /* pad to 8 bytes */
};

#define MOUSE_EVENT_F_ABSOLUTE  0x01

void ps2mouse_init(void);
void mouse_input_push_event(const struct mouse_event *ev);

/* Process one byte read from the i8042 data port that the
 * controller marked as auxiliary (mouse) data.  Exposed so the
 * keyboard IRQ handler can dispatch mouse bytes that arrive
 * while it holds the controller (single shared output buffer). */
void ps2mouse_handle_byte(uint8 byte);

#endif /* __KERNEL_DEV_PS2MOUSE_H */
