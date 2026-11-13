#include "config.h"
#include "openbsd-compat/openbsd-compat.h"
#undef waitpid
#undef execve
static int raw(int n,int a,int b,int c) {
#ifdef __MYOS_CC__
    return __syscall(n,a,b,c);
#else
    int ret;__asm__ volatile("int $0x80":"=a"(ret):"a"(n),"b"(a),"c"(b),"d"(c):"memory");return ret;
#endif
}
static int posix(int r) {if(r<0){errno=-r;return -1;}return r;}
static int ctl(int op,int arg,void *p) {return posix(raw(SYS_SSHCTL,op,arg,(int)p));}
unsigned short htons(unsigned short v) {return (v>>8)|(v<<8);}
unsigned short ntohs(unsigned short v) {return htons(v);}
unsigned htonl(unsigned v) {return (v>>24)|((v>>8)&0xff00)|((v<<8)&0xff0000)|(v<<24);}
unsigned ntohl(unsigned v) {return htonl(v);}
int socket(int domain,int type,int protocol) {
    (void)protocol;if(domain!=AF_INET){errno=EAFNOSUPPORT;return -1;}
    if(type!=SOCK_STREAM && type!=SOCK_DGRAM){errno=EPROTONOSUPPORT;return -1;}
    return posix(raw(SYS_SOCKET,type,0,0));
}
int listen(int fd,int backlog){return posix(raw(SYS_LISTEN,fd,backlog,0));}
int socketpair(int domain,int type,int protocol,int fds[2]) {
    if(domain!=AF_UNIX || type!=SOCK_STREAM || protocol){errno=EAFNOSUPPORT;return -1;}
    return ctl(2,0,fds);
}
int connect(int fd,const struct sockaddr *addr,socklen_t len) {
    if(len<sizeof(struct sockaddr_in)||addr->sa_family!=AF_INET){errno=EAFNOSUPPORT;return -1;}
    const struct sockaddr_in *in=(const void*)addr;
    return posix(raw(SYS_CONNECT,fd,ntohl(in->sin_addr.s_addr),ntohs(in->sin_port)));
}
int bind(int fd,const struct sockaddr *addr,socklen_t len) {
    if(len<sizeof(struct sockaddr_in)||addr->sa_family!=AF_INET){errno=EAFNOSUPPORT;return -1;}
    const struct sockaddr_in *in=(const void*)addr;
    /* Native bind is INADDR_ANY only: reject narrower binds, never silently widen. */
    if(in->sin_addr.s_addr!=INADDR_ANY){errno=EADDRNOTAVAIL;return -1;}
    return posix(raw(SYS_BIND,fd,ntohs(in->sin_port),0));
}
static void address(struct sockaddr *out,socklen_t *len,unsigned ip,unsigned port) {
    struct sockaddr_in in;memset(&in,0,sizeof(in));in.sin_family=AF_INET;in.sin_addr.s_addr=htonl(ip);in.sin_port=htons(port);
    if(out && len){memcpy(out,&in,*len<sizeof(in)?*len:sizeof(in));*len=sizeof(in);}
}
int accept(int fd,struct sockaddr *out,socklen_t *len) {
    /* SYS_ACCEPT returns the kernel ABI pair, not a POSIX sockaddr. */
    struct { unsigned ip, port; } a;
    int r=posix(raw(SYS_ACCEPT,fd,(int)&a,0));
    if(r>=0) address(out,len,a.ip,a.port);
    return r;
}
int getsockname(int fd,struct sockaddr *out,socklen_t *len) {unsigned a[4];if(ctl(5,fd,a)<0)return -1;address(out,len,a[0],a[1]);return 0;}
int getpeername(int fd,struct sockaddr *out,socklen_t *len) {unsigned a[4];if(ctl(5,fd,a)<0)return -1;address(out,len,a[2],a[3]);return 0;}
int setsockopt(int fd,int level,int opt,const void *v,socklen_t len) {
    (void)fd;(void)v;(void)len;
    if((level==SOL_SOCKET&&(opt==SO_REUSEADDR||opt==SO_KEEPALIVE||opt==SO_OOBINLINE||opt==SO_LINGER))||
       (level==IPPROTO_TCP&&opt==TCP_NODELAY)||(level==IPPROTO_IP&&(opt==IP_TOS||opt==IP_TTL))) return 0;
    errno=ENOPROTOOPT;return -1;
}
int getsockopt(int fd,int level,int opt,void *out,socklen_t *len) {
    unsigned a[4];if(ctl(5,fd,a)<0)return -1;
    if(level!=SOL_SOCKET||*len<sizeof(int)){errno=ENOPROTOOPT;return -1;}
    if(opt==SO_ERROR) *(int*)out=0;
    else if(opt==SO_RCVBUF) *(int*)out=32768;
    else if(opt==SO_SNDBUF) *(int*)out=16384;
    else {errno=ENOPROTOOPT;return -1;}
    *len=sizeof(int);return 0;
}
int shutdown(int fd,int how) {(void)how;return ctl(9,fd,NULL);}
ssize_t send(int fd,const void *buf,size_t len,int flags) {if(flags){errno=EOPNOTSUPP;return -1;}return write(fd,buf,len);}
ssize_t recv(int fd,void *buf,size_t len,int flags) {if(flags){errno=EOPNOTSUPP;return -1;}return read(fd,buf,len);}
ssize_t writev(int fd,const struct iovec *v,int n) {
    ssize_t total=0;for(int i=0;i<n;i++){ssize_t k=write(fd,v[i].iov_base,v[i].iov_len);if(k<0)return total?total:-1;total+=k;if((size_t)k<v[i].iov_len)break;}return total;
}
ssize_t readv(int fd,const struct iovec *v,int n) {
    ssize_t total=0;for(int i=0;i<n;i++){ssize_t k=read(fd,v[i].iov_base,v[i].iov_len);if(k<0)return total?total:-1;total+=k;if((size_t)k<v[i].iov_len)break;}return total;
}
ssize_t sendmsg(int fd,const struct msghdr *m,int flags) {
    if(flags){errno=EOPNOTSUPP;return -1;}
    struct cmsghdr *c=CMSG_FIRSTHDR(m);
    if(c){if(c->cmsg_level!=SOL_SOCKET||c->cmsg_type!=SCM_RIGHTS||c->cmsg_len!=CMSG_LEN(sizeof(int))){errno=EINVAL;return -1;}
        if(ctl(3,fd,(void*)*(int*)CMSG_DATA(c))<0)return -1;}
    return writev(fd,m->msg_iov,m->msg_iovlen);
}
ssize_t recvmsg(int fd,struct msghdr *m,int flags) {
    if(flags){errno=EOPNOTSUPP;return -1;}
    ssize_t n=readv(fd,m->msg_iov,m->msg_iovlen);if(n<=0)return n;
    size_t cap=m->msg_controllen;m->msg_controllen=0;m->msg_flags=0;
    int passed=ctl(4,fd,NULL);
    if(passed>=0) {if(cap<CMSG_SPACE(sizeof(int))){close(passed);m->msg_flags=MSG_CTRUNC;}
        else {struct cmsghdr *c=m->msg_control;c->cmsg_len=CMSG_LEN(sizeof(int));c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;
            *(int*)CMSG_DATA(c)=passed;m->msg_controllen=CMSG_SPACE(sizeof(int));}}
    return n;
}
int poll(struct pollfd *fds,nfds_t n,int timeout) {
    unsigned start=ticks();for(;;) {int r=ctl(1,n,fds);if(r || !timeout)return r;
        if(timeout>0 && ticks()-start>=(unsigned)timeout)return 0;sleep_ms(10);}
}
int ppoll(struct pollfd *fds,nfds_t n,const struct timespec *t,const sigset_t *mask) {
    (void)mask;return poll(fds,n,t?(t->tv_sec>2147483?2147483647:t->tv_sec*1000+(t->tv_nsec+999999)/1000000):-1);
}
int select(int n,fd_set *r,fd_set *w,fd_set *e,struct timeval *t) {
    if(n<0||n>FD_SETSIZE){errno=EINVAL;return -1;}
    struct pollfd p[FD_SETSIZE];for(int i=0;i<n;i++){p[i].fd=i;p[i].events=(r&&FD_ISSET(i,r)?POLLIN:0)|(w&&FD_ISSET(i,w)?POLLOUT:0);if(!p[i].events)p[i].fd=-1;}
    int v=poll(p,n,t?t->tv_sec*1000+(t->tv_usec+999)/1000:-1);if(v<0)return v;
    if(r)FD_ZERO(r);if(w)FD_ZERO(w);if(e)FD_ZERO(e);
    int count=0;for(int i=0;i<n;i++){if(r&&(p[i].revents&(POLLIN|POLLHUP))) {FD_SET(i,r);count++;}
        if(w&&(p[i].revents&POLLOUT)){FD_SET(i,w);count++;}}return count;
}
int getaddrinfo(const char *host,const char *service,const struct addrinfo *hint,struct addrinfo **out) {
    *out=NULL;if(hint && hint->ai_family!=AF_INET && hint->ai_family!=AF_UNSPEC)return EAI_FAMILY;
    unsigned ip=0,port=0;
    if(service){char *end;unsigned long v=strtoul(service,&end,10);if(!*service||*end||v>65535)return EAI_SERVICE;port=v;}
    if(!host)ip=hint&&(hint->ai_flags&AI_PASSIVE)?0:INADDR_LOOPBACK;
    else if(hint&&(hint->ai_flags&AI_NUMERICHOST)){if(!ip_parse(host,&ip))return EAI_NONAME;}
    else if(resolve(host,&ip)<0)return EAI_NONAME;
    struct addrinfo *a=calloc(1,sizeof(*a));struct sockaddr_in *in=calloc(1,sizeof(*in));
    if(!a||!in){free(a);free(in);return EAI_MEMORY;}
    in->sin_family=AF_INET;in->sin_addr.s_addr=htonl(ip);in->sin_port=htons(port);
    a->ai_family=AF_INET;a->ai_socktype=hint?hint->ai_socktype:SOCK_STREAM;a->ai_protocol=IPPROTO_TCP;
    a->ai_addr=(void*)in;a->ai_addrlen=sizeof(*in);if(host&&hint&&(hint->ai_flags&AI_CANONNAME))a->ai_canonname=strdup(host);*out=a;return 0;
}
void freeaddrinfo(struct addrinfo *a) {while(a){struct addrinfo *next=a->ai_next;free(a->ai_addr);free(a->ai_canonname);free(a);a=next;}}
const char *gai_strerror(int e) {(void)e;return "IPv4 address resolution failed";}
int getnameinfo(const struct sockaddr *a,socklen_t len,char *host,socklen_t hn,char *serv,socklen_t sn,int flags) {
    if(a->sa_family!=AF_INET||len<sizeof(struct sockaddr_in))return EAI_FAMILY;
    if(flags&NI_NAMEREQD)return EAI_NONAME;
    const struct sockaddr_in *in=(const void*)a;char b[16];ip_str(ntohl(in->sin_addr.s_addr),b);
    if(host){if(strlen(b)>=hn)return EAI_MEMORY;strcpy(host,b);}
    if(serv){if(snprintf(serv,sn,"%u",ntohs(in->sin_port))>=(int)sn)return EAI_MEMORY;}return 0;
}
int inet_pton(int af,const char *s,void *out) {if(af!=AF_INET){errno=EAFNOSUPPORT;return -1;}unsigned ip;if(!ip_parse(s,&ip))return 0;*(unsigned*)out=htonl(ip);return 1;}
const char *inet_ntop(int af,const void *in,char *out,socklen_t n) {if(af!=AF_INET){errno=EAFNOSUPPORT;return NULL;}char b[16];ip_str(ntohl(*(const unsigned*)in),b);if(strlen(b)>=n){errno=ENOSPC;return NULL;}strcpy(out,b);return out;}
int inet_aton(const char *s,struct in_addr *a) {return inet_pton(AF_INET,s,a)==1;}
char *inet_ntoa(struct in_addr a) {static char b[16];inet_ntop(AF_INET,&a,b,sizeof(b));return b;}
int gethostname(char *s,size_t n) {if(n<5){errno=ENAMETOOLONG;return -1;}strcpy(s,"myos");return 0;}
int uname(struct utsname *u) {memset(u,0,sizeof(*u));strcpy(u->sysname,"MyOS");strcpy(u->nodename,"myos");strcpy(u->release,"1");strcpy(u->machine,"i386");return 0;}
int getentropy(void *out,size_t n) {if(n>256){errno=EIO;return -1;}return ctl(6,n,out);}
long sysconf(int n) {switch(n){case _SC_OPEN_MAX:return 16;case _SC_PAGESIZE:return 4096;case _SC_CLK_TCK:return 100;case _SC_NGROUPS_MAX:return 1;}errno=EINVAL;return -1;}
int getpagesize(void){return 4096;}
int getrlimit(int what,struct rlimit *l) {l->rlim_cur=l->rlim_max=what==RLIMIT_NOFILE?16:RLIM_INFINITY;return 0;}
int setrlimit(int what,const struct rlimit *l) {if(what==RLIMIT_NOFILE&&l->rlim_cur>16){errno=EINVAL;return -1;}return 0;}
int getrusage(int who,struct rusage *u) {(void)who;memset(u,0,sizeof(*u));return 0;}
int clock_gettime(int clockid,struct timespec *t) {if(clockid==CLOCK_MONOTONIC){unsigned ms=ticks();t->tv_sec=ms/1000;t->tv_nsec=(ms%1000)*1000000;return 0;}
    if(clockid==CLOCK_REALTIME){t->tv_sec=time(NULL);t->tv_nsec=0;return 0;}errno=EINVAL;return -1;}
