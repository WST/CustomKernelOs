/* Native SMP regression: independent leaf work plus serialized VM/files. */
#include "myos.h"
#define CHECK(x) do { if(!(x)) { printf("KERNEL PARALLEL FAIL line=%d pid=%d\n",__LINE__,getpid());return 1; } } while(0)
static char history[KLOG_CAPACITY];
static volatile unsigned stop,failures,protected_reads;
static void *race_buffer;
static void read_race(void *unused) {
    (void)unused;
    while(!stop) {
        struct klog_cursor cursor={.flags=KLOG_OLDEST};
        int n=klog_read(race_buffer,4096,&cursor);
        if(n<0 && errno!=EFAULT)__sync_fetch_and_add(&failures,1);
        struct system_info *out=race_buffer;
        int r=sysinfo(out,sizeof(*out));
        if(r<0 && r!=-EFAULT)__sync_fetch_and_add(&failures,1);
        __sync_fetch_and_add(&protected_reads,1);
    }
    thread_exit(0);
}
static int worker(int index,int fd) {
    unsigned pid=getpid(),tid=gettid(),last=ticks(),marker=1;
    CHECK(write(fd,&marker,4)==4);close(fd);
    for(unsigned i=0;i<30000;i++) {
        CHECK((unsigned)getpid()==pid && (unsigned)gettid()==tid);
        unsigned now=ticks();CHECK((int)(now-last)>=0);last=now;
        if(!(i%64)) {
            struct system_info info;CHECK(!sysinfo(&info,sizeof(info)));
            CHECK(info.ram_used_kb<=info.ram_total_kb && info.ram_free_kb+info.ram_used_kb==info.ram_total_kb);
            struct klog_cursor cursor={.flags=KLOG_OLDEST};
            int bytes=klog_read(history,sizeof(history),&cursor);CHECK(bytes>=0 && bytes<=KLOG_CAPACITY);
            char *pages=mmap(0,16384,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(pages!=(void *)-1);
            memset(pages,index+1,16384);CHECK(!mprotect(pages,16384,PROT_READ));
            CHECK(pages[8192]==index+1);CHECK(!munmap(pages,16384));
        }
    }
    char path[48],text[64],back[64];snprintf(path,sizeof(path),"/tmp/parallel-%d",index);
    memset(text,'a'+index,sizeof(text));int out=open(path,O_WRITE);CHECK(out>=0);
    CHECK(write(out,text,sizeof(text))==sizeof(text));close(out);out=open(path,O_READ);CHECK(out>=0);
    CHECK(read(out,back,sizeof(back))==sizeof(back));close(out);unlink(path);CHECK(!memcmp(text,back,sizeof(text)));
    return 0;
}
int main(void) {
    struct kernel_info before,after;CHECK(!kernelinfo(&before,sizeof(before)));
    CHECK(before.abi==KERNELINFO_ABI && before.size==sizeof(before));
    CHECK(kernelinfo(&after,0)==-EINVAL && kernelinfo((void *)1,sizeof(after))==-EFAULT);
    int gate[2],children[8];CHECK(!pipe(gate));unsigned start=ticks();
    for(int i=0;i<8;i++) { int pid=fork();CHECK(pid>=0);if(!pid){close(gate[0]);return worker(i,gate[1]);}children[i]=pid; }
    close(gate[1]);for(int i=0;i<8;i++){unsigned value;CHECK(read(gate[0],&value,4)==4);}close(gate[0]);
    for(int i=0;i<8;i++){int status=-1;CHECK(waitpid(children[i],&status,0)==children[i] && !status);}
    /* Sibling permission changes race against both leaf copyout paths and
     * force TLB shootdowns while other CPUs run without the BKL. */
    race_buffer=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(race_buffer!=(void *)-1);
    void *stack=mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(stack!=(void *)-1);
    int tid=thread_create(read_race,0,stack,65536);CHECK(tid>0);
    while(protected_reads<8)yield();
    for(unsigned i=0;i<500;i++) { CHECK(!mprotect(race_buffer,4096,0 /* PROT_NONE */));yield();CHECK(!mprotect(race_buffer,4096,PROT_READ|PROT_WRITE)); }
    stop=1;int status=-1;CHECK(thread_join(tid,&status)==0 && !status && !failures);
    CHECK(!munmap(race_buffer,4096) && !munmap(stack,65536));
    /* A tight fast-syscall loop must still be killable remotely. */
    int victim=fork();CHECK(victim>=0);if(!victim)for(;;)getpid();
    sleep_ms(30);CHECK(!kill(victim));CHECK(waitpid(victim,&status,0)==victim && status==137);
    CHECK(!kernelinfo(&after,sizeof(after)));
    CHECK(after.fast_calls-before.fast_calls>=8u*30000*3);
    CHECK(after.leaf_sections-before.leaf_sections>=8u*400*2);
    if(after.cpus>1)CHECK(after.parallel_peak>1 || after.overlap_with_bkl);
    printf("KERNEL PARALLEL PASS cpus=%u elapsed=%u ms fast=%u leaves=%u peak=%u overlap=%u races=%u\n",
        after.cpus,ticks()-start,after.fast_calls-before.fast_calls,after.leaf_sections-before.leaf_sections,
        after.parallel_peak,after.overlap_with_bkl,protected_reads);return 0;
}
