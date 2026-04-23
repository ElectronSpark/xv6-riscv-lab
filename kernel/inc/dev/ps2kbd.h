#ifndef __KERNEL_DEV_PS2KBD_H
#define __KERNEL_DEV_PS2KBD_H

#include <types.h>

/*
 * PS/2 keyboard driver — exposes /dev/kbd as a character device.
 *
 * Reads return struct kbd_event: a single key press/release.
 */

/* PS/2 ports (shared with mouse) */
#define PS2_DATA_PORT    0x60
#define PS2_STATUS_PORT  0x64
#define PS2_CMD_PORT     0x64

/* Keyboard IRQ (ISA IRQ 1) */
#define KBD_IRQ  1

/* Device numbers */
#define KBD_MAJOR  14
#define KBD_MINOR   0

/* Keyboard event structure (read from /dev/kbd) */
struct kbd_event {
    uint8  keycode;     /* ASCII code (0 if non-printable) */
    uint8  scancode;    /* raw PS/2 scancode */
    uint8  pressed;     /* 1 = press, 0 = release */
    uint8  modifiers;   /* bit0=shift, bit1=ctrl, bit2=alt */
};

/* Modifier bit masks */
#define KBD_MOD_SHIFT  0x01
#define KBD_MOD_CTRL   0x02
#define KBD_MOD_ALT    0x04

/* Special (non-ASCII) key codes (0x80+) */
#define KBD_KEY_UP      0x80
#define KBD_KEY_DOWN    0x81
#define KBD_KEY_LEFT    0x82
#define KBD_KEY_RIGHT   0x83
#define KBD_KEY_HOME    0x84
#define KBD_KEY_END     0x85
#define KBD_KEY_PGUP    0x86
#define KBD_KEY_PGDN    0x87
#define KBD_KEY_INSERT  0x88
#define KBD_KEY_DELETE  0x89

void ps2kbd_init(void);

/* Process one scancode byte read from the i8042 data port.
 * Exposed so the mouse IRQ handler can dispatch keyboard data
 * that arrives while it holds the controller (the i8042 has a
 * single output buffer shared by both devices). */
void ps2kbd_handle_byte(uint8 byte);

#endif /* __KERNEL_DEV_PS2KBD_H */
