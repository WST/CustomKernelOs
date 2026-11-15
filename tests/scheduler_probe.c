/* Wakeup latency under CPU/FPU load; usable before and after scheduler changes. */
#include "myos.h"
static int hog(int fd) {
    unsigned marker=1;if(write(fd,&marker,sizeof(marker))!=sizeof(marker))return 20;close(fd);
    for(;;) {
        double sum=0;
        for(int i=1;i<=4000;i++)sum+=1.0/((double)i*i);
        if(sum<1.64468 || sum>1.64470)return 21;
    }
}
int main(int argc,char **argv) {
    int bound=argc>1?atoi(argv[1]):0,gate[2],children[12],count=0;
    if(pipe(gate)<0)return 1;
    for(int i=0;i<12;i++) {
        int pid=fork();
        if(pid<0)break;
        if(!pid){close(gate[0]);return hog(gate[1]);}
        children[count++]=pid;
    }
    close(gate[1]);
    int failed=count!=12;
    for(int i=0;i<count;i++){unsigned value;if(read(gate[0],&value,sizeof(value))!=sizeof(value))failed=1;}
    close(gate[0]);
    if(argc>2) {
        int gui=fork();
        if(!gui) { char *args[]={"dragcheck",0}; exec("/bin/dragcheck",args); return 22; }
        int status=-1;
        if(gui<0 || waitpid(gui,&status,0)!=gui || status)failed=1;
    }
    sleep_ms(100);
    unsigned sample[16],total=0,maximum=0;
    for(unsigned i=0;i<16;i++) {
        unsigned start=ticks();sleep_ms(30);unsigned delay=ticks()-start;
        sample[i]=delay>30?delay-30:0;total+=sample[i];if(sample[i]>maximum)maximum=sample[i];
    }
    for(int i=0;i<count;i++)kill(children[i]);
    for(int i=0;i<count;i++){int status=-1;if(waitpid(children[i],&status,0)!=children[i] || status!=137)failed=1;}
    for(unsigned i=0;i<16;i++)for(unsigned j=i+1;j<16;j++)if(sample[j]<sample[i]){unsigned t=sample[i];sample[i]=sample[j];sample[j]=t;}
    printf("SCHED METRICS p95=%u mean=%u max=%u ms workers=%d\n",sample[15],total/16,maximum,count);
    if(bound && sample[15]>(unsigned)bound)failed=1;
    if(bound) {
        struct scheduler_cpu_info stats[8];
        int n=schedinfo(stats,8);
        unsigned moved_in=0,moved_out=0;
        if(n<1 || n>8 || schedinfo(stats,0)>=0 || schedinfo((void *)1,8)>=0)failed=1;
        for(int i=0;i<n;i++) {
            unsigned bins=0;
            for(unsigned b=0;b<SCHED_WAIT_BUCKETS;b++)bins+=stats[i].wait_hist[b];
            if(stats[i].cpu!=(unsigned)i || bins!=stats[i].dispatches)failed=1;
            moved_in+=stats[i].migrations_in;moved_out+=stats[i].migrations_out;
        }
        if(moved_in!=moved_out)failed=1;
        printf("SCHED STATS cpus=%d migrations=%u\n",n,moved_in);
    }
    puts(failed?"SCHED PROBE FAIL":"SCHED PROBE PASS");return failed?1:0;
}
