#include "block.h"
#include "ata.h"
#include "string.h"
/* Five stable IDs per disk: whole device, then four primary MBR slots.
 * Rescanning one disk never invalidates handles on the other disk. */
static struct block_device devices[BLOCK_MAX];
static bool tables[2];
static u32 sectors(int u) {
#ifdef HOST_TEST
    return u==0 ? ata_sectors() : 0;
#else
    return ata_unit_sectors(u);
#endif
}
static int read_sector(int u,u32 lba,void *buf) {
#ifdef HOST_TEST
    return u==0 ? ata_read(lba,buf) : -1;
#else
    return ata_unit_read(u,lba,buf);
#endif
}
static int write_sector(int u,u32 lba,const void *buf) {
#ifdef HOST_TEST
    return u==0 ? ata_write(lba,buf) : -1;
#else
    return ata_unit_write(u,lba,buf);
#endif
}
static u32 le32(const u8 *p) { return p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
const struct block_device *block_get(int id) { return id>=0 && id<BLOCK_MAX && devices[id].sectors ? &devices[id] : NULL; }
int block_count(void) { int n=0; for(int i=0;i<BLOCK_MAX;i++) if(block_get(i)) n++; return n; }
int block_at(int index) { for(int i=0;i<BLOCK_MAX;i++) if(block_get(i) && index--==0) return i; return -1; }
bool block_disk_partitioned(int disk) { return disk>=0 && disk<2 && tables[disk]; }
bool block_partitioned(void) { return tables[0]; }
int block_find(const char *name) {
    if(!strncmp(name,"/dev/",5)) name+=5;
    for(int i=0;i<BLOCK_MAX;i++) if(block_get(i) && !strcmp(name,devices[i].name)) return i;
    return -1;
}
int block_rescan(int disk) {
    if(disk<0 || disk>=2) return -1;
    int base=disk*5; memset(devices+base,0,5*sizeof(*devices)); tables[disk]=false;
    u32 total=sectors(disk); if(!total) return -1;
    if(total>0x10000000u) total=0x10000000u;
    struct block_device *whole=&devices[base]; strcpy(whole->name,disk ? "sdc" : "sdb"); whole->sectors=total;
    u8 buf[512]; if(read_sector(disk,0,buf)) return -1;
    if(buf[510]!=0x55 || buf[511]!=0xaa) return 0;
    tables[disk]=true;
    for(int i=0;i<4;i++) {
        const u8 *p=buf+446+i*16; u32 start=le32(p+8),size=le32(p+12);
        if(!p[4] || p[4]==0xee || p[4]==5 || p[4]==0x0f || p[4]==0x85) continue;
        if(!start || !size || start>=total || size>total-start) continue;
        bool overlap=false;
        for(int j=1;j<=i;j++) { const struct block_device *d=&devices[base+j];
            if(d->sectors && start<d->start+d->sectors && d->start<start+size) overlap=true;
        }
        if(overlap) continue;
        struct block_device *d=&devices[base+i+1]; strcpy(d->name,whole->name); d->name[3]='1'+i; d->name[4]=0;
        d->start=start; d->sectors=size; d->partition=true;
    }
    return 0;
}
int block_init(void) { int r=block_rescan(0); block_rescan(1); return r; }
int block_read(int id,u32 sector,void *buf) {
    const struct block_device *d=block_get(id);
    return !d || sector>=d->sectors ? -1 : read_sector(id/5,d->start+sector,buf);
}
int block_write(int id,u32 sector,const void *buf) {
    const struct block_device *d=block_get(id);
    return !d || sector>=d->sectors ? -1 : write_sector(id/5,d->start+sector,buf);
}
