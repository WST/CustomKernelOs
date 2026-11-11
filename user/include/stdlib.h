#ifndef STDLIB_H
#define STDLIB_H
#include "stddef.h"
#include "stdint.h"
void abort(void) __attribute__((noreturn));
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1
extern char *__progname;
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
long long strtoll(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
int system(const char *command);
int abs(int x);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
intmax_t strtoimax(const char *nptr, char **endptr, int base);
uintmax_t strtoumax(const char *nptr, char **endptr, int base);
char *strdup(const char *s);
char *strndup(const char *s, size_t n);
void qsort(void *base, size_t nmemb, size_t size, int (*compar)(const void *, const void *));
char *getenv(const char *name);
char *mktemp(char *template);
long long strtonum(const char *nptr, long long minval, long long maxval, const char **errstr);
#endif
