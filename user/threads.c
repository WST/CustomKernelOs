/* Example: independent ring-3 workers share memory and run across CPUs. */
#include "myos.h"
#define LIMIT 100
#define STACK 65536
static volatile unsigned stop;
static volatile unsigned counts[LIMIT];
static void work(void *arg) {
    unsigned i=(unsigned)arg;
    while (!stop) counts[i]++;
}
int main(int argc,char **argv) {
    int n=argc>1?atoi(argv[1]):100;
    if(n<1 || n>LIMIT) {printf("usage: threads [1..100]\n");return 1;}
    int tids[LIMIT],started=0; void *stacks[LIMIT];
    for(int i=0;i<n;i++) {
        stacks[i]=malloc(STACK);
        if(!stacks[i]) break;
        tids[i]=thread_create(work,(void *)i,stacks[i],STACK);
        if(tids[i]<0) {free(stacks[i]);break;}
        started++;
    }
    sleep_ms(3000); stop=1;
    unsigned min=~0u,max=0;
    for(int i=0;i<started;i++) {
        int code;
        if(thread_join(tids[i],&code)) {printf("join failed\n");return 1;}
        if(counts[i]<min) min=counts[i];
        if(counts[i]>max) max=counts[i];
        free(stacks[i]);
    }
    struct system_info info; sysinfo(&info,sizeof(info));
    printf("%d threads, %u CPUs, iterations per thread: %u..%u\n",started,info.cpu_online,started?min:0,max);
    return started==n?0:1;
}
