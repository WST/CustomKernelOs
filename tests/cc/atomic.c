/* Атомики cc: __atomic_* и квалификатор _Atomic. */
#include <myos.h>
int main(void) {
    _Atomic int x;
    int exp;
    __atomic_store_n(&x, 3, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&x, __ATOMIC_SEQ_CST) != 3) return 1;
    if (__atomic_fetch_add(&x, 4, __ATOMIC_SEQ_CST) != 3) return 2;
    if (__atomic_load_n(&x, __ATOMIC_SEQ_CST) != 7) return 3;
    exp = 7;
    if (!__atomic_compare_exchange_n(&x, &exp, 9, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 4;
    if (__atomic_load_n(&x, __ATOMIC_SEQ_CST) != 9) return 5;
    exp = 1;
    if (__atomic_compare_exchange_n(&x, &exp, 2, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) return 6;
    if (exp != 9) return 7;
    if (__atomic_exchange_n(&x, 11, __ATOMIC_SEQ_CST) != 9) return 8;
    if (__atomic_fetch_or(&x, 4, __ATOMIC_SEQ_CST) != 11) return 9;
    if (__atomic_load_n(&x, __ATOMIC_SEQ_CST) != 15) return 10;
    {
        _Atomic(int) y;
        __atomic_store_n(&y, 1, __ATOMIC_RELAXED);
        if (__atomic_fetch_sub(&y, 1, __ATOMIC_RELAXED) != 1) return 11;
        if (__atomic_load_n(&y, __ATOMIC_RELAXED) != 0) return 12;
    }
    return 0;
}
