#ifndef SYS_STAT_H
#define SYS_STAT_H
#include "sys/types.h"

struct stat {
    unsigned st_mode;
    unsigned st_size;
};

#define S_IFMT   0170000
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)

int fstat(int fd, struct stat *st);
#endif
