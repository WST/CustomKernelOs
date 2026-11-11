#ifndef WCHAR_H
#define WCHAR_H
#include "stddef.h"
int mbtowc(wchar_t *wc, const char *s, size_t n);
#ifndef MB_CUR_MAX
#define MB_CUR_MAX 1
#endif
#endif
