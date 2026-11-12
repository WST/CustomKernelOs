#ifndef FCNTL_H
#define FCNTL_H
#include "myos.h"

#define F_DUPFD 0
#define F_GETFD 1
#define F_SETFD 2
#define F_GETFL 3
#define F_SETFL 4
#define FD_CLOEXEC 1
#define O_NONBLOCK 04000

int fcntl(int fd, int cmd, ...);
#endif
