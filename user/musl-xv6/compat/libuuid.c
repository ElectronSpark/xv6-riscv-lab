#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "uuid/uuid.h"

#define UUID_EPOCH_OFFSET_100NS 0x01B21DD213814000ULL

static uint64_t uuid_counter;

static uint64_t
mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static uint64_t
next_entropy(uint64_t salt)
{
    struct timespec ts;
    uint64_t now;
    uint64_t pid_mix;
    uint64_t addr_mix;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
    }

    now = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    pid_mix = ((uint64_t)getpid() << 32) ^ (uint64_t)getppid();
    addr_mix = (uintptr_t)&ts ^ (uintptr_t)&salt ^ (uintptr_t)&uuid_counter;
    return mix64(now ^ pid_mix ^ addr_mix ^ salt ^ (++uuid_counter * 0x9e3779b97f4a7c15ULL));
}

static void
make_node(uint8_t node[6])
{
    uint64_t x = next_entropy(0x6e6f6465ULL);

    memcpy(node, &x, 6);
    node[0] |= 0x01;
}

void
uuid_generate_random(uuid_t out)
{
    uint64_t hi = next_entropy(0x72616e64ULL);
    uint64_t lo = next_entropy(0x6c6f7765ULL);

    memcpy(out, &hi, 8);
    memcpy(out + 8, &lo, 8);

    out[6] = (unsigned char)((out[6] & 0x0f) | 0x40);
    out[8] = (unsigned char)((out[8] & 0x3f) | 0x80);
}

void
uuid_generate(uuid_t out)
{
    uuid_generate_random(out);
}

void
uuid_generate_time(uuid_t out)
{
    struct timespec ts;
    uint64_t timestamp;
    uint16_t clock_seq;
    uint8_t node[6];

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        ts.tv_sec = time(NULL);
        ts.tv_nsec = 0;
    }

    timestamp = (uint64_t)ts.tv_sec * 10000000ULL;
    timestamp += (uint64_t)ts.tv_nsec / 100ULL;
    timestamp += UUID_EPOCH_OFFSET_100NS;
    timestamp += (uuid_counter++ & 0x0fffULL);

    clock_seq = (uint16_t)next_entropy(0x73657100ULL);
    make_node(node);

    out[0] = (unsigned char)(timestamp >> 24);
    out[1] = (unsigned char)(timestamp >> 16);
    out[2] = (unsigned char)(timestamp >> 8);
    out[3] = (unsigned char)(timestamp >> 0);
    out[4] = (unsigned char)(timestamp >> 40);
    out[5] = (unsigned char)(timestamp >> 32);
    out[6] = (unsigned char)(((timestamp >> 56) & 0x0f) | 0x10);
    out[7] = (unsigned char)(timestamp >> 48);
    out[8] = (unsigned char)(((clock_seq >> 8) & 0x3f) | 0x80);
    out[9] = (unsigned char)(clock_seq & 0xff);
    memcpy(out + 10, node, sizeof(node));
}

int
uuid_generate_time_safe(uuid_t out)
{
    uuid_generate_time(out);
    return 0;
}
