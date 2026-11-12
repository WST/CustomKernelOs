#ifndef NETDB_H
#define NETDB_H
#include "netinet/in.h"
#define AI_PASSIVE 1
#define AI_CANONNAME 2
#define AI_NUMERICHOST 4
#define NI_NUMERICHOST 1
#define NI_NUMERICSERV 2
#define NI_NAMEREQD 8
#define NI_MAXHOST 1025
#define NI_MAXSERV 32
#define EAI_NONAME -2
#define EAI_FAIL -4
#define EAI_FAMILY -6
#define EAI_SERVICE -8
#define EAI_MEMORY -10
struct addrinfo {
    int ai_flags, ai_family, ai_socktype, ai_protocol;
    socklen_t ai_addrlen;
    struct sockaddr *ai_addr;
    char *ai_canonname;
    struct addrinfo *ai_next;
};
struct hostent {
    char *h_name;
    char **h_aliases;
    int h_addrtype;
    int h_length;
    char **h_addr_list;
};
struct servent {
    char *s_name;
    char **s_aliases;
    int s_port;
    char *s_proto;
};
extern int h_errno;
int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
void freeaddrinfo(struct addrinfo *res);
const char *gai_strerror(int err);
int getnameinfo(const struct sockaddr *sa, socklen_t len, char *host, socklen_t hostlen, char *serv, socklen_t servlen, int flags);
struct hostent *gethostbyname(const char *name);
struct servent *getservbyname(const char *name, const char *proto);
#endif
