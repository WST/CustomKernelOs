/* Model PCI/MDIO/DMA around the actual driver; no privileged host I/O. */
#define HOST_TEST
#include "../kernel/e1000.c"
extern int printf(const char *,...);
extern void exit(int);
#define CHECK(x) do { if(!(x)) { printf("Ethernet test failed line %d: %s\n",__LINE__,#x);exit(1); } } while(0)
static u32 regs[MMIO_BYTES/4],pci_cmd;
static u16 phy_regs[800][32],emi_regs[65536],emi_address;
static unsigned selected_page,allocated,freed,elapsed,mdio_count,enabled_early;
static int fail_alloc;
static u16 model_id;
static unsigned model_bus,model_dev;
static bool busy,mdio_timeout,mdio_error,wrong_reg,stuck_reset,stuck_master,stuck_ulp,bad_bar;
static bool has_pm,stuck_pm,stuck_host_wake;
static bool firmware_claims_after_reset;
static u32 pmcsr;
static u8 pool[sizeof(pages)/sizeof(pages[0])*PAGE_SIZE] __attribute__((aligned(4096)));
void klogf(const char *fmt,...) { (void)fmt; }
void sleep_ms(u32 ms) {
    elapsed+=ms;
    if (!stuck_ulp)regs[PCH_FWSM/4]&=~0x400u;
}
u32 pmm_alloc(void) {
    if ((int)allocated==fail_alloc) return 0;
    CHECK(allocated<sizeof(pages)/sizeof(pages[0]));
    return (u32)&pool[allocated++*PAGE_SIZE];
}
void pmm_free(u32 address) {
    CHECK(address>=(u32)pool && address<(u32)pool+sizeof(pool) && !(address&4095));freed++;
}
int vmm_map(u32 virt,u32 phys,u32 flags) {
    CHECK(virt>=MMIO_VIRT && virt<MMIO_VIRT+MMIO_BYTES);
    CHECK(phys>=0xf0000000u && (flags&0x18)==0x18);return 0;
}
u32 pci_read(u8 bus,u8 dev,u8 fn,u8 offset) {
    if (bus!=model_bus || dev!=model_dev)return 0xffffffff;
    unsigned wanted=model_id==0x100e || model_id==0x10d3?0:6;
    if (!fn && wanted) {
        if (!offset)return 0x12348086;
        if (offset==0x0c)return 0x800000;
        return offset==8?0x06010000:0;
    }
    if (fn!=wanted)return 0xffffffff;
    if (!offset)return ((u32)model_id<<16)|0x8086;
    if (offset==8)return 0x02000000;
    if (offset==4)return pci_cmd|(has_pm?(1u<<20):0);
    if (offset==0x34)return has_pm?0x50:0;
    if (offset==0x50)return 1;
    if (offset==0x54)return pmcsr;
    if (offset==0x10)return bad_bar?0xf0000004:0xf0000000;
    if (offset==0x14)return bad_bar?1:0;
    return 0;
}
void pci_write(u8 bus,u8 dev,u8 fn,u8 offset,u32 value) {
    CHECK(bus==model_bus && dev==model_dev && (offset==4 || offset==0x54));
    CHECK(fn==(model_id==0x100e || model_id==0x10d3?0:6));
    if(offset==0x54) { CHECK(!(value&0x8000));if(!stuck_pm)pmcsr=(value&~0x8000u)|(pmcsr&0x8000);return; }
    CHECK(!(value&0xffff0000u));pci_cmd=value;
}
u32 e1000_test_read(u32 reg) {
    CHECK(reg<MMIO_BYTES && !(reg&3));return regs[reg/4];
}
void e1000_test_write(u32 reg,u32 value) {
    CHECK(reg<MMIO_BYTES && !(reg&3));
    CHECK(reg<0x12000); /* no flash sequencing/programming aperture writes */
    if ((reg==REG_RCTL || reg==REG_TCTL) && (value&2) && allocated!=34)enabled_early++;
    if (reg==PCH_EXTCNF && busy)value|=PCH_SWFLAG;
    if (reg==REG_CTRL) {
        if ((value&4) && !stuck_master)regs[REG_STATUS/4]&=~0x80000u;
        if ((value&0x04000000) && !stuck_reset) {
            value&=~0x04000000u;regs[PCH_EXTCNF/4]&=~PCH_SWFLAG;
            if(firmware_claims_after_reset)regs[PCH_EXTCNF/4]|=PCH_SWFLAG;
        }
        CHECK(!(value&0x80000000u)); /* no destructive PHY reset */
    }
    if (reg==PCH_MDIC) {
        mdio_count++;
        unsigned address=(value>>21)&31,index=(value>>16)&31;
        CHECK(address==1 || address==2);
        if (mdio_timeout) { regs[reg/4]=value;return; }
        if (mdio_error) { regs[reg/4]=value|0x50000000u;return; }
        unsigned page=address==1?selected_page:0;
        CHECK(page<800);
        if (value&0x04000000) {
            if (address==1 && index==31)selected_page=(value&0xffff)>>5;
            else if(address==2 && index==16)emi_address=value;
            else if(address==2 && index==17)emi_regs[emi_address]=value;
            else {
                /* Host wake bit cannot change until MACPD was unlocked by
                 * an earlier write. Preserve simulated ME/reserved bits. */
                u16 data=value;
                if(page==769 && index==17 && !(phy_regs[page][index]&4))
                    data=(data&~0x10u)|(phy_regs[page][index]&0x10u);
                if(page==769 && index==17 && stuck_host_wake)data=phy_regs[page][index];
                phy_regs[page][index]=data;
            }
        } else value=(value&0xffff0000u)|
            ((address==2 && index==17)?emi_regs[emi_address]:phy_regs[page][index]);
        value|=0x10000000;
        if (wrong_reg)value^=1u<<16;
    }
    regs[reg/4]=value;
}
static void reset(u16 id) {
    memset(regs,0,sizeof(regs));memset(phy_regs,0,sizeof(phy_regs));
    memset(emi_regs,0,sizeof(emi_regs));emi_address=0;
    emi_regs[0x8001]=0xa506;phy_regs[772][20]=0x7880;
    phy_regs[772][28]=0xf800;phy_regs[776][20]=0xc023;
    mmio=NULL;rx=NULL;tx=NULL;ready=false;page_count=0;last_link=0xffffffffu;
    allocated=freed=elapsed=mdio_count=enabled_early=selected_page=0;
    fail_alloc=-1;model_id=id;model_bus=0;model_dev=id==0x100e?3:31;pci_cmd=0;
    has_pm=stuck_pm=stuck_host_wake=false;pmcsr=0;
    firmware_claims_after_reset=false;
    busy=mdio_timeout=mdio_error=wrong_reg=stuck_reset=stuck_master=stuck_ulp=bad_bar=false;
    regs[REG_STATUS/4]=0x83;regs[PCH_FWSM/4]=0x40;
    regs[REG_RAL/4]=0x12005452;regs[REG_RAH/4]=0x80005634;
    phy_regs[0][2]=0x0154;phy_regs[0][3]=0x00b1;
    phy_regs[769][23]=1;phy_regs[779][16]=0x1d74;
    n_rx=n_tx=b_rx=b_tx=rx_dropped=tx_busy=0;
    phy_regs[769][20]=0x4000;
    phy_regs[769][17]=0x38f0; /* host wake + active PD, unrelated bits retained */
}
static void rejected(void) {
    CHECK(!e1000_init());CHECK(!ready && !(pci_cmd&4));
    CHECK(e1000_send("not a frame",11)==-1);u8 frame[64];CHECK(e1000_receive(frame,64)==0);
    CHECK(allocated==freed && !enabled_early && elapsed<10000);
}
int main(void) {
    const u16 ids[]={0x100e,0x10d3,0x1570,0x15b8,0x15d8,0x15d6};
    for(unsigned i=0;i<sizeof(ids)/sizeof(ids[0]);i++) {
        reset(ids[i]);CHECK(e1000_init());CHECK(ready && (pci_cmd&4) && !enabled_early);
        CHECK(adapter.func==(ids[i]==0x100e || ids[i]==0x10d3?0:6));
        CHECK(e1000_link());regs[REG_STATUS/4]=0;CHECK(!e1000_link());regs[REG_STATUS/4]=0x83;
        if (kind==PCH) {
            CHECK(!(phy_regs[769][20]&0x4000));
            CHECK(phy_regs[769][17]==(0x38f0&~0x54u));
            CHECK(emi_regs[0x8001]==0xa500 && emi_regs[0xb20c]==1);
            CHECK(phy_regs[772][20]==0x1080);
            CHECK(phy_regs[772][28]==0xf8fa);
            CHECK((phy_regs[770][17]&0x200)!=0);
            CHECK(((phy_regs[776][20]>>2)&0x3ff)==0x18);
            CHECK((phy_regs[776][20]&0xf003)==0xc003);
            CHECK((regs[REG_TIPG/4]&0x3ff)==8);
            CHECK((regs[0x1000/4]&0xffff)==26);
            CHECK((regs[0xe4/4]&4) && (regs[0x5bb4/4]&0x1800)==0x1800);
            CHECK(!(regs[PCH_EXTCNF/4]&PCH_SWFLAG));
            CHECK((phy_regs[0][0]&0x1200)==0x1200);
            CHECK((phy_regs[0][9]&0x300)==0x200);
            CHECK(!(phy_regs[779][16]&0x1d74));
            CHECK(regs[0x100c/4]&0x10000);
        }
    }
    /* Every reconnect/speed change reapplies PHY clock/FIFO settings. */
    reset(0x15b8);regs[0xf18/4]=0x80000000u;CHECK(e1000_init());
    CHECK(regs[0x10/4]&0x80000000u);
    regs[REG_STATUS/4]=0x43;CHECK(e1000_link());
    CHECK(phy_regs[772][28]==0xfbe8 && phy_regs[776][20]==0xc023);
    CHECK((regs[REG_TIPG/4]&0x3ff)==12 && emi_regs[0xb20c]==1);
    regs[REG_STATUS/4]=2;CHECK(e1000_link());
    CHECK((regs[REG_TIPG/4]&0x3ff)==255 && emi_regs[0xb20c]==0);
    regs[REG_STATUS/4]=0;CHECK(!e1000_link());
    regs[REG_STATUS/4]=0x83;CHECK(e1000_link());
    CHECK(phy_regs[772][28]==0xf8fa && ((phy_regs[776][20]>>2)&0x3ff)==0x18);
    /* Firmware-owned PHY configuration must be left untouched, even link up. */
    reset(0x15b8);regs[PCH_FWSM/4]=0;CHECK(e1000_init());
    CHECK(emi_regs[0x8001]==0xa506 && !emi_regs[0xb20c]);
    CHECK(phy_regs[769][17]==0x38f0);
    CHECK(phy_regs[772][20]==0x7880 && phy_regs[772][28]==0xf800);
    /* Preserve a larger calibrated FIFO gap at 1 Gb/s. */
    reset(0x15b8);phy_regs[776][20]=0xc203;CHECK(e1000_init());
    CHECK(phy_regs[776][20]==0xc203);
    reset(0x15b8);model_bus=4;CHECK(e1000_init());CHECK(adapter.bus==4);
    reset(0x15b8);has_pm=true;pmcsr=0x8003;CHECK(e1000_init());CHECK(pmcsr==0x8000);
    reset(0x15b8);has_pm=stuck_pm=true;pmcsr=3;rejected();
    reset(0x15b8);regs[PCH_FWSM/4]=0x8440;CHECK(e1000_init());CHECK(!(regs[PCH_H2ME/4]&0x1800));
    reset(0x15b8);regs[PCH_FWSM/4]=0;CHECK(e1000_init());CHECK(!phy_regs[0][0]);
    reset(0x15b8);busy=true;regs[PCH_EXTCNF/4]=PCH_SWFLAG;rejected();CHECK(regs[PCH_EXTCNF/4]&PCH_SWFLAG);
    reset(0x15b8);firmware_claims_after_reset=true;rejected();CHECK(regs[PCH_EXTCNF/4]&PCH_SWFLAG);
    reset(0x15b8);stuck_host_wake=true;rejected();
    reset(0x15b8);mdio_timeout=true;rejected();
    reset(0x15b8);mdio_error=true;rejected();
    reset(0x15b8);wrong_reg=true;rejected();
    reset(0x15b8);stuck_reset=true;rejected();
    reset(0x15b8);stuck_master=true;regs[REG_STATUS/4]|=0x80000;rejected();
    reset(0x15b8);stuck_ulp=true;regs[PCH_FWSM/4]=0x8440;rejected();
    reset(0x15b8);bad_bar=true;rejected();
    reset(0x15b8);regs[REG_RAL/4]=0xffffffff;regs[REG_RAH/4]=0xffffffff;rejected();
    for(int i=0;i<34;i++) { reset(0x15b8);fail_alloc=i;rejected(); }
    reset(0x100e);CHECK(e1000_init());
    u8 input[BUF],output[BUF];memset(input,0x5a,sizeof(input));
    for(unsigned i=0;i<96;i++) {
        unsigned slot=tx_cur;tx[slot].status=DD;
        CHECK(e1000_send(input,14)==0);CHECK(tx[slot].len==60);
        CHECK(!memcmp(tx_buf[slot],input,14));
        for(unsigned j=14;j<60;j++)CHECK(!tx_buf[slot][j]);
    }
    tx[tx_cur].status=0;CHECK(e1000_send(input,60)==-1);
    CHECK(e1000_send(input,BUF+1)==-1);
    for(unsigned i=0;i<96;i++) {
        unsigned slot=rx_cur;memcpy(rx_buf[slot],input,64);
        rx[slot].len=64;rx[slot].errors=0;rx[slot].status=DD|EOP;
        CHECK(e1000_receive(output,sizeof(output))==64);CHECK(!memcmp(input,output,64));
    }
    unsigned a=rx_cur,b=(a+1)%NRX,c=(a+2)%NRX;
    rx[a].len=BUF+1;rx[a].status=DD|EOP;
    rx[b].len=64;rx[b].errors=1;rx[b].status=DD|EOP;
    rx[c].len=64;rx[c].errors=0;rx[c].status=DD|EOP;memcpy(rx_buf[c],input,64);
    CHECK(e1000_receive(output,BUF)==64);CHECK(rx_cur==(c+1)%NRX);
    a=rx_cur;b=(a+1)%NRX;rx[a].status=DD;rx[a].len=64;rx[a].errors=0;
    rx[b].status=DD|EOP;rx[b].len=64;rx[b].errors=0;
    CHECK(e1000_receive(output,BUF)==0);CHECK(!discard_rx);
    const u16 extended_models[]={0x10d3,0x15b8};
    for(unsigned model=0;model<2;model++) {
        reset(extended_models[model]);CHECK(e1000_init());
        CHECK(regs[0x5008/4]&0x8000);
        CHECK(regs[REG_RDT/4]==NRX-1 && (regs[REG_RCTL/4]&2));
        for(unsigned i=0;i<96;i++) {
            unsigned slot=rx_cur;memcpy(rx_buf[slot],input,64);
            /* Hardware replaces the read buffer pointer with metadata. */
            rx[slot].extended.mrq=0x1234;rx[slot].extended.rss=0xdeadbeef;
            rx[slot].extended.length=64;rx[slot].extended.vlan=0;
            rx[slot].extended.status_error=DD|EOP;
            CHECK(e1000_receive(output,BUF)==64 && !memcmp(input,output,64));
            CHECK(rx[slot].addr==(u32)rx_buf[slot]);
            CHECK(!rx[slot].extended.status_error && !rx[slot].extended.length);
        }
        unsigned slot=rx_cur;
        rx[slot].extended.length=BUF+1;rx[slot].extended.status_error=DD|EOP;
        CHECK(!e1000_receive(output,BUF));
        slot=rx_cur;rx[slot].extended.length=64;rx[slot].extended.status_error=DD|EOP|0x80000000u;
        CHECK(!e1000_receive(output,BUF));
    }
    printf("Intel Ethernet: PCI function/bridge discovery, PHY/ULP, bounded failures, DMA allocation, RX/TX wrap and malformed frames: PASS\n");
    return 0;
}
