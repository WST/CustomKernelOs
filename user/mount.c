#include "myos.h"
int main(int argc,char **argv) {
    if(argc==1) {
        char device[128],target[128];
        for(unsigned i=0;i<4;i++) {
            int r=mountinfo(i,device,target);
            if(r<0) { dprintf(STDERR,"mount: %s\n",strerror(-r)); return 1; }
            if(r) printf("%s on %s type myfs (rw)\n",device,target);
        }
        puts("devfs on /dev type devfs (devices)"); return 0;
    }
    if(argc!=3) { dprintf(STDERR,"usage: mount [DEVICE DIRECTORY]\n"); return 2; }
    int r=mount(argv[1],argv[2]);
    if(r<0) { dprintf(STDERR,"mount: %s: %s\n",argv[1],strerror(-r)); return 1; }
    return 0;
}
