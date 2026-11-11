/* USB HID 1.11 short items. Decode declared fields, never guess a fourth
 * boot-protocol byte: report IDs and 12/16-bit axes are common on real mice. */
#include "hid_mouse.h"
#include "string.h"
struct globals {u32 page,size,count,id;i32 minimum;};
struct candidate {struct hid_mouse_layout layout;};
bool hid_mouse_parse(const u8 *d,u32 length,struct hid_mouse_layout *out) {
    if(!d || !out || !length || length>1024)return false;
    struct globals g={0},saved[4];u32 depth=0;
    struct candidate layouts[8];memset(layouts,0,sizeof(layouts));
    u16 offsets[256];memset(offsets,0,sizeof(offsets));
    u32 nlayouts=0,usages[32],nusage=0,umin=0,umax=0;
    bool has_min=false,has_max=false,numbered=false;
    for(u32 p=0;p<length;) {
        u8 item=d[p++];if(item==0xfe)return false;
        u32 bytes=item&3;if(bytes==3)bytes=4;
        if(bytes>length-p)return false;
        u32 value=0;for(u32 j=0;j<bytes;j++)value|=(u32)d[p++]<<(8*j);
        unsigned type=(item>>2)&3,tag=item>>4;
        if(type==1) {
            switch(tag) {
            case 0:g.page=value;break;
            case 1:
                g.minimum=(bytes && bytes<4 && (value&(1u<<(bytes*8-1))))?
                    (i32)(value|(~0u<<(bytes*8))):(i32)value;break;
            case 7:if(value>32)return false;g.size=value;break;
            case 8:if(!value || value>255)return false;g.id=value;numbered=true;break;
            case 9:if(value>512)return false;g.count=value;break;
            case 10:if(depth==4)return false;saved[depth++]=g;break;
            case 11:if(!depth)return false;g=saved[--depth];break;
            }
        } else if(type==2) {
            if(bytes!=4)value|=g.page<<16;
            if(tag==0) {if(nusage==32)return false;usages[nusage++]=value;}
            else if(tag==1) {umin=value;has_min=true;}
            else if(tag==2) {umax=value;has_max=true;}
        } else if(type==0) {
            if(tag==8) { /* Input: output/feature reports have separate offsets. */
                if(!g.size || !g.count || g.size*g.count>512u-offsets[g.id])return false;
                struct hid_mouse_layout *l=NULL;
                for(u32 i=0;i<nlayouts;i++)if(layouts[i].layout.id==g.id)l=&layouts[i].layout;
                if(!l) {
                    if(nlayouts==8)return false;
                    l=&layouts[nlayouts++].layout;l->id=g.id;
                }
                if((value&3)==2 && g.size<=16) { /* Data, Variable */
                    for(u32 j=0;j<g.count;j++) {
                        u32 usage=0;
                        if(nusage)usage=usages[j<nusage?j:nusage-1];
                        else if(has_min && has_max && umin<=umax && j<=umax-umin)usage=umin+j;
                        struct hid_mouse_field f={offsets[g.id]+j*g.size,g.size,g.minimum<0};
                        if((value&4) && usage==0x10030 && !l->x.size)l->x=f;
                        if((value&4) && usage==0x10031 && !l->y.size)l->y=f;
                        if((value&4) && usage==0x10038 && !l->wheel.size)l->wheel=f;
                        if(usage>=0x90001 && usage<=0x90003 && !l->buttons[usage-0x90001].size)
                            l->buttons[usage-0x90001]=f;
                    }
                }
                offsets[g.id]+=g.size*g.count;l->bits=offsets[g.id];
            }
            nusage=0;has_min=has_max=false;
        }
    }
    if(depth)return false;
    for(u32 i=0;i<nlayouts;i++) {
        struct hid_mouse_layout l=layouts[i].layout;
        if(l.x.size && l.y.size && (!numbered || l.id) && l.bits+(numbered?8:0)<=512) {
            l.numbered=numbered;*out=l;return true;
        }
    }
    return false;
}
static i32 field(const struct hid_mouse_field *f,const u8 *data) {
    if(!f->size)return 0;
    u32 value=0;
    for(u32 i=0;i<f->size;i++)value|=((data[(f->bit+i)/8]>>((f->bit+i)%8))&1u)<<i;
    if(f->sign && (value&(1u<<(f->size-1))))value|=~((1u<<f->size)-1);
    return (i32)value;
}
bool hid_mouse_decode(const struct hid_mouse_layout *l,const u8 *data,u32 length,
                      u32 *buttons,i32 *dx,i32 *dy,i32 *wheel) {
    if(!l || !data || !buttons || !dx || !dy || !wheel)return false;
    if(l->numbered) {if(!length || *data++!=l->id)return false;length--;}
    if(!l->x.size || !l->y.size || (l->bits+7u)/8>length)return false;
    *buttons=0;
    for(u32 i=0;i<3;i++)if(field(l->buttons+i,data))*buttons|=1u<<i;
    *dx=field(&l->x,data);*dy=field(&l->y,data);*wheel=field(&l->wheel,data);
    return true;
}
