#ifndef STDLIB_H
#define STDLIB_H
#include "stddef.h"
void abort(void) __attribute__((noreturn));
extern char *__progname;
#endif
