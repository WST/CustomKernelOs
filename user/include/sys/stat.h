#ifndef SYS_STAT_H
#define SYS_STAT_H
#include "sys/types.h"

struct stat {
    unsigned st_mode;
    unsigned st_size;
};

#define S_IFMT   0170000
#define S_IFCHR  0020000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

int fstat(int fd, struct stat *st);
int stat(const char *path, struct stat *st);
int lstat(const char *path, struct stat *st);
#endif
