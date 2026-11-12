#include "block.h"
#include "ata.h"
#ifndef HOST_TEST
#include "ahci.h"
#endif
#include "string.h"
#include "task.h"
#ifndef HOST_TEST
#include "heap.h"
#endif
#ifdef HOST_TEST
static unsigned io_reads, io_writes;
void block_io_reset(void) { io_reads = io_writes = 0; }
void block_io_stats(unsigned *reads, unsigned *writes) {
    if (reads) *reads = io_reads;
    if (writes) *writes = io_writes;
}
int atexit(void (*fn)(void));
static void host_exit_flush(void) { block_flush(); }
void *calloc(unsigned long, unsigned long);
#endif
/* Five stable IDs per disk: whole device, then four primary MBR slots.
 * Rescanning one disk never invalidates handles on the other disk.
 * File data may sit in the cache until block_flush. Metadata goes through
 * block_write_through and reaches the device before that call returns. */
static struct block_device devices[BLOCK_MAX];
static bool tables[2];
static struct mutex block_lock;
#define CACHE_SLOTS 128
#define CACHE_RUN 32
/* The boot stack occupies 0x9b000..0x9f000. These buffers are ~80 KiB and
 * used to sit in BSS, which the linker rejects. The heap is ready before
 * the first disk command. */
struct cache_ent {
    int unit;
    u32 lba;
    u8 data[512];
    bool valid, dirty;
    u32 age;
};
static struct cache_ent *cache;
static u8 *run_buf;
static u32 cache_clock;

static int ensure_cache(void) {
    if (cache && run_buf) return 0;
    mutex_lock(&block_lock);
    if (!cache || !run_buf) {
#ifdef HOST_TEST
        if (!cache) cache = calloc(CACHE_SLOTS, sizeof *cache);
        if (!run_buf) run_buf = calloc(CACHE_RUN, 512);
#else
        if (!cache) cache = kcalloc(CACHE_SLOTS, sizeof *cache);
        if (!run_buf) run_buf = kcalloc(CACHE_RUN, 512);
#endif
    }
    int rc = (cache && run_buf) ? 0 : -1;
    mutex_unlock(&block_lock);
    return rc;
}

