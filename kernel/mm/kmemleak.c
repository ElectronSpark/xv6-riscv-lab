#include "types.h"
#include "string.h"
#include "defs.h"
#include "printf.h"
#include "lock/spinlock.h"
#include "cmdline.h"
#include <mm/kmemleak.h>

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#define KMEMLEAK_TABLE_SIZE 65536

struct kmemleak_entry {
    uint64 ptr;
    uint64 size;
    uint64 caller;
    uint64 seq;
    const char *kind;
    const char *tag;
    uint8 active;
};

static struct {
    spinlock_t lock;
    struct kmemleak_entry entries[KMEMLEAK_TABLE_SIZE];
    uint64 seq;
    uint64 allocs;
    uint64 frees;
    uint64 current;
    uint64 bytes;
    uint64 high_current;
    uint64 high_bytes;
    uint64 collisions;
    uint64 table_full;
    uint64 unknown_frees;
    int ready;
    int enabled;
} kmemleak = {
    .lock = SPINLOCK_INITIALIZED("kmemleak"),
};

static uint64 kmemleak_hash(uint64 ptr)
{
    ptr >>= 4;
    ptr ^= ptr >> 33;
    ptr *= 0xff51afd7ed558ccdULL;
    ptr ^= ptr >> 33;
    return ptr;
}

void kmemleak_init(void)
{
    char value[16];
    int enabled = 0;

#ifdef XV6_KASAN
    enabled = 1;
#endif
    if (cmdline_get_param("kmemleak", value, sizeof(value)) == 0) {
        if (cmdline_value_is_true(value))
            enabled = 1;
        else if (cmdline_value_is_false(value))
            enabled = 0;
    }

    spin_init(&kmemleak.lock, "kmemleak");
    kmemleak.enabled = enabled;
    __atomic_store_n(&kmemleak.ready, 1, __ATOMIC_RELEASE);
    if (enabled)
        printf("kmemleak: enabled fixed-table allocation tracker entries=%d\n",
               KMEMLEAK_TABLE_SIZE);
}

void kmemleak_alloc(const void *ptr, size_t size, const char *kind,
                    const char *tag, unsigned long caller)
{
    uint64 key = (uint64)ptr;
    uint64 start;

    if (key == 0 || size == 0 ||
        !__atomic_load_n(&kmemleak.ready, __ATOMIC_ACQUIRE) ||
        !kmemleak.enabled)
        return;

    start = kmemleak_hash(key) & (KMEMLEAK_TABLE_SIZE - 1);
    spin_lock(&kmemleak.lock);
    for (uint64 i = 0; i < KMEMLEAK_TABLE_SIZE; i++) {
        uint64 idx = (start + i) & (KMEMLEAK_TABLE_SIZE - 1);
        struct kmemleak_entry *e = &kmemleak.entries[idx];

        if (!e->active || e->ptr == key) {
            if (e->active && e->ptr == key) {
                if (kmemleak.bytes >= e->size)
                    kmemleak.bytes -= e->size;
                if (kmemleak.current != 0)
                    kmemleak.current--;
            }
            e->ptr = key;
            e->size = size;
            e->caller = caller;
            e->seq = ++kmemleak.seq;
            e->kind = kind != NULL ? kind : "?";
            e->tag = tag != NULL ? tag : "?";
            e->active = 1;
            kmemleak.allocs++;
            kmemleak.current++;
            kmemleak.bytes += size;
            if (kmemleak.current > kmemleak.high_current)
                kmemleak.high_current = kmemleak.current;
            if (kmemleak.bytes > kmemleak.high_bytes)
                kmemleak.high_bytes = kmemleak.bytes;
            spin_unlock(&kmemleak.lock);
            return;
        }
        kmemleak.collisions++;
    }
    kmemleak.table_full++;
    spin_unlock(&kmemleak.lock);
}

void kmemleak_free(const void *ptr)
{
    uint64 key = (uint64)ptr;
    uint64 start;

    if (key == 0 ||
        !__atomic_load_n(&kmemleak.ready, __ATOMIC_ACQUIRE) ||
        !kmemleak.enabled)
        return;

    start = kmemleak_hash(key) & (KMEMLEAK_TABLE_SIZE - 1);
    spin_lock(&kmemleak.lock);
    for (uint64 i = 0; i < KMEMLEAK_TABLE_SIZE; i++) {
        uint64 idx = (start + i) & (KMEMLEAK_TABLE_SIZE - 1);
        struct kmemleak_entry *e = &kmemleak.entries[idx];

        if (e->active && e->ptr == key) {
            e->active = 0;
            kmemleak.frees++;
            if (kmemleak.current != 0)
                kmemleak.current--;
            if (kmemleak.bytes >= e->size)
                kmemleak.bytes -= e->size;
            spin_unlock(&kmemleak.lock);
            return;
        }
        if (!e->active && e->ptr == 0)
            break;
    }
    kmemleak.unknown_frees++;
    spin_unlock(&kmemleak.lock);
}

size_t kmemleak_format(char *buf, size_t size)
{
    size_t pos = 0;
    uint64 shown = 0;

    if (buf == NULL || size == 0)
        return 0;

    spin_lock(&kmemleak.lock);
    pos += snprintf(buf + pos, pos < size ? size - pos : 0,
                    "enabled=%d current=%lu bytes=%lu high_current=%lu "
                    "high_bytes=%lu allocs=%lu frees=%lu collisions=%lu "
                    "table_full=%lu unknown_frees=%lu\n",
                    kmemleak.enabled, kmemleak.current, kmemleak.bytes,
                    kmemleak.high_current, kmemleak.high_bytes,
                    kmemleak.allocs, kmemleak.frees, kmemleak.collisions,
                    kmemleak.table_full, kmemleak.unknown_frees);

    if (!kmemleak.enabled) {
        pos += snprintf(buf + pos, pos < size ? size - pos : 0,
                        "hint=boot with kmemleak=1 or enable XV6_KASAN\n");
        goto out;
    }

    for (uint64 i = 0; i < KMEMLEAK_TABLE_SIZE && pos + 96 < size; i++) {
        struct kmemleak_entry *e = &kmemleak.entries[i];

        if (!e->active)
            continue;
        pos += snprintf(buf + pos, size - pos,
                        "leak seq=%lu ptr=0x%lx size=%lu kind=%s tag=%s "
                        "caller=0x%lx\n",
                        e->seq, e->ptr, e->size, e->kind, e->tag,
                        e->caller);
        shown++;
        if (shown >= 256) {
            pos += snprintf(buf + pos, pos < size ? size - pos : 0,
                            "truncated=1 shown=%lu\n", shown);
            break;
        }
    }

out:
    spin_unlock(&kmemleak.lock);
    if (pos >= size)
        pos = size - 1;
    buf[pos] = '\0';
    return pos;
}
