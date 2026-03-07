#ifndef XV6_COMPAT_UUID_UUID_H
#define XV6_COMPAT_UUID_UUID_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef unsigned char uuid_t[16];

void uuid_generate(uuid_t out);
void uuid_generate_random(uuid_t out);
void uuid_generate_time(uuid_t out);
int uuid_generate_time_safe(uuid_t out);

#ifdef __cplusplus
}
#endif

#endif
