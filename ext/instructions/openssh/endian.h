#include "myos-posix.h"
#define LITTLE_ENDIAN 1234
#define BIG_ENDIAN 4321
#define BYTE_ORDER LITTLE_ENDIAN
#define le32toh(x) (x)
#define le64toh(x) (x)
#define htole32(x) (x)
#define htole64(x) (x)
#define htobe32(x) htonl(x)
#define be32toh(x) ntohl(x)
