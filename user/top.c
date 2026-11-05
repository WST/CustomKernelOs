/* top — процессы, обновляется раз в секунду. Q — выход. */
#include "myos.h"

static const char *states[] = { "ready", "run", "sleep", "block", "zombie" };

int main(void) {
    ttymode(TTY_RAW | TTY_NONBLOCK);
    printf(ANSI_CLEAR ANSI_HIDE);
    for (;;) {
        struct proc_info p[32];
        int n = ps(p, 32);
        /* сортировка по CPU% */
        for (int i = 1; i < n; i++)
            for (int j = i; j > 0 && p[j].cpu_percent > p[j - 1].cpu_percent; j--) {
                struct proc_info t = p[j]; p[j] = p[j - 1]; p[j - 1] = t;
            }
        unsigned up = ticks() / 1000, total_mem = 0, user = 0;
        for (int i = 0; i < n; i++) { total_mem += p[i].mem_kb; if (p[i].ring == 3) user++; }

        printf("\x1b[H" ANSI_BOLD "top" ANSI_RESET " - up %u:%02u:%02u, %d tasks (%u user processes), %u KB in tasks\x1b[K\n",
               up / 3600, (up / 60) % 60, up % 60, n, user, total_mem);
        printf("\x1b[K\n" ANSI_REVERSE "  PID  PPID RING STATE   CPU%%    CPU ms     MEM  NAME                         " ANSI_RESET "\n");
        for (int i = 0; i < n && i < SCREEN_H - 5; i++) {
            const char *color = p[i].cpu_percent > 50 ? ANSI_RED : p[i].ring == 3 ? ANSI_CYAN : "";
            printf("%s%5u %5u    %u %-6s %4u%% %9u %5u KB  %-15s" ANSI_RESET "\x1b[K\n",
                   color, p[i].pid, p[i].ppid, p[i].ring, states[p[i].state % 5],
                   p[i].cpu_percent, p[i].cpu_ms, p[i].mem_kb, p[i].name);
        }
        printf("\x1b[J\x1b[%d;1H" ANSI_CYAN "Q" ANSI_RESET " quit", SCREEN_H);
        for (int t = 0; t < 10; t++) {                 /* 1 секунда, но на Q реагируем сразу */
            int k = readkey();
            if (k == 'q' || k == 'Q' || k == 27) {
                printf(ANSI_RESET ANSI_CLEAR ANSI_SHOW);
                ttymode(0);
                return 0;
            }
            sleep_ms(100);
        }
    }
}
