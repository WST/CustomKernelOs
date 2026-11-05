#ifndef STRING_H
#define STRING_H
#include "types.h"

void  *memset(void *dst, int c, size_t n);
void  *memcpy(void *dst, const void *src, size_t n);
void  *memmove(void *dst, const void *src, size_t n);
size_t strlen(const char *s);
int    strcmp(const char *a, const char *b);
int    strncmp(const char *a, const char *b, size_t n);
int    atoi(const char *s);
u32    strtou(const char *s, const char **end);

#endif
