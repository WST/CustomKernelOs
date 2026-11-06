#ifndef ERR_H
#define ERR_H
#include "stdarg.h"
void warn(const char *fmt, ...);
void warnx(const char *fmt, ...);
void err(int status, const char *fmt, ...) __attribute__((noreturn));
void errx(int status, const char *fmt, ...) __attribute__((noreturn));
#endif
