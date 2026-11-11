/* Read the kernel memory ring through a nondestructive per-reader cursor. */
#include "myos.h"
struct cursor { struct klog_cursor log;bool seen; };
static char snapshot[KLOG_CAPACITY];
static unsigned tail_start(const char *text,unsigned length,int lines) {
    if(lines<0)return 0;
    if(!lines)return length;
    int found=0;
    for(unsigned i=length;i>0;i--) {
        if(text[i-1]=='\n' && i!=length && ++found==lines)return i;
    }
    return 0;
}
static int write_all(const char *buf,unsigned n) {
    unsigned sent=0;
    while(sent<n) {
        int k=write(STDOUT,buf+sent,n-sent);
        if(k<0 && errno==EINTR)continue;
        if(k<=0) { if(!k)errno=EIO;return -1; }
        sent+=k;
    }
    return 0;
}
static int show(struct cursor *state,int lines) {
    struct klog_cursor next=state->log;
    if(!state->seen)next.flags=lines==0?KLOG_END:KLOG_OLDEST;
    int n=klog_read(snapshot,sizeof(snapshot),&next);
    if(n<0)return -1;
    if(next.dropped)dprintf(STDERR,"dmesg: %u older log bytes overwritten\n",next.dropped);
    unsigned start=state->seen?0:tail_start(snapshot,n,lines);
    if(write_all(snapshot+start,n-start)<0)return -1;
    state->log=next;state->seen=true;return 0;
}
static void usage(int fd) {
    dprintf(fd,"usage: dmesg [-w|--follow] [-n LINES] [-h|--help]\n"
        "  dmesg          show the kernel log\n"
        "  dmesg -n 50    show the last 50 lines\n"
        "  dmesg -w       show messages and follow new ones (Ctrl+C exits)\n"
        "  dmesg -n 0 -w  show only new messages\n");
}
int main(int argc,char **argv) {
    bool follow=false;int lines=-1;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"-w") || !strcmp(argv[i],"--follow"))follow=true;
        else if(!strcmp(argv[i],"-h") || !strcmp(argv[i],"--help")) { usage(STDOUT);return 0; }
        else if(!strcmp(argv[i],"-n")) {
            if(++i==argc || !argv[i][0]) { usage(STDERR);return 2; }
            unsigned value=0;
            for(const char *p=argv[i];*p;p++) {
                if(*p<'0' || *p>'9' || value>(1000000u-(*p-'0'))/10) {
                    dprintf(STDERR,"dmesg: invalid line count '%s'\n",argv[i]);return 2;
                }
                value=value*10+(*p-'0');
            }
            lines=value;
        } else { usage(STDERR);return 2; }
    }
    struct cursor state={0};
    do {
        if(show(&state,lines)<0 && !(follow && errno==EAGAIN)) {
            dprintf(STDERR,"dmesg: %s\n",strerror(errno));return 1;
        }
        if(follow)sleep_ms(500);
    } while(follow);
    return 0;
}
