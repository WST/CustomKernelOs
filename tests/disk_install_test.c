/* Installer state machine and I/O failures against disposable memory disks. */
#include "fs.h"
#include "block.h"
#include "task.h"
#include "string.h"
void *calloc(unsigned long,unsigned long);void exit(int);int printf(const char*,...);
void *mmap(void*,unsigned long,int,int,int,long);int fork(void);int waitpid(int,int*,int);void _exit(int);
static u8 *disks[3];static u32 sizes[3]={131072,135168,16384};
static unsigned writes[3],barriers[3];static bool fail_write,fail_flush;
static int cut_lba=-1;
bool ata_init(void){return true;}
u32 ata_unit_sectors(int u){return u>=0&&u<3?sizes[u]:0;}
u32 ata_sectors(void){return sizes[0];}
int ata_unit_read(int u,u32 l,void *b){if(l>=sizes[u])return -1;memcpy(b,disks[u]+l*512,512);return 0;}
int ata_read(u32 l,void *b){return ata_unit_read(0,l,b);}
int ata_unit_write(int u,u32 l,const void *b){if(u==1 && cut_lba>=0 && l==(u32)cut_lba)_exit(77);if(l>=sizes[u] || (u==1&&fail_write))return -1;writes[u]++;memcpy(disks[u]+l*512,b,512);return 0;}
int ata_write(u32 l,const void *b){return ata_unit_write(0,l,b);}
int ata_unit_flush(int u){if(u==1&&fail_flush)return -1;barriers[u]++;return 0;}
void mutex_lock(struct mutex *m){(void)m;}void mutex_unlock(struct mutex *m){(void)m;}
#define CHECK(c) do{if(!(c)){printf("INSTALL FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
static void put32(u8 *p,u32 x){for(int i=0;i<4;i++)p[i]=x>>(8*i);}
static u32 get32(const u8 *p){return p[0]|(u32)p[1]<<8|(u32)p[2]<<16|(u32)p[3]<<24;}
static u32 crc(const void *buf,u32 n){const u8 *p=buf;u32 c=~0u;while(n--){c^=*p++;for(int i=0;i<8;i++)c=(c>>1)^(0xedb88320u&(0u-(c&1)));}return ~c;}
static bool bootable(void){return !memcmp(disks[1]+400,"MYOSINST",8);}
static void file(const char *path,const void *data,u32 n){u32 ino;CHECK(!fs_create(1,path,FS_TYPE_FILE,&ino));CHECK(fs_write(ino,0,data,n)==(int)n);}
int main(void){
 for(int u=0;u<3;u++){disks[u]=mmap(0,(unsigned long)sizes[u]*512,3,0x21,-1,0);CHECK(disks[u]!=(void*)-1);}
 bool formatted;CHECK(!fs_init(&formatted)&&formatted);
 CHECK(!fs_create(1,"/boot",FS_TYPE_DIR,0));CHECK(!fs_create(1,"/boot/install",FS_TYPE_DIR,0));
 u8 *payload=calloc(2048,512);CHECK(payload);memset(payload+512,0xa5,1048576-512);
 memcpy(payload+400,"MYOSINST",8);put32(payload+408,1);put32(payload+412,2048);put32(payload+416,1);
 payload[510]=0x55;payload[511]=0xaa;put32(payload+420,crc(payload+512,1048576-512));
 file(INSTALL_PAYLOAD,payload,1048576);
 CHECK(!fs_create(1,"/etc",FS_TYPE_DIR,0));
 file("/etc/accounts","private accounts",16);
 struct disk_install_info info;
 CHECK(!fs_disk_install(INSTALL_PROBE,0,&info));
 CHECK(fs_disk_install(INSTALL_LIST,2,&info)==FS_ENODEV);
 CHECK(!fs_disk_install(INSTALL_LIST,0,&info)&&!strcmp(info.device,"/dev/sdc"));
 u32 payload_ino;CHECK(!fs_resolve(1,INSTALL_PAYLOAD,&payload_ino));
 u8 broken=1;CHECK(fs_write(payload_ino,512,&broken,1)==1);
 CHECK(fs_disk_install(INSTALL_PLAN,0,&info)==FS_EIO&&!writes[1]);
 CHECK(fs_write(payload_ino,512,payload+512,1)==1);
 CHECK(fs_disk_install(INSTALL_PLAN,1,&info)==FS_ENOSPC);
 CHECK(!fs_disk_install(INSTALL_PLAN,0,&info)&&info.state==INSTALL_PLANNED&&!writes[1]);
 struct disk_install_info bad=info;bad.token++;
 CHECK(fs_disk_install(INSTALL_PREPARE,0,&bad)==FS_EINVAL&&!writes[1]);
 CHECK(!fs_disk_install(INSTALL_ABORT,0,&info)&&!writes[1]);
 /* A protective GPT MBR must be rejected before any destructive write. */
 u8 mbr[512]={0};mbr[510]=0x55;mbr[511]=0xaa;mbr[450]=0xee;
 CHECK(!block_write_through(5,0,mbr));unsigned initial=writes[1];
 CHECK(fs_disk_install(INSTALL_PLAN,0,&info)==FS_EINVAL&&writes[1]==initial);
 memset(mbr,0,512);CHECK(!block_write_through(5,0,mbr));
 CHECK(!fs_disk_install(INSTALL_PLAN,0,&info));
 /* Disk changes after the confirmation plan invalidate it. */
 mbr[1]=1;CHECK(!block_write_through(5,0,mbr));
 CHECK(fs_disk_install(INSTALL_PREPARE,0,&info)==FS_EBUSY);
 CHECK(!fs_disk_install(INSTALL_PLAN,0,&info));
 CHECK(!fs_disk_install(INSTALL_PREPARE,0,&info)&&!bootable());
 CHECK(fs_disk_install(INSTALL_COMMIT,0,&info)<0&&!bootable());
 CHECK(!fs_create(1,"/target",FS_TYPE_DIR,0));CHECK(!fs_mount(1,"/dev/sdc1","/target"));
 CHECK(fs_disk_install(INSTALL_COMMIT,0,&info)==FS_EBUSY);
 CHECK(!fs_create(1,"/target/bin",FS_TYPE_DIR,0));CHECK(!fs_create(1,"/target/lib",FS_TYPE_DIR,0));
 CHECK(!fs_create(1,"/target/etc",FS_TYPE_DIR,0));
 u8 elf[52]={0x7f,'E','L','F'};file("/target/bin/sh",elf,sizeof(elf));
 file("/target/lib/ld.so",elf,sizeof(elf));file("/target/lib/libc.so",elf,sizeof(elf));
 CHECK(!fs_unmount(1,"/target"));
 /* Abrupt process death skips cache flushing, just like losing CPU power.
  * Shared backing memory retains only sectors already submitted to the disk. */
 CHECK(!fs_sync());block_cache_invalidate();
 u8 *baseline=calloc(sizes[1],512);CHECK(baseline);memcpy(baseline,disks[1],sizes[1]*512);
 int cuts[]={1,2,32,33,64,500,2047,0};
 for(unsigned n=0;n<sizeof(cuts)/sizeof(cuts[0]);n++) {
     cut_lba=cuts[n];int child=fork();CHECK(child>=0);
     if(!child){int result=fs_disk_install(INSTALL_COMMIT,0,&info);(void)result;_exit(78);}
     int status;CHECK(waitpid(child,&status,0)==child && status==(77<<8));
     CHECK(!bootable());memcpy(disks[1],baseline,sizes[1]*512);block_cache_invalidate();
 }
 cut_lba=-1;
 fail_write=true;CHECK(fs_disk_install(INSTALL_COMMIT,0,&info)==FS_EIO&&!bootable());fail_write=false;
 fail_flush=true;CHECK(fs_disk_install(INSTALL_COMMIT,0,&info)==FS_EIO&&!bootable());fail_flush=false;
 CHECK(!fs_disk_install(INSTALL_COMMIT,0,&info)&&info.state==INSTALL_DONE&&bootable());
 CHECK(!memcmp(disks[1]+512,payload+512,1048576-512));CHECK(barriers[1]>0);
 CHECK(!fs_mount(1,"/dev/sdc1","/target"));u32 ino;CHECK(!fs_resolve(1,"/target/bin/sh",&ino));
 CHECK(!fs_resolve(1,"/target/etc/accounts",&ino));
 char accounts[16];CHECK(fs_read(ino,0,accounts,16)==16&&!memcmp(accounts,"private accounts",16));
 CHECK(!fs_unmount(1,"/target"));
 printf("Installer plan/cancel, token, corrupt payload, GPT, changed/busy disk, incomplete root, 8 abrupt cuts, write/flush failures and final commit: PASS\n");
 /* Compact v2 image: reserve a 64 MiB ESP, copy only its allocated prefix,
  * and expose the EFI partition only in the final durable MBR. */
 CHECK(!fs_disk_install(INSTALL_ABORT,0,&info));CHECK(!fs_sync());block_cache_invalidate();
 sizes[1]=133120+131072;disks[1]=mmap(0,(unsigned long)sizes[1]*512,3,0x21,-1,0);CHECK(disks[1]!=(void*)-1);
 CHECK(!block_rescan(1));
 u8 *dual=calloc(6144,512);CHECK(dual);memcpy(dual,payload,512);
 put32(dual+408,2);put32(dual+412,133120);put32(dual+424,6144);
 put32(dual+428,2048);put32(dual+432,131072);put32(dual+436,4500);
 u8 *cfg=dual+4500*512;memcpy(cfg,"MYOSEFI1",8);put32(cfg+8,1);put32(cfg+12,2);put32(cfg+20,133120);
 put32(dual+420,crc(dual+512,6144*512-512));
 CHECK(!fs_truncate(payload_ino));CHECK(fs_write(payload_ino,0,dual,6144*512)==6144*512);
 CHECK(!fs_disk_install(INSTALL_PLAN,0,&info));CHECK(info.root_start==133120);
 CHECK(!fs_disk_install(INSTALL_PREPARE,0,&info));CHECK(!disks[1][466]&&!bootable());
 CHECK(!fs_mount(1,"/dev/sdc1","/target"));
 CHECK(!fs_create(1,"/target/bin",FS_TYPE_DIR,0));CHECK(!fs_create(1,"/target/lib",FS_TYPE_DIR,0));CHECK(!fs_create(1,"/target/etc",FS_TYPE_DIR,0));
 file("/target/bin/sh",elf,sizeof(elf));file("/target/lib/ld.so",elf,sizeof(elf));file("/target/lib/libc.so",elf,sizeof(elf));
 CHECK(!fs_unmount(1,"/target"));CHECK(!fs_sync());block_cache_invalidate();
 cut_lba=4500;int child=fork();CHECK(child>=0);
 if(!child){fs_disk_install(INSTALL_COMMIT,0,&info);_exit(78);}
 int status;CHECK(waitpid(child,&status,0)==child&&status==(77<<8));
 CHECK(!disks[1][466]&&!bootable());cut_lba=-1;block_cache_invalidate();
 CHECK(!fs_disk_install(INSTALL_COMMIT,0,&info));
 CHECK(disks[1][466]==0xef && get32(disks[1]+470)==2048 && get32(disks[1]+474)==131072);
 CHECK(bootable()&&get32(disks[1]+4500*512+16)==info.token&&get32(disks[1]+4500*512+24)==131072);
 CHECK(get32(disks[1]+420)==crc(disks[1]+512,6144*512-512));
 u8 original[512];CHECK(fs_read(payload_ino,4500*512,original,512)==512&&!get32(original+16));
 printf("UEFI compact payload: ESP hidden after abrupt cut, config patched to target, checked readback, final EFI activation, source unchanged: PASS\n");
 return 0;
}
