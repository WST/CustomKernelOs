/* Minimal polling xHCI 1.x + USB HID boot protocol.
 * DMA lives in PMM pages below 1 GiB, never in the virtual kernel heap.
 * Hotplug and USB 2.x hubs; HID input. Mass-storage transport is not implemented.
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
#include "hid_mouse.h"

#ifdef MYOS_HARDWARE

#define MMIO 0xe4000000u
#define WINDOW 0x40000u
#define NDEV 32
#define NHID 64
#define NCYCLE 256
#define TYPE(n) ((u32)(n)<<10)
#define BARRIER() __asm__ volatile ("" ::: "memory")
struct trb { u32 lo,hi,status,control; };
struct ring { struct trb *base; unsigned next,cycle; };
struct usbdev {
    unsigned slot,port,speed,mps;
    int parent;
    unsigned parent_port,route,depth,tt_slot,tt_port,hub_ports,seen_ports,generation;
    bool used,published,removing,failed,quarantined;
    struct hw_usb_info info;
    u32 *context,*input;
    struct ring control;
    u8 *buffer;
};
struct hid {
    struct usbdev *dev;
    struct ring ring;
    unsigned dci,protocol,packet,interface,descriptor_bytes;
    struct hid_mouse_layout mouse;
    bool report_protocol;
    u8 *report;
    u8 previous[8];
    u32 pending;
    u8 repeat_key;
    u32 repeat_at;
    bool active;
    u32 buttons;
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
static u8 root_seen[256],key_refs[256],mod_refs[8];
static unsigned generation;
static u32 waiting,wait_code,wait_slot,waiting_slot,waiting_dci;
static bool running,poisoned;
static bool inventory_ready;
static struct pci_dev inventory_controller;
static struct hw_usb_info inventory[256];

#ifdef HOST_TEST
u32 xhci_test_read(u32 offset);
void xhci_test_write(u32 offset,u32 value);
static u32 rd(u32 o) { return xhci_test_read(o); }
static void wr(u32 o,u32 v) { xhci_test_write(o,v); }
#else
static u32 rd(u32 o) { return *(volatile u32 *)(mmio+o); }
static void wr(u32 o,u32 v) { *(volatile u32 *)(mmio+o)=v; }
#endif
static void pump(void);
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
        if(running) pump();
        sleep_ms(10);
    } while (timer_ticks()-start<(ms*TIMER_HZ+999)/1000);
    return false;
}
static bool ring_init(struct ring *r) {
    r->base=page(); r->next=0; r->cycle=1;
    return r->base!=NULL;
}
static void ring_link(struct ring *r) {
    struct trb *link=&r->base[r->next];
    link->lo=(u32)r->base; link->hi=0; link->status=0;
    BARRIER(); link->control=TYPE(6)|2|r->cycle;
    r->cycle^=1; r->next=0;
}
static u32 enqueue(struct ring *r,u32 lo,u32 hi,u32 status,u32 control) {
    if(r->next==NCYCLE-1) ring_link(r);
    struct trb *t=&r->base[r->next++];
    t->lo=lo; t->hi=hi; t->status=status;
    BARRIER(); t->control=control|r->cycle;
    return (u32)t;
}
static void ring_doorbell(unsigned slot,unsigned dci) {
    BARRIER(); wr(db+slot*4,dci);
}
static void arm(struct hid *h) {
    if (!h->active || h->dev->removing || h->dev->failed) return;
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
/* Aggregate USB keyboards: removing one must release only its own keys. */
static bool earlier_key(const u8 *r,int at) {
    for(int i=2;i<at;i++) if(r[i]==r[at]) return true;
    return false;
}
static void keyboard_apply(struct hid *h,const u8 *r) {
    for(int i=2;i<8;i++) if(r[i] && r[i]<=3) return;
    static const u16 mods[8]={29,42,56,0xe05b,0xe01d,54,0xe038,0xe05c};
    u32 flags=irq_save();
    for(int i=0;i<8;i++) if((r[0]^h->previous[0])&(1<<i)) {
        bool pressed=(r[0]&(1<<i))!=0,emit=false;
        if(pressed) emit=mod_refs[i]++==0;
        else if(mod_refs[i]) emit=--mod_refs[i]==0;
        if(emit) {
            if(mods[i]&0xff00) kb_scancode(0xe0);
            kb_scancode((mods[i]&255)|(pressed?0:0x80));
        }
    }
    for(int i=2;i<8;i++) if(h->previous[i] && !earlier_key(h->previous,i) && !contains(r,h->previous[i])) {
        u8 usage=h->previous[i];
        if(key_refs[usage] && --key_refs[usage]==0) key(usage,true);
        if(h->repeat_key==usage) h->repeat_key=0;
    }
    for(int i=2;i<8;i++) if(r[i] && !earlier_key(r,i) && !contains(h->previous,r[i])) {
        if(key_refs[r[i]]++==0) key(r[i],false);
        if(r[i]!=57) { h->repeat_key=r[i]; h->repeat_at=timer_ticks()+TIMER_HZ/2; }
    }
    memcpy(h->previous,r,8); irq_restore(flags);
}
static u32 mouse_buttons(void) {
    u32 buttons=0;
    for(unsigned i=0;i<NHID;i++) if(hids[i].active && hids[i].protocol==2) buttons|=hids[i].buttons;
    return buttons;
}
static void release_hid(struct hid *h) {
    h->active=false; h->repeat_key=0;
    if(h->protocol==1) { const u8 empty[8]={0}; keyboard_apply(h,empty); }
    else if(h->protocol==2) { h->buttons=0; mouse_usb_report(mouse_buttons(),0,0,0); }
}
static void repeat_keys(void) {
    for(unsigned i=0;i<NHID;i++) {
        struct hid *h=&hids[i];
        if(!h->active || !h->repeat_key || h->dev->failed || h->dev->removing ||
           (i32)(timer_ticks()-h->repeat_at)<0) continue;
        bool owner=true;
        for(unsigned j=0;j<i;j++) if(hids[j].active && hids[j].repeat_key==h->repeat_key) owner=false;
        if(!owner) continue;
        u32 flags=irq_save();key(h->repeat_key,false);irq_restore(flags);
        h->repeat_at=timer_ticks()+TIMER_HZ/20;
    }
}
static void pump(void) {
    if(!events) return;
    for (unsigned n=0;n<NCYCLE;n++) {
        volatile struct trb *v=&events[event_index];
        u32 ctl=v->control;
        if ((ctl&1)!=event_cycle) break;
        BARRIER();
        struct trb e={v->lo,v->hi,v->status,ctl};
        unsigned type=(ctl>>10)&63,code=e.status>>24;
        if (((type==33 && !waiting_slot && e.lo==waiting) ||
             (type==32 && waiting_slot==(ctl>>24) && waiting_dci==((ctl>>16)&31) &&
              (e.lo==waiting || (code!=1 && code!=13)))) && waiting && !e.hi) {
            wait_code=code; wait_slot=ctl>>24;
        } else if (type==32) {
            for (unsigned i=0;i<NHID;i++) {
                struct hid *h=&hids[i];
                if (h->active && h->pending==e.lo && !e.hi &&
                    h->dev->slot==(ctl>>24) && h->dci==((ctl>>16)&31)) {
                    unsigned transferred=h->packet-(e.status&0xffffff);
                    if ((code==1 || code==13) && transferred>=(h->protocol==1?8u:3u)) {
                        if (h->protocol==1) keyboard_apply(h,h->report);
                        else if(h->report_protocol) {
                            u32 buttons;i32 dx,dy,wheel;
                            if(hid_mouse_decode(&h->mouse,h->report,transferred,&buttons,&dx,&dy,&wheel))
                                { h->buttons=buttons; mouse_usb_report(mouse_buttons(),dx,dy,wheel); }
                        } else { h->buttons=h->report[0]&7; mouse_usb_report(mouse_buttons(),(i8)h->report[1],(i8)h->report[2],0); }
                    }
                    if (code==1 || code==13) arm(h);
                    else { release_hid(h); h->dev->failed=true; kprintf("USB HID slot %u stopped: completion %u\n",h->dev->slot,code); }
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
    repeat_keys();
}
static unsigned completion(u32 pointer,unsigned ms,bool fatal) {
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
        if(fatal) poisoned=true;
        kprintf("xHCI timed out waiting for %x\n",pointer);
    }
    return wait_code;
}
static bool command(u32 lo,u32 ctl) {
#ifdef HOST_TEST
    extern bool xhci_test_command(u32,u32);
    return xhci_test_command(lo,ctl);
#endif
    waiting_slot=waiting_dci=0;
    if (poisoned) return false;
    u32 pointer=enqueue(&commands,lo,0,0,ctl);
    ring_doorbell(0,0);
    unsigned code=completion(pointer,1000,true);
    if (code!=1) kprintf("xHCI command %u completion %u\n",(ctl>>10)&63,code);
    return code==1;
}
static bool control(struct usbdev *d,u8 request_type,u8 request,u16 value,u16 index,u16 length) {
    if (poisoned || d->failed || d->removing || length>1024) return false;
    if(request_type&0x80) memset(d->buffer,0,1024);
    bool input=request_type&0x80;
    if(d->control.next+(length?3u:2u)>NCYCLE-1) ring_link(&d->control);
    enqueue(&d->control,request_type|((u32)request<<8)|((u32)value<<16),
            index|((u32)length<<16),8,TYPE(2)|(1<<6)|((length?(input?3:2):0)<<16));
    if (length) enqueue(&d->control,(u32)d->buffer,0,length,TYPE(3)|(input?(1<<16):0));
    u32 pointer=enqueue(&d->control,0,0,0,TYPE(4)|(1<<5)|((!length || !input)?(1<<16):0));
    ring_doorbell(d->slot,1);
    waiting_slot=d->slot; waiting_dci=1;
    unsigned code=completion(pointer,1000,false);
    waiting_slot=waiting_dci=0;
    if(code==1 || code==13) return true;
    if(code==6) {
        /* A STALL halts EP0. Reset it and discard the failed TD before an
         * optional request can fall back to another protocol. */
        if(command(0,TYPE(14)|(1u<<16)|(d->slot<<24))) {
            memset(d->control.base,0,4096); d->control.next=0; d->control.cycle=1;
            if(command((u32)d->control.base|1,TYPE(16)|(1u<<16)|(d->slot<<24))) return false;
        }
    }
    d->failed=true;
    return false;
}
static u32 *ctx(u32 *base,unsigned index) { return (u32 *)((u8 *)base+index*context_bytes); }
static void copy_slot(struct usbdev *d) {
    memset(d->input,0,4096);
    memcpy(ctx(d->input,1),ctx(d->context,0),context_bytes);
    ctx(d->input,1)[3]=0;
}
static unsigned interval(unsigned speed,unsigned value) {
    if (!value) value=1;
    if (speed>=3) return value>16?15:value-1;
    unsigned ticks=value*8,n=0;
    while ((1u<<n)<ticks && n<10) n++;
    return n<3?3:n;
}
static void settle(unsigned ms) {
    u32 start=timer_ticks(),ticks=(ms*TIMER_HZ+999)/1000;
    do { pump(); sleep_ms(10); } while(timer_ticks()-start<ticks && !poisoned);
}
static bool configure_hub(struct usbdev *d,unsigned config) {
    /* USB3 hubs expose a separate USB2 companion for HID/full-speed traffic.
     * SuperSpeed-only hub routing is deliberately not enabled here. */
    if(d->speed==4 || d->depth>=5) return true;
    if(!control(d,0,9,config,0,0) || !control(d,0xa0,6,0x2900,0,7)) return false;
    unsigned count=d->buffer[2],power_good=d->buffer[5]*2;
    u32 characteristics=d->buffer[3]|((u32)d->buffer[4]<<8);
    if(d->buffer[0]<7 || d->buffer[1]!=0x29 || !count) return false;
    if(count>15) { kprintf("USB hub: %u ports exceed supported route capacity\n",count); return true; }
    copy_slot(d); d->input[1]=1;
    u32 *slot=ctx(d->input,1);
    slot[0]=(slot[0]|(1u<<26))&~(1u<<25); /* USB2 default alternate: single TT */
    slot[1]=(slot[1]&0x00ffffffu)|(count<<24);
    if(d->speed==3) slot[2]=(slot[2]&~(3u<<16))|(((characteristics>>5)&3)<<16);
    if(!command((u32)d->input,TYPE(13)|(d->slot<<24))) return false;
    for(unsigned port=1;port<=count;port++) if(!control(d,0x23,3,8,port,0)) return false;
    d->hub_ports=count;
    settle(power_good<20?20:power_good);
    kprintf("USB hub: root %u route %x, slot %u, %u ports (single TT)\n",d->port,d->route,d->slot,count);
    return true;
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
        ep[0]&=~7u;
        ep[1]=(ep[1]&0xffff)|(mps<<16);
        if (!command((u32)d->input,TYPE(13)|(d->slot<<24))) return false;
        d->mps=mps;
    }
    if (!control(d,0x80,6,0x100,0,18) || d->buffer[0]!=18 || d->buffer[1]!=1) return false;
    struct hw_usb_info *info=&d->info;
    info->vendor_id=d->buffer[8]|((u32)d->buffer[9]<<8);
    info->product_id=d->buffer[10]|((u32)d->buffer[11]<<8);
    info->bcd_usb=d->buffer[2]|((u32)d->buffer[3]<<8);
    info->device_class=d->buffer[4]; info->descriptor_valid=1;
    info->protocol=d->buffer[6];
    if(info->vendor_id==0xffff) return false;
    if (!control(d,0x80,6,0x200,0,9) || d->buffer[1]!=2) return false;
    unsigned total=d->buffer[2]|d->buffer[3]<<8,config=d->buffer[5];
    if (total<9 || total>512 || !control(d,0x80,6,0x200,0,total)) return false;
    if(info->device_class==9) return configure_hub(d,config);
    u8 descriptors[512]; memcpy(descriptors,d->buffer,total);
    unsigned found=0,protocol=0,iface=0,maxdci=1,report_bytes=0;
    copy_slot(d); d->input[1]=1;
    for (unsigned pos=0;pos+2<=total;) {
        u8 *p=descriptors+pos; unsigned len=p[0];
        if (len<2 || pos+len>total) break;
        if (p[1]==4 && len>=9) {
            protocol=(p[3]==0 && p[5]==3 && p[6]==1 && (p[7]==1 || p[7]==2))?p[7]:0;
            if (!info->interface_class) {
                info->interface_class=p[5]; info->protocol=p[7];
            }
            iface=p[2];report_bytes=0;
        } else if(p[1]==0x21 && protocol && len>=6) {
            for(u32 i=0;i<p[5] && 6+i*3+3<=len;i++)
                if(p[6+i*3]==0x22)report_bytes=p[7+i*3]|((u32)p[8+i*3]<<8);
        } else if (p[1]==5 && len>=7 && protocol && (p[2]&0x80) && (p[3]&3)==3) {
            unsigned dci=(p[2]&15)*2+1,packet=(p[4]|p[5]<<8)&0x7ff;
            if (dci<3 || packet<(protocol==1?8u:3u) || packet>64 || (d->input[1]&(1u<<dci))) { pos+=len; continue; }
            struct hid *h=NULL;
            for(unsigned i=0;i<NHID;i++) if(!hids[i].dev) { h=&hids[i]; break; }
            if(!h) return false;
            memset(h,0,sizeof(*h));
            h->dev=d; h->dci=dci; h->protocol=protocol; h->packet=packet; h->interface=iface;
            h->descriptor_bytes=report_bytes;
            h->report=page();
            if (!h->report || !ring_init(&h->ring)) return false;
            u32 *ep=ctx(d->input,dci+1);
            ep[0]=interval(d->speed,p[6])<<16;
            ep[1]=(packet<<16)|(7<<3)|(3<<1);
            ep[2]=(u32)h->ring.base|1;
            ep[4]=packet|(packet<<16);
            d->input[1]|=1u<<dci;
            if (dci>maxdci) maxdci=dci;
            found++; protocol=0;
        }
        pos+=len;
    }
    if (!found) return true; /* Descriptor-only device; no class driver. */
    u32 *slot=ctx(d->input,1); slot[0]=(slot[0]&0x07ffffff)|(maxdci<<27);
    if (!command((u32)d->input,TYPE(12)|(d->slot<<24))) return false;
    if (!control(d,0,9,config,0,0)) return false;
    for (unsigned i=0;i<NHID;i++) {
        struct hid *h=&hids[i];
        if(h->dev!=d) continue;
        if(h->protocol==2 && h->descriptor_bytes && h->descriptor_bytes<=1024 &&
           control(d,0x81,6,0x2200,h->interface,h->descriptor_bytes) &&
           hid_mouse_parse(d->buffer,h->descriptor_bytes,&h->mouse) &&
           (h->mouse.bits+7u)/8+(h->mouse.numbered?1u:0u)<=h->packet &&
           control(d,0x21,11,1,h->interface,0)) {
            h->report_protocol=true;
            kprintf("USB mouse report protocol: id=%u, wheel=%s, axes=%u/%u bits\n",
                    h->mouse.id,h->mouse.wheel.size?"yes":"no",h->mouse.x.size,h->mouse.y.size);
        } else if (!control(d,0x21,11,0,h->interface,0)) continue; /* Boot fallback */
        /* SET_IDLE is optional; some devices stall it. No dependence on it. */
        h->active=true; arm(h);
        kprintf("USB HID %s: port %u, slot %u, endpoint %u\n",h->protocol==1?"keyboard":"mouse",d->port,d->slot,h->dci);
    }
    return true;
}
static void free_page(void *p) { if(p) pmm_free((u32)p); }
static bool detach(struct usbdev *d) {
    if(!d->used || d->removing) return !d->quarantined;
    d->removing=true; d->published=false;
    if(d->parent<0) memset(&inventory[d->port-1],0,sizeof(inventory[0]));
    for(unsigned i=0;i<NHID;i++) if(hids[i].dev==d) release_hid(&hids[i]);
    for(unsigned i=0;i<NDEV;i++) if(devices[i].used && devices[i].parent==(int)(d-devices))
        if(!detach(&devices[i])) { d->quarantined=true; return false; }
    /* Disable Slot is the DMA ownership barrier. Never recycle a page if
     * the host cannot confirm that this slot has stopped. */
    if(d->slot && (!command(0,TYPE(10)|(d->slot<<24)) && wait_code!=11)) {
        d->quarantined=true; poisoned=true;
        kprintf("USB slot %u quarantined: cannot stop DMA\n",d->slot); return false;
    }
    pump();
    if(d->slot) dcbaa[d->slot]=0;
    for(unsigned i=0;i<NHID;i++) if(hids[i].dev==d) {
        free_page(hids[i].ring.base); free_page(hids[i].report); memset(&hids[i],0,sizeof(hids[i]));
    }
    free_page(d->control.base); free_page(d->context); free_page(d->input); free_page(d->buffer);
    kprintf("USB disconnected: root %u route %x slot %u\n",d->port,d->route,d->slot);
    memset(d,0,sizeof(*d)); return true;
}
static struct usbdev *child(int parent,unsigned port) {
    for(unsigned i=0;i<NDEV;i++) if(devices[i].used && devices[i].parent==parent &&
                                  devices[i].parent_port==port) return &devices[i];
    return NULL;
}
static bool device_route(struct usbdev *d,int parent,unsigned port,unsigned speed) {
    if(parent<0) return port>0 && port<=255;
    if(parent>=NDEV || !port || port>15) return false;
    struct usbdev *hub=&devices[parent];
    if(!hub->used || hub->removing || hub->failed || hub->depth>=5) return false;
    d->port=hub->port; d->depth=hub->depth+1; d->route=hub->route|(port<<(hub->depth*4));
    if(speed<=2) {
        d->tt_slot=hub->speed==3?hub->slot:hub->tt_slot;
        d->tt_port=hub->speed==3?port:hub->tt_port;
    }
    return true;
}
static bool address_device(int parent,unsigned port,unsigned speed) {
    if(poisoned || !speed || speed>4) return false;
    struct usbdev *d=NULL;
    for(unsigned i=0;i<NDEV;i++) if(!devices[i].used) { d=&devices[i]; break; }
    if(!d) { kprintf("USB: device table full\n"); return false; }
    memset(d,0,sizeof(*d)); d->used=true; d->parent=parent; d->parent_port=port;
    d->port=port; d->speed=speed; d->mps=speed==4?512:speed==3?64:8; d->generation=++generation;
    if(!device_route(d,parent,port,speed)) { memset(d,0,sizeof(*d)); return false; }
    d->context=page(); d->input=page(); d->buffer=page();
    if(!d->context || !d->input || !d->buffer || !ring_init(&d->control)) { detach(d); return false; }
    if(!command(0,TYPE(9))) { detach(d); return false; }
    d->slot=wait_slot;
    if(!d->slot || d->slot>maxslots) { poisoned=true; return false; }
    dcbaa[d->slot]=(u32)d->context; BARRIER();
    d->input[1]=3;
    u32 *slot=ctx(d->input,1),*ep=ctx(d->input,2);
    slot[0]=d->route|(speed<<20)|(1u<<27); slot[1]=d->port<<16;
    slot[2]=d->tt_slot|(d->tt_port<<8);
    ep[1]=(d->mps<<16)|(4<<3)|(3<<1);
    ep[2]=(u32)d->control.base|1; ep[4]=8;
    if(!command((u32)d->input,TYPE(11)|(d->slot<<24)) || !configure(d) || d->failed) {
        kprintf("USB enumeration failed: root %u route %x\n",d->port,d->route);
        detach(d); return false;
    }
    d->published=true;
    if(parent<0) inventory[d->port-1]=d->info;
    kprintf("USB attached: root %u route %x slot %u, %x:%x\n",d->port,d->route,d->slot,
            d->info.vendor_id,d->info.product_id);
    return true;
}
#define PORT_CHANGES 0x00fe0000u
static void ack_root(unsigned port,u32 status) {
    if(status&PORT_CHANGES) wr(op+0x400+(port-1)*16,(status&(1u<<9))|(status&PORT_CHANGES));
}
static bool attach_root(unsigned port) {
    u32 reg=op+0x400+(port-1)*16,status=rd(reg);
    if(!(status&1)) return false;
    settle(100); status=rd(reg);
    if(!(status&1)) return false;
    if(!(status&2)) {
        wr(reg,(status&(1u<<9))|(1u<<4));
        if(!wait_reg(reg,1u<<4,0,1000) || !(rd(reg)&2)) return false;
    }
    settle(20); status=rd(reg);
    if(!(status&1)) return false;
    return address_device(-1,port,(status>>10)&15);
}
static bool hub_status(struct usbdev *hub,unsigned port,u32 *status) {
    if(!control(hub,0xa3,0,0,port,4)) return false;
    *status=(u32)hub->buffer[0]|((u32)hub->buffer[1]<<8)|((u32)hub->buffer[2]<<16)|((u32)hub->buffer[3]<<24);
    return true;
}
static bool attach_hub_port(struct usbdev *hub,unsigned port) {
    if(hub->depth>=5) return false;
    settle(100);
    if(!control(hub,0x23,3,4,port,0)) return false;
    u32 status=0,start=timer_ticks();
    do {
        settle(10);
        if(!hub_status(hub,port,&status) || !(status&1)) return false;
        if(!(status&16) && (status&2)) break;
    } while(timer_ticks()-start<TIMER_HZ);
    if((status&16) || !(status&2)) return false;
    if(status&(1u<<20)) control(hub,0x23,1,20,port,0); /* C_PORT_RESET */
    settle(20);
    unsigned speed=(status&(1u<<9))?2:(status&(1u<<10))?3:1;
    return address_device((int)(hub-devices),port,speed);
}
static void topology_poll(void) {
    for(unsigned port=1;port<=ports && !poisoned;port++) {
        u32 status=rd(op+0x400+(port-1)*16); bool connected=(status&1)!=0;
        struct usbdev *d=child(-1,port);
        if((status&(1u<<17)) || connected!=root_seen[port-1] || (d && d->failed)) {
            if(d && !detach(d)) return;
            memset(&inventory[port-1],0,sizeof(inventory[0]));
            root_seen[port-1]=connected; ack_root(port,status);
            if(connected) attach_root(port);
        } else ack_root(port,status);
    }
    for(unsigned i=0;i<NDEV && !poisoned;i++) {
        struct usbdev *hub=&devices[i];
        if(!hub->used || !hub->published || hub->removing || !hub->hub_ports) continue;
        if(hub->failed) { detach(hub); continue; }
        for(unsigned port=1;port<=hub->hub_ports && !poisoned;port++) {
            u32 status;
            if(!hub_status(hub,port,&status)) { hub->failed=true; break; }
            bool connected=(status&1)!=0,seen=(hub->seen_ports&(1u<<port))!=0;
            struct usbdev *d=child(i,port);
            bool change=(status&(1u<<16)) || connected!=seen || (d && d->failed);
            if(change && d && !detach(d)) return;
            if(connected) hub->seen_ports|=1u<<port; else hub->seen_ports&=~(1u<<port);
            for(unsigned bit=0;bit<5;bit++) if(status&(1u<<(16+bit)))
                if(!control(hub,0x23,1,16+bit,port,0)) { hub->failed=true; break; }
            if(change && connected && !hub->failed) attach_hub_port(hub,port);
        }
    }
}
static void stop_controller(void) {
    for(unsigned i=0;i<NHID;i++) if(hids[i].dev) release_hid(&hids[i]);
    running=false;
    wr(op,rd(op)&~1u);
    /* Keep quarantined pages allocated even after disabling bus mastering. */
    u32 cmd=pci_read(inventory_controller.bus,inventory_controller.dev,inventory_controller.func,4);
    pci_write(inventory_controller.bus,inventory_controller.dev,inventory_controller.func,4,cmd&0xfffbu);
}
static void worker(void *unused) {
    (void)unused; u32 next=0;
    while(running && !poisoned) {
        pump();
        if((i32)(timer_ticks()-next)>=0) { topology_poll(); next=timer_ticks()+TIMER_HZ/10; }
        sleep_ms(10);
    }
    stop_controller();
}
static bool device_visible(const struct usbdev *d) {
    for(unsigned depth=0;depth<=5;depth++) {
        if(!d->used || !d->published || d->removing || d->quarantined || d->failed) return false;
        if(d->parent<0) return true;
        d=&devices[d->parent];
    }
    return false;
}
static bool has_hid(const struct usbdev *d) {
    for(unsigned i=0;i<NHID;i++) if(hids[i].dev==d && hids[i].active) return true;
    return false;
}
int xhci_port_info(unsigned index,struct hw_usb_info *out) {
    if(!inventory_ready || index>=ports) return 0;
    *out=inventory[index];
    out->bus=inventory_controller.bus; out->device=inventory_controller.dev;
    out->function=inventory_controller.func; out->port=index+1;
    u32 status=rd(op+0x400+index*16);
    out->raw_status=status; out->connected=status&1; out->enabled=(status>>1)&1;
    out->speed=(status>>10)&15; out->controller_running=running && !poisoned;
    struct usbdev *d=child(-1,index+1);out->hid_active=d && device_visible(d) && has_hid(d);
    return 1;
}
int xhci_device_info(unsigned index,struct hw_usb_device_info *out) {
    for(unsigned i=0;i<NDEV;i++) if(device_visible(&devices[i])) {
        if(index--) continue;
        const struct usbdev *d=&devices[i]; memset(out,0,sizeof(*out)); out->usb=d->info;
        out->usb.bus=inventory_controller.bus; out->usb.device=inventory_controller.dev;
        out->usb.function=inventory_controller.func; out->usb.port=d->parent_port;
        out->usb.connected=1; out->usb.enabled=1; out->usb.speed=d->speed;
        out->usb.hid_active=has_hid(d); out->usb.controller_running=running && !poisoned;
        out->slot=d->slot; out->parent_slot=d->parent<0?0:devices[d->parent].slot;
        out->root_port=d->port; out->route=d->route; out->depth=d->depth;
        out->hub_ports=d->hub_ports; out->generation=d->generation;
        return 1;
    }
    return 0;
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
    inventory_controller=controller;
    inventory_ready=ports!=0;
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
    topology_poll();
    if(poisoned || task_create("usb-hid",worker,NULL)<0) {
        stop_controller(); return false;
    }
    kprintf("USB hotplug ready: up to %u devices, USB2 hubs, HID input\n",NDEV);
    return true;
}
#else
int xhci_port_info(unsigned index, struct hw_usb_info *out) { (void)index; (void)out; return 0; }
int xhci_device_info(unsigned index, struct hw_usb_device_info *out) { (void)index; (void)out; return 0; }
bool xhci_init(void) { return false; }
#endif