int nanosleep(const struct timespec *t,struct timespec *rem) {if(t->tv_sec<0||t->tv_nsec<0||t->tv_nsec>=1000000000){errno=EINVAL;return -1;}sleep_ms(t->tv_sec*1000+(t->tv_nsec+999999)/1000000);if(rem)memset(rem,0,sizeof(*rem));return 0;}
uid_t getuid(void) {int v[3];return ctl(8,0,v)<0?(uid_t)-1:(uid_t)v[0];}
uid_t geteuid(void) {int v[3];return ctl(8,0,v)<0?(uid_t)-1:(uid_t)v[1];}
gid_t getgid(void) {int v[3];return ctl(12,0,v)<0?(gid_t)-1:(gid_t)v[0];}
gid_t getegid(void) {int v[3];return ctl(12,0,v)<0?(gid_t)-1:(gid_t)v[1];}
int setresuid(uid_t a,uid_t b,uid_t c) {int v[3];v[0]=a;v[1]=b;v[2]=c;return ctl(8,1,v);}
int seteuid(uid_t uid) {return setresuid(-1,uid,-1);}
int setuid(uid_t uid) {return setresuid(uid,uid,uid);}
int setgid(gid_t gid) {return setresgid(gid,gid,gid);}
int setegid(gid_t gid) {return setresgid(-1,gid,-1);}
int setresgid(gid_t a,gid_t b,gid_t c) {int v[3];v[0]=a;v[1]=b;v[2]=c;return ctl(12,1,v);}
int getgroups(int n,gid_t *g){if(!n)return 1;if(n<1){errno=EINVAL;return -1;}g[0]=getgid();return 1;}
int setgroups(size_t n,const gid_t *g){(void)n;(void)g;return 0;}
int initgroups(const char *name,gid_t g){(void)name;(void)g;return 0;}
struct group *getgrgid(gid_t id){static struct group g;static char *members[1];g.gr_name=id?"users":"root";g.gr_passwd="*";g.gr_gid=id;g.gr_mem=members;return &g;}
struct group *getgrnam(const char *n){if(!strcmp(n,"root"))return getgrgid(0);return NULL;}
void endgrent(void){}
struct passwd *getpwnam(const char *name) {
    static struct passwd pw;static char user[24],home[40];int id;
    if(!strcmp(name,"sshd"))id=65534;else {id=account(8,name,NULL);if(id<0){errno=ENOENT;return NULL;}}
    strlcpy(user,name,sizeof(user));snprintf(home,sizeof(home),id==65534?"/var/empty":(id?"/home/%s":"/root"),user);
    pw.pw_name=user;pw.pw_passwd="*";pw.pw_uid=pw.pw_gid=id;pw.pw_gecos=user;pw.pw_dir=home;pw.pw_shell="/bin/sh";return &pw;
}
struct passwd *getpwuid(uid_t uid){char n[24];if(uid==65534)return getpwnam("sshd");for(unsigned i=0;account_list(n,i)==0;i++){int id=account(8,n,NULL);if(id==(int)uid)return getpwnam(n);}errno=ENOENT;return NULL;}
void endpwent(void){}
int chroot(const char *p){return ctl(10,0,(void*)p);}
int ftruncate(int fd,off_t n){struct stat st;if(fstat(fd,&st)<0)return -1;if(st.st_size==(unsigned)n)return 0;errno=ENOSYS;return -1;}
int truncate(const char *p,off_t n){int fd=open(p,O_WRONLY);if(fd<0)return -1;int ret=ftruncate(fd,n);close(fd);return ret;}
int fsync(int fd){struct stat st;return fstat(fd,&st);}
int fdatasync(int fd){return fsync(fd);}
int utimes(const char *p,const struct timeval t[2]){(void)p;(void)t;errno=ENOSYS;return -1;}
int futimes(int fd,const struct timeval t[2]){(void)fd;(void)t;errno=ENOSYS;return -1;}
int statvfs(const char *p,struct statvfs *s){(void)p;(void)s;errno=ENOSYS;return -1;}
int fstatvfs(int fd,struct statvfs *s){(void)fd;(void)s;errno=ENOSYS;return -1;}
int link(const char *p,const char *q){(void)p;(void)q;errno=ENOSYS;return -1;}
int flock(int fd,int how){(void)fd;(void)how;errno=ENOSYS;return -1;}
int sigemptyset(sigset_t *s){*s=0;return 0;}
int sigfillset(sigset_t *s){*s=-1;return 0;}
int sigaddset(sigset_t *s,int n){if(n>0&&n<32)*s|=1U<<(n-1);return 0;}
int sigdelset(sigset_t *s,int n){if(n>0&&n<32)*s&=~(1U<<(n-1));return 0;}
int sigismember(const sigset_t *s,int n){return n>0&&n<32?!!(*s&(1U<<(n-1))):0;}
int sigaction(int n,const struct sigaction *a,struct sigaction *old){void (*h)(int)=signal(n,a?a->sa_handler:SIG_DFL);if(h==SIG_ERR)return -1;if(old){memset(old,0,sizeof(*old));old->sa_handler=h;}return 0;}
int sigsuspend(const sigset_t *s){(void)s;sleep_ms(10);errno=EINTR;return -1;}
int killpg(pid_t p,int s){return kill(-p,s);}
pid_t setsid(void){if(setpgid(0,0)<0)return -1;return getpid();}
pid_t getsid(pid_t p){return p?p:getpid();}
pid_t getpgrp(void){return getpid();}
int myos_ssh_waitpid(int pid,int *st,int opts){int code=0;int ret=posix(waitpid(pid,&code,opts));if(ret>0&&st)*st=(code&255)<<8;return ret;}
pid_t wait(int *st){return myos_ssh_waitpid(-1,st,0);}
int myos_ssh_execve(const char *p,char *const av[],char *const ev[]){return posix(execve(p,av,ev));}
void _exit(int s){exit(s);}
int myos_ssh_fcntl(int fd,int cmd,...){va_list ap;va_start(ap,cmd);int v[2];v[0]=cmd;v[1]=cmd==F_SETFD||cmd==F_SETFL||cmd==F_DUPFD?va_arg(ap,int):0;va_end(ap);
    if(cmd==F_DUPFD) {for(int i=v[1];i<16;i++){struct stat st;if(fstat(i,&st)<0&&errno==EBADF)return posix(dup2(fd,i));}errno=EMFILE;return -1;}
    return ctl(7,fd,v);
}
int tcgetattr(int fd,struct termios *t){int v[2]={0,0};if(ctl(11,fd,v)<0)return -1;int m=v[1];memset(t,0,sizeof(*t));t->c_iflag=ICRNL|IXON;int output=ttyctl(fd,TTYCTL_GETOFLAGS,0);t->c_oflag=output<0?(OPOST|ONLCR):output;t->c_cflag=CS8|CREAD;t->c_lflag=(m&TTY_RAW?0:ICANON|ECHO)|(m&TTY_NOSIG?0:ISIG);t->c_cc[VINTR]=3;t->c_cc[VEOF]=4;t->c_cc[VERASE]=127;t->c_cc[VKILL]=21;t->c_cc[VMIN]=1;t->c_ispeed=t->c_ospeed=B38400;return 0;}
int tcsetattr(int fd,int how,const struct termios *t){(void)how;if(!isatty(fd)){errno=ENOTTY;return -1;}int m=(t->c_lflag&(ICANON|ECHO))==(ICANON|ECHO)?0:TTY_RAW;if(!(t->c_lflag&ISIG))m|=TTY_NOSIG;int v[2];v[0]=1;v[1]=m;if(ctl(11,fd,v)<0)return -1;if(ttyctl(fd,TTYCTL_GETOFLAGS,0)>=0)return posix(ttyctl(fd,TTYCTL_SETOFLAGS,t->c_oflag&(OPOST|ONLCR)));return 0;}
speed_t cfgetispeed(const struct termios *t){return t->c_ispeed;}
speed_t cfgetospeed(const struct termios *t){return t->c_ospeed;}
int cfsetispeed(struct termios *t,speed_t s){t->c_ispeed=s;return 0;}
int cfsetospeed(struct termios *t,speed_t s){t->c_ospeed=s;return 0;}
void cfmakeraw(struct termios *t){t->c_iflag=0;t->c_oflag=0;t->c_lflag=0;t->c_cflag=CS8|CREAD;t->c_cc[VMIN]=1;t->c_cc[VTIME]=0;}
int tcgetpgrp(int fd){return posix(ttyctl(fd,TTYCTL_GETFG,0));}
int tcsetpgrp(int fd,pid_t p){(void)fd;return posix(setpgid(0,p));}
int openpty(int *master,int *slave,char *name,const struct termios *t,const struct winsize *w){int f[2];if(posix(pty_open(f))<0)return -1;*master=f[0];*slave=f[1];if(name)strcpy(name,"/dev/tty");if(w)ttyctl(f[0],TTYCTL_SETSIZE,(w->ws_col<<16)|w->ws_row);if(t)tcsetattr(f[1],TCSANOW,t);return 0;}
int myos_ssh_ioctl(int fd,unsigned long request,...){va_list ap;va_start(ap,request);void *p=va_arg(ap,void*);va_end(ap);
    if(request==TIOCGWINSZ){int s=ttyctl(fd,TTYCTL_GETSIZE,0);if(s<0)return posix(s);struct winsize *w=p;memset(w,0,sizeof(*w));w->ws_col=(unsigned)s>>16;w->ws_row=s&65535;return 0;}
    if(request==TIOCSWINSZ){const struct winsize *w=p;return posix(ttyctl(fd,TTYCTL_SETSIZE,(w->ws_col<<16)|w->ws_row));}
    if(request==TIOCSCTTY)return isatty(fd)?0:-1;
    if(request==FIONREAD){int n=sockctl(fd,SOCK_NREAD,0);if(n<0)return posix(n);*(int*)p=n;return 0;}
    errno=ENOTTY;return -1;
}
void openlog(const char *ident,int opts,int facility){(void)ident;(void)opts;(void)facility;}
void closelog(void){}
void vsyslog(int prio,const char *fmt,va_list ap){(void)prio;vfprintf(stderr,fmt,ap);fprintf(stderr,"\n");}
void syslog(int prio,const char *fmt,...){va_list ap;va_start(ap,fmt);vsyslog(prio,fmt,ap);va_end(ap);}
int vasprintf(char **out,const char *fmt,va_list ap){va_list aq;va_copy(aq,ap);int n=vsnprintf(NULL,0,fmt,aq);va_end(aq);if(n<0||!(*out=malloc(n+1))){*out=NULL;return -1;}return vsnprintf(*out,n+1,fmt,ap);}
int asprintf(char **out,const char *fmt,...){va_list ap;va_start(ap,fmt);int n=vasprintf(out,fmt,ap);va_end(ap);return n;}

