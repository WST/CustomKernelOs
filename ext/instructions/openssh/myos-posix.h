/* OpenSSH's private POSIX adapter for the MyOS ABI. */
#ifndef MYOS_SSH_POSIX_H
#define MYOS_SSH_POSIX_H
#define socket myos_socket
#define connect myos_connect
#define bind myos_bind
#define accept myos_accept
#define sendto myos_sendto
#define recvfrom myos_recvfrom
#define sockaddr myos_sockaddr
#include <myos.h>
#undef socket
#undef connect
#undef bind
#undef accept
#undef sendto
#undef recvfrom
#undef sockaddr
#define socket ssh_posix_socket
#define connect ssh_posix_connect
#define bind ssh_posix_bind
#define accept ssh_posix_accept
#define listen ssh_posix_listen
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pwd.h>
#define getuid myos_ssh_getuid
#define geteuid myos_ssh_geteuid
#define getgid myos_ssh_getgid
#define getegid myos_ssh_getegid
#define setuid myos_ssh_setuid
#define setgid myos_ssh_setgid
#define getpwuid myos_ssh_getpwuid
#define getpwnam myos_ssh_getpwnam
#define endpwent myos_ssh_endpwent
uid_t myos_ssh_getuid(void);
uid_t myos_ssh_geteuid(void);
gid_t myos_ssh_getgid(void);
gid_t myos_ssh_getegid(void);
int myos_ssh_setuid(uid_t);
int myos_ssh_setgid(gid_t);
struct passwd *myos_ssh_getpwnam(const char *);
struct passwd *myos_ssh_getpwuid(uid_t);
void myos_ssh_endpwent(void);

