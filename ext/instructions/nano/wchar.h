#ifndef NANO_WCHAR_H
#define NANO_WCHAR_H
#include <stddef.h>
typedef unsigned mbstate_t;
int nano_mbrtowc(wchar_t*,const char*,unsigned,void*);
int nano_wctomb(char*,wchar_t);int nano_wcwidth(wchar_t);
#endif
