/* Real block cache against a counted/failing in-memory device. */
#include "../kernel/block.h"
#include "../kernel/task.h"
#include "../kernel/string.h"
int printf(const char *, ...);int puts(const char *);void exit(int);
#define CHECK(c) do { if (!(c)) { printf("BLOCK FAIL line %d: %s\n",__LINE__,#c);exit(1); } } while(0)
static u8 disk[1024][512];
static unsigned reads,writes;
static bool fail_write;
u32 ata_sectors(void) { return 1024; }
int ata_read(u32 lba,void *buf) { CHECK(lba<1024);reads++;memcpy(buf,disk[lba],512);return 0; }
int ata_write(u32 lba,const void *buf) { CHECK(lba<1024);writes++;if(fail_write)return -1;memcpy(disk[lba],buf,512);return 0; }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
int main(void) {
    u8 buf[512],data[512];memset(data,0x5a,512);
    CHECK(block_init()==0);
    for(unsigned i=0;i<128;i++)CHECK(block_read(0,i,buf)==0);
    for(unsigned i=128;i<192;i++)CHECK(block_read(0,i,buf)==0);
    reads=0;
    for(unsigned i=128;i<192;i++)CHECK(block_read(0,i,buf)==0);
    CHECK(reads==0); /* The new 64-sector working set fits in 128 slots. */
    /* Access refreshes LRU: protect sector 128 while filling the other slots. */
    CHECK(block_read(0,128,buf)==0);
    for(unsigned i=192;i<255;i++)CHECK(block_read(0,i,buf)==0);
    CHECK(block_read(0,128,buf)==0);
    CHECK(block_read(0,255,buf)==0);
    reads=0;CHECK(block_read(0,128,buf)==0 && reads==0);
    CHECK(block_write(0,500,data)==0);writes=0;fail_write=true;
    /* Clean victims must remain usable even if the disk rejects dirty writes. */
    CHECK(block_read(0,600,buf)==0 && writes==0);
    CHECK(block_flush()<0 && writes>0);
    CHECK(block_read(0,500,buf)==0 && !memcmp(buf,data,512));
    fail_write=false;CHECK(block_flush()==0 && !memcmp(disk[500],data,512));
    /* If every slot is dirty, failed eviction must retain the old contents. */
    block_cache_invalidate();
    for(unsigned i=0;i<128;i++)CHECK(block_write(0,i,data)==0);
    fail_write=true;CHECK(block_read(0,700,buf)<0);
    CHECK(block_read(0,0,buf)==0 && !memcmp(buf,data,512));
    fail_write=false;CHECK(block_flush()==0);
    puts("BLOCK CACHE PASS: working-set hits, clean LRU, write errors and retained dirty data");
    return 0;
}
