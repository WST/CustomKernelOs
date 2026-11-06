#ifndef LOCALE_H
#define LOCALE_H
#define LC_ALL   0
#define LC_CTYPE 1
char *setlocale(int category, const char *locale);
#endif