int getrrsetbyname(const char *n,unsigned c,unsigned t,unsigned f,struct rrsetinfo **out){(void)n;(void)c;(void)t;(void)f;*out=NULL;return ERRSET_NODATA;}
void freerrset(struct rrsetinfo *r){(void)r;}

void bzero(void *p,size_t n){memset(p,0,n);}
int isascii(int c){return (unsigned)c<128;}
int getdtablesize(void){return 16;}
int getgrouplist(const char *n,gid_t g,gid_t *v,int *count){(void)n;if(*count<1){*count=1;return -1;}v[0]=g;*count=1;return 1;}
int rmdir(const char *p){return posix(unlink(p));}
int execvp(const char *p,char *const av[]){if(strchr(p,'/'))return myos_ssh_execve(p,av,environ);char path[1024];snprintf(path,sizeof(path),"/bin/%s",p);myos_ssh_execve(path,av,environ);if(errno!=ENOENT)return -1;snprintf(path,sizeof(path),"/ext/bin/%s",p);return myos_ssh_execve(path,av,environ);}
static int vaexec(const char *p,const char *first,va_list ap,int search){char *v[64];int n=0;const char *s=first;while(s&&n<63){v[n++]=(char*)s;s=va_arg(ap,const char*);}v[n]=NULL;if(s){errno=E2BIG;return -1;}return search?execvp(p,v):myos_ssh_execve(p,v,environ);}
int execl(const char *p,const char *s,...){va_list ap;va_start(ap,s);int ret=vaexec(p,s,ap,0);va_end(ap);return ret;}
int execlp(const char *p,const char *s,...){va_list ap;va_start(ap,s);int ret=vaexec(p,s,ap,1);va_end(ap);return ret;}
void setpwent(void){}
struct passwd *getpwent(void){return NULL;}
char *crypt(const char *pw,const char *salt){(void)pw;(void)salt;errno=ENOSYS;return NULL;}
unsigned if_nametoindex(const char *n){return !strcmp(n,"lo")?1:!strcmp(n,"eth0")?2:0;}
char *if_indextoname(unsigned n,char *s){if(n!=1&&n!=2){errno=EINVAL;return NULL;}strcpy(s,n==1?"lo":"eth0");return s;}

