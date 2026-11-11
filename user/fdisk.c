#include "myos.h"
static int number(const char *s,unsigned max,unsigned *out) {
    unsigned n=0; if(!*s) return -1;
    while(*s) { unsigned d=(unsigned)(*s++-'0'); if(d>9 || n>(max-d)/10 || d>max) return -1; n=n*10+d; }
    *out=n; return 0;
}
int main(int argc,char **argv) {
    unsigned op=0,slot=0,start=0,size=0;
    if(argc==2) {
        struct block_info info; bool found=false;
        for(unsigned i=0;blockinfo(i,&info)==1;i++) {
            const char *name=argv[1]; if(!strncmp(name,"/dev/",5)) name+=5;
            if(!strncmp(name,info.name,3) && strlen(name)==3) {
                printf("/dev/%s  start=%u sectors=%u size=%u MiB %s\n",info.name,info.start,info.sectors,info.sectors/2048,info.partition?"partition":"disk"); found=true;
            }
        }
        return found ? 0 : 1;
    }
    if(argc==4 && !strcmp(argv[1],"init") && !strcmp(argv[3],"--yes")) op=0;
    else if(argc==6 && !strcmp(argv[1],"add") && !number(argv[3],4,&slot) && slot && !number(argv[4],131071,&start) && start && !number(argv[5],8192,&size) && size) { op=1; start*=2048; size*=2048; }
    else if(argc==5 && !strcmp(argv[1],"del") && !strcmp(argv[4],"--yes") && !number(argv[3],4,&slot) && slot) op=2;
    else {
        dprintf(STDERR,"usage:\n fdisk DEVICE\n fdisk init DEVICE --yes\n fdisk add DEVICE SLOT START_MiB SIZE_MiB\n fdisk del DEVICE SLOT --yes\ninit/del destroy partition metadata. Mounted disks are refused.\n"); return 2;
    }
    int r=partctl(argv[2],op,slot,start,size);
    if(r<0) { dprintf(STDERR,"fdisk: %s: %s\n",argv[2],strerror(-r)); return 1; }
    puts("MBR updated; devices rescanned."); return 0;
}
