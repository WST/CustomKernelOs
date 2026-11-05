/* Куча ядра: kmalloc / kfree / krealloc.
 *
 * Куча — непрерывная область виртуальных адресов с HEAP_START. Когда места
 * не хватает, к её концу подключаются новые физические страницы (PMM + paging).
 *
 * Память нарезана на блоки, идущие подряд; у каждого 16-байтный заголовок:
 *
 *   [hdr|данные....][hdr|данные..][hdr|данные.......]
 *
 * Блоки связаны в двусвязный список в порядке адресов. Свободные соседи
 * сразу склеиваются, поэтому двух свободных блоков подряд не бывает. */
#include "heap.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "idt.h"

#define ALIGN      16
#define MAGIC_USED 0xA110CA7E
#define MAGIC_FREE 0xF4EEB10C
#define MIN_SPLIT  (sizeof(struct block) + ALIGN)

struct block {
    u32 magic;
    u32 size;                       /* размер данных (без заголовка), кратен 16 */
    struct block *prev, *next;
} __attribute__((aligned(ALIGN)));  /* 16 байт в ядре, 32 в 64-битных тестах */
_Static_assert(sizeof(struct block) % ALIGN == 0, "header must keep data aligned");

static struct block *first, *last;
static uptr heap_end;                /* конец отображённой части кучи */
static u32 n_allocs, n_frees;

static inline u8 *data(struct block *b)        { return (u8 *)(b + 1); }
static inline struct block *hdr(void *p)       { return (struct block *)p - 1; }
static inline u32 align_up(u32 v)              { return (v + ALIGN - 1) & ~(ALIGN - 1); }
static inline bool is_free(struct block *b)    { return b->magic == MAGIC_FREE; }

/* подключить страницы, чтобы куча покрывала адреса до new_end */
static bool grow_to(uptr new_end) {
    if (new_end > HEAP_MAX) return false;
    while (heap_end < new_end) {
        u32 frame = pmm_alloc();
        if (!frame) return false;
        if (vmm_map(heap_end, frame, PG_WRITE)) { pmm_free(frame); return false; }
        heap_end += PAGE_SIZE;
    }
    return true;
}

void heap_init(void) {
    heap_end = HEAP_START;
    grow_to(HEAP_START + PAGE_SIZE);
    first = last = (struct block *)HEAP_START;
    first->magic = MAGIC_FREE;
    first->size  = PAGE_SIZE - sizeof(struct block);
    first->prev  = first->next = NULL;
}

/* отрезать от блока b хвост, если он достаточно большой */
static void split(struct block *b, u32 size) {
    if (b->size < size + MIN_SPLIT) return;
    struct block *rest = (struct block *)(data(b) + size);
    rest->magic = MAGIC_FREE;
    rest->size  = b->size - size - sizeof(struct block);
    rest->prev  = b;
    rest->next  = b->next;
    if (b->next) b->next->prev = rest; else last = rest;
    b->next = rest;
    b->size = size;
}

/* склеить свободный b со следующим свободным */
static void merge_next(struct block *b) {
    struct block *n = b->next;
    if (!n || !is_free(n)) return;
    b->size += sizeof(struct block) + n->size;
    b->next = n->next;
    if (n->next) n->next->prev = b; else last = b;
    n->magic = 0;
}

/* Отдать всё пространство между концом последнего блока и heap_end
 * свободному блоку: у кучи не должно быть «ничьих» байт. */
static void absorb_tail(void) {
    uptr end = (uptr)data(last) + last->size;
    if (end >= heap_end) return;
    if (is_free(last)) {
        last->size = heap_end - (uptr)data(last);
        return;
    }
    struct block *b = (struct block *)end;  /* зазор >= 16: концы блоков выровнены */
    b->magic = MAGIC_FREE;
    b->size  = heap_end - (uptr)data(b);
    b->prev  = last;
    b->next  = NULL;
    last->next = b;
    last = b;
}

static void *kmalloc_nolock(size_t size) {
    if (size == 0) return NULL;
    if (size > (uptr)(HEAP_MAX - HEAP_START)) return NULL;
    u32 need = align_up(size);

    /* first fit: первый подходящий свободный блок */
    struct block *b = first;
    for (; b; b = b->next)
        if (is_free(b) && b->size >= need) break;

    if (!b) {
        /* не нашли — растим кучу. Если последний блок свободен, удлиняем его,
         * иначе за ним появится новый свободный блок */
        uptr target = is_free(last)
            ? (uptr)data(last) + need
            : (uptr)data(last) + last->size + sizeof(struct block) + need;
        bool grown = grow_to(target);
        absorb_tail();                      /* даже если памяти хватило лишь частично */
        if (!grown) return NULL;
        b = last;
    }

    split(b, need);
    b->magic = MAGIC_USED;
    n_allocs++;
    return data(b);
}

