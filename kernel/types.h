#ifndef TYPES_H
#define TYPES_H

/* Своя мини-«стандартная библиотека»: никаких системных заголовков. */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed char        i8;
typedef short              i16;
typedef int                i32;
typedef unsigned int       size_t;
typedef unsigned long      uptr;      /* целое размером с указатель */

#define NULL ((void *)0)
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L   /* в C23 bool/true/false — уже ключевые слова */
typedef _Bool bool;
#define true  1
#define false 0
#endif

typedef __builtin_va_list va_list;
#define va_start(v, l) __builtin_va_start(v, l)
#define va_end(v)      __builtin_va_end(v)
#define va_arg(v, t)   __builtin_va_arg(v, t)

/* Порты ввода-вывода */
static inline void outb(u16 port, u8 val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline u8 inb(u16 port) {
    u8 r;
    __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
static inline void outw(u16 port, u16 val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline u16 inw(u16 port) {
    u16 r;
    __asm__ volatile ("inw %1, %0" : "=a"(r) : "Nd"(port));
    return r;
}
/* Простейшая защита общих данных на одном процессоре: запретить прерывания
 * (а значит и переключение задач) и потом вернуть флаг IF как было. */
#ifdef HOST_TEST                    /* юнит-тесты на хосте: cli там недоступен */
static inline u32 irq_save(void) { return 0; }
static inline void irq_restore(u32 f) { (void)f; }
#else
static inline u32 irq_save(void) {
    u32 f;
    __asm__ volatile ("pushf; pop %0; cli" : "=r"(f) : : "memory");
    return f;
}
static inline void irq_restore(u32 f) {
    if (f & 0x200) __asm__ volatile ("sti" : : : "memory");
}
#endif
static inline void io_wait(void) { outb(0x80, 0); }

#endif
