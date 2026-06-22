#ifndef __KERNEL_DEV_EVDEV_H
#define __KERNEL_DEV_EVDEV_H

#include <types.h>
#include <dev/ps2kbd.h>
#include <dev/ps2mouse.h>

void evdev_init(void);
void evdev_keyboard_event(const struct kbd_event *ev);
void evdev_pointer_event(const struct mouse_event *ev);

#endif /* __KERNEL_DEV_EVDEV_H */
