/* Real PTY output and echo paths with simulated reader backpressure. */
#define HOST_TEST
#include "../kernel/tty.c"
extern int puts(const char*);extern int printf(const char*,...);extern void exit(int);
extern void *malloc(unsigned long);extern void free(void*);
static struct task current_task;
static struct tty *active;
static u8 captured[32768];static u32 used,waits;static bool interrupt_wait;
#define CHECK(c) do{if(!(c)){printf("PTY FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
void *kcalloc(u32 n,u32 size){void *p=malloc(n*size);if(p)memset(p,0,n*size);return p;}
void kfree(void *p){free(p);}
struct task *task_current(void){return &current_task;}
struct task *task_process(struct task *t){return t;}
void task_wakeup(void *p){(void)p;}
int task_kill_group(u32 group){(void)group;return 0;}
static void drain(void){CHECK(used+active->out.count<=sizeof(captured));used+=ring_get(&active->out,captured+used,active->out.count);}
void task_wait(void *p){CHECK(p==&active->out);waits++;if(interrupt_wait)current_task.kill_pending=true;else drain();}
static void reset(struct tty *t){memset(t,0,sizeof(*t));t->masters=t->slaves=1;t->oflags=TTY_OUTPUT_DEFAULT;active=t;used=waits=0;interrupt_wait=false;current_task.kill_pending=false;}
int main(void) {
    struct tty t;struct file slave={.type=FILE_PTY_SLAVE,.tty=&t},master={.type=FILE_PTY_MASTER,.tty=&t};
    reset(&t);CHECK(pty_write(&slave,"one\ntwo\n",8)==8);drain();CHECK(used==10 && !memcmp(captured,"one\r\ntwo\r\n",10));
    reset(&t);t.mode=TTY_RAW;CHECK(pty_write(&slave,"x\n",2)==2);drain();CHECK(used==3 && !memcmp(captured,"x\r\n",3));
    const u8 binary[]={0,13,10,255};
    CHECK(tty_ctl(&slave,TTYCTL_SETOFLAGS,0)==0);used=0;CHECK(pty_write(&slave,binary,4)==4);drain();CHECK(used==4 && !memcmp(captured,binary,4));
    for(u32 flags=1;flags<=4;flags+=3){CHECK(tty_ctl(&slave,TTYCTL_SETOFLAGS,flags)==0);used=0;CHECK(pty_write(&slave,"\n",1)==1);drain();CHECK(used==1 && captured[0]==10);}
    CHECK(tty_ctl(&slave,TTYCTL_SETOFLAGS,2)==-EINVAL);
    reset(&t);memset(t.out.buf,'a',TTY_BUF-1);t.out.count=TTY_BUF-1;
    CHECK(pty_write(&slave,"\nz",2)==2);drain();CHECK(waits==1 && used==TTY_BUF+2 && captured[TTY_BUF-1]==13 && captured[TTY_BUF]==10 && captured[TTY_BUF+1]=='z');
    static u8 lines[TTY_BUF*2];memset(lines,10,sizeof(lines));reset(&t);
    CHECK(pty_write(&slave,lines,sizeof(lines))==(int)sizeof(lines));drain();CHECK(used==sizeof(lines)*2 && waits>0);
    for(u32 i=0;i<used;i++)CHECK(captured[i]==(i&1?10:13));
    reset(&t);t.out.count=TTY_BUF-1;interrupt_wait=true;
    CHECK(pty_write(&slave,"\n",1)==-EINTR);CHECK(t.out.count==TTY_BUF-1); /* no partial CR */
    reset(&t);t.masters=0;CHECK(pty_write(&slave,"x",1)==-EIO);
    reset(&t);CHECK(pty_write(&master,"hello\r",6)==6);drain();CHECK(used==7 && !memcmp(captured,"hello\r\n",7));
    u8 input[8];CHECK(pty_read(&slave,input,sizeof(input))==6 && !memcmp(input,"hello\n",6));
    puts("PASS: PTY CRLF, independent raw input/output, canonical echo, full rings, cancellation and byte counts");return 0;
}
