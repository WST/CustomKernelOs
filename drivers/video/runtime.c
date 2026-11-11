/* Runtime VBE interpreter. BIOS CPU state and low RAM are private copies;
 * paging, IDT, PIC and the running kernel never enter real mode. */
#include "video.h"
#include "nvidia_io.h"
#include "runtime_io.h"
#ifdef MYOS_HARDWARE
#include "../../kernel/heap.h"
#include "../../kernel/string.h"
#include "../../kernel/pci.h"
#include "../../kernel/paging.h"
#include "../../kernel/vga.h"
#include "x86emu/include/x86emu.h"

#define LOW_BYTES 0x100000u
#define SCRATCH 0x4000u
#define PROGRAM 0x3000u
#define MODE_LIMIT 256
#define NV_MMIO_VIRT 0xe5000000u /* separate from xHCI at 0xe4000000 */
#define NV_MMIO_BYTES 0x01000000u
static x86emu_t cpu;
static u64 msrs[X86EMU_MSRS];
static u8 msr_perm[X86EMU_MSRS];
static u8 *low;
static bool enabled,failed,returned;
static u32 nv_mmio,nv_io,lfb_base,lfb_bytes;
static u16 vbe_version,current_mode,last_call;
struct mode {u16 id;u16 pad;struct fb_info info;};
static struct mode modes[MODE_LIMIT];
static u32 nmodes;
struct mode_audit {
    u32 queried,bios,attributes,bpp,model,masks,layout,logged;
    u32 max_w,max_h,max_bpp;
};
static struct mode_audit audit;
static bool reject_mode(u16 id,u32 w,u32 h,u32 bpp,const char *reason) {
    if((w>=1280 || h>=1024) && audit.logged<24) {
        kprintf("VBE mode: id=%x %ux%u %u bpp rejected: %s\n",id,w,h,bpp,reason);
        audit.logged++;
    }
    return false;
}
static inline u16 rd16(u32 at) {return low[at]|((u16)low[at+1]<<8);}
static inline u32 rd32(u32 at) {return rd16(at)|((u32)rd16(at+2)<<16);}

