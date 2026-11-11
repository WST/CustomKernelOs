#ifndef STRING_H
#define STRING_H
#include "stddef.h"
void *memchr(const void *s, int c, size_t n);
char *strncat(char *dst, const char *src, size_t n);
size_t strspn(const char *s, const char *accept);
size_t strcspn(const char *s, const char *reject);
char *strpbrk(const char *s, const char *accept);
int strcoll(const char *a, const char *b);
char *strtok(char *s, const char *delim);
void bcopy(const void *src, void *dst, size_t n);
#endif
