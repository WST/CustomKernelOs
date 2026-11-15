#ifndef WCHAR_H
#define WCHAR_H
#include "stddef.h"
int mbtowc(wchar_t *wc, const char *s, size_t n);
int wctomb(char *s, wchar_t wc);
size_t mbstowcs(wchar_t *dest, const char *src, size_t n);
size_t wcstombs(char *dest, const wchar_t *src, size_t n);
int wcscmp(const wchar_t *a, const wchar_t *b);
int wcsncmp(const wchar_t *a, const wchar_t *b, size_t n);
size_t wcslen(const wchar_t *s);
wchar_t *wcschr(const wchar_t *s, wchar_t c);
wchar_t *wcsrchr(const wchar_t *s, wchar_t c);
wchar_t *wcscpy(wchar_t *dst, const wchar_t *src);
wchar_t *wcsncpy(wchar_t *dst, const wchar_t *src, size_t n);
wchar_t *wcsdup(const wchar_t *s);
wchar_t *wmemchr(const wchar_t *s, wchar_t c, size_t n);
long wcstol(const wchar_t *s, wchar_t **end, int base);
wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save);
#ifndef MB_CUR_MAX
#define MB_CUR_MAX 4
#endif
#endif
