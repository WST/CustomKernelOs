/* Persistent DAC and namespace checks against the real MyFS/VFS implementation. */
#include "fs.h"
#include "ata.h"
#include "task.h"
#include "string.h"
int printf(const char*,...);void exit(int);
int host_open_image(const char*);unsigned host_sectors(void);int host_read_sector(unsigned,void*);int host_write_sector(unsigned,const void*);void host_close_image(void);
bool ata_init(void){return true;}u32 ata_sectors(void){return host_sectors();}const char *ata_model(void){return "permission fixture";}
int ata_read(u32 l,void *b){return host_read_sector(l,b);}int ata_write(u32 l,const void *b){return host_write_sector(l,b);}
void mutex_lock(struct mutex *m){(void)m;}void mutex_unlock(struct mutex *m){(void)m;}
static u32 uid,gid,mask=022;
void fs_test_credentials(u32 *u,u32 *g,u32 *m,bool real){(void)real;*u=uid;*g=gid;*m=mask;}
#define CHECK(c) do{if(!(c)){printf("PERMISSIONS FAIL line %d: %s\n",__LINE__,#c);exit(1);}}while(0)
int main(int argc,char **argv) {
 CHECK(argc==2 && !host_open_image(argv[1]));bool formatted;CHECK(!fs_init(&formatted));
 u32 dir,secret,group,world,found;struct fs_stat st;
 if(!formatted){char data[8];CHECK(!fs_resolve(1,"/legacy",&secret));CHECK(fs_read(secret,0,data,8)==8 && !memcmp(data,"legacy!\n",8));CHECK(!fs_chmod(secret,0600));}
 CHECK(!fs_create(1,"/private",FS_TYPE_DIR,&dir));CHECK(!fs_chmod(dir,0700));
 CHECK(!fs_create_mode(1,"/private/secret",FS_TYPE_FILE,0600,&secret));
 CHECK(!fs_create_mode(1,"/group",FS_TYPE_FILE,0640,&group));CHECK(!fs_chown(group,0,11));
 CHECK(!fs_create(1,"/shared",FS_TYPE_DIR,&world));CHECK(!fs_chmod(world,01777));
 CHECK(!fs_create_mode(world,"root",FS_TYPE_FILE,0600,&found));
 uid=11;gid=11;mask=027;
 CHECK(fs_resolve(1,"/private/secret",&found)==FS_EACCES);
 CHECK(fs_resolve(1,"/private/../group",&found)==FS_EACCES);
 CHECK(!fs_access(group,4,false));CHECK(fs_access(group,2,false)==FS_EACCES);
 CHECK(fs_create(1,"/forbidden",FS_TYPE_FILE,&found)==FS_EACCES);
 CHECK(fs_chmod(group,0666)==FS_EPERM);CHECK(fs_chown(group,11,11)==FS_EPERM);
 CHECK(fs_remove(world,"root")==FS_EPERM);CHECK(fs_rename(world,"root","moved")==FS_EPERM);
 CHECK(!fs_create_mode(world,"owned",FS_TYPE_FILE,0666,&found));CHECK(!fs_stat(found,&st));
 CHECK(st.uid==11 && st.gid==11 && st.mode==0640);u32 owned=found;
 CHECK(!fs_chmod(owned,0600));CHECK(!fs_chown(owned,(u32)-1,11));CHECK(fs_chown(owned,0,(u32)-1)==FS_EPERM);
 CHECK(fs_chmod(owned,04755)==FS_EINVAL);
 CHECK(!fs_symlink(world,"/private/secret","link"));CHECK(fs_resolve(world,"link",&found)==FS_EACCES);
 CHECK(!fs_resolve_nofollow(world,"link",&found));CHECK(!fs_stat(found,&st) && st.uid==11 && st.mode==0777);
 CHECK(!fs_remove(world,"link"));CHECK(!fs_symlink(world,"/private/secret","persisted"));
 uid=12;gid=12;CHECK(fs_access(owned,4,false)==FS_EACCES);CHECK(fs_remove(world,"owned")==FS_EPERM);
 uid=0;gid=0;mask=022;CHECK(!fs_rename(world,"owned","kept"));CHECK(!fs_chmod(owned,0640));
 CHECK(!fs_init(&formatted) && !formatted);CHECK(!fs_resolve(1,"/shared/kept",&found));CHECK(!fs_stat(found,&st));CHECK(st.uid==11 && st.gid==11 && st.mode==0640);
 CHECK(!fs_resolve(1,"/private/secret",&secret));CHECK(!fs_stat(secret,&st) && st.mode==0600);
 CHECK(!fs_remove(world,"root"));CHECK(!fs_create_mode(world,"reuse",FS_TYPE_FILE,0644,&found));CHECK(!fs_stat(found,&st) && st.uid==0 && st.mode==0644);
 host_close_image();printf("Persistent owner/group/mode, DAC, sticky directories, symlinks and inode reuse: PASS\n");return 0;
}
