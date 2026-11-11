/* No filesystem/tasking stubs: the log must work before either exists. */
#define HOST_TEST
#include "../kernel/klog.c"
extern int printf(const char *,...);
extern void exit(int);
#define CHECK(x) do { if(!(x)) { printf("klog test failed at %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static char output[KLOG_CAPACITY],input[KLOG_CAPACITY];
static void reset(void) { end_sequence=0;guard=0;memset(ring,0,sizeof(ring)); }
int main(void) {
    reset();struct klog_cursor a={.flags=KLOG_OLDEST},b=a;
    CHECK(!klog_read(output,sizeof(output),&a));
    klog_write("early boot\n",11);
    CHECK(klog_read(output,sizeof(output),&a)==11 && !memcmp(output,"early boot\n",11));
    CHECK(klog_read(output,sizeof(output),&b)==11 && !memcmp(output,"early boot\n",11));
    CHECK(!klog_read(output,sizeof(output),&a));
    b=(struct klog_cursor){.flags=KLOG_END};CHECK(!klog_read(NULL,0,&b));
    klog_write("new\n",4);CHECK(klog_read(output,2,&a)==2 && !memcmp(output,"ne",2));
    CHECK(klog_read(output,sizeof(output),&a)==2 && !memcmp(output,"w\n",2));
    CHECK(klog_read(output,sizeof(output),&b)==4 && !memcmp(output,"new\n",4));
    for(unsigned i=0;i<sizeof(input);i++)input[i]='a'+i%26;
    klog_write(input,sizeof(input));klog_write("xyz",3);
    CHECK(klog_read(output,sizeof(output),&b)==KLOG_CAPACITY && b.dropped==3);
    CHECK(!memcmp(output,input+3,sizeof(input)-3) && !memcmp(output+sizeof(input)-3,"xyz",3));
    CHECK(!klog_read(output,sizeof(output),&b) && !b.dropped);
    a=(struct klog_cursor){.flags=KLOG_OLDEST};CHECK(klog_read(output,sizeof(output),&a)==KLOG_CAPACITY && !a.dropped);
    a.flags=3;CHECK(klog_read(output,1,&a)==-EINVAL);
    a=(struct klog_cursor){.hi=~0u,.lo=~0u};CHECK(klog_read(output,1,&a)==-EINVAL);
    CHECK(klog_read(NULL,1,&a)==-EINVAL && klog_read(output,KLOG_CAPACITY+1,&a)==-EINVAL);
    CHECK(klog_read(output,1,NULL)==-EINVAL);
    /* Cross the 32-bit byte counter boundary without losing the ring index. */
    reset();end_sequence=0xfffffff0ull;
    a=(struct klog_cursor){.flags=KLOG_END};CHECK(!klog_read(NULL,0,&a));
    klog_write(input,64);CHECK(klog_read(output,64,&a)==64 && !memcmp(output,input,64));
    CHECK(a.hi==1 && a.lo==48 && !a.dropped);
    printf("Kernel ring: early boot, independent readers, overwrite recovery, invalid cursors and 64-bit sequences: PASS\n");return 0;
}
