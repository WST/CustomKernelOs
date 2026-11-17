/* pcre.h — классический PCRE, не PCRE2.
 * Литералы, . * + ? {n,m}, классы, ^ $, группы, |, \d \w \s.
 * Без lookaround, рекурсии и свойств Unicode. */
#ifndef PCRE_H
#define PCRE_H
#include "stddef.h"

#define PCRE_CASELESS  0x00000001
#define PCRE_MULTILINE 0x00000002
#define PCRE_DOTALL    0x00000004
#define PCRE_UTF8      0x00000800

#define PCRE_ERROR_NOMATCH        (-1)
#define PCRE_ERROR_NULL           (-2)
#define PCRE_ERROR_BADOPTION      (-3)
#define PCRE_ERROR_BADMAGIC       (-4)
#define PCRE_ERROR_NOMEMORY       (-6)

typedef struct pcre pcre;
typedef struct pcre_extra { void *unused; } pcre_extra;

pcre *pcre_compile(const char *pattern, int options, const char **errptr, int *erroffset,
                   const unsigned char *tableptr);
int pcre_exec(const pcre *code, const pcre_extra *extra, const char *subject, int length,
              int startoffset, int options, int *ovector, int ovecsize);

extern void *(*pcre_malloc)(size_t);
extern void (*pcre_free)(void *);

#endif