static void *kcalloc_nolock(size_t n, size_t size) {
    if (size && n > 0xFFFFFFFFu / size) return NULL;     /* переполнение */
    void *p = kmalloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

static void kfree_nolock(void *ptr) {
    if (!ptr) return;
    if ((uptr)ptr < HEAP_START + sizeof(struct block) || (uptr)ptr >= heap_end)
        kpanic("kfree: pointer outside the heap");
    struct block *b = hdr(ptr);
    if (b->magic == MAGIC_FREE) kpanic("kfree: double free");
    if (b->magic != MAGIC_USED) kpanic("kfree: bad pointer or heap corruption");

    b->magic = MAGIC_FREE;
    n_frees++;
    merge_next(b);
    if (b->prev && is_free(b->prev)) merge_next(b->prev);
}

static void *krealloc_nolock(void *ptr, size_t size) {
    if (!ptr) return kmalloc(size);
    if (!size) { kfree(ptr); return NULL; }
    struct block *b = hdr(ptr);
    if (b->magic != MAGIC_USED) kpanic("krealloc: bad pointer");
    u32 need = align_up(size);
    if (need <= b->size) { split(b, need); if (b->next && is_free(b->next)) merge_next(b->next); return ptr; }

    /* попробуем расшириться за счёт свободного соседа справа */
    if (b->next && is_free(b->next) &&
        b->size + sizeof(struct block) + b->next->size >= need) {
        b->magic = MAGIC_FREE;          /* временно, чтобы merge_next сработал по правилам */
        merge_next(b);
        b->magic = MAGIC_USED;
        split(b, need);
        return ptr;
    }

    void *n = kmalloc(size);
    if (!n) return NULL;
    memcpy(n, ptr, b->size);
    kfree(ptr);
    return n;
}

static void heap_stats_nolock(struct heap_stats *st) {
    memset(st, 0, sizeof(*st));
    st->mapped = heap_end - HEAP_START;
    st->allocs = n_allocs;
    st->frees  = n_frees;
    for (struct block *b = first; b; b = b->next) {
        if (is_free(b)) { st->free += b->size; st->free_blocks++; }
        else            { st->used += b->size; st->used_blocks++; }
    }
}

static int heap_check_nolock(void) {
    uptr expect = HEAP_START;
    struct block *prev = NULL;
    for (struct block *b = first; b; prev = b, b = b->next) {
        if ((uptr)b != expect)                                   return 1;  /* дыра или наложение */
        if (b->magic != MAGIC_USED && b->magic != MAGIC_FREE)   return 2;  /* затёрт заголовок */
        if (b->prev != prev)                                    return 3;  /* сломан список */
        if (prev && is_free(prev) && is_free(b))                return 4;  /* не склеено */
        if (b->size % ALIGN)                                    return 5;
        expect = (uptr)data(b) + b->size;
    }
    if (prev != last)    return 6;
    if (expect != heap_end) return 7;
    return 0;
}

/* ---------------------------------------------------------------- блокировка
 * Куча — общая для всех задач. Если планировщик переключит задачи посреди
 * kmalloc, список блоков окажется наполовину изменённым. Поэтому каждая
 * операция выполняется с запрещёнными прерываниями (на одном CPU этого хватает). */

void *kmalloc(size_t size)             { u32 f = irq_save(); void *p = kmalloc_nolock(size);    irq_restore(f); return p; }
void *kcalloc(size_t n, size_t size)   { u32 f = irq_save(); void *p = kcalloc_nolock(n, size); irq_restore(f); return p; }
void  kfree(void *ptr)                 { u32 f = irq_save(); kfree_nolock(ptr);                 irq_restore(f); }
void *krealloc(void *ptr, size_t size) { u32 f = irq_save(); void *p = krealloc_nolock(ptr, size); irq_restore(f); return p; }
void  heap_stats(struct heap_stats *st){ u32 f = irq_save(); heap_stats_nolock(st);             irq_restore(f); }
int   heap_check(void)                 { u32 f = irq_save(); int r = heap_check_nolock();       irq_restore(f); return r; }
