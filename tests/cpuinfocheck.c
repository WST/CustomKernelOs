#include "myos.h"
int main(void) {
    struct system_info info;
    struct { struct cpu_info c[8]; unsigned guard; } a, b;
    if (sysinfo(&info,sizeof(info)) < 0) return 1;
    memset(&a,0,sizeof(a)); a.guard=0x12345678;
    int n=cpuinfo(a.c,8);
    if (n!=(int)info.cpu_online || n<1 || n>8 || a.guard!=0x12345678) return 2;
    if (cpuinfo(a.c,n-1)!=-EINVAL || cpuinfo(0,8)!=-EFAULT) return 3;
    int pid=fork();
    if (pid<0) return 4;
    if (!pid) for (;;) __asm__ volatile("pause");
    sleep_ms(100); cpuinfo(a.c,8); sleep_ms(800);
    if (cpuinfo(b.c,8)!=n) return 5;
    int status; kill(pid); waitpid(pid,&status,0);
    unsigned low=100, high=0;
    for (int i=0;i<n;i++) {
        unsigned total=b.c[i].total_ticks-a.c[i].total_ticks;
        unsigned idle=b.c[i].idle_ticks-a.c[i].idle_ticks;
        if (!total || idle>total) return 6;
        unsigned busy=(unsigned)((unsigned long long)(total-idle)*100/total);
        if (busy<low) low=busy;
        if (busy>high) high=busy;
        printf("CPUINFO core=%d busy=%u%% ticks=%u\n",i,busy,total);
    }
    if (high<60 || (n>1 && low>40)) return 7;
    puts("CPUINFO PASS"); return 0;
}
