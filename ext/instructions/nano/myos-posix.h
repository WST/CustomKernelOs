#ifndef MYOS_NANO_POSIX_H
#define MYOS_NANO_POSIX_H
#include <myos.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef ENOSYS
#define ENOSYS 38
#endif
#include <unistd.h>
#include <strings.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>
#include <setjmp.h>
#undef MB_CUR_MAX
#define MB_CUR_MAX 4
#define P_tmpdir "/tmp"
#define SA_RESETHAND 1
struct sigaction {void (*sa_handler)(int);sigset_t sa_mask;int sa_flags;};
int nano_sigaction(int,const struct sigaction*,struct sigaction*);
#define sigaction(...) nano_sigaction(__VA_ARGS__)
int sigemptyset(sigset_t*);int sigaddset(sigset_t*,int);int sigfillset(sigset_t*);
int nano_sigprocmask(int,const sigset_t*,sigset_t*);
#define sigprocmask nano_sigprocmask
int nano_kill(int,int);
#define kill nano_kill
int putenv(char*);
struct passwd;struct passwd *getpwent(void);
#define O_NONBLOCK 04000
int mkstemps(char*,int);
int fsync(int);int futimens(int,const struct timespec*);
char *realpath(const char*,char*);int mkstemp(char*);int fchmod(int,unsigned);int fchown(int,unsigned,unsigned);
int nano_fcntl(int,int,...);
#define fcntl nano_fcntl
int nano_unlink(const char*);
#define unlink nano_unlink
int nano_gettimeofday(struct timeval*,void*);
#define gettimeofday nano_gettimeofday
int nano_tcgetattr(int,void*);int nano_tcsetattr(int,int,const void*);
int nano_mbrtowc(wchar_t*,const char*,unsigned,void*);int nano_wctomb(char*,wchar_t);int nano_wcwidth(wchar_t);
#define mbrtowc nano_mbrtowc
#define wctomb nano_wctomb
#define wcwidth nano_wcwidth
int nano_iswalnum(wchar_t);int nano_iswblank(wchar_t);int nano_iswpunct(wchar_t);int nano_iswspace(wchar_t);
int nano_iswcntrl(wchar_t);int nano_iswalpha(wchar_t);wchar_t nano_towlower(wchar_t);wchar_t nano_towupper(wchar_t);
#define iswalnum nano_iswalnum
#define iswblank nano_iswblank
#define iswpunct nano_iswpunct
#define iswspace nano_iswspace
#define iswcntrl nano_iswcntrl
#define iswalpha nano_iswalpha
#define towlower nano_towlower
#define towupper nano_towupper
#include <regex.h>
#define REG_STARTEND 8
int nano_regexec(const regex_t*,const char*,unsigned,regmatch_t*,int);
#define regexec nano_regexec
char *strcasestr(const char*,const char*);
void nano_terminal_restore(const void*);
void nano_signal_resize(void);
#endif
