/* Positive name-cache invalidation and an I/O bound for repeated #include lookup. */
#include "fs.h"
#include "ata.h"
#include "task.h"
#include "string.h"
int printf(const char*,...);int snprintf(char*,unsigned long,const char*,...);void exit(int);
int host_open_image(const char*);unsigned host_sectors(void);int host_read_sector(unsigned,void*);int host_write_sector(unsigned,const void*);void host_close_image(void);
static unsigned reads;
bool ata_init(void){return true;}u32 ata_sectors(void){return host_sectors();}const char *ata_model(void){return "lookup test";}
int ata_read(u32 l,void *b){reads++;return host_read_sector(l,b);}int ata_write(u32 l,const void *b){return host_write_sector(l,b);}
void mutex_lock(struct mutex *m){(void)m;}void mutex_unlock(struct mutex *m){(void)m;}
#define CHECK(c) do {if(!(c)){printf("LOOKUP FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(int argc,char **argv){
 CHECK(argc==2&&host_open_image(argv[1])==0);bool formatted;CHECK(fs_init(&formatted)==0&&formatted);
 u32 dir,ino,found;CHECK(fs_create(1,"cache",FS_TYPE_DIR,&dir)==0);
 for(int i=0;i<400;i++){char n[32];snprintf(n,sizeof(n),"f%d",i);CHECK(fs_create(dir,n,FS_TYPE_FILE,&ino)==0);}
 CHECK(fs_resolve(1,"/cache/f399",&ino)==0);unsigned start=reads;
 for(int i=0;i<100;i++)CHECK(fs_resolve(1,"/cache/f399",&found)==0&&found==ino);
 CHECK(reads-start<600);printf("100 repeated lookups: %u sector reads\n",reads-start);
 CHECK(fs_rename(1,"/cache/f399","/cache/moved")==0);CHECK(fs_resolve(1,"/cache/f399",&found)==FS_ENOENT);
 CHECK(fs_resolve(1,"/cache/moved",&found)==0&&found==ino);CHECK(fs_remove(1,"/cache/moved")==0);
 CHECK(fs_create(dir,"different",FS_TYPE_FILE,&ino)==0);CHECK(fs_resolve(1,"/cache/moved",&found)==FS_ENOENT);
 CHECK(fs_resolve(1,"/cache/new",&found)==FS_ENOENT);CHECK(fs_create(dir,"new",FS_TYPE_FILE,&ino)==0);CHECK(fs_resolve(1,"/cache/new",&found)==0);
 CHECK(fs_rename(1,"/cache","/renamed")==0);CHECK(fs_resolve(1,"/cache/new",&found)==FS_ENOENT);CHECK(fs_resolve(1,"/renamed/new",&found)==0);
 CHECK(fs_symlink(1,"/renamed/new","/alias")==0);CHECK(fs_resolve(1,"/alias",&found)==0&&found==ino);
 CHECK(fs_remove(1,"/alias")==0);CHECK(fs_symlink(1,"/renamed/f398","/alias")==0);CHECK(fs_resolve(1,"/alias",&found)==0&&found!=ino);
 CHECK(fs_init(&formatted)==0&&!formatted);CHECK(fs_resolve(1,"/renamed/new",&found)==0);
 CHECK(fs_format()==0);CHECK(fs_resolve(1,"/renamed/new",&found)==FS_ENOENT);CHECK(fs_resolve(1,"/alias",&found)==FS_ENOENT);
 host_close_image();printf("VFS name cache invalidation: PASS\n");return 0;
}
