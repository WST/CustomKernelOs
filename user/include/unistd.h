#ifndef UNISTD_H
#define UNISTD_H
#include "sys/types.h"
#include "myos.h"
#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2
extern char *optarg;
extern int optind, opterr, optopt;
int getopt(int argc, char *const argv[], const char *optstring);
int pledge(const char *promises, const char *execpromises);
int isatty(int fd);
int symlink(const char *target, const char *linkpath);
ssize_t readlink(const char *path, char *buf, size_t bufsiz);
unsigned sleep(unsigned seconds);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
#endif
