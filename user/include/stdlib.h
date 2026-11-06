#ifndef STDLIB_H
#define STDLIB_H
#include "stddef.h"
void abort(void) __attribute__((noreturn));
extern char *__progname;
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
char *strdup(const char *s);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
char *getenv(const char *name);
long long strtonum(const char *nptr, long long minval, long long maxval, const char **errstr);
#endif
