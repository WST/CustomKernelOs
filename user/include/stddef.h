#ifndef STDDEF_H
#define STDDEF_H
#include "myos.h"
typedef int ptrdiff_t;
typedef int wchar_t;
#define offsetof(st, m) ((size_t)((char *)&((st *)0)->m - (char *)0))
#endif
