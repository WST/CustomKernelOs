/* Small native adapter. Unsupported persistence/job-control operations return
 * ENOSYS; no successful signal delivery or fsync is simulated. */
#include "config.h"
#include "termios.h"
#include "getopt.h"
#include <fcntl.h>
#include <pwd.h>
#include <ctype.h>
#include <string.h>
#include <locale.h>
#undef unlink
#undef fcntl
#undef kill
#undef regexec
static int raw(int number,int a,int b,int c){
#ifdef __MYOS_CC__
 return __syscall(number,a,b,c);
#else
 int out;__asm__ volatile("int $0x80":"=a"(out):"a"(number),"b"(a),"c"(b),"d"(c):"memory");return out;
#endif
}
static int result(int n){if(n<0){errno=-n;return -1;}return n;}
static int ctl(int op,int fd,int *values){return result(raw(SYS_SSHCTL,op,fd,(int)values));}
static struct sigaction handlers[NSIG];
int nano_sigaction(int sig,const struct sigaction *in,struct sigaction *old){if(sig<=0||sig>=NSIG){errno=EINVAL;return -1;}if(old)*old=handlers[sig];if(in)handlers[sig]=*in;return 0;}
int sigemptyset(sigset_t *s){*s=0;return 0;}
int sigfillset(sigset_t *s){*s=-1;return 0;}
int sigaddset(sigset_t *s,int n){if(n>0&&n<32)*s|=1u<<n;return 0;}
int nano_sigprocmask(int how,const sigset_t *in,sigset_t *old){(void)how;(void)in;if(old)*old=0;return 0;}
void nano_signal_resize(void){void(*fn)(int)=handlers[SIGWINCH].sa_handler;if((unsigned)fn>1 && fn!=SIG_ERR)fn(SIGWINCH);}
int nano_kill(int pid,int sig){if(sig==SIGTERM || sig==SIGKILL || sig==SIGHUP || sig==SIGINT)return result(kill(pid));errno=ENOSYS;return -1;}
int nano_fcntl(int fd,int cmd,...){int v[2]={cmd,0};if(cmd==F_SETFL||cmd==F_SETFD){va_list ap;va_start(ap,cmd);v[1]=va_arg(ap,int);va_end(ap);}return ctl(7,fd,v);}
int nano_unlink(const char *p){return result(unlink(p));}
int fsync(int fd){(void)fd;errno=ENOSYS;return -1;}
int futimens(int fd,const struct timespec *times){(void)fd;(void)times;errno=ENOSYS;return -1;}
int nano_gettimeofday(struct timeval *out,void *zone){(void)zone;out->tv_sec=time(NULL);out->tv_usec=(ticks()%1000)*1000;return 0;}
/* putenv keeps the caller's string, as required by its API. */
extern char **environ;
int putenv(char *s){
 static char **owned;
 char *equal=s?strchr(s,'='):NULL;
 if(!equal || equal==s){errno=EINVAL;return -1;}
 unsigned name=equal-s,count=0;
 while(environ && environ[count]){
  if(!strncmp(environ[count],s,name) && environ[count][name]=='='){
   environ[count]=s;return 0;
  }
  count++;
 }
 char **next=malloc((count+2)*sizeof(*next));
 if(!next){errno=ENOMEM;return -1;}
 for(unsigned i=0;i<count;i++)next[i]=environ[i];
 next[count]=s;next[count+1]=NULL;
 free(owned);owned=next;environ=next;return 0;
}
char *nl_langinfo(int item){(void)item;return "UTF-8";}
static int passwd_index;
struct passwd *getpwent(void){char name[24];if(account_list(name,passwd_index++)<0){passwd_index=0;return NULL;}return getpwnam(name);}
int nano_tcgetattr(int fd,void *ptr){struct termios *t=ptr;int v[2]={0,0};if(ctl(11,fd,v)<0)return -1;memset(t,0,sizeof(*t));t->myos_mode=v[1];t->c_lflag=(v[1]&TTY_RAW?0:ICANON|ECHO)|(v[1]&TTY_NOSIG?0:ISIG);int output=ttyctl(fd,TTYCTL_GETOFLAGS,0);t->c_oflag=output<0?(OPOST|ONLCR):output;t->c_iflag=ICRNL|IXON;return 0;}
int nano_tcsetattr(int fd,int how,const void *ptr){(void)how;const struct termios *t=ptr;int v[2]={1,TTY_NOSIG};if(!(t->c_lflag&ICANON))v[1]|=TTY_RAW|TTY_NONBLOCK|TTY_RESIZE_EVENT;if(ctl(11,fd,v)<0)return -1;if(ttyctl(fd,TTYCTL_GETOFLAGS,0)>=0)return result(ttyctl(fd,TTYCTL_SETOFLAGS,t->c_oflag&(OPOST|ONLCR)));return 0;}
void nano_terminal_restore(const void *ptr){const struct termios *t=ptr;int v[2]={1,t->myos_mode};ctl(11,0,v);if(ttyctl(0,TTYCTL_GETOFLAGS,0)>=0)ttyctl(0,TTYCTL_SETOFLAGS,t->c_oflag&(OPOST|ONLCR));}
int nano_mbrtowc(wchar_t *wc,const char *s,unsigned n,void *state){(void)state;if(!s)return 0;if(!n)return -2;unsigned c=(unsigned char)s[0],len,min;if(c<128){if(wc)*wc=c;return c?1:0;}if(c>=0xc2&&c<=0xdf){len=2;c&=31;min=0x80;}else if(c>=0xe0&&c<=0xef){len=3;c&=15;min=0x800;}else if(c>=0xf0&&c<=0xf4){len=4;c&=7;min=0x10000;}else return -1;if(n<len)return -2;for(unsigned i=1;i<len;i++){unsigned ch=(unsigned char)s[i];if((ch&0xc0)!=0x80)return -1;c=(c<<6)|(ch&63);}if(c<min || c>0x10ffff || (c>=0xd800&&c<=0xdfff))return -1;if(wc)*wc=c;return len;}
int nano_wctomb(char *s,wchar_t c){if(!s)return 0;if(c<128){s[0]=c;return 1;}if(c<0x800){s[0]=0xc0|(c>>6);s[1]=0x80|(c&63);return 2;}if(c>=0xd800&&c<=0xdfff)return -1;if(c<0x10000){s[0]=0xe0|(c>>12);s[1]=0x80|((c>>6)&63);s[2]=0x80|(c&63);return 3;}if(c>0x10ffff)return -1;s[0]=0xf0|(c>>18);s[1]=0x80|((c>>12)&63);s[2]=0x80|((c>>6)&63);s[3]=0x80|(c&63);return 4;}
int nano_wcwidth(wchar_t c){if(!c)return 0;if(c<32 || (c>=127&&c<160))return -1;if((c>=0x300&&c<=0x36f)||(c>=0x1ab0&&c<=0x1aff)||(c>=0xfe00&&c<=0xfe0f))return 0;return c>=0x1100 && ((c<=0x115f)||(c>=0x2e80&&c<=0xa4cf)||(c>=0xac00&&c<=0xd7a3)||(c>=0xf900&&c<=0xfaff)||(c>=0xff01&&c<=0xff60)||(c>=0x1f300&&c<=0x1faff))?2:1;}
int nano_iswspace(wchar_t c){return c<128?isspace(c):c==0xa0 || (c>=0x2000&&c<=0x200a)||c==0x2028||c==0x2029||c==0x3000;}
int nano_iswblank(wchar_t c){return c==' '||c=='\t'||c==0xa0||c==0x3000;}
int nano_iswalpha(wchar_t c){return c<128?isalpha(c):(c>=0xc0&&c<=0x2af)||(c>=0x370&&c<=0x52f)||(c>=0x4e00&&c<=0x9fff);}
int nano_iswalnum(wchar_t c){return nano_iswalpha(c)||(c>='0'&&c<='9');}
int nano_iswcntrl(wchar_t c){return c<32||(c>=127&&c<160);}
int nano_iswpunct(wchar_t c){return c<128?ispunct(c):!nano_iswalnum(c)&&!nano_iswspace(c)&&!nano_iswcntrl(c);}
wchar_t nano_towlower(wchar_t c){if(c<128)return tolower(c);if((c>=0xc0&&c<=0xd6)||(c>=0xd8&&c<=0xde)||(c>=0x410&&c<=0x42f))return c+32;if(c>=0x400&&c<=0x40f)return c+80;return c;}
wchar_t nano_towupper(wchar_t c){if(c<128)return toupper(c);if((c>=0xe0&&c<=0xf6)||(c>=0xf8&&c<=0xfe)||(c>=0x430&&c<=0x44f))return c-32;if(c>=0x450&&c<=0x45f)return c-80;return c;}
int nano_regexec(const regex_t *re,const char *text,unsigned n,regmatch_t *matches,int flags){if(!(flags&REG_STARTEND))return regexec(re,text,n,matches,flags);if(!n||matches[0].rm_so<0||matches[0].rm_eo<matches[0].rm_so)return REG_NOMATCH;int start=matches[0].rm_so,end=matches[0].rm_eo;char *copy=strndup(text+start,end-start);if(!copy)return REG_ESPACE;int r=regexec(re,copy,n,matches,flags&~REG_STARTEND);if(!r)for(unsigned i=0;i<n;i++)if(matches[i].rm_so>=0){matches[i].rm_so+=start;matches[i].rm_eo+=start;}free(copy);return r;}
int getopt_long(int argc,char *const argv[],const char *shorts,const struct option *opts,int *index){if(optind<argc&&!strncmp(argv[optind],"--",2)&&argv[optind][2]){char *arg=argv[optind++]+2,*equal=strchr(arg,'=');int len=equal?equal-arg:(int)strlen(arg);for(int i=0;opts[i].name;i++)if((int)strlen(opts[i].name)==len&&!strncmp(opts[i].name,arg,len)){if(index)*index=i;optarg=equal?equal+1:NULL;if(opts[i].has_arg==required_argument&&!optarg){if(optind==argc)return '?';optarg=argv[optind++];}if(!opts[i].has_arg&&optarg)return '?';if(opts[i].flag){*opts[i].flag=opts[i].val;return 0;}return opts[i].val;}return '?';}return getopt(argc,argv,shorts);}
int mkstemps(char *path,int suffix){int len=strlen(path),at=len-suffix-6;if(at<0 || strncmp(path+at,"XXXXXX",6)){errno=EINVAL;return -1;}static unsigned counter;for(int attempt=0;attempt<128;attempt++){unsigned n=(getpid()*2654435761u)^ticks()^++counter;for(int i=0;i<6;i++){path[at+i]="abcdefghijklmnopqrstuvwxyz0123456789"[n%36];n=n/36+counter*17;}int fd=open(path,O_RDWR|O_CREAT|O_EXCL,0600);if(fd>=0 || errno!=EEXIST)return fd;}errno=EEXIST;return -1;}
int mkstemp(char *path){return mkstemps(path,0);}
char *realpath(const char *path,char *resolved){char pending[4096],out[4096];if(!path||!*path){errno=ENOENT;return NULL;}if(*path=='/'){if(strlen(path)>=sizeof(pending)){errno=ENAMETOOLONG;return NULL;}strcpy(pending,path);}else{if(!getcwd(out,sizeof(out)))return NULL;if(snprintf(pending,sizeof(pending),"%s/%s",out,path)>=(int)sizeof(pending)){errno=ENAMETOOLONG;return NULL;}}int links=0;out[0]='/';out[1]=0;char *at=pending;while(*at){while(*at=='/')at++;if(!*at)break;char name[256];int n=0;while(*at&&*at!='/'){if(n==255){errno=ENAMETOOLONG;return NULL;}name[n++]=*at++;}name[n]=0;if(!strcmp(name,"."))continue;if(!strcmp(name,"..")){char *last=strrchr(out,'/');if(last==out)out[1]=0;else if(last)*last=0;continue;}int base=strlen(out);if(base+n+2>=(int)sizeof(out)){errno=ENAMETOOLONG;return NULL;}if(base>1)strcat(out,"/");strcat(out,name);struct stat st;if(lstat(out,&st)<0)return NULL;if(S_ISLNK(st.st_mode)){if(++links>40){errno=ELOOP;return NULL;}char target[4096],rest[4096];int got=readlink(out,target,sizeof(target)-1);if(got<0)return NULL;target[got]=0;if(snprintf(rest,sizeof(rest),"%s%s",target,at)>=(int)sizeof(rest)){errno=ENAMETOOLONG;return NULL;}strcpy(pending,rest);at=pending;if(target[0]=='/')out[1]=0;else out[base]=0;}}if(!resolved)return strdup(out);strcpy(resolved,out);return resolved;}
