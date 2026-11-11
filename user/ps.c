/* ps — список процессов и потоков ядра */
#include "myos.h"

static const char *states[] = { "ready", "running", "sleeping", "blocked", "zombie" };

int main(void) {
    struct proc_info p[256];
    int n = ps(p, 256);
    printf("  PID  PPID  PGID RING STATE     CPU%%   CPU ms    MEM  NAME\n");
    for (int i = 0; i < n; i++)
        printf("%5u %5u %5u    %u %-9s %3u%% %8u %4u KB  %s\n",
               p[i].pid, p[i].ppid, p[i].pgid, p[i].ring, states[p[i].state % 5],
               p[i].cpu_percent, p[i].cpu_ms, p[i].mem_kb, p[i].name);
    return 0;
}
