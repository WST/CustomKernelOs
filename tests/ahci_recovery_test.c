/* Simulated HBA exercises the real driver's DMA ownership/error paths. */
#define HOST_TEST
#include "../kernel/ahci.c"
int printf(const char *, ...);int puts(const char *);void exit(int);
#define CHECK(c) do { if (!(c)) { printf("AHCI FAIL line %d: %s\n",__LINE__,#c);exit(1); } } while(0)
static u32 regs[0x2000/4],now,pci_command,submitted,aborted,resets;
static u8 tables[2][4096] __attribute__((aligned(4096)));
static u8 pages[4][4096] __attribute__((aligned(4096)));
enum { SUCCESS,FATAL,TIMEOUT,COMPLETED_ERROR };
static int fault;
static bool stuck_cr,stuck_fr,stuck_device;
u32 timer_ticks(void) { return now; }
void task_sleep_ticks(u32 ticks) { now+=ticks; }
void kprintf(const char *fmt,...) { (void)fmt; }
u32 pci_read(u8 b,u8 d,u8 f,u8 off) { (void)b;(void)d;(void)f;CHECK(off==4);return pci_command; }
void pci_write(u8 b,u8 d,u8 f,u8 off,u32 v) { (void)b;(void)d;(void)f;CHECK(off==4);pci_command=v; }
u32 ahci_test_read(u32 off) { CHECK(off<sizeof regs);return regs[off/4]; }
void ahci_test_write(u32 off,u32 value) {
    CHECK(off>=PORT_BASE && off<PORT_BASE+2*PORT_STRIDE);
    u32 base=PORT_BASE+((off-PORT_BASE)/PORT_STRIDE)*PORT_STRIDE;
    u32 reg=off-base;
    if(reg==0x10 || reg==0x30) { regs[off/4]&=~value;return; } /* W1C */
    if(reg==0x18) {
        if(!(value&CMD_ST) && !stuck_cr) {
            if(regs[(base+0x38)/4])aborted++;
            regs[(base+0x38)/4]=regs[(base+0x34)/4]=0;
        }
        regs[off/4]=value;
        if(value&CMD_ST)regs[off/4]|=CMD_CR;
        else if(!stuck_cr)regs[off/4]&=~CMD_CR;
        if(value&CMD_FRE)regs[off/4]|=CMD_FR;
        else if(!stuck_fr)regs[off/4]&=~CMD_FR;
        return;
    }
    if(reg==0x2c && !(value&0xf) && (regs[off/4]&0xf)==1) {
        CHECK(regs[(base+0x18)/4]&CMD_FRE); /* receive reset signature FIS */
        CHECK(!(regs[(base+0x18)/4]&CMD_ST));resets++;
        regs[(base+0x28)/4]=stuck_device?1:3;
        regs[(base+0x20)/4]=stuck_device?0x80:0;
    }
    if(reg==0x38) {
        CHECK(!regs[off/4]);CHECK(!dma_failed);submitted++;
        regs[off/4]=fault==SUCCESS || fault==COMPLETED_ERROR ? 0 : value;
        regs[(base+0x10)/4]=fault==FATAL?1u<<30:0;
        regs[(base+0x20)/4]=fault==TIMEOUT?0x80:fault==SUCCESS?0:1;
        if(fault==SUCCESS)memset(pages[0],0x42,512);
        return;
    }
    regs[off/4]=value;
}
static void prepare(int mode) {
    memset(regs,0,sizeof regs);memset(tables,0,sizeof tables);memset(pages,0,sizeof pages);
    now=submitted=aborted=resets=0;pci_command=6;dma_failed=false;fault=mode;
    stuck_cr=stuck_fr=stuck_device=false;
    for(int i=0;i<4;i++)data_page[i]=(u32)pages[i];
    for(int i=0;i<2;i++) {
        unit[i].index=i;unit[i].page=(u32)tables[i];unit[i].sectors=1024;
        regs[(port_off(i)+0x18)/4]=CMD_ST|CMD_CR|CMD_FRE|CMD_FR;
        regs[(port_off(i)+0x28)/4]=3;
    }
}
int main(void) {
    u8 buf[512],saved[4096];memset(buf,0x5a,sizeof buf);
    prepare(FATAL);CHECK(ahci_read(0,1,buf)<0);CHECK(aborted==1 && !dma_failed && submitted==1);
    fault=SUCCESS;CHECK(ahci_read(0,2,buf)==0 && buf[0]==0x42 && submitted==2);
    prepare(TIMEOUT);CHECK(ahci_read(0,1,buf)<0);CHECK(now>=30*TIMER_HZ && resets==1 && aborted==1 && !dma_failed);
    fault=SUCCESS;CHECK(ahci_read(1,2,buf)==0);
    prepare(COMPLETED_ERROR);CHECK(ahci_read(0,1,buf)<0 && !dma_failed);
    fault=SUCCESS;CHECK(ahci_read(0,2,buf)==0);
    /* An unexpected active slot must be stopped before write copies data. */
    prepare(SUCCESS);regs[(port_off(0)+0x38)/4]=1;memcpy(saved,pages[0],4096);
    CHECK(ahci_write(0,1,buf,1)<0 && submitted==0 && aborted==1);
    CHECK(!memcmp(saved,pages[0],4096));CHECK(ahci_write(0,1,buf,1)==0);
    /* A stuck command/FIS engine or failed reset disables all shared DMA. */
    for(int mode=0;mode<3;mode++) {
        prepare(mode==2?TIMEOUT:FATAL);
        stuck_cr=mode==0;stuck_fr=mode==1;stuck_device=mode==2;
        CHECK(ahci_read(0,1,buf)<0 && dma_failed && !(pci_command&4));
        CHECK(ahci_present(0) && ahci_present(1)); /* no IDE fallback */
        unsigned before=submitted;memcpy(saved,pages[0],4096);
        CHECK(ahci_write(1,2,buf,1)<0 && ahci_read(1,2,buf)<0);
        CHECK(submitted==before && !memcmp(saved,pages[0],4096));
    }
    puts("AHCI RECOVERY PASS: fatal error, timeout, reset, retry and controller quarantine");return 0;
}
