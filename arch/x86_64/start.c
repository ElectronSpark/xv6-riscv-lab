#include "types.h"

void start_kernel(int hartid, void *fdt_base, bool is_boot_hart);

static inline void outb(uint16 port, uint8 value) {
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

static inline uint8 inb(uint16 port) {
    uint8 value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_init(void) {
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8 + 0, 0x01);
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);
    outb(0x3F8 + 2, 0xC7);
    outb(0x3F8 + 4, 0x0B);
}

static void serial_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0) {
    }
    outb(0x3F8, (uint8)c);
}

static void serial_puts(const char *s) {
    while (*s) {
        if (*s == '\n') {
            serial_putc('\r');
        }
        serial_putc(*s++);
    }
}

static void debug_puts(const char *s) {
    while (*s) {
        outb(0xE9, (uint8)*s++);
    }
}

void start(void *boot_params) {
    serial_init();
    serial_puts("[xv6 x86_64] start(): entered C\n");
    debug_puts("[xv6 x86_64] start(): entered C\n");
    if (boot_params != 0) {
        serial_puts("[xv6 x86_64] boot params detected\n");
        debug_puts("[xv6 x86_64] boot params detected\n");
    }

    serial_puts("[xv6 x86_64] start(): calling start_kernel\n");
    debug_puts("[xv6 x86_64] start(): calling start_kernel\n");
    start_kernel(0, boot_params, true);

    serial_puts("[xv6 x86_64] start(): start_kernel returned\n");
    debug_puts("[xv6 x86_64] start(): start_kernel returned\n");
    for (;;) {
        asm volatile("hlt");
    }
}
