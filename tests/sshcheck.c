#include "config.h"
#include "openbsd-compat/openbsd-compat.h"
static int failures;
static thread_mutex_t cow_lock;
static volatile unsigned cow_stamp;
#define CHECK(x) do {if(!(x)){printf("SSH ABI FAIL line %d: %s (errno %d)\n",__LINE__,#x,errno);failures++;}}while(0)
static void rights_test(void) {
    int sv[2],p[2];CHECK(socketpair(AF_UNIX,SOCK_STREAM,0,sv)==0);CHECK(pipe(p)==0);
    CHECK(myos_ssh_fcntl(sv[0],F_SETFL,O_NONBLOCK)==0);
    char ch;CHECK(read(sv[0],&ch,1)==-1 && errno==EAGAIN);
    CHECK(write(sv[1],"x",1)==1);struct pollfd pollfd={sv[0],POLLIN,0};
    CHECK(poll(&pollfd,1,100)==1 && (pollfd.revents&POLLIN));CHECK(read(sv[0],&ch,1)==1 && ch=='x');
    CHECK(myos_ssh_fcntl(sv[0],F_SETFL,0)==0);
    unsigned char control[CMSG_SPACE(sizeof(int))];memset(control,0,sizeof(control));
    struct iovec iov={&ch,1};struct msghdr m;memset(&m,0,sizeof(m));m.msg_iov=&iov;m.msg_iovlen=1;m.msg_control=control;m.msg_controllen=sizeof(control);
    struct cmsghdr *c=CMSG_FIRSTHDR(&m);c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(sizeof(int));*(int*)CMSG_DATA(c)=p[1];
    CHECK(sendmsg(sv[1],&m,0)==1);close(p[1]);memset(control,0,sizeof(control));m.msg_controllen=sizeof(control);
    CHECK(recvmsg(sv[0],&m,0)==1);c=CMSG_FIRSTHDR(&m);CHECK(c!=NULL);
    if(c){int fd=*(int*)CMSG_DATA(c);CHECK(write(fd,"z",1)==1);CHECK(read(p[0],&ch,1)==1&&ch=='z');close(fd);}
    close(p[0]);close(sv[0]);close(sv[1]);
}
static void pty_output_test(void) {
    int master,slave;struct termios modes;char bytes[8];
    CHECK(openpty(&master,&slave,NULL,NULL,NULL)==0);
    CHECK(tcgetattr(slave,&modes)==0 && (modes.c_oflag&(OPOST|ONLCR))==(OPOST|ONLCR));
    CHECK(write(slave,"a\nb\n",4)==4);CHECK(read(master,bytes,6)==6 && !memcmp(bytes,"a\r\nb\r\n",6));
    cfmakeraw(&modes);CHECK(tcsetattr(slave,TCSANOW,&modes)==0);
    CHECK(tcgetattr(slave,&modes)==0 && modes.c_oflag==0);
    CHECK(write(slave,"\r\n",2)==2);CHECK(read(master,bytes,2)==2 && !memcmp(bytes,"\r\n",2));
    close(master);close(slave);
}
int main(int argc,char **argv) {
    if(argc>1&&!strcmp(argv[1],"cloexec")){struct stat st;CHECK(fstat(12,&st)==-1&&errno==EBADF);return failures?1:0;}
    rights_test();
    pty_output_test();
    int sync[2],code=0;CHECK(pipe(sync)==0);int cp=fork();
    if(!cp){close(sync[1]);char v;CHECK(read(sync[0],&v,1)==1);CHECK(thread_mutex_lock(&cow_lock)==0);CHECK(thread_mutex_unlock(&cow_lock)==0);exit(failures?1:0);}
    close(sync[0]);CHECK(thread_mutex_lock(&cow_lock)==0);cow_stamp++;
    CHECK(thread_mutex_unlock(&cow_lock)==0);CHECK(write(sync[1],"x",1)==1);close(sync[1]);
    CHECK(myos_ssh_waitpid(cp,&code,0)==cp&&code==0);
    unsigned char entropy[64],other[64];CHECK(getentropy(entropy,sizeof(entropy))==0);CHECK(getentropy(other,sizeof(other))==0);CHECK(memcmp(entropy,other,sizeof(entropy))!=0);
    CHECK(getentropy(entropy,257)==-1);
    CHECK(getpwnam("root")!=NULL);CHECK(getpwnam("no_such_account")==NULL);
    CHECK(account(10,"root","wrong-password")<0);CHECK(account(10,"no_such_account","review123")<0);CHECK(account(10,"root","review123")==0);
    int fds[2];CHECK(pipe(fds)==0);CHECK(dup2(fds[0],12)==12);CHECK(myos_ssh_fcntl(12,F_SETFD,FD_CLOEXEC)==0);
    int child=fork(),st=0;
    if(!child){char *av[]={"sshcheck","cloexec",NULL};myos_ssh_execve("/bin/sshcheck",av,environ);exit(2);}
    CHECK(myos_ssh_waitpid(child,&st,0)==child&&st==0);close(12);close(fds[0]);close(fds[1]);
    child=fork();
    if(!child){struct stat sb;CHECK(chroot("/var/empty")==0);CHECK(chdir("/")==0);CHECK(stat("/bin/sh",&sb)<0);CHECK(stat("../../bin/sh",&sb)<0);CHECK(setresuid(65534,65534,65534)==0);CHECK(seteuid(0)<0);exit(failures?1:0);}
    CHECK(myos_ssh_waitpid(child,&st,0)==child&&st==0);
    int a,b;char str[16];CHECK(sscanf("SSH-2.0-OpenSSH_10","SSH-%d.%d-%[^\n]",&a,&b,str)==3&&a==2&&b==0&&!strcmp(str,"OpenSSH_10"));
    printf(failures?"SSH ABI FAILED\n":"SSH ABI PASS\n");return failures?1:0;
}
