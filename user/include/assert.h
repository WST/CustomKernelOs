#ifndef ASSERT_H
#define ASSERT_H
#include "stdlib.h"
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
void __assert_fail(const char *file, int line, const char *expr);
#define assert(e) ((e) ? (void)0 : __assert_fail(__FILE__, __LINE__, #e))
#endif
#ifndef static_assert
#define static_assert(expr, msg) _Static_assert(expr, msg)
#endif
#endif
