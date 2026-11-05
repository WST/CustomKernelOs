/* Демонстрационные потоки для команды spawn. */
#include "demo.h"
#include "task.h"
#include "timer.h"
#include "vga.h"
#include "string.h"
#include "heap.h"

/* число -> строка с ведущими нулями */
static void utoa_pad(u32 v, char *out, int width) {
    for (int i = width - 1; i >= 0; i--) { out[i] = '0' + v % 10; v /= 10; }
    out[width] = 0;
}

/* clock: время работы системы в правом верхнем углу, обновление 10 раз в секунду */
static void demo_clock(void *arg) {
    (void)arg;
    const char spin[] = "|/-\\";
    char buf[24];
    for (u32 n = 0;; n++) {
        u32 s = timer_ticks() / TIMER_HZ;
        memcpy(buf, " up ", 4);
        utoa_pad(s / 3600, buf + 4, 2);       buf[6] = ':';
        utoa_pad((s / 60) % 60, buf + 7, 2);  buf[9] = ':';
        utoa_pad(s % 60, buf + 10, 2);
        buf[12] = ' '; buf[13] = spin[n % 4]; buf[14] = ' '; buf[15] = 0;
        vga_write_at(80 - 15, 0, buf, BLACK, YELLOW);
        sleep_ms(100);
    }
}

/* counter: «жадный» поток — считает без остановки и никогда не спит.
 * Без вытеснения он бы навсегда занял процессор; с планировщиком оболочка
 * продолжает отвечать, а в ps видно, кто сколько CPU съел. */
static void demo_counter(void *arg) {
    u32 row = (u32)arg;
    char buf[24];
    memcpy(buf, " count ", 7);
    for (u32 n = 0;; n++) {
        if ((n & 0xFFFF) == 0) {
            utoa_pad(n >> 16, buf + 7, 7);
            buf[14] = ' '; buf[15] = 0;
            vga_write_at(80 - 15, row, buf, WHITE, BLUE);
        }
    }
}

/* primes: конечная работа — решето Эратосфена в куче, печатает итог и выходит */
static void demo_primes(void *arg) {
    u32 limit = (u32)arg;
    u32 t0 = timer_ticks();
    u8 *sieve = kcalloc(limit + 1, 1);
    if (!sieve) { kprintf("[primes] out of memory\n"); return; }
    u32 count = 0;
    for (u32 i = 2; i <= limit; i++) {
        if (sieve[i]) continue;
        count++;
        for (u32 j = i * 2; j <= limit; j += i) sieve[j] = 1;
    }
    kfree(sieve);
    kprintf("\n[primes] %u primes below %u, computed in %u ms (task %u done)\n",
            count, limit, (timer_ticks() - t0) * 10, task_current()->id);
}

/* crash: немного живёт и разыменовывает NULL — система должна выжить */
static void demo_crash(void *arg) {
    (void)arg;
    kprintf("\n[crash] I will dereference NULL in 1 second...\n");
    sleep_ms(1000);
    volatile u32 *p = (volatile u32 *)0;
    *p = 42;
}

/* deep: бесконечная рекурсия — переполнение стека ловит «канарейка» */
static u32 recurse(u32 n) {
    volatile u8 pad[256];
    pad[0] = (u8)n;
    if (n > 1000000) return 0;         /* не случится: стек кончится раньше */
    task_yield();                      /* дать планировщику проверить глубину стека */
    return recurse(n + 1) + pad[0];
}
static void demo_deep(void *arg) {
    (void)arg;
    kprintf("\n[deep] recursing forever...\n");
    recurse(1);
}

static u32 counter_rows;

const struct demo demos[] = {
    { "clock",   "uptime clock in the top-right corner" },
    { "counter", "CPU-hungry loop that never sleeps" },
    { "primes",  "sieve of Eratosthenes, then exits [N]" },
    { "crash",   "dereferences NULL after 1 s" },
    { "deep",    "infinite recursion -> stack overflow" },
};
const u32 demo_count = sizeof(demos) / sizeof(demos[0]);

int demo_spawn(const char *name, const char *arg) {
    if (!strcmp(name, "clock"))   return task_create("clock", demo_clock, NULL);
    if (!strcmp(name, "counter")) return task_create("counter", demo_counter, (void *)(1 + counter_rows++ % 20));
    if (!strcmp(name, "primes")) {
        u32 n = *arg ? strtou(arg, NULL) : 2000000;
        if (n < 10) n = 10;
        if (n > 50000000) n = 50000000;
        return task_create("primes", demo_primes, (void *)n);
    }
    if (!strcmp(name, "crash"))   return task_create("crash", demo_crash, NULL);
    if (!strcmp(name, "deep"))    return task_create("deep", demo_deep, NULL);
    return -100;
}
