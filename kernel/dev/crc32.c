/*
 * CRC32 (ISO 3309 / ITU-T V.42) — table-driven implementation.
 * Used for GPT header and partition array validation.
 *
 * Polynomial: 0xEDB88320   (reflected representation of 0x04C11DB7)
 */

#include <types.h>

static uint32 __crc32_table[256];
static int    __crc32_table_ready = 0;

static void __crc32_init_table(void) {
    for (uint32 i = 0; i < 256; i++) {
        uint32 crc = i;
        for (int j = 0; j < 8; j++) {
            if (crc & 1)
                crc = (crc >> 1) ^ 0xEDB88320U;
            else
                crc >>= 1;
        }
        __crc32_table[i] = crc;
    }
    __crc32_table_ready = 1;
}

uint32 crc32(uint32 crc, const void *buf, size_t len) {
    if (!__crc32_table_ready)
        __crc32_init_table();

    const uint8 *p = (const uint8 *)buf;
    crc ^= 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++)
        crc = __crc32_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFU;
}
