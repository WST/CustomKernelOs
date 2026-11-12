#include "myos.h"
#include "sys/stat.h"
#define CHECK(x) do { if(!(x)) { printf("SPAWN FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
static int child(int argc,char **argv) {
    CHECK(argc==7 && !strcmp(argv[1],"child"));
    CHECK(getppid()==atoi(argv[2]));
    char name[24]={0},cwd[128]={0},byte;
    CHECK(account(5,name,NULL)==0 && !strcmp(name,argv[3]));
    CHECK(getcwd(cwd,sizeof(cwd))==cwd && !strcmp(cwd,argv[4]));
    CHECK(read(0,&byte,1)==0); /* stdin was replaced with /dev/null */
    CHECK(!strcmp(argv[6],"argument with spaces"));
    CHECK(write(atoi(argv[5]),"ok",2)==2); /* inherited descriptor */
    return 23;
}
static int exercise(void) {
    char name[24]={0},cwd[128]={0},parent[24],out[24],reply[3]={0};
    CHECK(account(5,name,NULL)==0 && getcwd(cwd,sizeof(cwd))==cwd);
    int fds[2]; CHECK(pipe(fds)==0);
    int input=open("/dev/null",O_READ); CHECK(input>=0);
    snprintf(parent,sizeof(parent),"%d",getpid());
    snprintf(out,sizeof(out),"%d",fds[1]);
    char *args[]={"spawncheck","child",parent,name,cwd,out,"argument with spaces",NULL};
    int pid=spawn("/bin/spawncheck",args,input); CHECK(pid>0);
    close(input); close(fds[1]);
    CHECK(read(fds[0],reply,2)==2 && !strcmp(reply,"ok")); close(fds[0]);
    int status=-1; CHECK(waitpid(pid,&status,0)==pid && status==23);
    char after[128]; CHECK(getcwd(after,sizeof(after))==after && !strcmp(after,cwd));
    CHECK(spawn("/missing",args,-1)==-ENOENT);
    CHECK(spawn((char *)0xffffffff,args,-1)==-EFAULT);
    CHECK(spawn("/bin/spawncheck",(char **)0xffffffff,-1)==-EFAULT);
    CHECK(spawn("/bin/spawncheck",args,1000)==-EBADF);
    CHECK(spawn("/bin/spawncheck",args,-2)==-EBADF);
    CHECK(spawn("/tmp",args,-1)==-EISDIR);
    int fd=open("/tmp/not-elf",O_WRITE);CHECK(fd>=0);
    char data[128];memset(data,0,sizeof(data));CHECK(write(fd,data,sizeof(data))==sizeof(data));close(fd);
    CHECK(chmod("/tmp/not-elf",0755)==0); /* reach ELF validation after the execute-permission check */
    CHECK(spawn("/tmp/not-elf",args,-1)==-ENOEXEC);unlink("/tmp/not-elf");
    /* Repeated failures must not consume process slots or leak descriptor refs. */
    for(int i=0;i<40;i++) CHECK(spawn("/missing",args,-1)==-ENOENT);
    return 0;
}
int main(int argc,char **argv) {
    if(argc>1) return child(argc,argv);
    CHECK(exercise()==0);
    CHECK(account(2,"spawnuser","spawn123")==0);
    int pid=fork();CHECK(pid>=0);
    if(!pid) { CHECK(account(1,"spawnuser","spawn123")==0);return exercise(); }
    int status=-1;CHECK(waitpid(pid,&status,0)==pid && status==0);
    puts("SPAWN PASS");return 0;
}