int getifaddrs(struct ifaddrs **out){(void)out;errno=ENOSYS;return -1;}
void freeifaddrs(struct ifaddrs *a){(void)a;}
struct servent *getservbyname(const char *name,const char *proto){static struct servent s;if(strcmp(name,"ssh")||(proto&&strcmp(proto,"tcp")))return NULL;memset(&s,0,sizeof(s));s.s_name="ssh";s.s_proto="tcp";s.s_port=htons(22);return &s;}
int setitimer(int which,const struct itimerval *t,struct itimerval *old){(void)which;if(old)memset(old,0,sizeof(*old));if(t->it_value.tv_sec||t->it_value.tv_usec){errno=ENOSYS;return -1;}return 0;}
char *ctime(const time_t *t){static char b[32];struct tm tm;localtime_r(t,&tm);strftime(b,sizeof(b),"%a %b %d %H:%M:%S %Y\n",&tm);return b;}
static int popen_pid[16];
FILE *popen(const char *cmd,const char *mode){int fds[2];if(pipe(fds)<0)return NULL;int pid=fork();if(pid<0){close(fds[0]);close(fds[1]);return NULL;}int writing=mode[0]=='w';if(!pid){dup2(fds[writing?0:1],writing?0:1);close(fds[0]);close(fds[1]);execl("/bin/sh","sh","-c",cmd,NULL);_exit(127);}close(fds[writing?0:1]);int fd=fds[writing?1:0];FILE *f=fdopen(fd,mode);if(!f){close(fd);return NULL;}popen_pid[fd]=pid;return f;}
int pclose(FILE *f){int fd=fileno(f),st=0,pid=popen_pid[fd];fclose(f);return myos_ssh_waitpid(pid,&st,0)<0?-1:st;}

