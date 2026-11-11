#ifndef MYOS_EMU_PORT_H
#define MYOS_EMU_PORT_H
#include "../../kernel/heap.h"
#include "../../kernel/string.h"
#define malloc kmalloc
#define calloc kcalloc
#define free kfree
typedef unsigned long time_t;
static inline time_t time(void *p) { (void)p; return 0; }
/* Tracing is disabled in production; no libc formatting dependency. */
static inline int vsnprintf(char *s,size_t n,const char *f,va_list a) {
    (void)f;(void)a;if(n)*s=0;return 0;
}
static inline char *strcat(char *d,const char *s) {strcpy(d+strlen(d),s);return d;}
#endif