static u32 sectors(int u) {
#ifdef HOST_TEST
    return u==0 ? ata_sectors() : 0;
#else
    if (ahci_present(u)) return ahci_sectors(u);
    return ata_unit_sectors(u);
#endif
}
static int device_read(int u,u32 lba,void *buf) {
#ifdef HOST_TEST
    return u==0 ? ata_read(lba,buf) : -1;
#else
    if (ahci_present(u)) return ahci_read(u,lba,buf);
    return ata_unit_read(u,lba,buf);
#endif
}
static int device_write_run(int u,u32 lba,const void *buf,u32 n) {
    if (!n || n>CACHE_RUN) return -1;
#ifdef HOST_TEST
    if (u) return -1;
    const u8 *p=buf;
    for (u32 i=0;i<n;i++) if (ata_write(lba+i,p+i*512)) return -1;
    return 0;
#else
    if (ahci_present(u)) return ahci_write(u,lba,buf,n);
    return ata_unit_write_sectors(u,lba,(void *)buf,n);
#endif
}
static int cache_find(int unit,u32 lba) {
    for (int i=0;i<CACHE_SLOTS;i++)
        if (cache[i].valid && cache[i].unit==unit && cache[i].lba==lba) return i;
    return -1;
}
/* Caller holds block_lock. Writes a consecutive dirty run and clears dirty. */
static int flush_from(int unit,u32 lba) {
    u32 n=0;
    while (n<CACHE_RUN) {
        int i=cache_find(unit,lba+n);
        if (i<0 || !cache[i].dirty) break;
        memcpy(run_buf+n*512,cache[i].data,512);
        n++;
    }
    if (!n) return 0;
    if (device_write_run(unit,lba,run_buf,n)) return -1;
    for (u32 k=0;k<n;k++) {
        int i=cache_find(unit,lba+k);
        if (i>=0) cache[i].dirty=false;
    }
    return 0;
}
static int claim_slot(void) {
    for (int i=0;i<CACHE_SLOTS;i++) if (!cache[i].valid) return i;
    int clean=-1, dirty=-1;
    for (int i=0;i<CACHE_SLOTS;i++) {
        int *oldest=cache[i].dirty ? &dirty : &clean;
        if (*oldest<0 || (i32)(cache[i].age-cache[*oldest].age)<0) *oldest=i;
    }
    /* Reuse the oldest clean sector before writing back any dirty data. */
    if (clean>=0) return clean;
    int oldest=dirty;
    if (flush_from(cache[oldest].unit,cache[oldest].lba)) return -1;
    return oldest;
}
static int cache_store(int unit,u32 lba,const void *buf,bool through) {
    if (ensure_cache()) return -1;
    mutex_lock(&block_lock);
    int i=cache_find(unit,lba);
    if (i<0) i=claim_slot();
    int rc=-1;
    if (i>=0) {
        memcpy(cache[i].data,buf,512);
        cache[i].unit=unit;
        cache[i].lba=lba;
        cache[i].valid=true;
        cache[i].age=++cache_clock;
        cache[i].dirty=!through;
        rc=0;
        if (through && device_write_run(unit,lba,buf,1)) { cache[i].dirty=true; rc=-1; }
    }
    mutex_unlock(&block_lock);
    return rc;
}
static int cache_load(int unit,u32 lba,void *buf) {
    if (ensure_cache()) return -1;
    mutex_lock(&block_lock);
    int i=cache_find(unit,lba);
    int rc=0;
    if (i>=0) { cache[i].age=++cache_clock; memcpy(buf,cache[i].data,512); }
    else {
        i=claim_slot();
        if (i<0 || device_read(unit,lba,cache[i].data)) { if (i>=0) cache[i].valid=false; rc=-1; }
        else {
            cache[i].unit=unit; cache[i].lba=lba; cache[i].valid=true; cache[i].dirty=false;
            cache[i].age=++cache_clock;
            memcpy(buf,cache[i].data,512);
        }
    }
    mutex_unlock(&block_lock);
    return rc;
}
static int flush_locked(void) {
    int rc=0;
    if (!cache || !run_buf) return 0;
    for (;;) {
        int i=-1;
        for (int n=0;n<CACHE_SLOTS;n++) if (cache[n].valid && cache[n].dirty) { i=n; break; }
        if (i<0) break;
        if (flush_from(cache[i].unit,cache[i].lba)) { rc=-1; break; }
    }
    return rc;
}
int block_flush(void) {
    mutex_lock(&block_lock);
    int rc=flush_locked();
    mutex_unlock(&block_lock);
    return rc;
}
void block_flush_idle(void) {
    /* The idle task must not sleep on this lock. The owner may itself be
     * asleep inside an ATA command, waiting for the timer, and a waiter
     * here would run with interrupts cleared. */
#ifndef HOST_TEST
    if (!mutex_trylock(&block_lock)) return;
    flush_locked();
    mutex_unlock(&block_lock);
#else
    block_flush();
#endif
}
void block_cache_invalidate(void) {
    mutex_lock(&block_lock);
    if (cache)
        for (int i=0;i<CACHE_SLOTS;i++) { cache[i].valid=false; cache[i].dirty=false; }
    mutex_unlock(&block_lock);
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
    u8 buf[512]; if(cache_load(disk,0,buf)) return -1;
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
int block_init(void) {
#ifdef HOST_TEST
    static bool hooked;
    if (!hooked) { atexit(host_exit_flush); hooked=true; }
#endif
    int r=block_rescan(0); block_rescan(1); return r;
}
static int access_sector(int id,u32 sector,void *buf,bool write,bool through) {
    const struct block_device *d=block_get(id);
    if (!d || sector>=d->sectors) return -1;
#ifdef HOST_TEST
    if (write) io_writes++; else io_reads++;
#endif
    return write ? cache_store(id/5,d->start+sector,buf,through) : cache_load(id/5,d->start+sector,buf);
}
int block_read(int id,u32 sector,void *buf) { return access_sector(id,sector,buf,false,false); }
int block_write(int id,u32 sector,const void *buf) { return access_sector(id,sector,(void *)buf,true,false); }
int block_write_through(int id,u32 sector,const void *buf) { return access_sector(id,sector,(void *)buf,true,true); }
