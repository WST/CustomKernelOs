#include "myos.h"
static char text[512*1024];
int main(void) {
    for(unsigned i=0;i<sizeof(text);i++) text[i]=(i%32==31)?'\n':'V';
    int previous=ttymode(TTY_RAW|TTY_NONBLOCK);
    printf("CONSOLE LOAD READY\n");
    while(readkey()!='g') yield();
    printf("CONSOLE LOAD BEGIN\n");
    unsigned start=ticks();
    int n=write(STDOUT,text,sizeof(text));
    unsigned elapsed=ticks()-start;
    int key=readkey();
    printf("CONSOLE LOAD END bytes=%d ms=%u key=%d\n",n,elapsed,key);
    ttymode(previous);
    return n==sizeof(text)?0:1;
}
