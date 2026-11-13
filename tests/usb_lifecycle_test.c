/* Real detach/input code with a modeled Disable Slot DMA ownership barrier. */
#define HOST_TEST
#define MYOS_HARDWARE
#include "../kernel/xhci.c"
int printf(const char *,...);int puts(const char *);void exit(int);
#define CHECK(c) do { if(!(c)) { printf("USB LIFECYCLE FAIL line %d: %s\n",__LINE__,#c);exit(1); } } while(0)
static u8 pool[32][4096] __attribute__((aligned(4096)));
static u64 addresses[256];
static unsigned allocated,freed,disabled[256],order[32],norder,fail_slot;
static unsigned codes[64],ncodes,buttons;
u32 xhci_test_read(u32 offset) { (void)offset;return 0; }
void xhci_test_write(u32 offset,u32 value) { (void)offset;(void)value; }
void kprintf(const char *fmt,...) { (void)fmt; }
void kb_scancode(u8 code) { CHECK(ncodes<64);codes[ncodes++]=code; }
void mouse_usb_report(u32 value,i32 x,i32 y,i32 wheel) { (void)x;(void)y;(void)wheel;buttons=value; }
u32 timer_ticks(void) { return 10; }
u32 pmm_alloc(void) { CHECK(allocated<32);return (u32)pool[allocated++]; }
void pmm_free(u32 pointer) {
    CHECK(pointer>=(u32)pool && pointer<(u32)(pool+32));
    /* A root and its child each own disjoint page runs. */
    unsigned page=(pointer-(u32)pool)/4096,slot=page<6?1:2;
    CHECK(disabled[slot]);freed++;
}
bool xhci_test_command(u32 lo,u32 control) {
    (void)lo;CHECK(((control>>10)&63)==10);
    unsigned slot=control>>24;CHECK(slot==1 || slot==2);
    for(unsigned i=0;i<NHID;i++) if(hids[i].dev && hids[i].dev->slot==slot)CHECK(!hids[i].active);
    if(slot==fail_slot)return false;
    disabled[slot]=1;order[norder++]=slot;return true;
}
static void reset(void) {
    memset(devices,0,sizeof(devices));memset(hids,0,sizeof(hids));memset(inventory,0,sizeof(inventory));
    memset(key_refs,0,sizeof(key_refs));memset(mod_refs,0,sizeof(mod_refs));memset(disabled,0,sizeof(disabled));
    allocated=freed=norder=fail_slot=ncodes=buttons=0;poisoned=false;events=0;dcbaa=addresses;
}
static void device(unsigned index,int parent,unsigned slot) {
    struct usbdev *d=&devices[index];d->used=d->published=true;d->parent=parent;d->slot=slot;d->port=1;
    d->context=page();d->input=page();d->buffer=page();CHECK(ring_init(&d->control));
    hids[index].dev=d;hids[index].protocol=index?2:1;hids[index].active=true;
    hids[index].report=page();CHECK(ring_init(&hids[index].ring));
}
int main(void) {
    reset();device(0,-1,1);device(1,0,2);
    const u8 pressed[8]={3,0,4,0,0,0,0,0};keyboard_apply(&hids[0],pressed);
    hids[1].buttons=1;buttons=1;
    CHECK(detach(&devices[0]));
    CHECK(order[0]==2 && order[1]==1 && freed==12 && !devices[0].used && !devices[1].used);
    CHECK(!buttons && !key_refs[4] && !mod_refs[0] && !mod_refs[1]);
    CHECK(codes[ncodes-1]==0x9e); /* 'a' released, after Ctrl and Shift. */
    reset();device(0,-1,1);fail_slot=1;
    CHECK(!detach(&devices[0]) && !freed && devices[0].quarantined && devices[0].used && poisoned);
    CHECK(!device_visible(&devices[0]));
    reset();
    struct hid one={0},two={0};one.protocol=two.protocol=1;
    const u8 duplicate[8]={0,0,4,4,0,0,0,0},single[8]={0,0,4,0,0,0,0,0},empty[8]={0};
    keyboard_apply(&one,duplicate);keyboard_apply(&one,single);keyboard_apply(&two,single);
    CHECK(key_refs[4]==2 && ncodes==1);keyboard_apply(&one,empty);CHECK(key_refs[4]==1 && ncodes==1);
    keyboard_apply(&two,empty);CHECK(!key_refs[4] && ncodes==2);
    /* Control TDs can wrap early without splitting setup/data/status. */
    struct ring r;CHECK(ring_init(&r));r.next=254;ring_link(&r);
    CHECK(r.next==0 && r.cycle==0 && ((r.base[254].control>>10)&63)==6);
    reset();
    devices[0].used=true;devices[0].port=7;devices[0].depth=2;devices[0].route=0x21;
    devices[0].speed=3;devices[0].slot=9;
    struct usbdev routed={0};CHECK(device_route(&routed,0,3,2));
    CHECK(routed.port==7 && routed.route==0x321 && routed.depth==3 && routed.tt_slot==9 && routed.tt_port==3);
    devices[0].speed=1;devices[0].tt_slot=9;devices[0].tt_port=3;
    routed=(struct usbdev){0};CHECK(device_route(&routed,0,4,1));
    CHECK(routed.route==0x421 && routed.tt_slot==9 && routed.tt_port==3);
    devices[0].depth=5;CHECK(!device_route(&routed,0,1,1));
    CHECK(!device_route(&routed,0,16,1) && !device_route(&routed,NDEV,1,1));
    puts("USB lifecycle: child-first DMA stop, page reclamation, quarantine, key/button release and reusable rings: PASS");
    return 0;
}
