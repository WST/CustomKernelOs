#include "myos.h"
#include "stdio.h"
#include "sys/stat.h"
#include "unistd.h"
static int failures;
#define CHECK(c) do{if(!(c)){printf("PERMISSION FAIL %d: %s errno=%d\n",__LINE__,#c,errno);failures++;}}while(0)
static int make_file(const char *p,unsigned mode,const char *data){int fd=open(p,O_RDWR|O_CREAT|O_EXCL,mode);CHECK(fd>=0);if(fd>=0){CHECK(write(fd,data,strlen(data))==(int)strlen(data));CHECK(close(fd)==0);}return fd;}
static void metadata(const char *p,unsigned mode,unsigned uid,unsigned gid){struct stat st;CHECK(stat(p,&st)==0);CHECK((st.st_mode&07777)==mode && st.st_uid==uid && st.st_gid==gid);}
static void wait_ok(int pid){int status=-1;CHECK(pid>0);if(pid>0){CHECK(waitpid(pid,&status,0)==pid);CHECK(status==0);}}
int main(int argc,char **argv){
 if(argc>1 && !strcmp(argv[1],"orphan")){make_file("/tmp/dac/power-orphan",0600,"orphan");int fd=open("/tmp/dac/power-orphan",O_RDONLY);CHECK(fd>=0);struct stat st;CHECK(fstat(fd,&st)==0);CHECK(unlink("/tmp/dac/power-orphan")==0);printf("ORPHAN_PENDING inode=%u\n",st.st_ino);for(;;)sleep_ms(1000);}
 if(argc>1 && !strcmp(argv[1],"mask")){make_file("/tmp/dac/mask-exec",0666,"mask");metadata("/tmp/dac/mask-exec",0600,1001,1001);return failures?1:0;}
 if(argc>1 && !strcmp(argv[1],"verify")){metadata("/tmp/dac/secret",0600,0,0);metadata("/tmp/dac/owned",0640,1001,1001);metadata("/tmp/dac/mask-exec",0600,1001,1001);puts(failures?"PERMISSIONS REBOOT FAIL":"PERMISSIONS REBOOT PASS");return failures?1:0;}
 puts("DAC root setup");
 CHECK(getuid()==0 && geteuid()==0 && isatty(0));
 int mixed=fork();if(!mixed){int ids[3]={1001,0,0};CHECK(__syscall(SYS_SSHCTL,8,1,(int)ids)==0);CHECK(getuid()==1001 && geteuid()==0);CHECK(access("/root",X_OK)<0 && errno==EACCES);CHECK(chdir("/root")==0);exit(failures?1:0);}wait_ok(mixed);
CHECK(umask(022)==022);
 CHECK(mkdir("/tmp/dac",0777)==0);CHECK(chmod("/tmp/dac",01777)==0);
 make_file("/tmp/dac/secret",0600,"secret");make_file("/tmp/dac/group",0640,"group");CHECK(chown("/tmp/dac/group",0,1001)==0);
 CHECK(mkdir("/tmp/dac/list-only",0744)==0);make_file("/tmp/dac/list-only/entry",0600,"private");
 CHECK(mkdir("/tmp/dac/private",0700)==0);make_file("/tmp/dac/private/secret",0600,"hidden");
 int fd=open("/tmp/dac/zero",O_RDWR|O_CREAT|O_EXCL,0000);CHECK(fd>=0);CHECK(write(fd,"z",1)==1);CHECK(close(fd)==0);CHECK(access("/tmp/dac/zero",X_OK)<0 && errno==EACCES);
 puts("DAC descriptor and user checks");
 int inherited=open("/tmp/dac/secret",O_RDONLY);CHECK(inherited>=0);
 int child=fork();if(child==0){
  puts("DAC child identity");
  CHECK(setgid(1001)==0 && setuid(1001)==0);CHECK(getuid()==1001 && geteuid()==1001 && getgid()==1001);CHECK(setuid(0)<0 && errno==EPERM);
  char data[8];CHECK(read(inherited,data,6)==6 && !memcmp(data,"secret",6));close(inherited);
  CHECK(open("/tmp/dac/secret",O_RDONLY)<0 && errno==EACCES);
  CHECK(open("/tmp/dac/secret",O_WRITE)<0 && errno==EACCES);
  CHECK(chmod("/tmp/dac/secret",0666)<0 && errno==EPERM);CHECK(chown("/tmp/dac/secret",1001,1001)<0 && errno==EPERM);
  CHECK(unlink("/tmp/dac/secret")==-EPERM);CHECK(rename("/tmp/dac/secret","/tmp/dac/stolen")<0);
  CHECK(mkdir("/root/denied",0700)<0 && errno==EACCES);
  CHECK(chdir("/tmp/dac/private")<0);struct stat denied;CHECK(stat("/tmp/dac/private/../secret",&denied)<0 && errno==EACCES);
  CHECK(access("/tmp/dac/group",R_OK)==0 && access("/tmp/dac/group",W_OK)<0);fd=open("/tmp/dac/group",O_RDONLY);CHECK(fd>=0);if(fd>=0)close(fd);
  CHECK(symlink("/tmp/dac/secret","/tmp/dac/link")==0);CHECK(open("/tmp/dac/link",O_RDONLY)<0 && errno==EACCES);CHECK(chmod("/tmp/dac/link",0666)<0);CHECK(unlink("/tmp/dac/link")==0);
  struct dirent_info listing;CHECK(dirread("/tmp/dac/list-only",0,&listing)==1 && !strcmp(listing.name,"entry") && listing.type==0 && listing.size==0);
  struct stat hidden;CHECK(stat("/tmp/dac/list-only/entry",&hidden)<0 && errno==EACCES);
  puts("DAC child create");
  umask(027);fd=open("/tmp/dac/owned",O_RDWR|O_CREAT|O_EXCL,0666);CHECK(fd>=0);metadata("/tmp/dac/owned",0640,1001,1001);
  CHECK(fchmod(fd,0600)==0);struct stat st;CHECK(fstat(fd,&st)==0 && (st.st_mode&0777)==0600 && st.st_uid==1001);
  CHECK(fchown(fd,0,0)<0 && errno==EPERM);CHECK(fchmod(fd,0640)==0);CHECK(close(fd)==0);
  puts("DAC child exec");
  umask(077);int p=fork();if(!p){char *av[]={"permissions-check","mask",NULL};exec("/bin/permissions-check",av);exit(99);}wait_ok(p);
  CHECK(open("/dev/sdb",O_RDONLY)<0);fd=open("/dev/null",O_WRONLY);CHECK(fd>=0 && !isatty(fd));if(fd>=0)close(fd);
  exit(failures?1:0);
 }wait_ok(child);close(inherited);
 metadata("/tmp/dac/secret",0600,0,0);struct stat st;CHECK(stat("/tmp/dac/secret",&st)==0 && st.st_size==6);
 puts("DAC unlink lifetime");
 /* An unlinked open inode must not become a capability to a newly created secret. */
 fd=open("/tmp/dac/unlinked",O_RDWR|O_CREAT|O_EXCL,0600);CHECK(fd>=0);CHECK(write(fd,"old",3)==3);CHECK(lseek(fd,0,SEEK_SET)==0);CHECK(unlink("/tmp/dac/unlinked")==0);
 CHECK(fstat(fd,&st)==0 && st.st_nlink==0);make_file("/tmp/dac/new-secret",0600,"new");char old[4];CHECK(read(fd,old,3)==3 && !memcmp(old,"old",3));close(fd);
 make_file("/tmp/dac/reclaim",0600,"ok");
 /* Execute permission is enforced even for root, before parsing ELF. */
 make_file("/tmp/dac/noexec",0644,"plain");char *av[]={"noexec",NULL};CHECK(exec("/tmp/dac/noexec",av)==-EACCES);
 puts(failures?"PERMISSIONS ABI FAIL":"PERMISSIONS ABI PASS");return failures?1:0;
}
