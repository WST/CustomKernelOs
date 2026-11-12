#include "myos.h"
#include "hwformat.h"
int main(int argc,char **argv) {
    int tree=0;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"-t")) tree=1;
        else if(!strcmp(argv[i],"-h") || !strcmp(argv[i],"--help")) {
            puts("usage: lsusb [-t]\n-t includes empty xHCI root ports. IDs are boot-time descriptors."); return 0;
        } else { dprintf(STDERR,"lsusb: unknown option: %s\n",argv[i]); return 2; }
    }
    struct hw_usb_info p; unsigned rows=0,connected=0; int rc;
    puts("USB root ports (one managed xHCI controller; no hub/hotplug enumeration):");
    while((rc=hwinfo(HWINFO_USB,rows,&p,sizeof(p)))==1) {
        rows++;
        if(p.connected) connected++;
        if(tree || p.connected) hw_usb_row(&p);
    }
    if(rc<0) { dprintf(STDERR,"lsusb: hardware query failed (%d)\n",rc); return 1; }
    if(!rows) puts("USB inventory unavailable: no xHCI diagnostic provider in this boot.");
    else if(!connected) puts("No connected root-port devices.");
    return 0;
}
