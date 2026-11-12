#ifndef ATOMIC_H
#define ATOMIC_H
/* fma.c подключает этот файл и берёт отсюда a_clz_64. */
#include <stdint.h>
static inline int a_clz_64(uint64_t x) {
    int n = 0;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8; x <<= 8; }
    if (!(x >> 60)) { n += 4; x <<= 4; }
    if (!(x >> 62)) { n += 2; x <<= 2; }
    if (!(x >> 63)) n += 1;
    return n;
}
#endif
