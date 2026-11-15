/* Real PMM and log locks under concurrent host pthreads, without a BKL. */
#define HOST_TEST
#include "../kernel/pmm.c"
#include "../kernel/klog.c"
extern int puts(const char *);
extern int printf(const char *,...);
extern void exit(int) __attribute__((noreturn));
extern int pthread_create(unsigned long *,const void *,void *(*)(void *),void *);
extern int pthread_join(unsigned long,void **);
#define CHECK(x) do { if(!(x)) { printf("LEAF LOCK FAIL line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static u8 reference_counts[4096];
static volatile u32 owners[4096];
void kpanic(const char *message) { printf("PANIC %s\n",message);exit(1); }
static void *worker(void *argument) {
    u32 id=(u32)argument+1;char record[64],snapshot[1024];memset(record,'a'+id,sizeof(record));
    for(unsigned i=0;i<5000;i++) {
        u32 frame=pmm_alloc();CHECK(frame>=LOW_RESERVED && frame<4096*PAGE_SIZE);
        CHECK(__sync_bool_compare_and_swap(&owners[frame/PAGE_SIZE],0,id));
        CHECK(pmm_share(frame) && pmm_refs(frame)==2);
        pmm_free(frame);CHECK(pmm_refs(frame)==1);
        klog_write(record,sizeof(record));
        struct klog_cursor cursor={.flags=KLOG_OLDEST};
        int n=klog_read(snapshot,sizeof(snapshot),&cursor);CHECK(n>=0 && !(n%64));
        for(int off=0;off<n;off+=64) {
            CHECK(snapshot[off]>='b' && snapshot[off]<='i');
            for(int j=1;j<64;j++)CHECK(snapshot[off+j]==snapshot[off]);
        }
        struct pmm_stats sample;pmm_stats(&sample);CHECK(sample.used_frames<=sample.total_frames);
        CHECK(__sync_bool_compare_and_swap(&owners[frame/PAGE_SIZE],id,0));pmm_free(frame);
    }
    return 0;
}
int main(void) {
    memset(bitmap,0xff,sizeof(bitmap));refs=reference_counts;limit_frame=4096;
    total_frames=limit_frame-LOW_RESERVED/PAGE_SIZE;used_frames=0;next_hint=LOW_RESERVED/PAGE_SIZE;
    for(u32 f=LOW_RESERVED/PAGE_SIZE;f<limit_frame;f++)set_free(f);
    unsigned long threads[8];
    for(u32 i=0;i<8;i++)CHECK(!pthread_create(&threads[i],0,worker,(void *)i));
    for(u32 i=0;i<8;i++)CHECK(!pthread_join(threads[i],0));
    struct pmm_stats sample;pmm_stats(&sample);CHECK(!sample.used_frames);
    for(u32 f=LOW_RESERVED/PAGE_SIZE;f<limit_frame;f++)CHECK(!is_used(f) && !refs[f] && !owners[f]);
    CHECK(end_sequence==8u*5000*64);
    puts("LEAF LOCK PASS: 8 threads, 40000 frame/refcount cycles and atomic log records");return 0;
}
