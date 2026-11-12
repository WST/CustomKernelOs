/* ATA PIO, LBA28. Primary master is the boot disk and is never exposed.
 * The VFS serializes I/O across both channels. All status polling is bounded. */
#include "ata.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "bootinfo.h"
static struct { u16 base,control; u8 slave; u32 sectors; char model[41]; } units[2] = {
    { .base=0x1f0, .control=0x3f6, .slave=0x10 },
    { .base=0x170, .control=0x376, .slave=0 }
};
static void delay(int u) { for(int i=0;i<4;i++) inb(units[u].control); }
static int wait_status(int u,bool drq,bool identifying) {
    /* Host sparse-file allocation/flush can take seconds. A count of port
     * reads is not a stable timeout across real CPUs and QEMU. */
    u32 began=timer_ticks();
    for(u32 i=0;i<100000000u && timer_ticks()-began<30*TIMER_HZ;i++) {
        u8 st=inb(units[u].base+7);
        if(st==0 || st==0xff) return -1;
        if(st&0x80) continue;
        if(identifying) {
            /* IDENTIFY DEVICE is rejected by packet devices (e.g. QEMU's
             * default CD-ROM). Inspect the signature after BSY clears,
             * before treating its expected ABRT as a disk I/O failure. */
            u8 mid=inb(units[u].base+4),hi=inb(units[u].base+5);
            if((mid==0x14 && hi==0xeb) || (mid==0x69 && hi==0x96)) return -1;
        }
        if(st&0x21) { kprintf("ATA unit %u: status=%x error=%x\n",u,st,inb(units[u].base+1)); return -1; }
        if(!drq || (st&8)) return 0;
    }
    kprintf("ATA unit %u: timeout, status=%x\n",u,inb(units[u].base+7)); return -1;
}
static int wait(int u,bool drq) { return wait_status(u,drq,false); }
static bool identify(int u) {
    u16 b=units[u].base; units[u].sectors=0;
    outb(units[u].control,2); outb(b+6,0xa0|units[u].slave); delay(u);
    for(int i=2;i<=5;i++) outb(b+i,0);
    outb(b+7,0xec); delay(u);
    if(wait_status(u,true,true)<0 || inb(b+4) || inb(b+5)) return false;
    u16 id[256]; for(int i=0;i<256;i++) id[i]=inw(b);
    if(!(id[49]&(1<<9))) return false;
    u32 n=id[60]|(u32)id[61]<<16;
    units[u].sectors=n>0x10000000u ? 0x10000000u : n;
    for(int i=0;i<20;i++) { units[u].model[2*i]=id[27+i]>>8; units[u].model[2*i+1]=id[27+i]; }
    units[u].model[40]=0;
    for(int i=39;i>=0 && units[u].model[i]==' ';i--) units[u].model[i]=0;
    return units[u].sectors!=0;
}
static const struct boot_info *live;
bool ata_init(void) {
    live=boot_info_get();
    if (live && (live->root_phys!=0x1000000 || !live->root_bytes ||
                 live->root_bytes>0x4000000 || (live->root_bytes&511))) live=NULL;
    if (live) {
        /* Live mode deliberately does not probe or expose internal ATA disks. */
        units[0].sectors=live->root_bytes/512;
        strcpy(units[0].model,"USB live RAM disk (volatile)");
        units[1].sectors=0;
        return true;
    }
#ifdef MYOS_HARDWARE
    /* A missing/corrupt boot contract must never expose internal disks. */
    units[0].sectors=units[1].sectors=0;
    return false;
#endif
    bool root=identify(0); identify(1); return root;
}
u32 ata_unit_sectors(int u) { return u>=0 && u<2 ? units[u].sectors : 0; }
u32 ata_sectors(void) { return ata_unit_sectors(0); }
const char *ata_model(void) { return units[0].model; }
static int setup(int u,u32 lba,u8 cmd,u8 count) {
    if(!count || !ata_unit_sectors(u) || lba>=units[u].sectors || (u32)count>units[u].sectors-lba) return -1;
    u16 b=units[u].base;
    /* Select first: status/error belongs to the selected drive. */
    outb(b+6,0xe0|units[u].slave|((lba>>24)&15)); delay(u);
    if(wait(u,false)<0) return -1;
    outb(b+2,count); outb(b+3,lba); outb(b+4,lba>>8); outb(b+5,lba>>16);
    outb(b+7,cmd); delay(u); return wait(u,true);
}
/* count is 1..32. PIO repeats DRQ once per sector; a count register of 0 means 256.
 * FLUSH CACHE is not sent: QEMU turns it into an image fsync and the status
 * poll then occupies the CPU for the whole write. */
static int pio(int u,u32 lba,void *buf,u32 count,bool write) {
    if(!count || count>32) return -1;
    if (live) {
        if (u || lba>=units[0].sectors || count>units[0].sectors-lba) return -1;
        u8 *disk=(u8 *)live->root_phys+lba*512;
        if (write) memcpy(disk,buf,count*512); else memcpy(buf,disk,count*512);
        return 0;
    }
    if(setup(u,lba,write?0x30:0x20,(u8)count)<0) return -1;
    u8 *p=buf;
    for(u32 s=0;s<count;s++) {
        if(s && wait(u,true)<0) return -1;
        if(write) { const u16 *w=(const u16 *)p; for(int i=0;i<256;i++) outw(units[u].base,w[i]); }
        else { u16 *w=(u16 *)p; for(int i=0;i<256;i++) w[i]=inw(units[u].base); }
        p+=512;
    }
    delay(u); return wait(u,false);
}
int ata_unit_read(int u,u32 lba,void *buf) { return pio(u,lba,buf,1,false); }
int ata_unit_write(int u,u32 lba,const void *buf) { return pio(u,lba,(void *)buf,1,true); }
int ata_unit_read_sectors(int u,u32 lba,void *buf,u32 count) { return pio(u,lba,buf,count,false); }
int ata_unit_write_sectors(int u,u32 lba,const void *buf,u32 count) { return pio(u,lba,(void *)buf,count,true); }
int ata_read(u32 lba,void *buf) { return ata_unit_read(0,lba,buf); }
int ata_write(u32 lba,const void *buf) { return ata_unit_write(0,lba,buf); }
