#ifndef SYS_SELECT_H
#define SYS_SELECT_H
#include "sys/time.h"
#define FD_SETSIZE 64
typedef struct { unsigned bits[FD_SETSIZE / 32]; } fd_set;
#define FD_ZERO(s) do { unsigned _i; for (_i = 0; _i < sizeof(fd_set) / sizeof(unsigned); _i++) (s)->bits[_i] = 0; } while (0)
#define FD_SET(n, s) ((s)->bits[(n) / 32] |= 1u << ((n) % 32))
#define FD_CLR(n, s) ((s)->bits[(n) / 32] &= ~(1u << ((n) % 32)))
#define FD_ISSET(n, s) ((s)->bits[(n) / 32] & (1u << ((n) % 32)))
int select(int nfds, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv);
#endif
