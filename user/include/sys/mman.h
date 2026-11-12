#ifndef SYS_MMAN_H
#define SYS_MMAN_H
#ifndef PROT_NONE
#define PROT_NONE  0
#endif
#ifndef PROT_READ
#define PROT_READ  1
#endif
#ifndef PROT_WRITE
#define PROT_WRITE 2
#endif
#ifndef PROT_EXEC
#define PROT_EXEC  4
#endif
#ifndef MAP_SHARED
#define MAP_SHARED 1
#endif
#ifndef MAP_PRIVATE
#define MAP_PRIVATE 2
#endif
#ifndef MAP_ANON
#define MAP_ANON   0x20
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#define MAP_FAILED ((void *)-1)
void *mmap(void *addr, unsigned len, int prot, int flags, int fd, unsigned off);
int munmap(void *addr, unsigned len);
int mprotect(void *addr, unsigned len, int prot);
#endif
