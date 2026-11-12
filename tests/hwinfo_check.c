#include "myos.h"
static int failures;
#define CHECK(c) do { if(!(c)) { printf("HWINFO FAIL line %u: %s\n",__LINE__,#c); failures++; } } while(0)
int main(int argc,char **argv) {
    int removed=argc>1 && !strcmp(argv[1],"removed");
    int usb=removed || (argc>1 && !strcmp(argv[1],"usb"));
    struct hw_pci_info p; unsigned cursor=0,count=0,secondary=0,multifunction=0; int rc;
    while((rc=hwinfo(HWINFO_PCI,cursor,&p,sizeof(p)))==1) {
        unsigned bdf=(p.bus<<8)|(p.device<<3)|p.function;
        CHECK(bdf>=cursor && bdf<65536);
        CHECK(p.vendor_id!=0xffff);
        if(p.bus>0 && p.class_code==2) secondary++;
        if(p.bus>0 && p.class_code==2 && p.function>0) multifunction++;
        cursor=bdf+1; count++;
        if(count>512) { CHECK(0); break; }
    }
    CHECK(rc==0 && count>0);
    CHECK(hwinfo(0,0,&p,sizeof(p))==-EINVAL);
    CHECK(hwinfo(HWINFO_PCI,0,&p,sizeof(p)-1)==-EINVAL);
    CHECK(hwinfo(HWINFO_PCI,0,(void *)1,sizeof(p))==-EFAULT);
    CHECK(hwinfo(HWINFO_PCI,65537,&p,sizeof(p))==-EINVAL);
    memset(&p,0x5a,sizeof(p)); CHECK(hwinfo(HWINFO_PCI,65536,&p,sizeof(p))==0);
    CHECK(p.bus==0x5a5a5a5a); /* EOF must not overwrite the caller's buffer. */
    unsigned char *readonly=mmap(0,4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK((unsigned)readonly<0xfffff000u);
    if((unsigned)readonly<0xfffff000u) {
        CHECK(mprotect(readonly,4096,PROT_READ)==0);
        CHECK(hwinfo(HWINFO_PCI,0,readonly,sizeof(p))==-EFAULT);
        munmap(readonly,4096);
    }
    if(usb) {
        struct hw_usb_info u; unsigned rows=0,known=0,hids=0;
        while((rc=hwinfo(HWINFO_USB,rows,&u,sizeof(u)))==1) {
            rows++; if(u.connected && u.descriptor_valid) known++;
            if(u.connected && u.hid_active && u.controller_running) hids++;
        }
        CHECK(rc==0 && rows>=3);
        if(removed) CHECK(known==2 && hids==1);
        else CHECK(known>=3 && hids>=2);
        printf("HWINFO USB ports=%u descriptors=%u HID=%u\n",rows,known,hids);
        struct hw_sata_info sata; unsigned disks=0,live_ports=0;
        while((rc=hwinfo(HWINFO_SATA,live_ports,&sata,sizeof(sata)))==1) {
            live_ports++; if(sata.detection==3) disks++;
            CHECK(!sata.driver_active && !sata.sectors && !sata.model[0]);
        }
        CHECK(rc==0 && disks==1 && live_ports>1);

    } else {
        CHECK(secondary>=2 && multifunction>=1);
        struct hw_sata_info s; unsigned rows=0,active=0,empty=0;
        while((rc=hwinfo(HWINFO_SATA,rows,&s,sizeof(s)))==1) {
            rows++; if(s.driver_active && s.detection==3 && s.model[0]) active++;
            if(s.detection==0) empty++;
        }
        CHECK(rc==0 && active==1 && empty>0);
        printf("HWINFO SATA ports=%u active=%u empty=%u\n",rows,active,empty);
    }
    puts(failures?"HWINFO FAIL":"HWINFO PASS");
    return failures?1:0;
}
