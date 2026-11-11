#include "myos.h"
int main(void) {
    unsigned bytes=64u*1024*1024;
    volatile char *memory=malloc(bytes);
    if(!memory) return 1;
    for(unsigned i=0;i<bytes;i+=4096) memory[i]=(char)i;
    unsigned start=ticks();
    for(int i=0;i<5;i++) {
        int pid=fork();
        if(pid==0) {
            char *args[]={"sleep","0",NULL};
            exec("/bin/sleep",args); return 127;
        }
        int status=-1;
        if(pid<0 || waitpid(pid,&status,0)!=pid || status) return 2;
    }
    printf("FORK LAUNCH BENCH ms=%u parent_mb=64 launches=5\n",ticks()-start);
    start=ticks();
    for(int i=0;i<5;i++) {
        char *args[]={"sleep","0",NULL};
        int pid=spawn("/bin/sleep",args,-1),status=-1;
        if(pid<0 || waitpid(pid,&status,0)!=pid || status) return 3;
    }
    printf("SPAWN LAUNCH BENCH ms=%u parent_mb=64 launches=5\n",ticks()-start);
    free((void *)memory);
    return 0;
}
