/* Тесты MyFS на хосте: fs.c собирается обычным gcc с «диском» в памяти.  Запуск: make test */
#include "fs.h"
#include "ata.h"
#include "string.h"
int printf(const char*,...); void *malloc(unsigned long); void exit(int);
#define SECTORS 2600     /* маленький диск, чтобы проверить ENOSPC */
static u8 disk[SECTORS*512];
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
  bool f; CHECK(fs_init(&f)==0 && f);
  struct fs_info fi0; fs_info(&fi0);
  u32 ino; CHECK(fs_create(1,"/big",FS_TYPE_FILE,&ino)==0);
  for(int i=0;i<BIGN;i++) big[i]=(u8)(i*7+i/512);
  int n=BIGN;                                  /* 1 МБ: прямые + косвенный + двойной косвенный */
  CHECK(fs_write(ino,0,big,n)==n);
  CHECK(fs_write(ino,(12+128+128*128)*512,big,1)==FS_EFBIG);
  CHECK(fs_read(ino,0,rd,BIGN)==n && !memcmp(big,rd,n));
  CHECK(fs_read(ino,1000,rd,10)==10 && !memcmp(big+1000,rd,10));
  CHECK(fs_read(ino,900000,rd,10)==10 && !memcmp(big+900000,rd,10));
  int data=(n+511)/512, mids=(data-140+127)/128;
  struct fs_info fi; fs_info(&fi); CHECK(fi0.free_blocks-fi.free_blocks==(u32)(data+1+1+mids));
  struct fs_stat st; CHECK(fs_stat(ino,&st)==0 && st.size==(u32)n && st.blocks==(u32)(data+2+mids));
  /* remount: всё на месте */
  CHECK(fs_init(&f)==0 && !f);
  CHECK(fs_resolve(1,"big",&ino)==0 && fs_read(ino,0,rd,n)==n && !memcmp(big,rd,n));
  struct fs_info fi2; fs_info(&fi2); CHECK(fi2.free_blocks==fi.free_blocks);
  CHECK(fs_remove(1,"/big")==0); fs_info(&fi); CHECK(fi.free_blocks==fi0.free_blocks);
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
  u32 pos=0,cnt=0; struct fs_dirent de; while(fs_readdir(1,&pos,&de)==1) cnt++; CHECK(cnt==102);
  CHECK(fs_resolve(1,"n77",&up)==0);
  CHECK(lock_depth == 0 && lock_calls > 0);
  printf("fs mutex: %d lock calls, all balanced, max nesting %d\n", lock_calls, max_depth);
  printf("ALL TESTS PASSED\n"); return 0; }
