#include "myos.h"
int main(void) {
    struct block_info info;
    puts("DEVICE        TYPE       SIZE(KiB)  START(sector)");
    for(unsigned i=0;blockinfo(i,&info)==1;i++)
        printf("/dev/%-8s %-10s %9u  %u\n",info.name,info.partition?"partition":"disk",info.sectors/2,info.start);
    return 0;
}
