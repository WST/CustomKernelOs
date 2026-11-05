#ifndef HEAP_H
#define HEAP_H
#include "types.h"

#ifndef HEAP_START                  /* тесты на хосте подставляют свои адреса */
#define HEAP_START 0xD0000000u
#define HEAP_MAX   0xD4000000u      /* до 64 МБ */
#endif

struct heap_stats {
    u32 mapped;                     /* сколько байт виртуальной кучи подкреплено RAM */
    u32 used, free;                 /* полезные байты в блоках */
    u32 used_blocks, free_blocks;
    u32 allocs, frees;              /* счётчики вызовов за всё время */
};

void  heap_init(void);
void *kmalloc(size_t size);
void *kcalloc(size_t n, size_t size);
void *krealloc(void *ptr, size_t size);
void  kfree(void *ptr);
void  heap_stats(struct heap_stats *st);
int   heap_check(void);             /* проверка целостности: 0 = ок */

#endif
