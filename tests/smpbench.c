#include "myos.h"
int main(void) {
    unsigned start=ticks(), sum=0;
    for(unsigned i=0;i<100000;i++) sum+=getpid();
    printf("SYSCALL BENCH ms=%u checksum=%u\n",ticks()-start,sum);
    int child=fork();
    if(child<0) return 1;
    struct msg message;
    memset(&message,0,sizeof(message));
    if(!child) {
        for(int i=0;i<100;i++) {
            if(msg_recv(&message,2000)<0 || msg_send(message.from,&message)<0) return 2;
        }
        return 0;
    }
    start=ticks();
    for(int i=0;i<100;i++) {
        if(msg_send(child,&message)<0 || msg_recv(&message,2000)<0) return 3;
    }
    int status;
    if(waitpid(child,&status,0)!=child || status) return 4;
    printf("IPC BENCH ms=%u roundtrips=100\n",ticks()-start);
    return 0;
}
