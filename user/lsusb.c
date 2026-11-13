#include "myos.h"
#include "hwformat.h"
int main(int argc,char **argv) {
    int tree=0;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"-t")) tree=1;
        else if(!strcmp(argv[i],"-h") || !strcmp(argv[i],"--help")) {
            puts("usage: lsusb [-t]\nLive USB devices, including USB2 hub children; -t also lists empty root ports."); return 0;
        } else { dprintf(STDERR,"lsusb: unknown option: %s\n",argv[i]); return 2; }
    }
    if(tree) {
        struct hw_usb_info p; unsigned n=0;int rc;
        puts("USB xHCI root ports:");
        while((rc=hwinfo(HWINFO_USB,n,&p,sizeof(p)))==1) { hw_usb_row(&p); n++; }
        if(rc<0) { dprintf(STDERR,"lsusb: root-port query failed (%d)\n",rc); return 1; }
        if(!n) puts("USB inventory unavailable: no xHCI diagnostic provider in this boot.");
    }
    puts("USB devices (live topology; path = root-port.downstream-port):");
    struct hw_usb_device_info d;unsigned n=0;int rc;
    while((rc=hwinfo(HWINFO_USB_DEVICE,n,&d,sizeof(d)))==1) {
        const struct hw_usb_info *p=&d.usb;
        printf("%02x:%02x.%x path %u",p->bus,p->device,p->function,d.root_port);
        for(unsigned tier=0;tier<d.depth && tier<5;tier++) printf(".%u",(d.route>>(tier*4))&15);
        printf(" slot %-3u %04x:%04x %-16s %s",d.slot,p->vendor_id,p->product_id,
               hw_usb_class(p->device_class?p->device_class:p->interface_class,p->protocol),hw_usb_speed(p->speed));
        if(d.hub_ports) printf(" [hub: %u ports]",d.hub_ports);
        else if(p->hid_active) printf(" [HID active]");
        else printf(" [no class driver]");
        if(!p->controller_running) printf(" [controller inactive]");
        putchar('\n'); n++;
    }
    if(rc<0) { dprintf(STDERR,"lsusb: device query failed (%d)\n",rc); return 1; }
    if(!n) {
        struct hw_usb_info port;
        int available=hwinfo(HWINFO_USB,0,&port,sizeof(port));
        if(available<0) { dprintf(STDERR,"lsusb: root-port query failed (%d)\n",available); return 1; }
        puts(available?"No enumerated USB devices.":"USB inventory unavailable: no xHCI diagnostic provider in this boot.");
    }
    return 0;
}
