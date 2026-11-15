#ifndef KERNEL_SPINLOCK_H
#define KERNEL_SPINLOCK_H
#include "types.h"
/* Leaf locks: no allocator callbacks, scheduling, BKL acquisition or user accesses
 * while held. Disable local IRQs to prevent same-CPU interrupt recursion. */
typedef volatile u32 spinlock_t;
static inline u32 spin_lock_irqsave(spinlock_t *lock) {
    u32 flags=irq_save();
    while(__sync_lock_test_and_set(lock,1))
        while(*lock)__asm__ volatile("pause" ::: "memory");
    return flags;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock,u32 flags) {
    __sync_lock_release(lock);irq_restore(flags);
}
#endif
