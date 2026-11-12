#include "myos.h"
#include "hwformat.h"
int main(int argc,char **argv) {
    int verbose=0;
    for(int i=1;i<argc;i++) {
        if(!strcmp(argv[i],"-v") || !strcmp(argv[i],"-vv")) verbose=1;
        else if(!strcmp(argv[i],"-n") || !strcmp(argv[i],"-nn")) {}
        else if(!strcmp(argv[i],"-h") || !strcmp(argv[i],"--help")) {
            puts("usage: lspci [-n|-nn] [-v]\nLists all PCI buses/functions. IDs are always numeric; -v shows raw BARs and IRQ."); return 0;
        } else { dprintf(STDERR,"lspci: unknown option: %s\n",argv[i]); return 2; }
    }
    unsigned cursor=0,count=0; int rc; struct hw_pci_info p;
    while((rc=hwinfo(HWINFO_PCI,cursor,&p,sizeof(p)))==1) {
        printf("%02x:%02x.%x %-24s [%02x%02x]: %s [%04x:%04x] (rev %02x)\n",
               p.bus,p.device,p.function,hw_pci_class(p.class_code,p.subclass,p.prog_if),
               p.class_code,p.subclass,hw_vendor(p.vendor_id),p.vendor_id,p.device_id,p.revision);
        if(verbose) {
            printf("  prog-if %02x, header %02x, IRQ %u\n",p.prog_if,p.header_type,p.irq);
            if(p.subsystem_vendor!=0xffff)
                printf("  Subsystem: %04x:%04x\n",p.subsystem_vendor,p.subsystem_device);
            for(unsigned i=0;i<6;i++) if(p.bar[i] && p.bar[i]!=0xffffffffu)
                printf("  BAR%u: %08x (raw)\n",i,p.bar[i]);
        }
        cursor=((p.bus<<8)|(p.device<<3)|p.function)+1; count++;
    }
    if(rc<0) { dprintf(STDERR,"lspci: hardware query failed (%d)\n",rc); return 1; }
    if(!count) puts("No PCI devices found through legacy configuration access.");
    return 0;
}
