#define write test_write
#define main dmesg_main
#include "../user/dmesg.c"
#undef main
#undef write
extern void exit(int);
#define CHECK(x) do { if(!(x)) { printf("dmesg test failed at %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static char output[3*KLOG_CAPACITY+1];
static unsigned output_size;
static bool broken;
static unsigned end_sequence;
static char ring[KLOG_CAPACITY];
static void klog_write(const char *s,unsigned n) {
    for(unsigned i=0;i<n;i++)ring[end_sequence++%KLOG_CAPACITY]=s[i];
}
int klog_read(void *buf,unsigned size,struct klog_cursor *cursor) {
    unsigned first=end_sequence>KLOG_CAPACITY?end_sequence-KLOG_CAPACITY:0;
    unsigned pos=cursor->flags==KLOG_OLDEST?first:cursor->flags==KLOG_END?end_sequence:cursor->lo;
    cursor->dropped=pos<first?first-pos:0;
    if(pos<first)pos=first;
    unsigned n=end_sequence-pos;if(n>size)n=size;
    for(unsigned i=0;i<n;i++)((char *)buf)[i]=ring[(pos+i)%KLOG_CAPACITY];
    cursor->lo=pos+n;cursor->flags=0;return n;
}
int test_write(int fd,const void *buf,unsigned n) {
    CHECK(fd==STDOUT);
    if(broken) { errno=EPIPE;return -1; }
    if(n>3)n=3;CHECK(output_size+n<sizeof(output));
    memcpy(output+output_size,buf,n);output_size+=n;output[output_size]=0;return n;
}
void sleep_ms(unsigned n) { (void)n;CHECK(false); }
static void clear_output(void) { output_size=0;output[0]=0; }
static void content(const char *s) { end_sequence=0;klog_write(s,strlen(s)); }
static void expect(int lines,const char *text) {
    struct cursor state={0};clear_output();CHECK(!show(&state,lines));CHECK(!strcmp(output,text));
}
int main(void) {
    content("");expect(-1,"");expect(2,"");
    content("one\ntwo\nthree\n");expect(-1,"one\ntwo\nthree\n");expect(1,"three\n");expect(2,"two\nthree\n");expect(0,"");expect(99,"one\ntwo\nthree\n");
    content("one\ntwo\nthree");expect(2,"two\nthree");
    content("boot\n");struct cursor a={0},b={0};clear_output();CHECK(!show(&a,-1));
    clear_output();CHECK(!show(&b,-1));CHECK(!strcmp(output,"boot\n"));
    klog_write("new\n",4);clear_output();CHECK(!show(&a,-1));CHECK(!strcmp(output,"new\n"));
    clear_output();CHECK(!show(&b,-1));CHECK(!strcmp(output,"new\n"));
    clear_output();CHECK(!show(&a,-1) && !output_size);
    char block[512];memset(block,'x',sizeof(block));
    for(unsigned i=0;i<KLOG_CAPACITY/sizeof(block)+1;i++)klog_write(block,sizeof(block));
    CHECK(!show(&a,-1) && output_size==KLOG_CAPACITY && a.log.dropped==512);
    a=(struct cursor){0};clear_output();CHECK(!show(&a,0) && !output_size);
    klog_write("only new\n",9);CHECK(!show(&a,0));CHECK(!strcmp(output,"only new\n"));
    broken=true;a=(struct cursor){0};CHECK(show(&a,-1)<0 && errno==EPIPE);
    printf("dmesg memory ring: tail selection, independent viewers, short writes, follow overwrite and output failure: PASS\n");return 0;
}
