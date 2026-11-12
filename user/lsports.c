#include "myos.h"
#include "hwformat.h"
int main(int argc,char **argv) {
    if(argc>1) {
        if(argc==2 && (!strcmp(argv[1],"-h") || !strcmp(argv[1],"--help"))) {
            puts("usage: lsports\nRead-only USB/SATA port state. Port numbers are logical, not case labels."); return 0;
        }
        dprintf(STDERR,"usage: lsports\n"); return 2;
    }
    puts("USB root ports (logical xHCI numbering):");
    struct hw_usb_info u; unsigned n=0; int rc;
    while((rc=hwinfo(HWINFO_USB,n,&u,sizeof(u)))==1) { hw_usb_row(&u); n++; }
    if(rc<0) { dprintf(STDERR,"lsports: USB query failed (%d)\n",rc); return 1; }
    if(!n) puts("  Unavailable: no xHCI diagnostic provider in this boot.");
    puts("SATA ports (managed AHCI controller, zero-based numbering):");
    struct hw_sata_info s; n=0;
    while((rc=hwinfo(HWINFO_SATA,n,&s,sizeof(s)))==1) {
        const char *state=s.detection==0?"empty":s.detection==3?"connected":s.detection==1?"no PHY link":"other";
        printf("%02x:%02x.%x port %-3u %-12s",s.bus,s.device,s.function,s.port,state);
        if(s.detection==3) {
            const char *speed=s.speed==1?"1.5G":s.speed==2?"3G":s.speed==3?"6G":"unknown";
            printf(" %s signature=%08x",speed,s.signature);
            if(s.model[0]) printf(" %s (%u MiB)",s.model,s.sectors/2048);
            printf(" [%s]",s.driver_active?"disk initialized at boot":"not managed as a disk");
        }
        putchar('\n'); n++;
    }
    if(rc<0) { dprintf(STDERR,"lsports: SATA query failed (%d)\n",rc); return 1; }
    if(!n) puts("  Unavailable: no accessible AHCI controller in this boot.");
    puts("PCI controllers: lspci -v; disks: lsblk. Physical connector labels and HDMI/DP/audio jack detection are not available.");
    return 0;
}