typedef unsigned char u_char;
typedef unsigned short u_short;
typedef unsigned int u_int;
typedef unsigned long u_long;
typedef unsigned char u_int8_t;
typedef unsigned short u_int16_t;
typedef unsigned int u_int32_t;
typedef unsigned long long u_int64_t;
typedef unsigned int socklen_t;
typedef char *caddr_t;
typedef unsigned short sa_family_t;
typedef unsigned int in_addr_t;
typedef unsigned short in_port_t;
typedef unsigned long long rlim_t;
#define SIZE_MAX 0xffffffffU
#define SSIZE_MAX 0x7fffffff
#define INTPTR_MAX 0x7fffffff
#define UINTPTR_MAX 0xffffffffU
#define AF_UNSPEC 0
#define AF_UNIX 1
#define AF_INET 2
#define AF_INET6 10
#define PF_UNIX AF_UNIX
#define PF_INET AF_INET
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define SOL_SOCKET 1
#define SO_REUSEADDR 2
#define SO_ERROR 4
#define SO_KEEPALIVE 9
#define SO_RCVBUF 8
#define SO_SNDBUF 7
#define SO_LINGER 13
#define SO_OOBINLINE 10
#define IPPROTO_IP 0
#define IPPROTO_TCP 6
#define IPPROTO_IPV6 41
#define TCP_NODELAY 1
#define IP_TOS 1
#define IP_TTL 2
#define IPV6_V6ONLY 26
#define IPV6_TCLASS 67
#define INADDR_ANY 0
#define INADDR_LOOPBACK 0x7f000001
#define IN_LOOPBACKNET 127
#define INADDR_NONE 0xffffffffU
#define INET_ADDRSTRLEN 16
#define INET6_ADDRSTRLEN 46
#define IPPORT_RESERVED 1024
#define IPPORT_HIFIRSTAUTO 49152
#define IPPORT_HILASTAUTO 65535
#define SHUT_RD 0
#define SHUT_WR 1
#define SHUT_RDWR 2
struct in_addr { in_addr_t s_addr; };
struct in6_addr { unsigned char s6_addr[16]; };
struct sockaddr { sa_family_t sa_family; char sa_data[14]; };
struct sockaddr_in { sa_family_t sin_family; in_port_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct sockaddr_in6 { sa_family_t sin6_family; in_port_t sin6_port; unsigned sin6_flowinfo; struct in6_addr sin6_addr; unsigned sin6_scope_id; };
struct sockaddr_storage { sa_family_t ss_family; char data[126]; };
struct sockaddr_un { sa_family_t sun_family; char sun_path[108]; };
struct linger { int l_onoff,l_linger; };
struct iovec { void *iov_base; size_t iov_len; };
struct msghdr { void *msg_name; socklen_t msg_namelen; struct iovec *msg_iov; size_t msg_iovlen; void *msg_control; size_t msg_controllen; int msg_flags; };
struct cmsghdr { size_t cmsg_len; int cmsg_level,cmsg_type; };
#define SCM_RIGHTS 1
#define CMSG_ALIGN(n) (((n)+3)&~3)
#define CMSG_SPACE(n) (CMSG_ALIGN(sizeof(struct cmsghdr))+CMSG_ALIGN(n))
#define CMSG_LEN(n) (CMSG_ALIGN(sizeof(struct cmsghdr))+(n))
#define CMSG_DATA(c) ((unsigned char *)(c)+CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_FIRSTHDR(m) ((m)->msg_controllen>=sizeof(struct cmsghdr)?(struct cmsghdr *)(m)->msg_control:NULL)
#define CMSG_NXTHDR(m,c) ((char *)(c)+CMSG_ALIGN((c)->cmsg_len)+sizeof(struct cmsghdr)<=(char *)(m)->msg_control+(m)->msg_controllen?(struct cmsghdr *)((char *)(c)+CMSG_ALIGN((c)->cmsg_len)):NULL)
#define MSG_CTRUNC 8
#define MSG_PEEK 2
#define MSG_WAITALL 256
#define EAFNOSUPPORT 97
#define EOPNOTSUPP 95
#define ENOSYS 38
#define ENOTSOCK 88
#define EINPROGRESS 115
#define EWOULDBLOCK EAGAIN
#define ENOBUFS 105
#define ENOPROTOOPT 92
#define ENOTTY 25
#define EOVERFLOW 75
#define EDESTADDRREQ 89
#define EPROTO 71
#define EALREADY 114
#define EADDRNOTAVAIL 99
#define ECANCELED 125
#define ECONNABORTED 103
#define EPROTONOSUPPORT 93
#define O_NONBLOCK 04000
#define O_NOCTTY 0400
#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IRWXU 0700
#define S_IRWXG 0070
#define S_IRWXO 0007
#define FD_SETSIZE 16
typedef struct { unsigned bits; } fd_set;
#define FD_ZERO(s) ((s)->bits=0)
#define FD_SET(n,s) ((s)->bits|=1U<<(n))
#define FD_CLR(n,s) ((s)->bits&=~(1U<<(n)))
#define FD_ISSET(n,s) (((s)->bits>>(n))&1)
struct pollfd { int fd; short events,revents; };
typedef unsigned nfds_t;
#define POLLIN 1
#define POLLOUT 4
#define POLLERR 8
#define POLLHUP 16
#define POLLNVAL 32
struct addrinfo { int ai_flags,ai_family,ai_socktype,ai_protocol; socklen_t ai_addrlen; struct sockaddr *ai_addr; char *ai_canonname; struct addrinfo *ai_next; };
struct hostent { char *h_name; char **h_aliases; int h_addrtype,h_length; char **h_addr_list; };
#define h_addr h_addr_list[0]
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define AI_NUMERICSERV 1024
#define AI_ADDRCONFIG 32
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NAMEREQD 8
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define EAI_SYSTEM -11
#define EAI_FAMILY -6
#define EAI_NONAME -2
#define EAI_SERVICE -8
#define EAI_MEMORY -10
#define EAI_AGAIN -3
#define MAXHOSTNAMELEN 255
#define UINT32_MAX 0xffffffffU
#define UINT16_MAX 65535
#define INT64_MIN (-9223372036854775807LL-1LL)
#define ENFILE 23
#define PF_UNSPEC AF_UNSPEC
#define SIGSYS 31
#define IFF_UP 1
#define ITIMER_REAL 0
#define timerisset(t) ((t)->tv_sec || (t)->tv_usec)
#define timerclear(t) ((t)->tv_sec=(t)->tv_usec=0)
#define timeradd(a,b,r) do { (r)->tv_sec=(a)->tv_sec+(b)->tv_sec; (r)->tv_usec=(a)->tv_usec+(b)->tv_usec; if((r)->tv_usec>=1000000){(r)->tv_sec++;(r)->tv_usec-=1000000;} } while(0)
#define timersub(a,b,r) do { (r)->tv_sec=(a)->tv_sec-(b)->tv_sec; (r)->tv_usec=(a)->tv_usec-(b)->tv_usec; if((r)->tv_usec<0){(r)->tv_sec--;(r)->tv_usec+=1000000;} } while(0)
int sscanf(const char *,const char *,...);
int dup(int);
int execv(const char *,char *const[]);
int getpeereid(int,uid_t *,gid_t *);

struct itimerval { struct timeval it_interval,it_value; };
int setitimer(int,const struct itimerval *,struct itimerval *);
struct servent { char *s_name; char **s_aliases; int s_port; char *s_proto; };
struct servent *getservbyname(const char *,const char *);
struct ifaddrs { struct ifaddrs *ifa_next; char *ifa_name; unsigned ifa_flags; struct sockaddr *ifa_addr,*ifa_netmask,*ifa_broadaddr; void *ifa_data; };
int getifaddrs(struct ifaddrs **);
void freeifaddrs(struct ifaddrs *);
FILE *popen(const char *,const char *);
int pclose(FILE *);
char *ctime(const time_t *);
void *memmem(const void *,size_t,const void *,size_t);
#define timercmp(a,b,op) ((a)->tv_sec==(b)->tv_sec?(a)->tv_usec op (b)->tv_usec:(a)->tv_sec op (b)->tv_sec)

#define UINT64_MAX 0xffffffffffffffffULL
#define INT32_MAX 0x7fffffff
#define EPFNOSUPPORT EAFNOSUPPORT
#define ST_RDONLY 1
#define ST_NOSUID 2
#define O_ACCMODE 3
#define IF_NAMESIZE 16
#define IFNAMSIZ 16
#define IN6_IS_ADDR_LOOPBACK(a) 0
#define IN6_IS_ADDR_V4MAPPED(a) 0
#define IN6_IS_ADDR_LINKLOCAL(a) 0
#define IN6_IS_ADDR_UNSPECIFIED(a) 0
#define IN6_IS_ADDR_MULTICAST(a) 0
#define IN6_ARE_ADDR_EQUAL(a,b) (!memcmp((a),(b),16))
struct if_nameindex { unsigned if_index; char *if_name; };
unsigned if_nametoindex(const char *);
char *if_indextoname(unsigned,char *);
size_t strlcpy(char *,const char *,size_t);
size_t strlcat(char *,const char *,size_t);
size_t strnlen(const char *,size_t);
void bzero(void *,size_t);
int isascii(int);
int getdtablesize(void);
int getpagesize(void);
int getgrouplist(const char *,gid_t,gid_t *,int *);
int rmdir(const char *);
int execvp(const char *,char *const[]);
int execl(const char *,const char *,...);
int execlp(const char *,const char *,...);
void setpwent(void);
struct passwd *getpwent(void);
char *crypt(const char *,const char *);

#define ERRSET_SUCCESS 0
#define ERRSET_NOMEMORY 1
#define ERRSET_FAIL 2
#define ERRSET_INVAL 3
#define ERRSET_NONAME 4
#define ERRSET_NODATA 5
#define RRSET_VALIDATED 1
struct rdatainfo { unsigned rdi_length; unsigned char *rdi_data; };
struct rrsetinfo { unsigned rri_flags,rri_rdclass,rri_rdtype,rri_ttl,rri_nrdatas,rri_nsigs; char *rri_name; struct rdatainfo *rri_rdatas,*rri_sigs; };
int getrrsetbyname(const char *,unsigned,unsigned,unsigned,struct rrsetinfo **);
void freerrset(struct rrsetinfo *);

struct group { char *gr_name,*gr_passwd; gid_t gr_gid; char **gr_mem; };
struct rlimit { rlim_t rlim_cur,rlim_max; };
#define RLIM_INFINITY (~(rlim_t)0)
#define RLIMIT_NOFILE 7
#define RLIMIT_CORE 4
#define RLIMIT_FSIZE 1
#define RLIMIT_DATA 2
#define RLIMIT_STACK 3
#define RUSAGE_SELF 0
#define RUSAGE_CHILDREN -1
struct rusage { struct timeval ru_utime,ru_stime; long rest[14]; };
struct statvfs { unsigned long f_bsize,f_frsize,f_blocks,f_bfree,f_bavail,f_files,f_ffree,f_favail,f_fsid,f_flag,f_namemax; };
struct utimbuf { time_t actime,modtime; };
struct utsname { char sysname[65],nodename[65],release[65],version[65],machine[65]; };
#define WUNTRACED 2
#define WIFEXITED(s) (((s)&127)==0)
#define WEXITSTATUS(s) (((s)>>8)&255)
#define WIFSIGNALED(s) (((s)&127)!=0)
#define WTERMSIG(s) ((s)&127)
#define WIFSTOPPED(s) 0
#define WSTOPSIG(s) 0
#define SIG_ATOMIC_MAX 0x7fffffff
#define SA_RESTART 0x10000000
struct sigaction { void (*sa_handler)(int); sigset_t sa_mask; int sa_flags; };
#define CLOCK_MONOTONIC 1
#define CLOCK_REALTIME 0
#define _SC_OPEN_MAX 4
#define _SC_NGROUPS_MAX 3
#define _SC_CLK_TCK 2
#define _SC_PAGESIZE 30
#define NGROUPS_MAX 16
#define LOG_EMERG 0
#define LOG_ALERT 1
#define LOG_CRIT 2
#define LOG_ERR 3
#define LOG_WARNING 4
#define LOG_NOTICE 5
#define LOG_INFO 6
#define LOG_DEBUG 7
#define LOG_AUTH 32
#define LOG_AUTHPRIV 80
#define LOG_DAEMON 24
#define LOG_USER 8
#define LOG_LOCAL0 128
#define LOG_LOCAL1 136
#define LOG_LOCAL2 144
#define LOG_LOCAL3 152
#define LOG_LOCAL4 160
#define LOG_LOCAL5 168
#define LOG_LOCAL6 176
#define LOG_LOCAL7 184
#define LOG_PID 1
#define LOG_NDELAY 8
#define LOG_CONS 2
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
/* termios bits follow Linux numbering; adapter maps relevant modes to MyOS. */
typedef unsigned tcflag_t;
typedef unsigned speed_t;
typedef unsigned char cc_t;
#define NCCS 32
struct termios { tcflag_t c_iflag,c_oflag,c_cflag,c_lflag; cc_t c_line,c_cc[NCCS]; speed_t c_ispeed,c_ospeed; };
#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2
#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VEOL 11
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VREPRINT 12
#define VWERASE 14
#define VLNEXT 15
#define VDISCARD 13
#define VEOL2 16
#define IGNBRK 1
#define BRKINT 2
#define IGNPAR 4
#define PARMRK 8
#define INPCK 16
#define ISTRIP 32
#define INLCR 64
#define IGNCR 128
#define ICRNL 256
#define IXON 1024
#define IXANY 2048
#define IXOFF 4096
#define OPOST 1
#define ONLCR 4
#define ISIG 1
#define ICANON 2
#define ECHO 8
#define ECHOE 16
#define ECHOK 32
#define ECHONL 64
#define NOFLSH 128
#define TOSTOP 256
#define IEXTEN 32768
#define ECHOCTL 512
#define ECHOKE 2048
#define PENDIN 16384
#define CSIZE 48
#define CS7 32
#define CS8 48
#define PARENB 256
#define PARODD 512
#define CREAD 128
#define B0 0
#define B4800 12
#define B2400 11
#define B1800 10
#define B1200 9
#define B600 8
#define B300 7
#define B200 6
#define B150 5
#define B134 4
#define B110 3
#define B75 2
#define B50 1
#define B9600 13
#define B19200 14
#define B38400 15
#define B57600 4097
#define B115200 4098
#define TIOCSWINSZ 0x5414
#define TIOCSCTTY 0x540e
#define FIONREAD 0x541b
#define _POSIX_VDISABLE 0
#define _PATH_TTY "/dev/tty"
#define _PATH_DEVNULL "/dev/null"
#define _PATH_BSHELL "/bin/sh"
#define _PATH_STDPATH "/bin:/ext/bin"
#define _PATH_MAILDIR "/var/mail"
#define _PATH_LASTLOG "/var/log/lastlog"
unsigned short htons(unsigned short);
unsigned short ntohs(unsigned short);
unsigned htonl(unsigned);
unsigned ntohl(unsigned);
int socket(int,int,int);
int listen(int,int);
int socketpair(int,int,int,int[2]);
int connect(int,const struct sockaddr *,socklen_t);
int bind(int,const struct sockaddr *,socklen_t);
int accept(int,struct sockaddr *,socklen_t *);
int getsockname(int,struct sockaddr *,socklen_t *);
int getpeername(int,struct sockaddr *,socklen_t *);
int getsockopt(int,int,int,void *,socklen_t *);
int setsockopt(int,int,int,const void *,socklen_t);
int shutdown(int,int);
ssize_t send(int,const void *,size_t,int);
ssize_t recv(int,void *,size_t,int);
ssize_t sendmsg(int,const struct msghdr *,int);
ssize_t recvmsg(int,struct msghdr *,int);
ssize_t writev(int,const struct iovec *,int);
ssize_t readv(int,const struct iovec *,int);
int poll(struct pollfd *,nfds_t,int);
int ppoll(struct pollfd *,nfds_t,const struct timespec *,const sigset_t *);
int select(int,fd_set *,fd_set *,fd_set *,struct timeval *);
int getaddrinfo(const char *,const char *,const struct addrinfo *,struct addrinfo **);
void freeaddrinfo(struct addrinfo *);
const char *gai_strerror(int);
int getnameinfo(const struct sockaddr *,socklen_t,char *,socklen_t,char *,socklen_t,int);
int inet_pton(int,const char *,void *);
const char *inet_ntop(int,const void *,char *,socklen_t);
int inet_aton(const char *,struct in_addr *);
char *inet_ntoa(struct in_addr);
int gethostname(char *,size_t);
int uname(struct utsname *);
int getentropy(void *,size_t);
int getrlimit(int,struct rlimit *);
int setrlimit(int,const struct rlimit *);
int getrusage(int,struct rusage *);
long sysconf(int);
int clock_gettime(int,struct timespec *);
int nanosleep(const struct timespec *,struct timespec *);
int getgroups(int,gid_t *);
int setgroups(size_t,const gid_t *);
int initgroups(const char *,gid_t);
struct group *getgrgid(gid_t);
struct group *getgrnam(const char *);
void endgrent(void);
int setresuid(uid_t,uid_t,uid_t);
int setresgid(gid_t,gid_t,gid_t);
int seteuid(uid_t);
int setegid(gid_t);
int chroot(const char *);
int chown(const char *,uid_t,gid_t);
int fchown(int,uid_t,gid_t);
int ftruncate(int,off_t);
int truncate(const char *,off_t);
int fsync(int);
int fdatasync(int);
int utimes(const char *,const struct timeval[2]);
int futimes(int,const struct timeval[2]);
int statvfs(const char *,struct statvfs *);
int fstatvfs(int,struct statvfs *);
int link(const char *,const char *);
int flock(int,int);
int sigaction(int,const struct sigaction *,struct sigaction *);
int sigemptyset(sigset_t *);
int sigfillset(sigset_t *);
int sigaddset(sigset_t *,int);
int sigdelset(sigset_t *,int);
int sigismember(const sigset_t *,int);
int sigsuspend(const sigset_t *);
int killpg(pid_t,int);

pid_t wait(int *);
pid_t setsid(void);
pid_t getsid(pid_t);
pid_t getpgrp(void);
int tcgetattr(int,struct termios *);
int tcsetattr(int,int,const struct termios *);
speed_t cfgetispeed(const struct termios *);
speed_t cfgetospeed(const struct termios *);
int cfsetispeed(struct termios *,speed_t);
int cfsetospeed(struct termios *,speed_t);
void cfmakeraw(struct termios *);
int tcgetpgrp(int);
int tcsetpgrp(int,pid_t);
int openpty(int *,int *,char *,const struct termios *,const struct winsize *);
void openlog(const char *,int,int);
void closelog(void);
void syslog(int,const char *,...);
void vsyslog(int,const char *,va_list);
int asprintf(char **,const char *,...);
int vasprintf(char **,const char *,va_list);
char *realpath(const char *,char *);
char *sftp_realpath(const char *,char *);
void _exit(int) __attribute__((noreturn));
int myos_ssh_fcntl(int,int,...);
int myos_ssh_ioctl(int,unsigned long,...);
int myos_ssh_waitpid(int,int *,int);
int myos_ssh_execve(const char *,char *const[],char *const[]);
#define fcntl myos_ssh_fcntl
#define ioctl myos_ssh_ioctl
#define waitpid myos_ssh_waitpid
#define execve myos_ssh_execve
#endif
