/* Sparse ACPI physical memory and reusable map slots expose pointer/truncation bugs. */
#include "../kernel/acpi_smp.h"
#include "../kernel/string.h"
int printf(const char *,...); int puts(const char *); void exit(int);
#define CHECK(c) do { if(!(c)) { printf("ACPI SMP FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static u8 rsdp[64],rsdt[128],xsdt[128],apic[2048],slots[2][4096];
static u32 rsdt_phys=0x80000ff0u,xsdt_phys=0x81000ff0u,madt_phys=0x90000ff0u;
static unsigned maps;
static void put32(u8 *p,u32 v) { for(unsigned i=0;i<4;i++) p[i]=v>>(i*8); }
static void checksum(u8 *p,unsigned length,unsigned at) {
    p[at]=0; u8 sum=0; for(unsigned i=0;i<length;i++) sum+=p[i]; p[at]=0-sum;
}
static void sdt(u8 *p,const char *sig,unsigned size) {
    memcpy(p,sig,4); put32(p+4,size); p[8]=1; checksum(p,size,9);
}
static void fix_rsdp(void) { checksum(rsdp,20,8); checksum(rsdp,36,32); }
static void reset(void) {
    memset(rsdp,0,sizeof(rsdp));memset(rsdt,0,sizeof(rsdt));memset(xsdt,0,sizeof(xsdt));
    memset(apic,0,sizeof(apic)); maps=0;
    memcpy(rsdp,"RSD PTR ",8); rsdp[15]=2; put32(rsdp+16,rsdt_phys);
    put32(rsdp+20,36); put32(rsdp+24,xsdt_phys); fix_rsdp();
    put32(rsdt+36,madt_phys); sdt(rsdt,"RSDT",40);
    put32(xsdt+36,madt_phys); sdt(xsdt,"XSDT",44);
    put32(apic+36,0xfee00000u); sdt(apic,"APIC",44);
}
static void append(u8 type,u32 id,u32 flags) {
    unsigned length=*(u32 *)(apic+4),size=type==0?8:16;
    u8 *p=apic+length;memset(p,0,size);p[0]=type;p[1]=size;
    if(type==0) { p[3]=id;put32(p+4,flags); }
    else { put32(p+4,id);put32(p+8,flags); }
    put32(apic+4,length+size);checksum(apic,length+size,9);
}
static const void *map(u32 physical,u32 bytes,unsigned slot,void *context) {
    (void)context; CHECK(slot<2 && bytes<=sizeof(slots[slot])); maps++;
    const u8 *source=physical==0xe0000?rsdp:physical==rsdt_phys?rsdt:physical==xsdt_phys?xsdt:physical==madt_phys?apic:0;
    unsigned size=physical==0xe0000?sizeof(rsdp):physical==madt_phys?sizeof(apic):sizeof(rsdt);
    if(!source || bytes>size) return 0;
    memset(slots[slot],0xa5,sizeof(slots[slot])); memcpy(slots[slot],source,bytes);
    return slots[slot];
}
static enum acpi_smp_error discover(struct acpi_smp_info *info) {
    return acpi_smp_discover(0xe0000,2,map,0,info);
}
int main(void) {
    struct acpi_smp_info info;
    reset(); for(unsigned i=0;i<8;i++) append(0,i,1);
    CHECK(discover(&info)==ACPI_SMP_OK && info.count==8 && info.xsdt);
    CHECK(info.ids[0]==2 && info.root==xsdt_phys && info.madt==madt_phys);
    /* Modern RSDP can fall back to the legacy root without truncating pointers. */
    rsdp[15]=0;checksum(rsdp,20,8);
    CHECK(discover(&info)==ACPI_SMP_OK && !info.xsdt && info.root==rsdt_phys);
    reset(); append(0,2,1);append(9,2,1);append(9,4,1);append(0,4,1);
    append(9,6,0);append(9,7,2);append(9,256,1);append(9,255,1);
    CHECK(discover(&info)==ACPI_SMP_OK && info.count==2 && info.reported==2);
    CHECK(info.ids[1]==4 && info.unaddressable==2);
    reset();for(unsigned i=0;i<12;i++) append(9,i,1);
    CHECK(discover(&info)==ACPI_SMP_OK && info.count==SMP_MAX_CPUS && info.reported==12);
    reset();append(0,3,1);xsdt[9]++;
    CHECK(discover(&info)==ACPI_SMP_OK && !info.xsdt && info.count==2);
    reset();put32(rsdp+28,1);fix_rsdp();
    CHECK(discover(&info)==ACPI_SMP_OK && !info.xsdt);
    put32(rsdp+16,0);fix_rsdp();
    CHECK(discover(&info)==ACPI_SMP_HIGH_ADDRESS && info.count==1);
    reset();rsdp[32]++;
    CHECK(discover(&info)==ACPI_SMP_BAD_RSDP);
    reset();rsdt[9]++;xsdt[9]++;
    CHECK(discover(&info)==ACPI_SMP_BAD_ROOT);
    reset();apic[9]++;
    CHECK(discover(&info)==ACPI_SMP_BAD_MADT && info.count==1);
    reset();append(0,3,1);apic[45]=0;checksum(apic,52,9);
    CHECK(discover(&info)==ACPI_SMP_BAD_MADT && info.count==1);
    reset();append(0,3,1);apic[45]=9;checksum(apic,52,9);
    CHECK(discover(&info)==ACPI_SMP_BAD_MADT && info.count==1);
    reset();sdt(apic,"APIC",45);
    CHECK(discover(&info)==ACPI_SMP_BAD_MADT);
    reset();put32(apic+4,ACPI_SMP_TABLE_MAX+1);
    CHECK(discover(&info)==ACPI_SMP_BAD_MADT);
    reset();put32(xsdt+4,45);checksum(xsdt,45,9);put32(rsdp+16,0);fix_rsdp();
    CHECK(discover(&info)==ACPI_SMP_BAD_ROOT);
    reset();apic[44]=5;apic[45]=12;put32(apic+48,0xfee01000u);sdt(apic,"APIC",56);
    CHECK(discover(&info)==ACPI_SMP_OK && info.lapic==0xfee01000u);
    put32(apic+52,1);checksum(apic,56,9);
    CHECK(discover(&info)==ACPI_SMP_HIGH_ADDRESS && info.count==1);
    reset();put32(rsdt+36,0xfffffff0u);checksum(rsdt,40,9);xsdt[9]++;
    CHECK(discover(&info)==ACPI_SMP_NO_MADT);
    puts("ACPI SMP: high physical tables, XSDT/RSDT, x2APIC entries, checksums, bounds and safe fallback: PASS");
    return 0;
}
