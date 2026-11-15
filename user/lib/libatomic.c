/* Атомарные операции для cc: одна глобальная блокировка на xchg.
 * Вытеснение не держит lock: чужой владелец отпускает его после yield.
 * На SMP xchg с памятью неявно имеет префикс lock, так что захват корректен. */
#include "myos.h"
#include "string.h"

static int atomic_lock;

static void acquire(void) {
    while (__myos_xchg(&atomic_lock, 1)) yield();
}
static void release(void) { __myos_xchg(&atomic_lock, 0); }

static unsigned long long load_raw(void *p, int size) {
    unsigned long long v = 0;
    memcpy(&v, p, size);
    return v;
}
static void store_raw(void *p, unsigned long long v, int size) { memcpy(p, &v, size); }

unsigned long long __myos_atomic_load(void *p, int size) {
    unsigned long long v;
    acquire();
    v = load_raw(p, size);
    release();
    return v;
}
void __myos_atomic_store(void *p, unsigned long long v, int size) {
    acquire();
    store_raw(p, v, size);
    release();
}
void __myos_atomic_copy(void *dst, void *src, int size) {
    acquire();
    memcpy(dst, src, size);
    release();
}
int __myos_atomic_cas(void *p, void *expected, void *desired, int size) {
    int ok;
    acquire();
    if (memcmp(p, expected, size) == 0) {
        memcpy(p, desired, size);
        ok = 1;
    } else {
        memcpy(expected, p, size);
        ok = 0;
    }
    release();
    return ok;
}
/* op: 1 add, 2 sub, 3 and, 4 or, 5 xor, 6 exchange. Возвращает старое значение. */
unsigned long long __myos_atomic_rmw(void *p, unsigned long long v, int size, int op) {
    unsigned long long old, now;
    acquire();
    old = load_raw(p, size);
    if (op == 1) now = old + v;
    else if (op == 2) now = old - v;
    else if (op == 3) now = old & v;
    else if (op == 4) now = old | v;
    else if (op == 5) now = old ^ v;
    else now = v;
    store_raw(p, now, size);
    release();
    return old;
}
float __myos_atomic_load_f32(void *p) {
    float v;
    acquire();
    memcpy(&v, p, 4);
    release();
    return v;
}
double __myos_atomic_load_f64(void *p) {
    double v;
    acquire();
    memcpy(&v, p, 8);
    release();
    return v;
}
void __myos_atomic_store_f32(void *p, float v) {
    acquire();
    memcpy(p, &v, 4);
    release();
}
void __myos_atomic_store_f64(void *p, double v) {
    acquire();
    memcpy(p, &v, 8);
    release();
}
void __myos_atomic_fence(void) {
    acquire();
    release();
}
