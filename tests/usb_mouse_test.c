#define HOST_TEST
#include "../kernel/hid_mouse.c"
#include "../kernel/mouse.c"
extern int printf(const char *,...);
extern void exit(int);
#define CHECK(c) do{if(!(c)){printf("USB mouse test line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
void fb_input_notify(void) {}
void fb_cursor_move(int x,int y) {(void)x;(void)y;}
static const u8 descriptor[]={
    0x05,1,0x09,2,0xa1,1,0x09,1,0xa1,0,
    0x05,9,0x19,1,0x29,3,0x15,0,0x25,1,0x75,1,0x95,3,0x81,2,
    0x75,5,0x95,1,0x81,1,
    0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,
    0x75,8,0x95,3,0x81,6,0xc0,0xc0
};
static void test_decode(void) {
    struct hid_mouse_layout layout;u32 buttons;i32 x,y,wheel;
    CHECK(hid_mouse_parse(descriptor,sizeof(descriptor),&layout));
    u8 report[]={5,0x80,127,255};
    CHECK(hid_mouse_decode(&layout,report,4,&buttons,&x,&y,&wheel));
    CHECK(buttons==5 && x==-128 && y==127 && wheel==-1);
    CHECK(!hid_mouse_decode(&layout,report,3,&buttons,&x,&y,&wheel));
    u8 numbered[sizeof(descriptor)+2];
    numbered[0]=0x85;numbered[1]=7;memcpy(numbered+2,descriptor,sizeof(descriptor));
    CHECK(hid_mouse_parse(numbered,sizeof(numbered),&layout));
    u8 id_report[]={7,2,1,255,1};
    CHECK(hid_mouse_decode(&layout,id_report,5,&buttons,&x,&y,&wheel));
    CHECK(buttons==2 && x==1 && y==-1 && wheel==1);
    id_report[0]=8;CHECK(!hid_mouse_decode(&layout,id_report,5,&buttons,&x,&y,&wheel));
    const u8 wide[]={0x05,1,0x09,0x30,0x09,0x31,0x16,0,0x80,0x26,0xff,0x7f,
                     0x75,16,0x95,2,0x81,6,0x09,0x38,0x15,0x81,0x75,8,0x95,1,0x81,6};
    CHECK(hid_mouse_parse(wide,sizeof(wide),&layout));
    u8 wide_report[]={0x00,0x80,0xff,0x7f,0x80};
    CHECK(hid_mouse_decode(&layout,wide_report,5,&buttons,&x,&y,&wheel));
    CHECK(x==-32768 && y==32767 && wheel==-128);
    const u8 twelve[]={0x05,1,0x09,0x30,0x09,0x31,0x16,0,0xf8,0x26,0xff,7,
                      0x75,12,0x95,2,0x81,6};
    CHECK(hid_mouse_parse(twelve,sizeof(twelve),&layout));
    u8 packed[]={0xff,0x0f,0x80};
    CHECK(hid_mouse_decode(&layout,packed,3,&buttons,&x,&y,&wheel));
    CHECK(x==-1 && y==-2048 && wheel==0);
    const u8 broken[]={0x75,8,0x95,255,0x81,6};
    CHECK(!hid_mouse_parse(broken,sizeof(broken),&layout));
    CHECK(!hid_mouse_parse(descriptor,7,&layout));
    /* Exercise malformed descriptors/lengths under ASan+UBSan. */
    u32 seed=3;u8 data[128],payload[64]={0};
    for(u32 n=0;n<20000;n++) {
        for(u32 j=0;j<sizeof(data);j++){seed=seed*1664525+1013904223;data[j]=seed>>24;}
        if(hid_mouse_parse(data,n%sizeof(data),&layout))
            hid_mouse_decode(&layout,payload,n%65,&buttons,&x,&y,&wheel);
    }
}
static void test_bounds(void) {
    mouse_set_bounds(1280,1024); /* USB-only startup, no mouse_init/PS2 success. */
    mouse_usb_report(0,5000,5000,1);
    struct mouse_state m;mouse_get(&m);CHECK(m.x==1279 && m.y==1023 && m.wheel==1);
    mouse_usb_report(1,-5000,-5000,-2);mouse_get(&m);
    CHECK(m.x==0 && m.y==0 && m.buttons==1 && (i32)m.wheel==-1);
    mouse_set_bounds(2560,1440);mouse_usb_report(0,5000,5000,0);mouse_get(&m);
    CHECK(m.x==2559 && m.y==1439);
    mouse_set_bounds(800,600);mouse_get(&m);CHECK(m.x==799 && m.y==599);
    mouse_set_bounds(0,-1);mouse_get(&m);CHECK(m.x==0 && m.y==0);
}
int main(void){test_decode();test_bounds();printf("USB mouse: boot bounds + resize + wheel + report IDs + 8/12/16-bit fields + malformed descriptors: OK\n");return 0;}
