/* Private per-user localStorage snapshots, atomically replaced on commit.
 * This file also compiles against Linux libc for filesystem regression tests. */
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <pwd.h>

static int profile_path(const char *origin,char *out,int max) {
    if(strlen(origin)>=320)return -1;
    const char *home=getenv("HOME");
    if(!home || !*home){struct passwd *user=getpwuid(getuid());home=user?user->pw_dir:NULL;}
    if(!home || !*home)return 0;
    if(home[0]!='/' || strlen(home)>700)return -1;
    char directory[800];snprintf(directory,sizeof(directory),"%s/.myos-browser",home);
    struct stat st;
    if(lstat(directory,&st)<0) {
        if(errno!=ENOENT || mkdir(directory,0700)<0 || lstat(directory,&st)<0)return -1;
    }
    if(!S_ISDIR(st.st_mode) || st.st_uid!=getuid() || (st.st_mode&077))return -1;
    unsigned char digest[32];sha256(origin,strlen(origin),digest);char name[65];
    for(int i=0;i<32;i++)snprintf(name+2*i,3,"%02x",digest[i]);
    if(snprintf(out,max,"%s/%s.local",directory,name)>=max)return -1;
    return 1;
}
static int profile_read_all(int fd,char *buf,int len) {
    int done=0;while(done<len){int n=read(fd,buf+done,len-done);if(n<=0)return -1;done+=n;}return 0;
}
static int profile_write_all(int fd,const char *buf,int len) {
    int done=0;while(done<len){int n=write(fd,buf+done,len-done);if(n<=0)return -1;done+=n;}return 0;
}
static int profile_read(const char *origin,char *out,int max) {
    if(max<1 || max>32768)return -1;
    char path[900];int available=profile_path(origin,path,sizeof(path));if(available<=0)return available;
    struct stat before,after;if(lstat(path,&before)<0)return errno==ENOENT?0:-1;
    if(!S_ISREG(before.st_mode) || before.st_uid!=getuid() || (before.st_mode&077))return -1;
    int fd=open(path,O_RDONLY);if(fd<0)return -1;
    int prefix_len=8+strlen(origin)+1,length=-1;char prefix[330];
    if(fstat(fd,&after)==0 && S_ISREG(after.st_mode) && after.st_uid==getuid() && !(after.st_mode&077) &&
       after.st_dev==before.st_dev && after.st_ino==before.st_ino && after.st_size>=(unsigned)prefix_len &&
       after.st_size<(unsigned)(prefix_len+max) && profile_read_all(fd,prefix,prefix_len)==0 &&
       !memcmp(prefix,"MYOSST1\n",8) && !memcmp(prefix+8,origin,strlen(origin)) && prefix[prefix_len-1]=='\n') {
        length=after.st_size-prefix_len;
        if(profile_read_all(fd,out,length)<0)length=-1;else out[length]=0;
    }
    close(fd);return length;
}
static int profile_write(const char *origin,const char *data,int len) {
    if(len<0 || len>=32768)return -1;
    char path[900],temp[950];int available=profile_path(origin,path,sizeof(path));if(available<=0)return available;
    static unsigned sequence;
    snprintf(temp,sizeof(temp),"%s.%d.%u.tmp",path,getpid(),++sequence);
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd<0)return -1;
    char prefix[330];int n=snprintf(prefix,sizeof(prefix),"MYOSST1\n%s\n",origin);
    int result=profile_write_all(fd,prefix,n);if(!result)result=profile_write_all(fd,data,len);
    if(close(fd)<0)result=-1;
    if(!result && rename(temp,path)<0)result=-1;
    if(result)unlink(temp);
    return result;
}
