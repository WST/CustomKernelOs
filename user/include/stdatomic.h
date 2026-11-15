#ifndef STDATOMIC_H
#define STDATOMIC_H
/* Подмножество C11 <stdatomic.h>. Операции идут в __atomic_*,
 * которые cc опускает в вызовы libatomic с одной глобальной блокировкой.
 * Порядки памяти не различаются: блокировка даёт sequential consistency. */
#define __ATOMIC_RELAXED 0
#define __ATOMIC_CONSUME 1
#define __ATOMIC_ACQUIRE 2
#define __ATOMIC_RELEASE 3
#define __ATOMIC_ACQ_REL 4
#define __ATOMIC_SEQ_CST 5

typedef enum memory_order {
    memory_order_relaxed = __ATOMIC_RELAXED,
    memory_order_consume = __ATOMIC_CONSUME,
    memory_order_acquire = __ATOMIC_ACQUIRE,
    memory_order_release = __ATOMIC_RELEASE,
    memory_order_acq_rel = __ATOMIC_ACQ_REL,
    memory_order_seq_cst = __ATOMIC_SEQ_CST
} memory_order;

#define ATOMIC_VAR_INIT(v) (v)
#define atomic_init(p, v) atomic_store((p), (v))

typedef _Atomic _Bool atomic_bool;
typedef _Atomic char atomic_char;
typedef _Atomic int atomic_int;
typedef _Atomic unsigned atomic_uint;
typedef _Atomic long atomic_long;
typedef _Atomic unsigned long atomic_ulong;
typedef _Atomic long long atomic_llong;
typedef _Atomic unsigned long long atomic_ullong;
typedef _Atomic void *atomic_uintptr;

#define atomic_load(p) __atomic_load_n((p), __ATOMIC_SEQ_CST)
#define atomic_load_explicit(p, o) __atomic_load_n((p), (o))
#define atomic_store(p, v) __atomic_store_n((p), (v), __ATOMIC_SEQ_CST)
#define atomic_store_explicit(p, v, o) __atomic_store_n((p), (v), (o))
#define atomic_exchange(p, v) __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_add(p, v) __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_sub(p, v) __atomic_fetch_sub((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_and(p, v) __atomic_fetch_and((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_or(p, v) __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define atomic_fetch_xor(p, v) __atomic_fetch_xor((p), (v), __ATOMIC_SEQ_CST)
#define atomic_compare_exchange_strong(p, exp, des) \
    __atomic_compare_exchange_n((p), (exp), (des), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)
#define atomic_thread_fence(o) __atomic_thread_fence(o)
#endif
