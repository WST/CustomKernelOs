/* Runs inside MyOS. Every worker checks its physical APIC ID in ring 3,
 * exercises FPU preemption, private address spaces, files, fork and wait. */
#include "myos.h"

static unsigned cpu_id(void) {
    unsigned a=1,b,c,d;
    __asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d));
    return b>>24;
}
static int worker(int index,int fd) {
    unsigned id=cpu_id();
    double sum=0;
    for(int i=1;i<=400000;i++) {
        sum+=1.0/((double)i*i);
        if((i&8191)==0) yield();
        if(cpu_id()!=id) return 10;
    }
    if(sum<1.6449315 || sum>1.6449317) return 11;
    char path[48], data[128], readback[128];
    snprintf(path,sizeof(path),"/tmp/smp-%d",index);
    memset(data,'a'+index,sizeof(data));
    int out=open(path,O_WRITE);
    if(out<0 || write(out,data,sizeof(data))!=sizeof(data)) return 12;
    close(out);
    out=open(path,O_READ);
    if(out<0 || read(out,readback,sizeof(readback))!=sizeof(readback)) return 13;
    close(out); unlink(path);
    if(memcmp(data,readback,sizeof(data))) return 14;
    if(write(fd,&id,sizeof(id))!=sizeof(id)) return 15;
    return 0;
}
int main(int argc,char **argv) {
    struct system_info info;
    if(sysinfo(&info,sizeof(info))<0) return 1;
    unsigned expected=argc>1 ? atoi(argv[1]) : info.cpu_online;
    if(info.cpu_online!=expected) { printf("SMP FAIL online=%u expected=%u\n",info.cpu_online,expected); return 1; }
    int fds[2],pids[8];
    if(pipe(fds)<0) return 2;
    unsigned start=ticks();
    for(int i=0;i<8;i++) {
        int pid=fork();
        if(pid<0) return 3;
        if(!pid) { close(fds[0]); int rc=worker(i,fds[1]); close(fds[1]); return rc; }
        pids[i]=pid;
    }
    close(fds[1]);
    unsigned ids[8],n=0;
    for(int i=0;i<8;i++) {
        unsigned id; int got=read(fds[0],&id,sizeof(id));
        if(got!=sizeof(id)) { printf("SMP FAIL pipe %d\n",got); return 4; }
        unsigned j; for(j=0;j<n;j++) if(ids[j]==id) break;
        if(j==n) ids[n++]=id;
    }
    close(fds[0]);
    for(int i=0;i<8;i++) {
        int status=-1;
        if(waitpid(pids[i],&status,0)!=pids[i] || status) { printf("SMP FAIL worker %d status %d\n",i,status); return 5; }
    }
    if(n!=expected) { printf("SMP FAIL executed on %u CPUs expected %u\n",n,expected); return 6; }
    /* Killing a CPU-bound remote process must not free its active page tables. */
    for(int i=0;i<12;i++) {
        int pid=fork();
        if(pid<0) return 7;
        if(!pid) { for(;;) __asm__ volatile("pause"); }
        sleep_ms(30);
        int status=0;
        if(kill(pid)<0 || waitpid(pid,&status,0)!=pid || status!=137) return 8;
    }
    /* Kill before the child can enter sleep: pending signals must be checked
     * on syscall entry, rather than after the entire sleep has elapsed. */
    for(int i=0;i<12;i++) {
        int pid=fork();
        if(pid<0) return 9;
        if(!pid) { sleep_ms(60000); return 10; }
        int status=0;
        if(kill(pid)<0 || waitpid(pid,&status,0)!=pid || status!=137) return 11;
    }
    printf("SMP PASS online=%u executed=%u elapsed=%u ms\n",info.cpu_online,n,ticks()-start);
    return 0;
}
