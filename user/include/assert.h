#ifndef ASSERT_H
#define ASSERT_H
#include "stdlib.h"
#ifdef NDEBUG
#define assert(e) ((void)0)
#else
#define assert(e) ((e) ? (void)0 : abort())
#endif
#endif
