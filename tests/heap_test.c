/* Тесты кучи ядра на хосте: heap.c собирается обычным gcc, «физическая память»
 * и paging заменены заглушками.  Запуск: make test */
#include "heap.h"
#include "pmm.h"
#include "paging.h"
#include "string.h"
#include "idt.h"
int printf(const char *, ...);
void *aligned_alloc(unsigned long, unsigned long);
void exit(int);

unsigned long heap_base;

/* --- заглушки PMM / paging --- */
static u32 frames_left = 1u << 30, frames_used;
u32  pmm_alloc(void) { if (!frames_left) return 0; frames_left--; frames_used++; return 0x100000 + frames_used * 4096; }
void pmm_free(u32 a) { (void)a; frames_used--; frames_left++; }
int  vmm_map(u32 v, u32 p, u32 f) { (void)v; (void)p; (void)f; return 0; }

/* --- kpanic: прыгаем обратно в тест --- */
static void *jb[5];
static const char *panic_msg;
void kpanic(const char *msg) { panic_msg = msg; __builtin_longjmp(jb, 1); }
#define EXPECT_PANIC(code) (panic_msg = 0, __builtin_setjmp(jb) == 0 ? ((code), 0) : 1)

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

static u32 seed = 12345;
static u32 rnd(void) { seed = seed * 1103515245 + 12345; return seed >> 8; }

int main(void) {
    heap_base = (unsigned long)aligned_alloc(4096, 8u << 20);
    heap_init();
    CHECK(heap_check() == 0);

    /* базовое: выравнивание, запись, освобождение, слияние */
    u8 *a = kmalloc(1), *b = kmalloc(100), *c = kmalloc(5000);
    CHECK(a && b && c);
    CHECK(((unsigned long)a | (unsigned long)b | (unsigned long)c) % 16 == 0);
    memset(a, 1, 1); memset(b, 2, 100); memset(c, 3, 5000);
    CHECK(kmalloc(0) == 0);
    kfree(b);
    CHECK(heap_check() == 0);
    u8 *b2 = kmalloc(64);
    CHECK(b2 == b);                      /* first fit переиспользует дыру */
    kfree(a); kfree(b2); kfree(c);
    struct heap_stats st; heap_stats(&st);
    CHECK(st.used_blocks == 0 && st.free_blocks == 1 && heap_check() == 0);

    /* kcalloc обнуляет и ловит переполнение */
    u32 *z = kcalloc(1000, 4);
    for (int i = 0; i < 1000; i++) CHECK(z[i] == 0);
    CHECK(kcalloc(0x40000000, 16) == 0);
    kfree(z);

    /* krealloc: сохраняет данные при росте и уменьшении */
    u8 *r = kmalloc(10);
    for (int i = 0; i < 10; i++) r[i] = i;
    r = krealloc(r, 20000);
    for (int i = 0; i < 10; i++) CHECK(r[i] == i);
    r = krealloc(r, 5);
    for (int i = 0; i < 5; i++) CHECK(r[i] == i);
    CHECK(heap_check() == 0);
    CHECK(krealloc(r, 0) == 0);
    u8 *n32 = krealloc(0, 32);
    CHECK(n32 != 0);
    kfree(n32);

    /* ошибки программиста -> kpanic */
    u8 *d = kmalloc(32);
    kfree(d);
    CHECK(EXPECT_PANIC(kfree(d)) && !strcmp(panic_msg, "kfree: double free"));
    u8 *e = kmalloc(32);
    CHECK(EXPECT_PANIC(kfree(e + 4)));                 /* указатель не на начало блока */
    CHECK(EXPECT_PANIC(kfree((void *)8)));             /* вообще не из кучи */
    kfree(e);

    /* случайный стресс с проверкой содержимого */
    #define N 200
    static u8 *p[N]; static u32 len[N]; static u8 tag[N];
    for (int round = 0; round < 50000; round++) {
        int k = rnd() % N;
        if (p[k]) for (u32 j = 0; j < len[k]; j++) CHECK(p[k][j] == (u8)(tag[k] + j));
        u32 size = rnd() % 10 == 0 ? 1 + rnd() % 30000 : 1 + rnd() % 300;
        switch (rnd() % 3) {
        case 0: kfree(p[k]); p[k] = 0; continue;
        case 1: if (p[k]) { u8 *q = krealloc(p[k], size); CHECK(q);
                    u32 keep = len[k] < size ? len[k] : size;
                    for (u32 j = 0; j < keep; j++) CHECK(q[j] == (u8)(tag[k] + j));
                    p[k] = q; break; }
                /* fallthrough */
        default: kfree(p[k]); p[k] = kmalloc(size); CHECK(p[k]);
        }
        len[k] = size; tag[k] = rnd();
        for (u32 j = 0; j < size; j++) p[k][j] = (u8)(tag[k] + j);
        if (round % 100 == 0) CHECK(heap_check() == 0);
    }
    for (int k = 0; k < N; k++) kfree(p[k]);
    heap_stats(&st);
    CHECK(st.used == 0 && st.free_blocks == 1 && heap_check() == 0);
    printf("stress: 50000 ops ok, heap grew to %u KB\n", st.mapped / 1024);

    /* нехватка памяти: kmalloc возвращает NULL, куча остаётся целой */
    frames_left = 4;
    CHECK(kmalloc(st.mapped + 100000) == 0);
    CHECK(heap_check() == 0);
    frames_left = 1u << 30;
    CHECK(kmalloc(8u << 20) == 0);                     /* больше HEAP_MAX */
    CHECK(heap_check() == 0);

    printf("ALL HEAP TESTS PASSED\n");
    return 0;
}
