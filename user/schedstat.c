#include "myos.h"
int main(void) {
    struct scheduler_cpu_info sample[8];
    int count=schedinfo(sample,8);
    if(count<0) { printf("schedstat: error %d\n",count); return 1; }
    printf("CPU PID PRI READY(H/N/B) SWITCHES MIG(IN/OUT) WAIT(mean/max ms)\n");
    for(int i=0;i<count;i++) {
        struct scheduler_cpu_info *s=&sample[i];
        printf("%u %u %u %u/%u/%u %u %u/%u %u/%u\n",s->cpu,s->running_pid,
            s->running_priority,s->ready[0],s->ready[1],s->ready[2],s->switches,
            s->migrations_in,s->migrations_out,
            s->dispatches?s->wait_ticks_total/s->dispatches*10:0,s->wait_ticks_max*10);
    }
    printf("PRI: 0 interactive, 1 normal, 2 background, 3 idle; timer resolution 10 ms\n");
    return 0;
}
