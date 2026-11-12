/* A Live AHCI query must never enable decoding, write registers or issue DMA. */
#define HOST_TEST
#define MYOS_HARDWARE
#include "../kernel/ahci.c"
int printf(const char *,...); int puts(const char *); void exit(int);
#define CHECK(c) do { if(!(c)) { printf("AHCI INVENTORY FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static unsigned mapped,writes,decode=2;
bool pci_find_class(u8 c,u8 s,struct pci_dev *d) {
    CHECK(c==1 && s==6); memset(d,0,sizeof(*d)); d->bus=2; d->dev=5; d->func=1; return true;
}
u32 pci_read(u8 b,u8 d,u8 f,u8 off) {
    CHECK(b==2 && d==5 && f==1);
    if(off==4) return decode;
    CHECK(off==0x24); return 0xfedc0400u;
}
void pci_write(u8 b,u8 d,u8 f,u8 o,u32 v) { (void)b;(void)d;(void)f;(void)o;(void)v; writes++; }
int vmm_map(u32 v,u32 p,u32 flags) {
    CHECK(v==MMIO_VIRT+mapped*4096 && p==0xfedc0000u+mapped*4096);
    CHECK(!(flags&PG_WRITE)); mapped++; return 0;
}
u32 ahci_test_read(u32 off) {
    if(off==0x10) return 0x10000;
    if(off==HBA_PI) return 3;
    if(off==port_off(0)+0x28) return 0x123;
    if(off==port_off(0)+0x24) return 0x101;
    CHECK(off==port_off(1)+0x28 || off==port_off(1)+0x24); return 0;
}
void ahci_test_write(u32 off,u32 v) { (void)off;(void)v; writes++; }
int main(void) {
    for(unsigned i=0;i<2;i++) unit[i].index=-1;
    decode=0; CHECK(!ahci_inventory_init() && !mapped && !writes);
    decode=2; CHECK(ahci_inventory_init() && mapped==MMIO_PAGES && !writes);
    CHECK((u32)mmio==MMIO_VIRT+0x400);
    struct hw_sata_info p;
    CHECK(ahci_port_info(0,&p)==1 && p.bus==2 && p.device==5 && p.function==1);
    CHECK(p.detection==3 && p.speed==2 && p.signature==0x101);
    CHECK(!p.driver_active && !p.sectors && !p.model[0]);
    CHECK(ahci_port_info(1,&p)==1 && !p.detection);
    CHECK(ahci_port_info(2,&p)==0 && !writes);
    puts("Live AHCI inventory: read-only MMIO, disabled decode, occupied/empty ports, no PCI/register writes: PASS");
    return 0;
}