static unsigned bad_access(x86emu_t *e,u32 at,unsigned type) {
    if (!failed) kprintf("VBE runtime: blocked access %x type=%x\n",at,type);
    failed=true;x86emu_stop(e);return 1;
}
static unsigned access(x86emu_t *e,u32 at,u32 *val,unsigned type) {
    unsigned bits=type&255,kind=type&~255u;
    if (bits>3) return bad_access(e,at,type);
    u32 bytes=bits==X86EMU_MEMIO_8_NOPERM?1:1u<<bits;
    if (kind==X86EMU_MEMIO_I || kind==X86EMU_MEMIO_O) {
        /* QEMU's option ROM debug console is diagnostic-only. */
        if(at==0x402 && kind==X86EMU_MEMIO_O)return 0;
        /* VGA, VBE, PCI config and port-80 delay; no PIC/PIT reprogramming. */
        bool allowed=(at>=0x3b0 && at+bytes<=0x3e0) ||
            (at>=0x1ce && at+bytes<=0x1d1) ||
            (at>=0xcf8 && at+bytes<=0xd00) || at==0x80 ||
            (kind==X86EMU_MEMIO_I && (at==0x40 || at==0x61)) ||
            nvidia_firmware_io_contains(nv_io,at,bytes);
        if (!allowed || at>65535 || at+bytes>65536) return bad_access(e,at,type);
        if (kind==X86EMU_MEMIO_I)*val=video_bios_port_read((u16)at,bytes);
        else video_bios_port_write((u16)at,*val,bytes);
        return 0;
    }
    volatile u8 *p;
    if (at<LOW_BYTES && bytes<=LOW_BYTES-at) {
        /* VGA aperture is device memory; ROM and BIOS RAM remain private. */
        if(at>=0xa0000 && at+bytes<=0xc0000 && kind!=X86EMU_MEMIO_X)
            p=(volatile u8 *)at;
        else p=low+at;
    } else if (kind!=X86EMU_MEMIO_X && nv_mmio && at>=nv_mmio &&
               at-nv_mmio<=NV_MMIO_BYTES-bytes) p=(volatile u8 *)(NV_MMIO_VIRT+at-nv_mmio);
    else if (kind!=X86EMU_MEMIO_X && lfb_base && at>=lfb_base &&
             at-lfb_base<=lfb_bytes-bytes) p=(volatile u8 *)(FB_VIRT+at-lfb_base);
    else return bad_access(e,at,type);
    if(kind==X86EMU_MEMIO_W) {
        if (bytes==4 && !((u32)p&3)) *(volatile u32 *)p=*val;
        else if(bytes==2 && !((u32)p&1)) *(volatile u16 *)p=*val;
        else for(u32 i=0;i<bytes;i++)p[i]=*val>>(8*i);
    } else {
        if(bytes==4 && !((u32)p&3))*val=*(volatile u32 *)p;
        else if(bytes==2 && !((u32)p&1))*val=*(volatile u16 *)p;
        else { *val=0;for(u32 i=0;i<bytes;i++)*val|=(u32)p[i]<<(8*i); }
    }
    return 0;
}
static int code(x86emu_t *e) {
    if (e->x86.R_CS_BASE+e->x86.R_EIP==PROGRAM+2) {returned=true;return 1;}
    /* BIOS delay loops observing BDA time must make forward progress. */
    if (!(e->x86.R_TSC&0xffff)) {
        u32 ticks=rd32(0x46c)+1;memcpy(low+0x46c,&ticks,4);
    }
    return failed;
}
static int interrupt(x86emu_t *e,u8 num,unsigned type) {
    if ((type&255)!=INTR_TYPE_SOFT || num<0x10) {
        kprintf("VBE runtime: exception/interrupt %x type=%x at %x:%x\n",
                num,type,e->x86.R_CS,e->x86.R_EIP);
        failed=true;x86emu_stop(e);return 1;
    }
    if(num==0x15 && e->x86.R_AH==0x86) {e->x86.R_EFLG&=~1u;return 1;}
    return 0;
}
static void no_msr(x86emu_t *e) {
    kprintf("VBE runtime: unsupported MSR %x at %x:%x\n",e->x86.R_ECX,e->x86.R_CS,e->x86.R_EIP);
    failed=true;x86emu_stop(e);
}
static bool bios(u16 ax,u16 bx,u16 cx,u16 dx) {
    last_call=ax;
    memset(&cpu,0,sizeof(cpu));memset(msrs,0,sizeof(msrs));
    cpu.x86.msr=msrs;cpu.x86.msr_perm=msr_perm;
    for(int i=0;i<8;i++) {cpu.x86.seg[i].limit=0xffff;cpu.x86.seg[i].acc=i==R_CS_INDEX?0x9b:0x93;}
    cpu.x86.R_IDT_LIMIT=0x3ff;cpu.x86.R_EFLG=2;
    cpu.x86.R_EIP=PROGRAM;cpu.x86.R_ESP=0x8000;
    cpu.x86.R_EAX=ax;cpu.x86.R_EBX=bx;cpu.x86.R_ECX=cx;cpu.x86.R_EDX=dx;
    cpu.x86.R_EDI=SCRATCH;
    cpu.memio=access;cpu.code_check=code;cpu.intr=interrupt;
    cpu.rdmsr=no_msr;cpu.wrmsr=no_msr;cpu.max_instr=10000000;
    low[PROGRAM]=0xcd;low[PROGRAM+1]=0x10;low[PROGRAM+2]=0xf4;
    failed=returned=false;
    /* Preserve the shared PCI config selector even when firmware fails. */
    u32 pci_address;__asm__ volatile("inl %1,%0":"=a"(pci_address):"Nd"((u16)0xcf8));
    x86emu_run(&cpu,X86EMU_RUN_MAX_INSTR);
    __asm__ volatile("outl %0,%1"::"a"(pci_address),"Nd"((u16)0xcf8));
    if(!returned)failed=true;
    if(failed)kprintf("VBE runtime: call=%x BX=%x CX=%x stopped at %x:%x after %u instructions\n",
                     ax,bx,cx,cpu.x86.R_CS,cpu.x86.R_EIP,(u32)cpu.x86.R_TSC);
    return returned && !failed && cpu.x86.R_AX==0x004f;
}
static bool query(u16 id,struct fb_info *out) {
    memset(low+SCRATCH,0,256);audit.queried++;
    if(!bios(0x4f01,0,id,0)) {audit.bios++;return false;}
    u32 p=SCRATCH,w=rd16(p+18),h=rd16(p+20),bpp=low[p+25];
    if(w*h>audit.max_w*audit.max_h) {audit.max_w=w;audit.max_h=h;audit.max_bpp=bpp;}
    if((rd16(p)&0x99)!=0x99) {
        audit.attributes++;return reject_mode(id,w,h,bpp,"no usable linear graphics mode");
    }
    if(bpp!=32) {audit.bpp++;return reject_mode(id,w,h,bpp,"renderer requires 32 bpp");}
    if(low[p+27]!=6) {audit.model++;return reject_mode(id,w,h,bpp,"not direct color");}
    u32 m=vbe_version>=0x300?p+54:p+31;
    if(rd16(m)!=0x1008 || rd16(m+2)!=0x0808 || rd16(m+4)!=8) {
        audit.masks++;return reject_mode(id,w,h,bpp,"not RGB 8:8:8");
    }
    struct boot_info b={.magic=BOOT_INFO_MAGIC,.version=1,.font_phys=0x5000,
        .fb_phys=rd32(p+40),.width=rd16(p+18),.height=rd16(p+20),.bpp=32,
        .pitch=rd16(p+(vbe_version>=0x300?50:16))};
    if(!video_firmware_init(&b,out) || out->vram>lfb_bytes ||
       out->phys>0xffffffffu-FB_VIRT_SZ) {
        audit.layout++;return reject_mode(id,w,h,bpp,"geometry/pitch/framebuffer limit");
    }
    kprintf("VBE mode: id=%x %ux%u %u bpp accepted\n",id,w,h,bpp);
    return true;
}
bool video_runtime_init(const struct boot_info *boot,struct fb_info *info) {
    if(!boot || boot->bios_snapshot_magic!=0x49564256) {
        kprintf("VBE runtime: BIOS snapshot missing; fixed boot resolution\n");return false;
    }
    low=kmalloc(LOW_BYTES);if(!low)return false;
    memcpy(low,(const void *)0x2000,4096);
    memcpy(low+4096,(const void *)4096,LOW_BYTES-4096);
    lfb_base=info->phys;lfb_bytes=FB_VIRT_SZ;
    /* TU116's BAR0 aperture is 16 MiB. Never guess the aperture of other GPUs. */
    struct pci_dev gpu;
    bool have_nvidia=pci_find_display_vendor(0x10de,&gpu);
    if(have_nvidia) {
        kprintf("VBE runtime: NVIDIA PCI %x:%x.%x BAR1=%x:%x framebuffer=%x\n",
                gpu.bus,gpu.dev,gpu.func,pci_read(gpu.bus,gpu.dev,gpu.func,0x18),
                pci_read(gpu.bus,gpu.dev,gpu.func,0x14),info->phys);
        kprintf("VBE runtime: NVIDIA BAR3=%x:%x BAR5=%x command=%x\n",
                pci_read(gpu.bus,gpu.dev,gpu.func,0x20),pci_read(gpu.bus,gpu.dev,gpu.func,0x1c),
                pci_read(gpu.bus,gpu.dev,gpu.func,0x24),pci_read(gpu.bus,gpu.dev,gpu.func,4)&0xffffu);
    }
    if(have_nvidia && nvidia_firmware_io_base(&gpu,&nv_io))
        kprintf("VBE runtime: NVIDIA BAR5 I/O %x-%x, PCI %x:%x.%x\n",
                nv_io,nv_io+NVIDIA_FIRMWARE_IO_BYTES-1,gpu.bus,gpu.dev,gpu.func);
    if(have_nvidia && gpu.device==0x21c4 &&
       (gpu.bar0&15)==0 && (gpu.bar0&~15u)>=LOW_BYTES &&
       (gpu.bar0&~15u)<=0xffffffffu-NV_MMIO_BYTES) {
        nv_mmio=gpu.bar0&~15u;
        for(u32 off=0;off<NV_MMIO_BYTES;off+=4096)
            if(vmm_map(NV_MMIO_VIRT+off,nv_mmio+off,PG_WRITE|0x18))goto fail;
    }
    memset(low+SCRATCH,0,512);memcpy(low+SCRATCH,"VBE2",4);
    if(!bios(0x4f00,0,0,0) || rd32(SCRATCH)!=0x41534556)goto fail;
    vbe_version=rd16(SCRATCH+4);if(vbe_version<0x200)goto fail;
    u32 bytes=(u32)rd16(SCRATCH+18)*65536;
    if(bytes<info->vram)goto fail;
    lfb_bytes=bytes<FB_VIRT_SZ?bytes:FB_VIRT_SZ;
    u32 far=rd32(SCRATCH+14),list=((far>>16)<<4)+(far&65535);
    u16 ids[MODE_LIMIT];u32 count=0;bool terminated=false;
    memset(&audit,0,sizeof(audit));
    /* Copy IDs before mode-info BIOS calls reuse firmware scratch buffers. */
    for(u32 i=0;i<MODE_LIMIT && list<=LOW_BYTES-2;i++,list+=2) {
        u16 id=rd16(list);if(id==0xffff) {terminated=true;break;}ids[count++]=id;
    }
    if(!bios(0x4f03,0,0,0))goto fail;
    current_mode=cpu.x86.R_BX&0x3fff;
    for(u32 i=0;i<count;i++) {
        struct fb_info fi;
        if(query(ids[i],&fi)) modes[nmodes++]=(struct mode){.id=ids[i],.info=fi};
        if(failed)goto fail;
    }
    kprintf("VBE audit: listed=%u queried=%u accepted=%u list_complete=%u\n",
            count,audit.queried,nmodes,terminated);
    kprintf("VBE audit: largest reported=%ux%u %u bpp; rejected depth=%u flags=%u color=%u masks=%u layout=%u BIOS=%u\n",
            audit.max_w,audit.max_h,audit.max_bpp,audit.bpp,audit.attributes,audit.model,
            audit.masks,audit.layout,audit.bios);
    if(!nmodes)goto fail;
    enabled=true;info->vram=lfb_bytes;info->caps&=~FB_CAP_FIXED_MODE;
    for(u32 i=0;i<nmodes;i++) {
        if(modes[i].info.width>info->max_w)info->max_w=modes[i].info.width;
        if(modes[i].info.height>info->max_h)info->max_h=modes[i].info.height;
    }
    kprintf("VBE runtime: %u validated modes, software BIOS interpreter; GPU acceleration unavailable\n",nmodes);
    return true;
fail:
    kprintf("VBE runtime: unavailable, call=%x result AX=%x, modes=%u; fixed boot resolution\n",
            last_call,cpu.x86.R_AX,nmodes);
    kfree(low);low=NULL;nv_mmio=nv_io=0;nmodes=0;return false;
}
bool video_runtime_mode_ok(u32 w,u32 h) {
    if(!enabled)return false;
    for(u32 i=0;i<nmodes;i++)if(modes[i].info.width==w && modes[i].info.height==h)return true;
    return false;
}
bool video_runtime_set_mode(u32 w,u32 h,struct fb_info *info) {
    if(!enabled)return false;
    for(u32 i=0;i<nmodes;i++) {
        struct mode *m=&modes[i];
        if(m->info.width!=w || m->info.height!=h)continue;
        if(!bios(0x4f02,m->id|0x4000,0,0)) {
            if(!bios(0x4f02,current_mode|0x4000,0,0)) {
                enabled=false;info->caps|=FB_CAP_FIXED_MODE;
            }
            return false;
        }
        current_mode=m->id;
        struct fb_info next=m->info;
        next.driver=info->driver;next.max_w=info->max_w;next.max_h=info->max_h;
        next.vram=info->vram;next.gen=info->gen+1;next.caps&=~FB_CAP_FIXED_MODE;
        *info=next;lfb_base=next.phys;
        kprintf("VBE runtime: switched mode=%x to %ux%u pitch=%u\n",m->id,w,h,next.pitch);
        return true;
    }
    return false;
}
#else
bool video_runtime_init(const struct boot_info *b,struct fb_info *i) {(void)b;(void)i;return false;}
bool video_runtime_mode_ok(u32 w,u32 h) {(void)w;(void)h;return false;}
bool video_runtime_set_mode(u32 w,u32 h,struct fb_info *i) {(void)w;(void)h;(void)i;return false;}
#endif
