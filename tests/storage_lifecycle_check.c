#include "myos.h"
#include <stdio.h>
#define CHECK(x) do{if(!(x)){dprintf(STDERR,"LIFECYCLE FAIL line %d: %s\n",__LINE__,#x);return 1;}}while(0)
int main(void) {
    char dev[128],target[128];
    CHECK(mountinfo(1,dev,target)==1 && !strcmp(dev,"/dev/sdc1") && !strcmp(target,"/mnt"));
    CHECK(umount("/")==-EBUSY);
    int fd=open("/mnt/file",O_WRITE|O_CREAT|O_TRUNC);CHECK(fd>=0);
    CHECK(write(fd,"persistent",10)==10 && fsync(fd)==0);
    CHECK(umount("/mnt")==-EBUSY && close(fd)==0);
    int old=open("/mnt/victim",O_WRITE,0600);CHECK(old>=0 && write(old,"old",3)==3 && close(old)==0);
    old=open("/mnt/victim",O_READ);CHECK(old>=0);
    int replacement=open("/mnt/replacement",O_WRITE,0600);CHECK(replacement>=0 && write(replacement,"new",3)==3 && close(replacement)==0);
    CHECK(rename("/mnt/replacement","/mnt/victim")==0);
    char contents[3];CHECK(read(old,contents,3)==3 && !memcmp(contents,"old",3));
    replacement=open("/mnt/victim",O_READ);CHECK(replacement>=0 && read(replacement,contents,3)==3 && !memcmp(contents,"new",3));
    CHECK(close(replacement)==0 && close(old)==0);
    CHECK(chdir("/mnt")==0 && umount("/mnt")==-EBUSY && chdir("/")==0);
    int pipefd[2];CHECK(pipe(pipefd)==0);int pid=fork();CHECK(pid>=0);
    if(!pid) {
        close(pipefd[1]);chdir("/mnt");char ready;
        /* Parent waits a little before checking the child's cwd. */
        read(pipefd[0],&ready,1);close(pipefd[0]);exit(0);
    }
    close(pipefd[0]);sleep_ms(100);
    CHECK(umount("/mnt")==-EBUSY);CHECK(write(pipefd[1],"x",1)==1);close(pipefd[1]);waitpid(pid,0,0);
    CHECK(mkdir("/mnt/inner",0755)==0);
    CHECK(mount("/dev/sdc2","/mnt/inner")==0 && umount("/mnt")==-EBUSY);
    CHECK(umount("/mnt/inner")==0 && umount("/dev/sdc1")==0);
    CHECK(mountinfo(1,dev,target)==0);
    CHECK(mount("/dev/sdc1","/mnt")==0);
    fd=open("/mnt/file",O_READ);char data[10];CHECK(fd>=0 && read(fd,data,10)==10 && !memcmp(data,"persistent",10));close(fd);
    CHECK(umount("/mnt")==0 && fsck("/dev/sdc1")==0);
    CHECK(mount("/dev/sdc1","/mnt")==0 && sync()==0);
    puts("STORAGE LIFECYCLE PASS: fstab, fsync, descriptors, process cwd, nested mounts, device umount, remount and fsck");
    return 0;
}
