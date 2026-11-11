/* Minimal polling xHCI 1.x + USB HID boot protocol.
 * DMA lives in PMM pages below 1 GiB, never in the virtual kernel heap.
 * Direct root-port devices only; no hubs, hotplug, storage, or MSI dependency.
 * Reference: Intel xHCI 1.2, sections 4.3, 4.5, 6.2 and 6.4.
 */
#include "xhci.h"
#include "pci.h"
#include "pmm.h"
#include "paging.h"
#include "string.h"
#include "timer.h"
#include "task.h"
#include "vga.h"
#include "keyboard.h"
#include "mouse.h"

#ifdef MYOS_HARDWARE

#define MMIO 0xe4000000u
#define WINDOW 0x40000u
#define NDEV 8
#define NHID 16
#define NCYCLE 256
#define TYPE(n) ((u32)(n)<<10)
#define BARRIER() __asm__ volatile ("" ::: "memory")
struct trb { u32 lo,hi,status,control; };
struct ring { struct trb *base; unsigned next,cycle; };
struct usbdev {
    unsigned slot,port,speed,mps;
    u32 *context,*input;
    struct ring control;
    u8 *buffer;
};
struct hid {
    struct usbdev *dev;
    struct ring ring;
    unsigned dci,protocol,packet,interface;
    u8 *report;
    u8 previous[8];
    u32 pending;
    u8 repeat_key;
    u32 repeat_at;
    bool active;
};
static volatile u8 *mmio;
static u32 op,db,rt;
static unsigned context_bytes,maxslots,ports;
static struct ring commands;
static volatile struct trb *events;
static unsigned event_index,event_cycle;
static u64 *dcbaa;
static struct usbdev devices[NDEV];
static struct hid hids[NHID];
static unsigned ndev,nhid;
static u32 waiting,wait_code,wait_slot;
static bool running,poisoned;

