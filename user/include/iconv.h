/* iconv.h — перекодирование между UTF-8/16/32, Latin-1, CP1251, KOI8-R и ASCII. */
#ifndef ICONV_H
#define ICONV_H
#include "stddef.h"

typedef void *iconv_t;

iconv_t iconv_open(const char *tocode, const char *fromcode);
size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft);
int iconv_close(iconv_t cd);

#endif
