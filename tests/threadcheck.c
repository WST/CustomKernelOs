#include "myos.h"
#define N 100
#define STACK 65536
static volatile unsigned go,stop;
static volatile unsigned progress[N],cpu[N],errors[N],raw[N];
static thread_mutex_t lock;
static unsigned total;
static int pid, shared_fd;
static void worker(void *arg) {
    unsigned n=(unsigned)arg,a=1,b,c,d;
    __asm__ volatile("cpuid":"+a"(a),"=b"(b),"=c"(c),"=d"(d));
    cpu[n]=b>>24;
    if (getpid()!=pid || gettid()==pid) errors[n]=1;
    raw[n]=(unsigned)sbrk(32);
    if(raw[n]==~0u) errors[n]=8;
    while (!go) {}
    unsigned count=0; double sum=0;
    while (!stop) { sum+=0.25; count++; progress[n]=count; }
    if (sum!=(double)count*0.25) errors[n]=2;
    for (int j=0;j<20;j++) {
        char *p=malloc(128+n);
        if (!p) { errors[n]=3; break; }
        memset(p,n,128+n);
        if (p[100]!=(char)n) errors[n]=4;
        free(p);
        if (thread_mutex_lock(&lock)) {errors[n]=5;break;}
        total++;
        if (thread_mutex_unlock(&lock)) errors[n]=6;
    }
    if (!n && close(shared_fd)) errors[n]=7;
    thread_exit(n+10);
}
static void busy(void *arg) { (void)arg; for (;;) {} }
static void exit_worker(void *arg) { exit((int)arg); }
static void quick(void *arg) { (void)arg; }
static volatile unsigned phase;
static volatile unsigned *mapping;
static void tlb_worker(void *arg) {
    (void)arg;
    unsigned v=*mapping;
    phase=1;
    while (phase!=2) {}
    if (*mapping!=v+1) thread_exit(1);
}
static int fail(const char *why,int value) {printf("THREAD FAIL %s %d\n",why,value);return 1;}
int main(int argc,char **argv) {
    struct system_info info;
    sysinfo(&info,sizeof(info));
    unsigned expected=argc>1?atoi(argv[1]):info.cpu_online;
    int tids[N]; void *stacks[N];
    pid=getpid(); shared_fd=open("/tmp/thread-fd",O_WRITE);
    if (shared_fd<0) return fail("open",shared_fd);
    if (thread_create(0,0,0,0)>=0) return fail("bad-pointer",0);
    if (thread_join(pid,0)>=0) return fail("bad-join",0);
    for (unsigned i=0;i<N;i++) {
        stacks[i]=malloc(STACK);
        if (!stacks[i]) return fail("stack",i);
        tids[i]=thread_create(worker,(void *)i,stacks[i],STACK);
        if (tids[i]<0) return fail("create",tids[i]);
    }
    if (fork()!=-EBUSY) return fail("fork-busy",0);
    char *av[]={"/bin/hello",0};
    if (exec(av[0],av)!=-EBUSY) return fail("exec-busy",0);
    go=1; sleep_ms(3000);
    unsigned min=~0u,max=0,seen[8],ncpu=0;
    static struct proc_info procs[256];
    int np=ps(procs,256);
    for (int i=0;i<N;i++) {
        if (!progress[i]) return fail("starvation",i);
        unsigned j; for(j=0;j<ncpu;j++) if(seen[j]==cpu[i]) break;
        if(j==ncpu && ncpu<8) seen[ncpu++]=cpu[i];
        unsigned ms=0;
        for(int k=0;k<np;k++) if(procs[k].pid==(unsigned)tids[i]) ms=procs[k].cpu_ms;
        if (!ms) return fail("preemption",i);
        if(ms<min) min=ms;
        if(ms>max) max=ms;
    }
    stop=1;
    for (int i=0;i<N;i++) {
        int status=-1;
        if(thread_join(tids[i],&status) || status!=i+10 || errors[i]) return fail("join",i);
        if(thread_join(tids[i],0)!=-ESRCH) return fail("double-join",i);
        free(stacks[i]);
    }
    for(int i=0;i<N;i++) for(int j=0;j<i;j++) if(raw[i]==raw[j]) return fail("shared-sbrk",i);
    if (ncpu!=expected) return fail("cpus",ncpu);
    if (total!=N*20) return fail("mutex",total);
    if (write(shared_fd,"x",1)!=-1 || errno!=EBADF) return fail("shared-fd",0);
    unlink("/tmp/thread-fd");
    /* Exercise remote cached translations after unmap and same-VA remap. */
    int id=shm_alloc(4096); mapping=shm_map(id);
    if (!mapping) return fail("shm",id);
    *mapping=41; phase=0; void *stack=malloc(STACK);
    int tid=thread_create(tlb_worker,0,stack,STACK);
    while(!phase) yield();
    if(shm_unmap((void *)mapping)) return fail("unmap",0);
    id=shm_alloc(4096); volatile unsigned *next=shm_map(id);
    if (next!=mapping) return fail("remap",0);
    *mapping=42; phase=2; int status=-1;
    if(thread_join(tid,&status) || status) return fail("tlb",status);
    shm_unmap((void *)mapping);
    /* More than the task-table capacity over time verifies successful reaping. */
    for(int i=0;i<280;i++) {
        tid=thread_create(quick,0,stack,STACK);
        if(tid<0 || thread_join(tid,&status) || status) return fail("reuse",i);
    }
    free(stack);
    for (int i=0;i<8;i++) {
        int child=fork();
        if(child<0) return fail("fork",child);
        if(!child) {
            void *s=malloc(STACK);
            if(thread_create(busy,0,s,STACK)<0) exit(2);
            if(i&1) { sleep_ms(60000); exit(3); }
            exit(0);
        }
        if(i&1) {sleep_ms(30); kill(child);}
        if(waitpid(child,&status,0)!=child || status!=((i&1)?137:0)) return fail("exit-group",status);
    }
    for (int i=0;i<2;i++) {
        int child=fork();
        if(child<0) return fail("exit-fork",child);
        if(!child) {
            void *s=malloc(STACK);
            if(thread_create(exit_worker,(void *)(i?23:0),s,STACK)<0) exit(2);
            for (;;) {}
        }
        if(waitpid(child,&status,0)!=child || status!=(i?23:0)) return fail("worker-exit",status);
    }
    printf("THREAD PASS count=%d cpus=%u cpu_ms=%u..%u mutex=%u\n",N,ncpu,min,max,total);
    return 0;
}