static u32 rd(u32 o) { return *(volatile u32 *)(mmio+o); }
static void wr(u32 o,u32 v) { *(volatile u32 *)(mmio+o)=v; }
static void wr64(u32 o,u32 low) { wr(o,low); wr(o+4,0); }
static void *page(void) {
    u32 p=pmm_alloc();
    if (p) memset((void *)p,0,4096);
    return (void *)p;
}
static bool wait_reg(u32 offset,u32 mask,u32 value,unsigned ms) {
    u32 start=timer_ticks();
    do {
        if ((rd(offset)&mask)==value) return true;
        sleep_ms(10);
    } while (timer_ticks()-start<(ms*TIMER_HZ+999)/1000);
    return false;
}
static bool ring_init(struct ring *r) {
    r->base=page(); r->next=0; r->cycle=1;
    return r->base!=NULL;
}
static u32 enqueue(struct ring *r,u32 lo,u32 hi,u32 status,u32 control) {
    if (r->next==NCYCLE-1) {
        struct trb *link=&r->base[NCYCLE-1];
        link->lo=(u32)r->base; link->hi=0; link->status=0;
        BARRIER(); link->control=TYPE(6)|2|r->cycle;
        r->cycle^=1; r->next=0;
    }
    struct trb *t=&r->base[r->next++];
    t->lo=lo; t->hi=hi; t->status=status;
    BARRIER(); t->control=control|r->cycle;
    return (u32)t;
}
static void ring_doorbell(unsigned slot,unsigned dci) {
    BARRIER(); wr(db+slot*4,dci);
}
static void arm(struct hid *h) {
    if (!h->active) return;
    memset(h->report,0,64);
    h->pending=enqueue(&h->ring,(u32)h->report,0,h->packet,TYPE(1)|(1<<5)|(1<<2));
    ring_doorbell(h->dev->slot,h->dci);
}
static bool contains(const u8 *report,u8 key) {
    for (int i=2;i<8;i++) if (report[i]==key) return true;
    return false;
}
/* USB HID Usage Tables -> scan set 1. Feed the existing layout/shortcut path. */
static u16 scancode(u8 usage) {
    static const u8 letters[26]={30,48,46,32,18,33,34,35,23,36,37,38,50,
                               49,24,25,16,19,31,20,22,47,17,45,21,44};
    static const u8 symbols[]={28,1,14,15,57,12,13,26,27,43,43,39,40,41,51,52,53,58};
    if (usage>=4 && usage<=29) return letters[usage-4];
    if (usage>=30 && usage<=38) return usage-28;
    if (usage==39) return 11;
    if (usage>=40 && usage<=57) return symbols[usage-40];
    if (usage>=58 && usage<=67) return 59+usage-58;
    switch (usage) {
    case 68:return 87; case 69:return 88;
    case 73:return 0xe052; case 74:return 0xe047; case 75:return 0xe049;
    case 76:return 0xe053; case 77:return 0xe04f; case 78:return 0xe051;
    case 79:return 0xe04d; case 80:return 0xe04b; case 81:return 0xe050; case 82:return 0xe048;
    case 84:return 0xe035; case 85:return 55; case 86:return 74; case 87:return 78;
    case 88:return 0xe01c; case 89:return 79; case 90:return 80; case 91:return 81;
    case 92:return 75; case 93:return 76; case 94:return 77; case 95:return 71;
    case 96:return 72; case 97:return 73; case 98:return 82; case 99:return 83;
    default:return 0;
    }
}
static void key(u8 usage,bool release) {
    u16 sc=scancode(usage);
    if (!sc) return;
    if (sc&0xff00) kb_scancode(0xe0);
    kb_scancode((sc&255)|(release?0x80:0));
}
static void keyboard_report(struct hid *h) {
    u8 *r=h->report;
    /* Rollover reports do not describe which keys have been released. */
    for (int i=2;i<8;i++) if (r[i] && r[i]<=3) return;
    static const u16 mods[8]={29,42,56,0xe05b,0xe01d,54,0xe038,0xe05c};
    u32 flags=irq_save();
    for (int i=0;i<8;i++) if ((r[0]^h->previous[0])&(1<<i)) {
        if (mods[i]&0xff00) kb_scancode(0xe0);
        kb_scancode((mods[i]&255)|((r[0]&(1<<i))?0:0x80));
    }
    for (int i=2;i<8;i++) if (h->previous[i] && !contains(r,h->previous[i])) {
        key(h->previous[i],true);
        if (h->repeat_key==h->previous[i]) h->repeat_key=0;
    }
    for (int i=2;i<8;i++) if (r[i] && !contains(h->previous,r[i])) {
        key(r[i],false);
        if (r[i]!=57) { h->repeat_key=r[i]; h->repeat_at=timer_ticks()+TIMER_HZ/2; }
    }
    memcpy(h->previous,r,8);
    irq_restore(flags);
}
static void pump(void) {
    for (unsigned n=0;n<NCYCLE;n++) {
        volatile struct trb *v=&events[event_index];
        u32 ctl=v->control;
        if ((ctl&1)!=event_cycle) break;
        BARRIER();
        struct trb e={v->lo,v->hi,v->status,ctl};
        unsigned type=(ctl>>10)&63,code=e.status>>24;
        if ((type==33 || type==32) && e.lo==waiting && !e.hi) {
            wait_code=code; wait_slot=ctl>>24;
        } else if (type==32) {
            for (unsigned i=0;i<nhid;i++) {
                struct hid *h=&hids[i];
                if (h->active && h->pending==e.lo && !e.hi &&
                    h->dev->slot==(ctl>>24) && h->dci==((ctl>>16)&31)) {
                    unsigned transferred=h->packet-(e.status&0xffffff);
                    if ((code==1 || code==13) && transferred>=(h->protocol==1?8u:3u)) {
                        if (h->protocol==1) keyboard_report(h);
                        else mouse_usb_report(h->report[0]&7,(i8)h->report[1],(i8)h->report[2]);
                    }
                    if (code==1 || code==13) arm(h);
                    else { h->active=false; kprintf("USB HID slot %u stopped: completion %u\n",h->dev->slot,code); }
                    break;
                }
            }
        } else if (type==37) {
            poisoned=true;
            kprintf("xHCI host controller event: %u\n",code);
        }
        event_index++;
        if (event_index==NCYCLE) { event_index=0; event_cycle^=1; }
        wr64(rt+0x38,(u32)&events[event_index]|8); /* ERDP, clear EHB */
    }
}
static unsigned completion(u32 pointer,unsigned ms) {
    waiting=pointer; wait_code=0; wait_slot=0;
    u32 start=timer_ticks();
    do {
        pump();
        if (wait_code || poisoned) break;
        sleep_ms(10);
    } while (timer_ticks()-start<(ms*TIMER_HZ+999)/1000);
    waiting=0;
    if (!wait_code) {
        /* Never reuse a ring after an uncompleted TD/command. */
        poisoned=true;
        kprintf("xHCI timed out waiting for %x\n",pointer);
    }
    return wait_code;
}
static bool command(u32 lo,u32 ctl) {
    if (poisoned) return false;
    u32 pointer=enqueue(&commands,lo,0,0,ctl);
    ring_doorbell(0,0);
    unsigned code=completion(pointer,1000);
    if (code!=1) kprintf("xHCI command %u completion %u\n",(ctl>>10)&63,code);
    return code==1;
}
static bool control(struct usbdev *d,u8 request_type,u8 request,u16 value,u16 index,u16 length) {
    if (poisoned || length>512) return false;
    bool input=request_type&0x80;
    enqueue(&d->control,request_type|((u32)request<<8)|((u32)value<<16),
            index|((u32)length<<16),8,TYPE(2)|(1<<6)|((length?(input?3:2):0)<<16));
    if (length) enqueue(&d->control,(u32)d->buffer,0,length,TYPE(3)|(input?(1<<16):0));
    u32 pointer=enqueue(&d->control,0,0,0,TYPE(4)|(1<<5)|((!length || !input)?(1<<16):0));
    ring_doorbell(d->slot,1);
    return completion(pointer,1000)==1;
}
static u32 *ctx(u32 *base,unsigned index) { return (u32 *)((u8 *)base+index*context_bytes); }
static void copy_slot(struct usbdev *d) {
    memset(d->input,0,4096);
    memcpy(ctx(d->input,1),ctx(d->context,0),context_bytes);
}
static unsigned interval(unsigned speed,unsigned value) {
    if (!value) value=1;
    if (speed>=3) return value>16?15:value-1;
    unsigned ticks=value*8,n=0;
    while ((1u<<n)<ticks && n<10) n++;
    return n<3?3:n;
}
static bool configure(struct usbdev *d) {
    if (!control(d,0x80,6,0x100,0,8)) return false;
    unsigned mps=d->buffer[7];
    if (d->speed==4) {
        if (mps!=9) return false;
        mps=512;
    }
    if (!(mps==8 || mps==16 || mps==32 || mps==64 || (d->speed==4 && mps==512))) return false;
    if (mps!=d->mps) {
        copy_slot(d);
        d->input[1]=2;
        memcpy(ctx(d->input,2),ctx(d->context,1),context_bytes);
        u32 *ep=ctx(d->input,2);
        ep[1]=(ep[1]&0xffff)|(mps<<16);
        if (!command((u32)d->input,TYPE(13)|(d->slot<<24))) return false;
        d->mps=mps;
    }
    if (!control(d,0x80,6,0x100,0,18) || d->buffer[0]!=18 || d->buffer[1]!=1) return false;
    if (d->buffer[4]==9) { kprintf("USB port %u: hubs are not supported\n",d->port); return false; }
    if (!control(d,0x80,6,0x200,0,9) || d->buffer[1]!=2) return false;
    unsigned total=d->buffer[2]|d->buffer[3]<<8,config=d->buffer[5];
    if (total<9 || total>512 || !control(d,0x80,6,0x200,0,total)) return false;
    u8 descriptors[512]; memcpy(descriptors,d->buffer,total);
    unsigned first=nhid,protocol=0,iface=0,maxdci=1;
    copy_slot(d); d->input[1]=1;
    for (unsigned pos=0;pos+2<=total;) {
        u8 *p=descriptors+pos; unsigned len=p[0];
        if (len<2 || pos+len>total) break;
        if (p[1]==4 && len>=9) {
            protocol=(p[3]==0 && p[5]==3 && p[6]==1 && (p[7]==1 || p[7]==2))?p[7]:0;
            iface=p[2];
        } else if (p[1]==5 && len>=7 && protocol && (p[2]&0x80) && (p[3]&3)==3 && nhid<NHID) {
            unsigned dci=(p[2]&15)*2+1,packet=(p[4]|p[5]<<8)&0x7ff;
            if (dci<3 || packet<(protocol==1?8u:3u) || packet>64 || (d->input[1]&(1u<<dci))) { pos+=len; continue; }
            struct hid *h=&hids[nhid];
            memset(h,0,sizeof(*h));
            h->dev=d; h->dci=dci; h->protocol=protocol; h->packet=packet; h->interface=iface;
            h->report=page();
            if (!h->report || !ring_init(&h->ring)) return false;
            u32 *ep=ctx(d->input,dci+1);
            ep[0]=interval(d->speed,p[6])<<16;
            ep[1]=(packet<<16)|(7<<3)|(3<<1);
            ep[2]=(u32)h->ring.base|1;
            ep[4]=packet|(packet<<16);
            d->input[1]|=1u<<dci;
            if (dci>maxdci) maxdci=dci;
            nhid++; protocol=0;
        }
        pos+=len;
    }
    if (first==nhid) return false;
    u32 *slot=ctx(d->input,1); slot[0]=(slot[0]&0x07ffffff)|(maxdci<<27);
    if (!command((u32)d->input,TYPE(12)|(d->slot<<24))) return false;
    if (!control(d,0,9,config,0,0)) return false;
    for (unsigned i=first;i<nhid;i++) {
        struct hid *h=&hids[i];
        if (!control(d,0x21,11,0,h->interface,0)) continue; /* SET_PROTOCOL(boot) */
        /* SET_IDLE is optional; some devices stall it. No dependence on it. */
        h->active=true; arm(h);
        kprintf("USB HID %s: port %u, slot %u, endpoint %u\n",h->protocol==1?"keyboard":"mouse",d->port,d->slot,h->dci);
    }
    return true;
}
static bool attach(unsigned port) {
    if (ndev>=NDEV || poisoned) return false;
    u32 reg=op+0x400+(port-1)*16,status=rd(reg);
    if (!(status&1)) return false;
    /* PORTSC change bits are RW1C; never echo unrelated writable state. */
    if (!(status&2)) {
        wr(reg,(status&(1u<<9))|(1u<<4));
        if (!wait_reg(reg,1u<<4,0,1000) || !(rd(reg)&2)) {
            kprintf("xHCI: port %u reset failed, PORTSC=%x\n",port,rd(reg));
            return false;
        }
    }
    sleep_ms(20); /* USB reset recovery before Address Device */
    status=rd(reg);
    unsigned speed=(status>>10)&15;
    if (!speed || speed>4) return false;
    if (!command(0,TYPE(9))) return false;
    unsigned slot=wait_slot;
    if (!slot || slot>maxslots) return false;
    struct usbdev *d=&devices[ndev++];
    d->slot=slot; d->port=port; d->speed=speed; d->mps=speed==4?512:speed==3?64:8;
    d->context=page(); d->input=page(); d->buffer=page();
    if (!d->context || !d->input || !d->buffer || !ring_init(&d->control)) return false;
    dcbaa[slot]=(u32)d->context;
    d->input[1]=3;
    u32 *s=ctx(d->input,1),*ep=ctx(d->input,2);
    s[0]=(speed<<20)|(1u<<27); s[1]=port<<16;
    ep[1]=(d->mps<<16)|(4<<3)|(3<<1);
    ep[2]=(u32)d->control.base|1; ep[4]=8;
    if (!command((u32)d->input,TYPE(11)|(slot<<24))) return false;
    kprintf("USB device: port %u, speed %u, slot %u\n",port,speed,slot);
    if (configure(d)) return true;
    /* Failed/unsupported devices must not leave enabled endpoints with DMA. */
    for (unsigned i=0;i<nhid;i++) if (hids[i].dev==d) hids[i].active=false;
    if (!poisoned) command(0,TYPE(10)|(slot<<24));
    return false;
}
static void worker(void *unused) {
    (void)unused;
    while (running && !poisoned) {
        pump();
        for (unsigned i=0;i<nhid;i++) {
            struct hid *h=&hids[i];
            if (h->active && h->repeat_key && (i32)(timer_ticks()-h->repeat_at)>=0) {
                u32 flags=irq_save(); key(h->repeat_key,false); irq_restore(flags);
                h->repeat_at=timer_ticks()+TIMER_HZ/20;
            }
        }
        sleep_ms(10);
    }
    wr(op,rd(op)&~1u);
}
bool xhci_init(void) {
    struct pci_dev controller={0}; bool found=false;
    /* PCH xHCI is often function 0 at bus 0/device 20. Search every function
     * and bridge bus as add-in controllers need not be on bus zero. */
    for (unsigned bus=0;bus<256 && !found;bus++) for (unsigned dev=0;dev<32 && !found;dev++) {
        u32 id=pci_read(bus,dev,0,0);
        if ((id&0xffff)==0xffff) continue;
        unsigned funcs=(pci_read(bus,dev,0,0x0c)&0x800000)?8:1;
        for (unsigned fn=0;fn<funcs;fn++) {
            if ((pci_read(bus,dev,fn,8)>>8)==0x0c0330) {
                controller.bus=bus; controller.dev=dev; controller.func=fn;
                controller.vendor=id&0xffff; found=true; break;
            }
        }
    }
    if (!found) return false;
    kprintf("xHCI: PCI %u:%u.%u\n",controller.bus,controller.dev,controller.func);
    u32 bar=pci_read(controller.bus,controller.dev,controller.func,0x10);
    if ((bar&1) || ! (bar&~15u) || ((bar&6)==4 && pci_read(controller.bus,controller.dev,controller.func,0x14))) {
        kprintf("xHCI: unsupported MMIO BAR (requires physical address below 4 GiB)\n"); return false;
    }
    u32 phys=bar&~15u;
    if (phys>0xffffffffu-WINDOW || (phys&4095)) return false;
    for (unsigned off=0;off<WINDOW;off+=4096)
        if (vmm_map(MMIO+off,phys+off,PG_WRITE|0x18)) return false;
    mmio=(volatile u8 *)MMIO;
    u32 pci_command=pci_read(controller.bus,controller.dev,controller.func,4);
    pci_write(controller.bus,controller.dev,controller.func,4,(pci_command&0xffff)|6|(1<<10));
    op=rd(0)&255; db=rd(0x14)&~3u; rt=rd(0x18)&~31u;
    maxslots=rd(4)&255; ports=rd(4)>>24; context_bytes=(rd(0x10)&4)?64:32;
    kprintf("xHCI: MMIO=%x version=%x slots=%u\n",phys,rd(0)>>16,maxslots);
    if (op<0x20 || db+1024>WINDOW || rt+0x40>WINDOW || op+0x400+ports*16>WINDOW || !maxslots) return false;
    /* BIOS ownership handoff before touching operational registers. */
    unsigned ext=rd(0x10)>>16;
    for (unsigned n=0;ext && n<64;n++) {
        unsigned offset=ext*4;
        if (offset+8>WINDOW) return false;
        u32 cap=rd(offset);
        if ((cap&255)==1) {
            wr(offset,cap|(1u<<24));
            if (!wait_reg(offset,1u<<16,0,2000)) {
                kprintf("xHCI: BIOS did not release ownership\n"); return false;
            }
            /* Disable USB legacy SMI enables; clear only defined RW1C status. */
            wr(offset+4,rd(offset+4)&0xe0000000u);
        }
        unsigned next=(cap>>8)&255;
        if (!next) break;
        ext+=next;
    }
    wr(op,rd(op)&~1u);
    if (!wait_reg(op+4,1,1,1000)) return false;
    wr(op,rd(op)|2);
    if (!wait_reg(op,2,0,1000) || !wait_reg(op+4,1u<<11,0,1000)) return false;
    if (!(rd(op+8)&1)) { kprintf("xHCI: 4 KiB pages unsupported\n"); return false; }
    dcbaa=page(); events=page(); u32 *erst=page();
    if (!dcbaa || !events || !erst || !ring_init(&commands)) return false;
    unsigned scratch=((rd(8)>>27)&31)|(((rd(8)>>21)&31)<<5);
    if (scratch>64) return false;
    if (scratch) {
        u64 *array=page(); if (!array) return false;
        for (unsigned i=0;i<scratch;i++) { void *p=page(); if (!p) return false; array[i]=(u32)p; }
        dcbaa[0]=(u32)array;
    }
    erst[0]=(u32)events; erst[2]=NCYCLE;
    event_index=0; event_cycle=1;
    wr64(op+0x30,(u32)dcbaa);
    wr64(op+0x18,(u32)commands.base|1);
    wr(op+0x38,maxslots);
    wr(rt+0x20,0); /* IMAN: poll instead of enabling interrupt delivery */
    wr(rt+0x28,1); wr64(rt+0x30,(u32)erst); wr64(rt+0x38,(u32)events);
    wr(op+4,0x1c); /* clear HSE/EINT/PCD */
    wr(op,1);
    if (!wait_reg(op+4,1,0,1000)) return false;
    running=true;
    kprintf("xHCI: %u ports, %u-byte contexts, polling\n",ports,context_bytes);
    kprintf("xHCI: scratchpads=%u, port power control=%u\n",scratch,(rd(0x10)>>3)&1);
    /* Real controllers may leave switchable port power off after reset.
     * Preserve only power when writing PORTSC, not its RW1C change bits. */
    if (rd(0x10)&8) {
        for (unsigned port=1;port<=ports;port++) {
            u32 reg=op+0x400+(port-1)*16;
            if (!(rd(reg)&(1u<<9))) wr(reg,1u<<9);
        }
    }
    sleep_ms(100); /* power-good / connection debounce before enumeration */
    unsigned connected=0;
    for (unsigned port=1;port<=ports && !poisoned;port++) {
        u32 status=rd(op+0x400+(port-1)*16);
        if (!(status&1)) continue;
        connected++;
        kprintf("xHCI: connected port %u, PORTSC=%x\n",port,status);
        if (!attach(port)) kprintf("xHCI: port %u has no active boot HID interface\n",port);
    }
    if (!connected) kprintf("xHCI: no connected root ports on this controller\n");
    unsigned active=0; for (unsigned i=0;i<nhid;i++) if (hids[i].active) active++;
    if (poisoned || task_create("usb-hid",worker,NULL)<0) {
        running=false; wr(op,rd(op)&~1u); return false;
    }
    return active!=0;
}
#else
bool xhci_init(void) { return false; }
#endif
