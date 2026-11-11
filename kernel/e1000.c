#include "klog.h"
/* Polling Intel Ethernet: 82540EM, 82574L and 100/200-series I219-V.
 * TX uses legacy descriptors; RX is legacy on 82540 and extended on e1000e.
 * PCH PHY/firmware initialization is isolated in e1000_pch.inc. */
#include "e1000.h"
#include "pci.h"
#include "paging.h"
#include "pmm.h"
#include "string.h"
#include "timer.h"
#include "vga.h"

#define MMIO_VIRT 0xe1000000u
#define MMIO_BYTES 0x20000u
#define REG_CTRL 0x0000
#define REG_STATUS 0x0008
#define REG_IMC 0x00d8
#define REG_RCTL 0x0100
#define REG_TCTL 0x0400
#define REG_TIPG 0x0410
#define REG_RDBAL 0x2800
#define REG_RDBAH 0x2804
#define REG_RDLEN 0x2808
#define REG_RDH 0x2810
#define REG_RDT 0x2818
#define REG_TDBAL 0x3800
#define REG_TDBAH 0x3804
#define REG_TDLEN 0x3808
#define REG_TDH 0x3810
#define REG_TDT 0x3818
#define REG_MTA 0x5200
#define REG_RAL 0x5400
#define REG_RAH 0x5404
#define NRX 32
#define NTX 32
#define BUF 2048
#define DD 1
#define EOP 2

struct rx_desc {
    union {
        struct { u64 addr;u16 len,csum;u8 status,errors;u16 special; };
        struct { u32 mrq,rss,status_error;u16 length,vlan; } extended;
    };
} __attribute__((packed));
struct tx_desc { u64 addr;u16 len;u8 cso,cmd,status,css;u16 special; } __attribute__((packed));
_Static_assert(sizeof(struct rx_desc)==16 && sizeof(struct tx_desc)==16,"Intel descriptor ABI");
enum { LEGACY,PCIE,PCH };
static volatile u8 *mmio;
static volatile struct rx_desc *rx;
static volatile struct tx_desc *tx;
static u8 *rx_buf[NRX],*tx_buf[NTX];
static u32 rx_cur,tx_cur,n_rx,n_tx,b_rx,b_tx;
static u32 rx_dropped,tx_busy;
static u8 mac[6];
static bool ready,discard_rx;
static unsigned kind;
static struct pci_dev adapter;
static u32 pages[2+(NRX+NTX)/2],page_count;
static u32 last_link=0xffffffffu;

#ifdef HOST_TEST
extern u32 e1000_test_read(u32);
extern void e1000_test_write(u32,u32);
static u32 rd(u32 reg) { return e1000_test_read(reg); }
static void wr(u32 reg,u32 value) { e1000_test_write(reg,value); }
#else
static u32 rd(u32 reg) { return *(volatile u32 *)(mmio+reg); }
static void wr(u32 reg,u32 value) { *(volatile u32 *)(mmio+reg)=value; }
#endif

static bool wait_bits(u32 reg,u32 mask,u32 value,unsigned ms) {
    for (unsigned elapsed=0;elapsed<=ms;elapsed+=10) {
        if ((rd(reg)&mask)==value) return true;
        if (elapsed<ms) sleep_ms(10);
    }
    return false;
}
#include "e1000_pch.inc"

