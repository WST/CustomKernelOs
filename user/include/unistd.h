#ifndef UNISTD_H
#define UNISTD_H
#include "sys/types.h"
#include "myos.h"
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4
int access(const char *path, int mode);
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);
int pledge(const char *promises, const char *execpromises);
int isatty(int fd);
char *ttyname(int fd);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
int execve(const char *path, char *const argv[], char *const envp[]);
unsigned sleep(unsigned seconds);
unsigned alarm(unsigned seconds);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
#endif
