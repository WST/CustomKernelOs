/* Sparse, file-backed 1 GiB test. No 1 GiB RAM allocation required. */
#include "fs.h"
#include "block.h"
#include "ata.h"
#include "task.h"
#include "string.h"
int printf(const char*,...); int snprintf(char*,unsigned long,const char*,...); void exit(int);
int host_open_image(const char*); unsigned host_sectors(void);
int host_read_sector(unsigned,void*); int host_write_sector(unsigned,const void*); void host_close_image(void);
bool ata_init(void) { return true; } u32 ata_sectors(void) { return host_sectors(); }
const char *ata_model(void) { return "sparse test"; }
int ata_read(u32 l,void *b) { return host_read_sector(l,b); }
int ata_write(u32 l,const void *b) { return host_write_sector(l,b); }
void mutex_lock(struct mutex *m) { (void)m; } void mutex_unlock(struct mutex *m) { (void)m; }
#define CHECK(c) do { if(!(c)) { printf("FAIL line %d: %s\n",__LINE__,#c); exit(1); } } while(0)
static u8 data[1048576],readback[1048576];
static u32 le32(const u8 *p) { return p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24; }
int main(int argc,char **argv) {
    CHECK(argc==2 && host_open_image(argv[1])==0 && ata_sectors()==2097152u);
    bool formatted; CHECK(fs_init(&formatted)==0 && formatted);
    struct fs_info before,after; fs_info(&before);
    CHECK(before.total_blocks>2000000 && before.total_inodes==16383);
    for(unsigned i=0;i<sizeof(data);i++) data[i]=(u8)(i*7+i/512);
    char name[28]; u32 ino=0;
    for(int i=0;i<40;i++) {
        snprintf(name,sizeof(name),"file%d",i);
        CHECK(fs_create(1,name,FS_TYPE_FILE,&ino)==0);
        CHECK(fs_write(ino,0,data,sizeof(data))==(int)sizeof(data));
    }
    u8 sector[512]; CHECK(block_read(0,0,sector)==0);
    u32 table=le32(sector+20);
    CHECK(block_read(0,table+ino/8,sector)==0);
    CHECK(le32(sector+(ino%8)*64+8)>65536); /* allocation beyond old 32 MiB limit */
    for(int i=0;i<400;i++) { snprintf(name,sizeof(name),"empty%d",i); CHECK(fs_create(1,name,FS_TYPE_FILE,0)==0); }
    CHECK(fs_init(&formatted)==0 && !formatted);
    CHECK(fs_resolve(1,"file39",&ino)==0 && fs_read(ino,0,readback,sizeof(readback))==(int)sizeof(readback));
    CHECK(!memcmp(data,readback,sizeof(data)));
    CHECK(fs_resolve(1,"empty399",&ino)==0);
    /* Physical end-of-volume access and bounds rejection. */
    CHECK(block_write(0,2097151,data)==0 && block_read(0,2097151,sector)==0 && !memcmp(data,sector,512));
    CHECK(block_write(0,2097152,data)<0 && block_read(0,2097152,sector)<0);
    for(int i=0;i<40;i++) { snprintf(name,sizeof(name),"file%d",i); CHECK(fs_remove(1,name)==0); }
    for(int i=0;i<400;i++) { snprintf(name,sizeof(name),"empty%d",i); CHECK(fs_remove(1,name)==0); }
    fs_info(&after); CHECK(after.free_inodes==before.free_inodes);
    /* Directory blocks remain allocated after deleting entries. */
    CHECK(after.free_blocks+32>before.free_blocks);
    printf("1 GiB MyFS: 40 MiB files, 440 inodes, high sectors and remount OK\n");
    host_close_image(); return 0;
}