static bool supported(u16 id,unsigned *type) {
    if (id==0x100e) { *type=LEGACY;return true; }
    if (id==0x10d3) { *type=PCIE;return true; }
    if (id==0x1570 || id==0x15b8 || id==0x15d8 || id==0x15d6) {
        *type=PCH;return true;
    }
    return false;
}
static bool find_adapter(void) {
    for (unsigned bus=0;bus<256;bus++) for (unsigned dev=0;dev<32;dev++) {
        if ((pci_read(bus,dev,0,0)&0xffff)==0xffff) continue;
        unsigned count=(pci_read(bus,dev,0,0x0c)&0x800000)?8:1;
        for (unsigned fn=0;fn<count;fn++) {
            u32 id=pci_read(bus,dev,fn,0),cls=pci_read(bus,dev,fn,8);
            unsigned type;
            if ((id&0xffff)==0x8086 && (cls>>16)==0x0200 && !supported(id>>16,&type))
                klogf("NET: unsupported Intel Ethernet PCI %u:%u.%u id=%x\n",bus,dev,fn,id>>16);
            if ((id&0xffff)!=0x8086 || (cls>>16)!=0x0200 || !supported(id>>16,&type)) continue;
            adapter=(struct pci_dev){.bus=bus,.dev=dev,.func=fn,.vendor=0x8086,
                .device=id>>16,.class_code=2,.subclass=0,.bar0=pci_read(bus,dev,fn,0x10)};
            kind=type;return true;
        }
    }
    return false;
}
const char *e1000_name(void) {
    return kind==PCH?"Intel I219-V":kind==PCIE?"Intel e1000e (82574L)":"Intel e1000 (82540EM)";
}
static void bus_master(bool on) {
    u32 cmd=pci_read(adapter.bus,adapter.dev,adapter.func,4)&0xffff;
    pci_write(adapter.bus,adapter.dev,adapter.func,4,on?cmd|6:(cmd|2)&~4u);
}
static bool power_d0(void) {
    if (!(pci_read(adapter.bus,adapter.dev,adapter.func,4)&(1u<<20))) return true;
    unsigned cap=pci_read(adapter.bus,adapter.dev,adapter.func,0x34)&0xfc;
    for (unsigned count=0;cap && count<48;count++) {
        if (cap<0x40 || cap>0xf8) return false;
        u32 header=pci_read(adapter.bus,adapter.dev,adapter.func,cap);
        if ((header&255)==1) {
            u32 pm=pci_read(adapter.bus,adapter.dev,adapter.func,cap+4);
            if (pm&3) {
                /* Do not accidentally acknowledge PME_Status (RW1C). */
                pci_write(adapter.bus,adapter.dev,adapter.func,cap+4,pm&~0x8003u);
                sleep_ms(10);
                klogf("NET: PCI power transition D%u -> D0\n",pm&3);
            }
            return !(pci_read(adapter.bus,adapter.dev,adapter.func,cap+4)&3);
        }
        cap=(header>>8)&0xfc;
    }
    return !cap;
}
static void *dma_page(void) {
    if (page_count>=sizeof(pages)/sizeof(pages[0])) return NULL;
    u32 p=pmm_alloc();
    if (p) { pages[page_count++]=p;memset((void *)p,0,PAGE_SIZE); }
    return (void *)p;
}
static bool valid_mac(void) {
    unsigned any=0,all=255;
    for (unsigned i=0;i<6;i++) { any|=mac[i];all&=mac[i]; }
    return any && all!=255 && !(mac[0]&1);
}
static void read_mac(void) {
    u32 lo=rd(REG_RAL),hi=rd(REG_RAH);
    for (unsigned i=0;i<4;i++)mac[i]=lo>>(i*8);
    mac[4]=hi;mac[5]=hi>>8;
}
bool e1000_init(void) {
    if (ready) return true;
    if (!find_adapter()) { klogf("NET: no supported Intel Ethernet controller\n");return false; }
    if (!power_d0()) { klogf("NET: PCI power/capability initialization failed\n");return false; }
    u32 bar=adapter.bar0,phys=bar&~15u;
    if ((bar&1) || !phys || (phys&4095) || phys>0xffffffffu-MMIO_BYTES ||
        ((bar&6)==4 && pci_read(adapter.bus,adapter.dev,adapter.func,0x14))) {
        klogf("NET: unsupported Ethernet MMIO BAR=%x\n",bar);return false;
    }
    for (u32 off=0;off<MMIO_BYTES;off+=PAGE_SIZE)
        if (vmm_map(MMIO_VIRT+off,phys+off,PG_WRITE|0x18)) return false;
    mmio=(volatile u8 *)MMIO_VIRT;
    u32 cmd=pci_read(adapter.bus,adapter.dev,adapter.func,4)&0xffff;
    pci_write(adapter.bus,adapter.dev,adapter.func,4,cmd|2);
    klogf("NET: %s PCI %u:%u.%u id=%x MMIO=%x\n",e1000_name(),adapter.bus,adapter.dev,adapter.func,adapter.device,phys);
    read_mac();
    u8 saved_mac[6];memcpy(saved_mac,mac,6);
    if (kind==PCH) {
        if (!pch_reset()) goto failed;
    } else {
        wr(REG_IMC,0xffffffff);wr(REG_RCTL,0);wr(REG_TCTL,0);
        if (kind==PCIE) {
            wr(REG_CTRL,rd(REG_CTRL)|4);
            if (!wait_bits(REG_STATUS,0x80000,0,100)) goto failed;
        }
        sleep_ms(10);
    }
    bus_master(false);
    read_mac();
    if (!valid_mac()) memcpy(mac,saved_mac,6);
    if (!valid_mac()) { klogf("NET: missing/invalid NVM MAC address\n");goto failed; }
    wr(REG_RAL,mac[0]|((u32)mac[1]<<8)|((u32)mac[2]<<16)|((u32)mac[3]<<24));
    wr(REG_RAH,mac[4]|((u32)mac[5]<<8)|0x80000000u);
    for (unsigned i=0;i<(kind==PCH?32u:128u);i++)wr(REG_MTA+i*4,0);
    /* Allocate every descriptor/buffer before enabling any DMA engine. */
    rx=dma_page();tx=dma_page();
    if (!rx || !tx) goto allocation_failed;
    for (unsigned i=0;i<NRX;i+=2) {
        u8 *p=dma_page();if (!p)goto allocation_failed;
        rx_buf[i]=p;rx_buf[i+1]=p+BUF;
    }
    for (unsigned i=0;i<NTX;i+=2) {
        u8 *p=dma_page();if (!p)goto allocation_failed;
        tx_buf[i]=p;tx_buf[i+1]=p+BUF;
    }
    for (unsigned i=0;i<NRX;i++)rx[i].addr=(u32)rx_buf[i];
    for (unsigned i=0;i<NTX;i++) { tx[i].addr=(u32)tx_buf[i];tx[i].status=DD; }
    rx_cur=tx_cur=0;discard_rx=false;
    wr(REG_RDBAL,(u32)rx);wr(REG_RDBAH,0);wr(REG_RDLEN,NRX*sizeof(*rx));wr(REG_RDH,0);wr(REG_RDT,0);
    wr(REG_TDBAL,(u32)tx);wr(REG_TDBAH,0);wr(REG_TDLEN,NTX*sizeof(*tx));wr(REG_TDH,0);wr(REG_TDT,0);
    if (kind!=LEGACY) {
        wr(0x5008,rd(0x5008)|0x80c0u); /* e1000e extended RX status */
        wr(0x5000,0); /* no IP/transport checksum offload */
        wr(0x3828,(rd(0x3828)&~0x003f003fu)|0x0141001fu);
        wr(0x2828,(rd(0x2828)&~0x003f3f3fu)|0x0101081fu);
        wr(0x5b00,rd(0x5b00)&~0x3fu); /* coherent/snooped DMA */
    }
    __sync_synchronize();bus_master(true);
    wr(REG_CTRL,(rd(REG_CTRL)&~0x80001804u)|0x40u);
    wr(REG_TIPG,0x0060200a);
    wr(REG_TCTL,(1u<<1)|(1u<<3)|(0x10u<<4)|(0x40u<<12));
    wr(REG_RCTL,(1u<<1)|(1u<<15)|(1u<<26)); /* exact MAC + broadcast, 2 KiB buffers */
    (void)rd(REG_STATUS);
    /* Publish the ready receive buffers after the receive engine is enabled. */
    __sync_synchronize();wr(REG_RDT,NRX-1);(void)rd(REG_STATUS);
    ready=true;
    klogf("NET: RX/TX ready, 32 descriptors each, polling\n");
    klogf("NET: RX mode=%s RCTL=%x RFCTL=%x PBA=%x\n",kind==LEGACY?"legacy":"extended",rd(REG_RCTL),rd(0x5008),rd(0x1000));
    e1000_link();
    return true;
allocation_failed:
    klogf("NET: insufficient DMA memory\n");
    /* DMA has not been enabled with these addresses yet. */
    while (page_count)pmm_free(pages[--page_count]);
failed:
    wr(REG_IMC,0xffffffff);wr(REG_RCTL,0);wr(REG_TCTL,0);bus_master(false);
    if (kind==PCH)wr(PCH_CTRL_EXT,rd(PCH_CTRL_EXT)&~0x10000000u);
    mmio=NULL;rx=NULL;tx=NULL;ready=false;
    klogf("NET: initialization failed for %s\n",e1000_name());
    return false;
}
bool e1000_link(void) {
    if (!ready) return false;
    u32 status=rd(REG_STATUS),link=status&0xc3;
    if (link!=last_link) {
        last_link=link;
        if (kind==PCH && (status&2)) {
            (void)pch_link_start(status);
            pch_phy_diagnose();
        }
        if (status&2) klogf("NET: link up, %u Mbps %s duplex\n",(status&0x80)?1000u:(status&0x40)?100u:10u,(status&1)?"full":"half");
        else klogf("NET: link down; waiting for cable/autonegotiation\n");
    }
    return (status&2)!=0;
}
void e1000_mac(u8 out[6]) { memcpy(out,mac,6); }
int e1000_send(const void *frame,u32 len) {
    if (!ready || !frame || len<14 || len>BUF) return -1;
    volatile struct tx_desc *t=&tx[tx_cur];
    if (!(t->status&DD)) { tx_busy++;return -1; }
    __sync_synchronize();memcpy(tx_buf[tx_cur],frame,len);
    if (len<60) { memset(tx_buf[tx_cur]+len,0,60-len);len=60; }
    t->len=len;t->cmd=0x0b;t->status=0;
    __sync_synchronize();tx_cur=(tx_cur+1)%NTX;wr(REG_TDT,tx_cur);
    n_tx++;b_tx+=len;return 0;
}
int e1000_receive(void *buf,u32 max) {
    if (!ready || !buf) return 0;
    for (unsigned budget=0;budget<NRX;budget++) {
        volatile struct rx_desc *r=&rx[rx_cur];
        u32 status=kind==LEGACY?r->status:r->extended.status_error;
        if (!(status&DD)) return 0;
        __sync_synchronize();u32 length=kind==LEGACY?r->len:r->extended.length;
        u32 errors=(kind==LEGACY?r->errors:status>>24)&0x97u; /* frame errors, not checksum hints */
        bool valid=!discard_rx && (status&EOP) && !errors && length>=14 && length<=BUF && length<=max;
        if (valid)memcpy(buf,rx_buf[rx_cur],length);
        discard_rx=!(status&EOP);
        /* Extended writeback replaces the buffer address. Restore the read
         * descriptor completely before returning this slot to the device. */
        r->addr=(u32)rx_buf[rx_cur];
        r->extended.status_error=0;r->extended.length=0;r->extended.vlan=0;
        __sync_synchronize();wr(REG_RDT,rx_cur);rx_cur=(rx_cur+1)%NRX;
        if (valid) { n_rx++;b_rx+=length;return length; }
        rx_dropped++;
    }
    return 0;
}
void e1000_stats(u32 *a,u32 *b,u32 *c,u32 *d) { *a=n_rx;*b=n_tx;*c=b_rx;*d=b_tx; }
void e1000_diagnose(void) {
    if (!ready) { klogf("NET: DMA diagnostics unavailable (adapter not ready)\n");return; }
    unsigned previous=(tx_cur+NTX-1)%NTX;
    klogf("NET: TX queued=%u busy=%u TDH=%u TDT=%u last_status=%x GPTC=%u\n",
        n_tx,tx_busy,rd(REG_TDH),rd(REG_TDT),tx[previous].status,rd(0x4080));
    klogf("NET: RX accepted=%u dropped=%u RDH=%u RDT=%u next_status=%x GPRC=%u CRC=%u\n",
        n_rx,rx_dropped,rd(REG_RDH),rd(REG_RDT),
        kind==LEGACY?rx[rx_cur].status:rx[rx_cur].extended.status_error,rd(0x4074),rd(0x4000));
    klogf("NET: RX config RCTL=%x RFCTL=%x RXDCTL=%x PBA=%x RAL=%x RAH=%x\n",
        rd(REG_RCTL),rd(0x5008),rd(0x2828),rd(0x1000),rd(REG_RAL),rd(REG_RAH));
    klogf("NET: RX path CTRL=%x STATUS=%x CTRL_EXT=%x RDBAL=%x RDLEN=%u RDTR=%x RADV=%x\n",
        rd(REG_CTRL),rd(REG_STATUS),rd(PCH_CTRL_EXT),rd(REG_RDBAL),rd(REG_RDLEN),rd(0x2820),rd(0x282c));
    klogf("NET: RX errors missed=%u total=%u length=%u align=%u\n",
        rd(0x4010),rd(0x40d0),rd(0x4040),rd(0x4004));
    if(kind==PCH)pch_phy_diagnose();
    if(kind==PCH) klogf("NET: PCH receive FWSM=%x MANC=%x MANC2H=%x FEXT7=%x FEXT9=%x\n",
        rd(PCH_FWSM),rd(0x5820),rd(0x5860),rd(0xe4),rd(0x5bb4));
}
