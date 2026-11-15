#include "fs.h"
#include "myfs.h"
#include "block.h"
#include "string.h"
#include "task.h"
int printf(const char *,...);void exit(int);
#define N 8192
static u8 stable[N][512],drive[N][512],baseline[N][512],after[N][512];
static void *jump[5];
static unsigned events,cut,policy;
static bool armed,fail_flush,fail_write,capture_commit;
static u32 file;
#define CHECK(x) do{if(!(x)){printf("POWER FAIL %d policy=%u cut=%u events=%u: %s\n",__LINE__,policy,cut,events,#x);exit(1);}}while(0)
void mutex_lock(struct mutex *m){(void)m;}void mutex_unlock(struct mutex *m){(void)m;}
u32 ata_sectors(void){return N;}
int ata_read(u32 b,void *p){if(b>=N)return -1;memcpy(p,drive[b],512);return 0;}
static void event(void){events++;if(armed && (events==cut || (capture_commit && ((u32 *)stable[((u32 *)stable[0])[11]])[0]==0x314c4e4au)))__builtin_longjmp(jump,1);}
int ata_write(u32 b,const void *p){
 if(b>=N || fail_write)return -1;
 memcpy(drive[b],p,512);
 /* A legal disk may persist sectors before FLUSH, in any order. */
 if(policy==0 || (policy==2 && ((events*17+b)&3)==0))memcpy(stable[b],p,512);
 event();return 0;
}
int ata_unit_flush(int unit){if(unit || fail_flush)return -1;memcpy(stable,drive,sizeof(stable));event();return 0;}
static void boot(void){bool formatted;block_cache_invalidate();memcpy(drive,stable,sizeof(drive));CHECK(fs_init(&formatted)==0 && !formatted);}
static void reset(void){armed=false;fail_flush=fail_write=false;memcpy(stable,baseline,sizeof(stable));boot();events=0;}
static int operation(unsigned op){
 u32 ino;int r;
 switch(op){
 case 0:r=fs_write(file,80,"NEW CONTENT",11)==11?0:-1;break;
 case 1:r=fs_write(file,7000,"APPEND",6)==6?0:-1;break;
 case 2:r=fs_create(FS_ROOT,"/new-directory",FS_TYPE_DIR,&ino);break;
 case 3:r=fs_rename(FS_ROOT,"/a/child","/b/moved");break;
 case 4:r=fs_remove(FS_ROOT,"/data");break;
 case 5:r=fs_truncate(file);break;
 case 6:r=fs_chmod(file,0600);break;
 case 7:r=fs_symlink(FS_ROOT,"/data","/link");break;
 default:r=-1;break;
 }
 return r?r:fs_sync();
}
static int large_save(const u8 *large){
 int w=fs_write(file,0,large,32768);
 if(w!=32768)return -1;
 return fs_sync();
}
struct snap{int data_err;u32 size,mode;u8 bytes[8192];int a,child,b,moved,newer,link_n;char link[64];};
static void fill_snap(struct snap *s){
 memset(s,0,sizeof *s);u32 ino=0;
 s->data_err=fs_resolve(1,"/data",&ino);
 if(!s->data_err){struct fs_stat st;if(fs_stat(ino,&st))s->data_err=-1;else{
  s->size=st.size;s->mode=st.mode;u32 n=st.size>sizeof s->bytes?sizeof s->bytes:st.size;
  if(n&&fs_read(ino,0,s->bytes,n)!=(int)n)s->data_err=-1;}}
 s->a=fs_resolve(1,"/a",&ino)==0;s->child=fs_resolve(1,"/a/child",&ino)==0;
 s->b=fs_resolve(1,"/b",&ino)==0;s->moved=fs_resolve(1,"/b/moved",&ino)==0;
 s->newer=fs_resolve(1,"/new-directory",&ino)==0;
 s->link_n=fs_readlink(1,"/link",s->link,sizeof s->link);
}
/* Compare complete disk images except the expendable journal payload sectors. */
static bool same(const void *a,const void *b){return !memcmp(a,b,(N-121)*512);}
int main(void){
 bool formatted;CHECK(fs_init(&formatted)==0 && formatted);
 CHECK(fs_create(1,"/data",FS_TYPE_FILE,&file)==0);
 static u8 data[2048];memset(data,0x5a,sizeof(data));CHECK(fs_write(file,0,data,sizeof(data))==sizeof(data));
 CHECK(fs_create(1,"/a",FS_TYPE_DIR,0)==0 && fs_create(1,"/b",FS_TYPE_DIR,0)==0);
 CHECK(fs_create(1,"/a/child",FS_TYPE_DIR,0)==0);
 CHECK(fs_sync()==0);memcpy(baseline,stable,sizeof(stable));
 struct snap base_snap;fill_snap(&base_snap);
 unsigned crashes=0;
 for(unsigned op=0;op<8;op++){
  policy=0;reset();CHECK(operation(op)==0);unsigned count=events;CHECK(fs_sync()==0);memcpy(after,stable,sizeof(after));
  struct snap after_snap;fill_snap(&after_snap);
  for(policy=0;policy<3;policy++)for(cut=1;cut<=count;cut++){
   reset();armed=true;
   if(!__builtin_setjmp(jump)){operation(op);CHECK(false);}
   armed=false;boot();struct snap got;fill_snap(&got);
   CHECK(!memcmp(&got,&base_snap,sizeof got)||!memcmp(&got,&after_snap,sizeof got));
   /* A second mount must leave the repaired filesystem unchanged. */
   static u8 recovered[N][512];memcpy(recovered,stable,sizeof(stable));boot();CHECK(same(recovered,stable));crashes++;
  }
  printf("POWER operation %u: %u sector-write/barrier cuts x 3 cache policies PASS\n",op,count);
 }
 /* Cut power again while replaying a committed log: recovery is restartable. */
 policy=1;reset();cut=~0u;capture_commit=true;armed=true;
 if(!__builtin_setjmp(jump)){operation(0);CHECK(false);}armed=false;capture_commit=false;
 static u8 replay[N][512];memcpy(replay,stable,sizeof(stable));
 events=0;boot();unsigned replay_events=events;memcpy(after,stable,sizeof(stable));
 for(unsigned point=1;point<=replay_events;point++){
  memcpy(stable,replay,sizeof(stable));memcpy(drive,stable,sizeof(drive));block_cache_invalidate();events=0;cut=point;armed=true;
  if(!__builtin_setjmp(jump)){bool f;fs_init(&f);CHECK(false);}armed=false;
  boot();CHECK(same(stable,after));crashes++;
 }
 memcpy(stable,replay,sizeof(stable));u32 log_start=((u32 *)stable[0])[11];stable[log_start+1][5]^=0x20;
 memcpy(drive,stable,sizeof(drive));block_cache_invalidate();bool bad;CHECK(fs_init(&bad)==FS_EIO);
 printf("POWER recovery: %u replay cuts and corrupt journal checksum rejection PASS\n",replay_events);

 /* Multi-transaction saves retain a complete, durable prefix of 16 KiB chunks. */
 static u8 large[32768],readback[32768];memset(large,0xa7,sizeof(large));
 policy=0;reset();CHECK(large_save(large)==0);unsigned large_events=events;
 for(policy=0;policy<3;policy++)for(cut=1;cut<=large_events;cut++){
  reset();armed=true;if(!__builtin_setjmp(jump)){large_save(large);CHECK(false);}armed=false;
  boot();struct fs_stat st;CHECK(fs_stat(file,&st)==0 && (st.size==2048 || st.size==16384 || st.size==32768));
  CHECK(fs_read(file,0,readback,st.size)==(int)st.size);
  CHECK(!memcmp(readback,st.size==2048?data:large,st.size));crashes++;
 }
 printf("POWER large save: %u cuts x 3 cache policies PASS\n",large_events);
 /* Failed FLUSH/sector writes must propagate, and a remount recovers safely. */
 for(unsigned fail=0;fail<2;fail++){
  policy=1;reset();if(fail)fail_write=true;else fail_flush=true;
  CHECK(operation(0)<0);CHECK(fs_sync()==FS_EIO);
  fail_flush=fail_write=false;boot();CHECK(same(stable,baseline));
 }
 /* Bitmap damage is repairable; duplicate ownership is rejected without repair. */
 reset();u32 *super=(u32 *)stable[0];u32 bitmap_sector=super[3];
 stable[bitmap_sector][10]^=0x80;boot();CHECK(same(stable,baseline));
 reset();super=(u32 *)stable[0];u32 inode_start=super[5];u32 ino;
 policy=0;CHECK(fs_create(1,"/duplicate",FS_TYPE_FILE,&ino)==0);CHECK(fs_write(ino,0,"X",1)==1);CHECK(fs_sync()==0);
 u32 *src=(u32 *)(stable[inode_start+file/8]+(file%8)*64);
 u32 *dst=(u32 *)(stable[inode_start+ino/8]+(ino%8)*64);dst[2]=src[2];
 block_cache_invalidate();memcpy(drive,stable,sizeof(drive));bool f;CHECK(fs_init(&f)==FS_EIO);
 /* Open-but-unlinked files left by a crash are reclaimed, restartably. */
 reset();CHECK(operation(4)==0);memcpy(after,stable,sizeof(after));
 reset();myfs_select(0,0);CHECK(myfs_remove_keep(1,"/data",true)==0);CHECK(myfs_sync()==0);
 static u8 orphaned[N][512];memcpy(orphaned,stable,sizeof(stable));events=0;boot();unsigned orphan_events=events;
 CHECK(same(stable,after));
 for(policy=0;policy<3;policy++)for(cut=1;cut<=orphan_events;cut++){
  memcpy(stable,orphaned,sizeof(stable));memcpy(drive,stable,sizeof(drive));block_cache_invalidate();events=0;armed=true;
  if(!__builtin_setjmp(jump)){bool mounted;fs_init(&mounted);CHECK(false);}armed=false;
  boot();CHECK(same(stable,after));crashes++;
 }
 printf("POWER orphans: %u recovery cuts x 3 cache policies PASS\n",orphan_events);
 /* Legacy upgrade must survive losing power before/after publishing v4. */
 reset();CHECK(fs_format_legacy()==0);u32 legacy_inode;
 CHECK(fs_create(1,"/legacy",FS_TYPE_FILE,&legacy_inode)==0 && fs_write(legacy_inode,0,"legacy",6)==6 && fs_sync()==0);
 static u8 legacy[N][512];memcpy(legacy,stable,sizeof(stable));events=0;boot();unsigned upgrade_events=events;
 for(policy=0;policy<3;policy++)for(cut=1;cut<=upgrade_events;cut++){
  memcpy(stable,legacy,sizeof(stable));memcpy(drive,stable,sizeof(drive));block_cache_invalidate();events=0;armed=true;
  if(!__builtin_setjmp(jump)){bool mounted;fs_init(&mounted);CHECK(false);}armed=false;
  boot();u32 found;char contents[6];CHECK(fs_resolve(1,"/legacy",&found)==0 && fs_read(found,0,contents,6)==6 && !memcmp(contents,"legacy",6));crashes++;
 }
 printf("POWER migration: %u upgrade cuts x 3 cache policies PASS\n",upgrade_events);
 /* Existing v4 volumes upgrade before timestamp metadata is modified. */
 reset();((u32 *)stable[0])[1]=4;boot();CHECK(((u32 *)stable[0])[1]==5);
 CHECK(fs_read(file,0,readback,2048)==2048 && !memcmp(readback,data,2048));
 /* Timestamp metadata survives v3 -> v4 migration at its original offset. */
 reset();CHECK(myfs_set_time(file,123456789u)==0 && fs_sync()==0);
 u32 *legacy_super=(u32 *)stable[0];u32 journal_start=legacy_super[11];
 legacy_super[1]=3;legacy_super[11]=legacy_super[13];legacy_super[12]=legacy_super[13]=0;
 for(u32 block=journal_start;block<N;block++)stable[1+block/4096][(block%4096)/8]&=(u8)~(1u<<(block%8));
 boot();struct fs_stat time_stat;CHECK(fs_stat(file,&time_stat)==0 && time_stat.mtime==123456789u);
 boot();CHECK(fs_stat(file,&time_stat)==0 && time_stat.mtime==123456789u);
 printf("TIMESTAMP MIGRATION PASS: legacy timestamps and v4 journal offsets preserved in v5\n");
 /* Return the fixture to a valid disk before atexit flush. */
 reset();
 printf("POWER CUT PASS: %u crashes; bitmap repair, duplicate rejection, I/O errors, idempotent recovery\n",crashes);
 return 0;
}
