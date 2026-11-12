/* ATA PIO, LBA28. Primary master is the boot disk and is never exposed.
 * The VFS serializes I/O across both channels. A task sleeps for IRQ 14 or 15
 * only after a short poll; the drive's nIEN bit stays set the rest of the
 * time, including the data transfer. FLUSH CACHE is not sent: QEMU turns it
 * into an image fsync. */
#include "ata.h"
#include "string.h"
#include "vga.h"
#include "timer.h"
#include "task.h"
#include "bootinfo.h"
static struct { u16 base,control; u8 slave; u32 sectors; char model[41]; } units[2] = {
    { .base=0x1f0, .control=0x3f6, .slave=0x10 },
    { .base=0x170, .control=0x376, .slave=0 }
};
static void delay(int u) { for(int i=0;i<4;i++) inb(units[u].control); }
/* nIEN lives in the device control register and is shared by both drives on
 * the cable. Leave it set (value 2) except while a task is asleep: with it
 * clear the drive holds IRQ 14/15 for the whole time DRQ is set, and the
 * boot disk on the same cable can do that too. The CPU then never reaches
 * the timer. */
static void dev_irq(int u,bool on) { outb(units[u].control,on?0:2); }
static void ata_service(int u) {
    /* Drop the line before acknowledging. DRQ may still be set; nIEN is what
     * stops the drive from raising the interrupt again until the next sleep. */
    dev_irq(u,false);
    delay(u);
    inb(units[u].base+7);
    if(tasking_ready()) task_wakeup(&units[u]);
}
static void ata_irq_primary(struct regs *r) { (void)r; ata_service(0); }
static void ata_irq_secondary(struct regs *r) { (void)r; ata_service(1); }
/* -1 failure, 0 ready, 1 still busy. */
static int classify(int u,u8 st,bool drq,bool identifying) {
    if(st==0 || st==0xff) return -1;
    if(st&0x80) return 1;
    if(identifying) {
        /* IDENTIFY DEVICE is rejected by packet devices (e.g. QEMU's
         * default CD-ROM). Inspect the signature after BSY clears,
         * before treating its expected ABRT as a disk I/O failure. */
        u8 mid=inb(units[u].base+4),hi=inb(units[u].base+5);
        if((mid==0x14 && hi==0xeb) || (mid==0x69 && hi==0x96)) return -1;
    }
    if(st&0x21) { kprintf("ATA unit %u: status=%x error=%x\n",u,st,inb(units[u].base+1)); return -1; }
    if(!drq || (st&8)) return 0;
    return 1;
}
static int take_status(int u,bool drq,bool identifying) {
    return classify(u,inb(units[u].base+7),drq,identifying);
}
static bool can_sleep(void) {
    u32 flags;
    __asm__ volatile ("pushfl; popl %0" : "=r"(flags));
    return tasking_ready() && !irq_nesting && (flags&0x200);
}
static int wait_status(int u,bool drq,bool identifying) {
    /* Host sparse-file allocation can take seconds. A ready sector usually
     * appears within a short poll; after that the task sleeps for IRQ 14/15.
     * The drive interrupt is enabled only for that sleep. One extra tick
     * covers a wake that was not our drive, so a stuck line cannot spin. */
    dev_irq(u,false);
    u32 began=timer_ticks();
    for(int i=0;i<1024;i++) {
        int r=take_status(u,drq,identifying);
        if(r<=0) return r;
    }
    if(!can_sleep()) {
        for(u32 i=0;i<100000000u && timer_ticks()-began<30*TIMER_HZ;i++) {
            int r=take_status(u,drq,identifying);
            if(r<=0) return r;
        }
        kprintf("ATA unit %u: timeout, status=%x\n",u,inb(units[u].base+7));
        return -1;
    }
    while(timer_ticks()-began<30*TIMER_HZ) {
        u32 flags=irq_save();
        int r=take_status(u,drq,identifying);
        if(r<=0) { irq_restore(flags); return r; }
        dev_irq(u,true); delay(u);
        r=take_status(u,drq,identifying);
        if(r<=0) { dev_irq(u,false); irq_restore(flags); return r; }
        u32 mark=timer_ticks();
        task_wait_timeout(&units[u],1);
        dev_irq(u,false);
        r=take_status(u,drq,identifying);
        if(r<=0) { irq_restore(flags); return r; }
        /* task_sleep_ticks ignores the disk IRQ. A stuck line therefore
         * waits for the timer instead of waking itself forever. */
        if(timer_ticks()==mark) task_sleep_ticks(1);
        irq_restore(flags);
    }
    kprintf("ATA unit %u: timeout, status=%x\n",u,inb(units[u].base+7));
    return -1;
}
static int wait(int u,bool drq) { return wait_status(u,drq,false); }
static bool identify(int u) {
    u16 b=units[u].base; units[u].sectors=0;
    dev_irq(u,false); outb(b+6,0xa0|units[u].slave); delay(u);
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
    /* Drain both devices on each channel before enabling IRQs. The boot disk
     * shares IRQ 14 with the system disk and may still be holding the line. */
    for(int ch=0;ch<2;ch++) {
        u16 base=units[ch].base, control=units[ch].control;
        outb(control,2);
        for(u8 sel=0xa0; sel<=0xb0; sel+=0x10) {
            outb(base+6,sel);
            for(int i=0;i<4;i++) inb(control);
            inb(base+7);
        }
    }
    irq_install(14,ata_irq_primary);
    irq_install(15,ata_irq_secondary);
    bool root=identify(0); identify(1); return root;
}
u32 ata_unit_sectors(int u) { return u>=0 && u<2 ? units[u].sectors : 0; }
u32 ata_sectors(void) { return ata_unit_sectors(0); }
const char *ata_model(void) { return units[0].model; }
static int setup(int u,u32 lba,u8 cmd,u8 count) {
    if(!count || !ata_unit_sectors(u) || lba>=units[u].sectors || (u32)count>units[u].sectors-lba) return -1;
    u16 b=units[u].base;
    /* Select first: status/error belongs to the selected drive.
     * nIEN stays set here; wait_status clears it only while asleep. */
    dev_irq(u,false);
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
