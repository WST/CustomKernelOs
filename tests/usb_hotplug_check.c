#include "myos.h"
#include "stdio.h"
int main(int argc,char **argv) {
    struct system_info system;
    if(sysinfo(&system,sizeof(system))<0)return 1;
    if(argc==2 && !strcmp(argv[1],"save")) {
        FILE *f=fopen("/tmp/usb-memory","w");if(!f)return 2;
        fprintf(f,"%u\n",system.ram_used_kb);fclose(f);puts("USB MEMORY BASELINE");return 0;
    }
    if(argc==2 && !strcmp(argv[1],"mem")) {
        FILE *f=fopen("/tmp/usb-memory","r");char text[32];if(!f)return 3;
        int n=fread(text,1,31,f);text[n]=0;fclose(f);unsigned previous=atoi(text);
        if(system.ram_used_kb>previous+64) { printf("USB MEMORY FAIL before=%u after=%u\n",previous,system.ram_used_kb);return 4; }
        printf("USB MEMORY PASS before=%u after=%u\n",previous,system.ram_used_kb);return 0;
    }
    if(argc!=4)return 5;
    unsigned count=0,hid=0,hubs=0;int rc;
    struct hw_usb_device_info d;
    while((rc=hwinfo(HWINFO_USB_DEVICE,count,&d,sizeof(d)))==1) {
        if(!d.usb.connected || !d.usb.descriptor_valid || !d.usb.controller_running || !d.generation)return 6;
        if(d.depth>5 || (!d.depth && d.route) || (d.depth && !d.parent_slot))return 7;
        if(d.usb.hid_active)hid++;
        if(d.hub_ports)hubs++;
        count++;
    }
    if(rc || count!=(unsigned)atoi(argv[1]) || hid!=(unsigned)atoi(argv[2]) || hubs!=(unsigned)atoi(argv[3])) {
        printf("USB TOPOLOGY FAIL devices=%u HID=%u hubs=%u rc=%d\n",count,hid,hubs,rc);return 8;
    }
    printf("USB TOPOLOGY PASS devices=%u HID=%u hubs=%u\n",count,hid,hubs);return 0;
}
