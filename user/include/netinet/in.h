#ifndef NETINET_IN_H
#define NETINET_IN_H
#include "sys/socket.h"
typedef unsigned in_addr_t;
typedef unsigned short in_port_t;
struct in_addr { in_addr_t s_addr; };
struct sockaddr_in {
    sa_family_t sin_family;
    in_port_t sin_port;
    struct in_addr sin_addr;
    unsigned char sin_zero[8];
};
#define INADDR_ANY ((in_addr_t)0)
#define INADDR_LOOPBACK ((in_addr_t)0x7f000001)
#endif