int dup(int fd){return myos_ssh_fcntl(fd,F_DUPFD,0);}
int execv(const char *p,char *const av[]){return myos_ssh_execve(p,av,environ);}
int getpeereid(int fd,uid_t *u,gid_t *g){(void)fd;(void)u;(void)g;errno=EOPNOTSUPP;return -1;}
/* OpenSSH uses integer, bounded string and scanset conversions only. */
int sscanf(const char *input,const char *fmt,...) {
    const char *s=input;int assigned=0;va_list ap;va_start(ap,fmt);
    while(*fmt) {
        if(isspace(*fmt)){while(isspace(*fmt))fmt++;while(isspace(*s))s++;continue;}
        if(*fmt!='%'){if(*s!=*fmt)break;s++;fmt++;continue;}
        fmt++;if(*fmt=='%'){if(*s!='%')break;s++;fmt++;continue;}
        int suppress=0,width=0,length=0;if(*fmt=='*'){suppress=1;fmt++;}
        while(isdigit(*fmt)){width=width*10+*fmt++-'0';if(width>1048576)width=1048576;}
        if(!width)width=1048576;
        if(*fmt=='h'){length=-1;fmt++;if(*fmt=='h'){length=-2;fmt++;}}
        else if(*fmt=='l'){length=1;fmt++;if(*fmt=='l'){length=2;fmt++;}}
        char kind=*fmt++;if(!kind)break;
        if(kind=='n'){if(!suppress)*va_arg(ap,int*)=s-input;continue;}
        if(kind!='c'&&kind!='[')while(isspace(*s))s++;
        if(kind=='s'||kind=='c'||kind=='[') {
            unsigned char set[256];memset(set,0,sizeof(set));int invert=0;
            if(kind=='['){if(*fmt=='^'){invert=1;fmt++;}if(*fmt==']'){set[']']=1;fmt++;}
                while(*fmt&&*fmt!=']'){unsigned char a=*fmt++;if(*fmt=='-'&&fmt[1]&&fmt[1]!=']'){fmt++;unsigned char b=*fmt++;for(unsigned k=a;k<=b;k++)set[k]=1;}else set[a]=1;}
                if(*fmt!=']')break;fmt++;}
            if(kind=='c'&&width==1048576)width=1;
            char *dst=suppress?NULL:va_arg(ap,char*);int n=0;
            while(*s&&n<width){unsigned char ch=*s;if(kind=='s'&&isspace(ch))break;if(kind=='['&&((set[ch]!=0)==invert))break;if(dst)dst[n]=ch;s++;n++;}
            if(!n)break;if(dst&&kind!='c')dst[n]=0;if(!suppress)assigned++;continue;
        }
        if(kind=='d'||kind=='i'||kind=='u'||kind=='o'||kind=='x'||kind=='X') {
            int base=kind=='o'?8:(kind=='x'||kind=='X'?16:(kind=='i'?0:10));
            const char *start=s;int neg=0,left=width;if(left&&(*s=='-'||*s=='+')){neg=*s=='-';s++;left--;}
            if((base==0||base==16)&&left>2&&s[0]=='0'&&(s[1]=='x'||s[1]=='X')){s+=2;left-=2;base=16;}
            if(!base)base=*s=='0'?8:10;
            unsigned long long value=0;int digits=0;
            while(left&&*s){unsigned ch=(unsigned char)*s;int d=ch>='0'&&ch<='9'?ch-'0':ch>='a'&&ch<='f'?ch-'a'+10:ch>='A'&&ch<='F'?ch-'A'+10:99;if(d>=base)break;value=value*base+d;s++;left--;digits++;}
            if(!digits){s=start;break;}if(neg)value=0-value;
            if(!suppress){if(length==2)*va_arg(ap,unsigned long long*)=value;else if(length==1)*va_arg(ap,unsigned long*)=(unsigned long)value;else if(length==-1)*va_arg(ap,unsigned short*)=value;else if(length==-2)*va_arg(ap,unsigned char*)=value;else *va_arg(ap,unsigned*)=value;assigned++;}
            continue;
        }
        break;
    }
    va_end(ap);return assigned?assigned:(!*s?-1:0);
}

int sigprocmask(int how,const sigset_t *set,sigset_t *old){static sigset_t mask;if(old)*old=mask;if(!set)return 0;if(how==SIG_SETMASK)mask=*set;else if(how==SIG_BLOCK)mask|=*set;else if(how==SIG_UNBLOCK)mask&=~*set;else{errno=EINVAL;return -1;}return 0;}
void *memmem(const void *hay,size_t hn,const void *needle,size_t nn){if(!nn)return (void*)hay;if(nn>hn)return NULL;const unsigned char *h=hay;for(size_t i=0;i<=hn-nn;i++)if(!memcmp(h+i,needle,nn))return (void*)(h+i);return NULL;}
