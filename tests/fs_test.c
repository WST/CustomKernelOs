/* Тесты MyFS на хосте: fs.c собирается обычным gcc с «диском» в памяти.  Запуск: make test */
#include "fs.h"
#include "ata.h"
#include "myfs.h"
#include "block.h"
#include "string.h"
int printf(const char*,...); void *malloc(unsigned long); void exit(int);
#define SECTORS 2600     /* маленький диск, чтобы проверить ENOSPC */
static u8 disk[SECTORS*512], snapshot[SECTORS*512];
bool ata_init(void){return 1;} u32 ata_sectors(void){return SECTORS;} const char*ata_model(void){return "fake";}
int ata_read(u32 l,void*b){if(l>=SECTORS)return -1;memcpy(b,disk+l*512,512);return 0;}
int ata_write(u32 l,const void*b){if(l>=SECTORS)return -1;memcpy(disk+l*512,b,512);return 0;}
/* заглушки мьютекса: проверяем, что каждый lock парный с unlock */
#include "task.h"
static int lock_depth, max_depth, lock_calls;
void mutex_lock(struct mutex *m)   { (void)m; lock_calls++; if (++lock_depth > max_depth) max_depth = lock_depth; }
void mutex_unlock(struct mutex *m) { (void)m; lock_depth--; }
#define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c);exit(1);} }while(0)
#define BIGN 1000000
static u8 big[BIGN], rd[BIGN];
int main(){
  /* Administrative APIs work before mounting; edits validate before writing. */
  CHECK(block_init()==0);
  CHECK(fs_partition("sdb",0,0,0,0)==0);
  CHECK(fs_partition("sdb",1,4,2048,400)==0 && block_find("sdb4")==4);
  memcpy(snapshot,disk,sizeof(disk));
  CHECK(fs_partition("sdb",1,1,2050,300)==FS_EINVAL && !memcmp(snapshot,disk,sizeof(disk)));
  CHECK(fs_partition("sdb",1,4,2048,400)==FS_EEXIST);
  CHECK(fs_partition("sdb",1,1,2500,400)==FS_EINVAL);
  CHECK(fs_partition("sdb",1,1,0,400)==FS_EINVAL);
  CHECK(fs_mkfs("sdb")==FS_EINVAL); /* table may not be overwritten by mkfs */
  CHECK(fs_mkfs("sdb4")==0);
  CHECK(fs_partition("sdb",2,4,0,0)==0 && block_find("sdb4")==-1);
  memset(disk,0,sizeof(disk)); block_cache_invalidate();
  bool f; CHECK(fs_init(&f)==0 && f);
  CHECK(fs_mkfs("sdb")==FS_EBUSY && fs_partition("sdb",0,0,0,0)==FS_EBUSY);
  struct fs_info fi0; fs_info(&fi0);
  u32 ino; CHECK(fs_create(1,"/big",FS_TYPE_FILE,&ino)==0);
  for(int i=0;i<BIGN;i++) big[i]=(u8)(i*7+i/512);
  int n=BIGN;                                  /* 1 МБ: прямые + косвенный + двойной косвенный */
  CHECK(fs_write(ino,0,big,n)==n);
  CHECK(fs_read(ino,0,rd,BIGN)==n && !memcmp(big,rd,n));
  CHECK(fs_read(ino,1000,rd,10)==10 && !memcmp(big+1000,rd,10));
  CHECK(fs_read(ino,900000,rd,10)==10 && !memcmp(big+900000,rd,10));
  int data=(n+511)/512, mids=(data-140+127)/128;
  struct fs_info fi; fs_info(&fi); CHECK(fi0.free_blocks-fi.free_blocks==(u32)(data+1+1+mids));
  struct fs_stat st; CHECK(fs_stat(ino,&st)==0 && st.size==(u32)n && st.blocks==(u32)(data+2+mids));
  /* Байт за прежним пределом ~8 МиБ лежит в тройном косвенном блоке. */
  u32 past=(12u+128u+128u*128u)*512u; char mark='Q';
  CHECK(fs_write(ino,past,&mark,1)==1);
  CHECK(fs_read(ino,past,rd,1)==1 && rd[0]=='Q');
  CHECK(fs_read(ino,2u*1024u*1024u,rd,1)==1 && rd[0]==0);
  CHECK(fs_write(ino,(11u+128u+128u*128u+128u*128u*128u)*512u,&mark,1)==FS_EFBIG);
  fs_info(&fi);
  /* remount: всё на месте */
  CHECK(fs_init(&f)==0 && !f);
  CHECK(fs_resolve(1,"big",&ino)==0 && fs_read(ino,0,rd,n)==n && !memcmp(big,rd,n));
  CHECK(fs_read(ino,past,rd,1)==1 && rd[0]=='Q');
  struct fs_info fi2; fs_info(&fi2); CHECK(fi2.free_blocks==fi.free_blocks);
  CHECK(fs_remove(1,"/big")==0); fs_info(&fi); CHECK(fi.free_blocks==fi0.free_blocks);
  /* Меньше секторных операций: один большой write, много созданий, куски по 512. */
  fs_format(); block_io_reset();
  {
    enum { BULK=256*1024 };
    static u8 bulk[BULK], back[BULK];
    for (int i=0;i<BULK;i++) bulk[i]=(u8)(i*3+1);
    CHECK(fs_create(1,"/bulk",FS_TYPE_FILE,&ino)==0);
    CHECK(fs_write(ino,0,bulk,BULK)==BULK);
    unsigned rd_n, wr_n; block_io_stats(&rd_n,&wr_n);
    printf("io bulk %u bytes: reads=%u writes=%u\n",BULK,rd_n,wr_n);
    CHECK(wr_n>=BULK/512 && wr_n<BULK/512+80);
    CHECK(rd_n<80);
    CHECK(fs_read(ino,0,back,BULK)==BULK && !memcmp(bulk,back,BULK));
    CHECK(fs_read(ino,100,back,3)==3 && !memcmp(bulk+100,back,3));
    char tail[8]; memset(tail,0x5a,sizeof tail);
    CHECK(fs_write(ino,10,tail,2)==2);
    CHECK(fs_read(ino,0,back,16)==16 && back[9]==bulk[9] && back[10]==0x5a && back[12]==bulk[12]);
    CHECK(fs_remove(1,"/bulk")==0);
    block_io_reset();
    for (int i=0;i<200;i++) {
      char nm[16]; nm[0]='p'; nm[1]='0'+i/100; nm[2]='0'+(i/10)%10; nm[3]='0'+i%10; nm[4]=0;
      CHECK(fs_create(1,nm,FS_TYPE_FILE,0)==0);
    }
    block_io_stats(&rd_n,&wr_n);
    printf("io 200 creates: reads=%u writes=%u\n",rd_n,wr_n);
    CHECK(rd_n<6000);
    block_io_reset();
    CHECK(fs_create(1,"/chunks",FS_TYPE_FILE,&ino)==0);
    for (int i=0;i<128;i++) CHECK(fs_write(ino,i*512,bulk+i*512,512)==512);
    block_io_stats(&rd_n,&wr_n);
    printf("io 128x512: reads=%u writes=%u\n",rd_n,wr_n);
    CHECK(rd_n<300 && wr_n<700);
    CHECK(fs_read(ino,0,back,128*512)==128*512 && !memcmp(bulk,back,128*512));
    fs_format();
  }
  /* заполнение диска -> ENOSPC, затем освобождение */
  int k=0, r; char name[16];
  for(;;){ memcpy(name,"f00",4); name[1]='a'+k/10; name[2]='0'+k%10;
     if(fs_create(1,name,FS_TYPE_FILE,&ino)) break;
     r=fs_write(ino,0,big,60000); k++; if(r<0){ CHECK(r==FS_ENOSPC); break; } }
  printf("filled disk with %d files, last write -> %s\n",k,fs_strerror(r));
  fs_info(&fi); CHECK(fi.free_blocks==0);
  /* вложенные каталоги, rename, getpath */
  fs_format();
  CHECK(fs_create(1,"a",FS_TYPE_DIR,0)==0 && fs_create(1,"a/b",FS_TYPE_DIR,0)==0 && fs_create(1,"a/b/c",FS_TYPE_DIR,&ino)==0);
  char p[128]; CHECK(fs_getpath(ino,p,128)==0 && !strcmp(p,"/a/b/c"));
  CHECK(fs_rename(1,"a","a/b/c")==FS_EINVAL);
  CHECK(fs_rename(1,"a/b","/")==0); CHECK(fs_getpath(ino,p,128)==0 && !strcmp(p,"/b/c"));
  u32 up; CHECK(fs_resolve(ino,"../..",&up)==0 && up==1);
  CHECK(fs_create(1,"b/c/",FS_TYPE_DIR,0)==FS_EEXIST);
  CHECK(fs_remove(1,"b")==FS_ENOTEMPTY);
  CHECK(fs_resolve(1,"b/c/x",&up)==FS_ENOENT);
  CHECK(fs_create(1,"b/c/x.txt",FS_TYPE_FILE,&up)==0 && fs_resolve(1,"b/c/x.txt/y",&up)==FS_ENOTDIR);
  /* 100 файлов в одном каталоге (каталог > 1 блока) */
  for(int i=0;i<100;i++){ name[0]='n'; name[1]='0'+i/10; name[2]='0'+i%10; name[3]=0; CHECK(fs_create(1,name,FS_TYPE_FILE,0)==0);}
  u32 pos=0,cnt=0; struct fs_dirent de; while(fs_readdir(1,&pos,&de)==1) cnt++; CHECK(cnt==103);
  CHECK(fs_resolve(1,"n77",&up)==0);
  /* Версия 2: имя длиннее 27 символов и путь длиннее 128. */
  {
    char longn[41], too[257], comp[181], nested[200], got[256];
    u32 dir;
    memset(longn, 'a', 40); longn[40] = 0;
    CHECK(fs_create(1, longn, FS_TYPE_FILE, &up)==0);
    CHECK(fs_resolve(1, longn, &up)==0);
    pos = 0; cnt = 0;
    while (fs_readdir(1, &pos, &de)==1) if (!strcmp(de.name, longn)) cnt++;
    CHECK(cnt==1);
    CHECK(fs_remove(1, longn)==0);
    CHECK(fs_resolve(1, longn, &up)==FS_ENOENT);
    CHECK(fs_create(1, longn, FS_TYPE_FILE, &up)==0);
    CHECK(fs_resolve(1, longn, &up)==0);
    CHECK(fs_remove(1, longn)==0);
    memset(too, 'b', 256); too[256] = 0;
    CHECK(fs_create(1, too, FS_TYPE_FILE, 0)==FS_ENAMETOOLONG);
    memset(comp, 'c', 180); comp[180] = 0;
    CHECK(fs_create(1, comp, FS_TYPE_DIR, &dir)==0);
    CHECK(fs_getpath(dir, got, sizeof got)==0 && strlen(got)==181);
    memcpy(nested, comp, 180);
    memcpy(nested + 180, "/z", 3);
    CHECK(strlen(nested) > 128);
    CHECK(fs_create(1, nested, FS_TYPE_FILE, &up)==0);
    CHECK(fs_init(&f)==0 && !f);
    CHECK(fs_resolve(1, nested, &up)==0);
  }
  /* Символическая ссылка: цель в теле файла, последний компонент раскрывается
   * при поиске и не раскрывается при unlink, rename и readlink. */
  {
    char tbuf[16], prev[8];
    u32 hello, a;
    CHECK(fs_create(1, "hello", FS_TYPE_FILE, &hello)==0);
    CHECK(fs_write(hello, 0, "hi", 2)==2);
    CHECK(fs_symlink(1, "", "empty")==FS_EINVAL);
    CHECK(fs_symlink(1, "hello", "alias")==0);
    CHECK(fs_readlink(1, "alias", tbuf, sizeof tbuf)==5 && !memcmp(tbuf, "hello", 5));
    CHECK(fs_readlink(1, "hello", tbuf, sizeof tbuf)==FS_EINVAL);
    CHECK(fs_resolve(1, "alias", &a)==0 && a==hello && fs_read(a, 0, rd, 2)==2 && !memcmp(rd, "hi", 2));
    CHECK(fs_resolve_nofollow(1, "alias", &a)==0 && fs_stat(a, &st)==0 && st.type==FS_TYPE_LINK && st.size==5);
    CHECK(fs_symlink(1, "alias", "again")==0);
    CHECK(fs_readlink(1, "again", tbuf, sizeof tbuf)==5 && !memcmp(tbuf, "alias", 5));
    CHECK(fs_resolve(1, "again", &a)==0 && a==hello);
    CHECK(fs_symlink(1, "../hello", "b/up")==0 && fs_resolve(1, "b/up", &a)==0 && a==hello);
    CHECK(fs_symlink(1, "/hello", "abs")==0 && fs_resolve(1, "abs", &a)==0 && a==hello);
    CHECK(fs_resolve(1, "alias/", &a)==FS_ENOTDIR);
    CHECK(fs_symlink(1, "b", "blink")==0);
    CHECK(fs_rename(1, "hello", "blink")==FS_EEXIST && fs_resolve(1, "hello", &a)==0 && a==hello);
    CHECK(fs_create(1, "blink/via", FS_TYPE_FILE, &a)==0 && fs_resolve(1, "b/via", &a)==0);
    CHECK(fs_rename(1, "alias", "alias2")==0);
    CHECK(fs_readlink(1, "alias2", tbuf, sizeof tbuf)==5 && fs_resolve(1, "alias", &a)==FS_ENOENT);
    CHECK(fs_remove(1, "alias2")==0 && fs_read(hello, 0, rd, 2)==2);
    CHECK(fs_symlink(1, "missing", "dangling")==0);
    CHECK(fs_resolve(1, "dangling", &a)==FS_ENOENT && fs_readlink(1, "dangling", tbuf, sizeof tbuf)==7);
    CHECK(fs_create(1, "dangling", FS_TYPE_FILE, &a)==FS_EEXIST);
    CHECK(fs_remove(1, "dangling")==0 && fs_resolve(1, "hello", &a)==0);
    CHECK(fs_symlink(1, "loop", "loop")==0 && fs_resolve(1, "loop", &a)==FS_ELOOP);
    CHECK(fs_symlink(1, "c2", "c1")==0 && fs_symlink(1, "c1", "c2")==0 && fs_resolve(1, "c1", &a)==FS_ELOOP);
    memcpy(prev, "t00", 4);
    CHECK(fs_create(1, prev, FS_TYPE_FILE, &a)==0);
    for (int i = 1; i <= 41; i++) {
      char name[8];
      name[0] = 't'; name[1] = '0' + i / 10; name[2] = '0' + i % 10; name[3] = 0;
      CHECK(fs_symlink(1, prev, name)==0);
      memcpy(prev, name, 4);
    }
    CHECK(fs_resolve(1, "t40", &a)==0);
    CHECK(fs_resolve(1, "t41", &a)==FS_ELOOP);
    char bigtarget[FS_PATH_MAX + 1];
    memset(bigtarget, 't', FS_PATH_MAX); bigtarget[FS_PATH_MAX] = 0;
    CHECK(fs_symlink(1, bigtarget, "toolong")==FS_ENAMETOOLONG);
  }
  /* Версия 1 сохраняет записи по 32 байта и предел в 27 символов. */
  CHECK(fs_format_legacy()==0);
  {
    char n27[28], n28[29];
    memset(n27, 'n', 27); n27[27] = 0;
    memset(n28, 'm', 28); n28[28] = 0;
    CHECK(fs_create(1, "ok", FS_TYPE_FILE, &up)==0);
    u32 ok = up;
    CHECK(fs_symlink(1, "ok", "oklink")==0);
    char linkgot[8];
    CHECK(fs_readlink(1, "oklink", linkgot, sizeof linkgot)==2 && !memcmp(linkgot, "ok", 2));
    CHECK(fs_resolve(1, "oklink", &up)==0 && up==ok);
    CHECK(fs_resolve_nofollow(1, "oklink", &up)==0 && fs_stat(up, &st)==0 && st.type==FS_TYPE_LINK);
    CHECK(fs_create(1, n27, FS_TYPE_FILE, &up)==0);
    CHECK(fs_create(1, n28, FS_TYPE_FILE, 0)==FS_ENAMETOOLONG);
    CHECK(fs_create(1, "bigv1", FS_TYPE_FILE, &up)==0);
    CHECK(fs_write(up, (12u+128u+128u*128u)*512u, "Z", 1)==FS_EFBIG);
    CHECK(fs_init(&f)==0 && !f);
    CHECK(fs_resolve(1, n27, &up)==0);
    CHECK(fs_remove(1, "ok")==0);
    CHECK(fs_resolve(1, "ok", &up)==FS_ENOENT);
  }
  CHECK(lock_depth == 0 && lock_calls > 0);
  printf("fs mutex: %d lock calls, all balanced, max nesting %d\n", lock_calls, max_depth);
  /* Virtual devices: byte reads, EOF and read-only raw access. */
  u32 dev,zero,raw; CHECK(fs_resolve(1,"/dev",&dev)==0);
  CHECK(fs_resolve(dev,"../dev/./zero",&zero)==0);
  memset(rd,0xff,100); CHECK(fs_read(zero,0,rd,100)==100);
  for(int i=0;i<100;i++) CHECK(rd[i]==0);
  CHECK(fs_resolve(dev,"null",&ino)==0 && fs_read(ino,0,rd,10)==0 && fs_write(ino,0,"x",1)==1);
  CHECK(fs_resolve(1,"/dev/sdb",&raw)==0 && fs_stat(raw,&st)==0 && st.type==FS_TYPE_BLOCK);
  CHECK(fs_read(raw,509,rd,20)==20 && !memcmp(rd,disk+509,20));
  CHECK(fs_write(raw,0,"x",1)==FS_EROFS && fs_truncate(raw)==FS_EROFS);
  CHECK(fs_create(dev,"fake",FS_TYPE_FILE,0)==FS_EROFS);
  CHECK(fs_remove(1,"/dev")==FS_EBUSY);
  CHECK(fs_getpath(dev,p,sizeof(p))==0 && !strcmp(p,"/dev"));
  /* Unknown/corrupt metadata must never be reformatted. */
  disk[0]^=1; block_cache_invalidate(); memcpy(snapshot,disk,sizeof(disk));
  CHECK(fs_init(&f)<0 && !memcmp(snapshot,disk,sizeof(disk))); disk[0]^=1; block_cache_invalidate();
  disk[20]=0xff; block_cache_invalidate(); memcpy(snapshot,disk,sizeof(disk));
  CHECK(fs_init(&f)<0 && !memcmp(snapshot,disk,sizeof(disk)));
  /* Two primary MBR partitions with independent MyFS instances. */
  memset(disk,0,sizeof(disk)); block_cache_invalidate(); disk[510]=0x55; disk[511]=0xaa;
  for(int j=0;j<2;j++) {
    u8 *e=disk+446+j*16; e[4]=0x83;
    u32 start=1+j*1200, size=1200;
    for(int b=0;b<4;b++) { e[8+b]=start>>(b*8); e[12+b]=size>>(b*8); }
  }
  CHECK(block_init()==0 && block_count()==3);
  CHECK(block_read(1,1200,rd)<0 && block_write(2,1200,rd)<0);
  CHECK(fs_init(&f)<0); /* unformatted partitions are not formatted implicitly */
  CHECK(fs_mkfs("sdb1")==0 && fs_mkfs("sdb2")==0);
  CHECK(fs_init(&f)==0 && !f);
  CHECK(fs_create(1,"mnt",FS_TYPE_DIR,0)==0);
  CHECK(fs_mount(1,"/dev/sdb2","/mnt")==0);
  CHECK(fs_mkfs("sdb2")==FS_EBUSY);
  CHECK(fs_partition("sdb",2,2,0,0)==FS_EBUSY);
  CHECK(fs_resolve(1,"/mnt",&ino)==0 && ino!=1);
  CHECK(fs_getpath(ino,p,sizeof(p))==0 && !strcmp(p,"/mnt"));
  CHECK(fs_resolve(ino,"..",&up)==0 && up==1);
  CHECK(fs_create(ino,"hello",FS_TYPE_FILE,&up)==0 && fs_write(up,0,"partition",9)==9);
  CHECK(fs_create(1,"hello",FS_TYPE_FILE,&raw)==0 && fs_write(raw,0,"root",4)==4);
  CHECK(fs_read(up,0,rd,9)==9 && !memcmp(rd,"partition",9));
  CHECK(fs_read(raw,0,rd,4)==4 && !memcmp(rd,"root",4));
  CHECK(fs_rename(1,"hello","/mnt/other")==FS_EXDEV);
  CHECK(fs_rename(1,"mnt","moved")==FS_EBUSY && fs_remove(1,"mnt")==FS_EBUSY);
  CHECK(fs_format()==FS_EBUSY);
  CHECK(fs_create(1,"other",FS_TYPE_DIR,0)==0);
  CHECK(fs_mount(1,"/dev/sdb","/other")==FS_EBUSY);
  CHECK(fs_mount(1,"/dev/sdb2","/other")==FS_EBUSY);
  char device[128],target[128];
  CHECK(fs_mount_info(1,device,target)==1 && !strcmp(device,"/dev/sdb2") && !strcmp(target,"/mnt"));
  CHECK(fs_init(&f)==0 && fs_mount(1,"sdb2","/mnt")==0);
  CHECK(fs_resolve(1,"/mnt/hello",&ino)==0 && fs_read(ino,0,rd,9)==9 && !memcmp(rd,"partition",9));
  /* Invalid and overlapping partition extents are skipped. */
  disk[446+16+8]=1; disk[446+16+9]=0; block_cache_invalidate();
  CHECK(block_init()==0 && block_count()==2);
  CHECK(lock_depth==0);
  printf("ALL TESTS PASSED (VFS, devfs, MBR, two volumes, corruption)\n"); return 0; }


