#ifndef ARPA_INET_H
#define ARPA_INET_H
#include "netinet/in.h"
unsigned short htons(unsigned short v);
unsigned short ntohs(unsigned short v);
unsigned htonl(unsigned v);
unsigned ntohl(unsigned v);
int inet_pton(int af, const char *s, void *dst);
const char *inet_ntop(int af, const void *src, char *dst, socklen_t n);
char *inet_ntoa(struct in_addr in);
#endif
