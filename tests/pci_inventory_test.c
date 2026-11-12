/* Exercise the real scanner with sparse buses and multifunction devices. */
#include "../kernel/pci.h"
int puts(const char *); int printf(const char *,...); void exit(int);
#define CHECK(c) do { if (!(c)) { printf("PCI FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static unsigned reads,writes;
u32 pci_test_read(u8 bus,u8 dev,u8 fn,u8 off) {
    reads++;
    int present=(bus==0 && dev==0 && fn==0) ||
        (bus==0 && dev==5 && (fn==0 || fn==3 || fn==7)) ||
        (bus==2 && dev==4 && fn==0) || (bus==255 && dev==31 && fn==0);
    if(!present) return 0xffffffffu;
    if(off==0) return 0x12348086u;
    if(off==8) return bus==2?0x03000001u:0x02000002u;
    if(off==12) return bus==0 && dev==5?0x00800000u:0;
    if(off==16) return 0x12345000u;
    return 0;
}
void pci_test_write(u8 b,u8 d,u8 f,u8 o,u32 v) { (void)b;(void)d;(void)f;(void)o;(void)v; writes++; }
int main(void) {
    struct pci_dev list[8],d;
    CHECK(pci_list(list,8)==6);
    CHECK(list[1].dev==5 && list[1].func==0);
    CHECK(list[2].func==3 && list[3].func==7);
    CHECK(list[4].bus==2 && list[4].bar0==0x12345000u);
    CHECK(list[5].bus==255 && list[5].dev==31);
    CHECK(pci_list(list,2)==2);
    CHECK(!pci_next(65536,&d)); CHECK(!pci_next(0xffffffffu,&d));
    CHECK(pci_next((2u<<8)|(4u<<3),&d) && d.bus==2 && d.dev==4);
    CHECK(pci_next((2u<<8)|(4u<<3)|1,&d) && d.bus==255);
    CHECK(pci_find_class(3,0,&d) && d.bus==2);
    CHECK(pci_find_display_vendor(0x8086,&d) && d.bus==2);
    CHECK(!pci_find_device(0xffff,0xffff,&d));
    CHECK(!writes && reads<200000);
    puts("PCI inventory: secondary buses, sparse functions, end cursors and read-only scan: PASS");
    return 0;
}
