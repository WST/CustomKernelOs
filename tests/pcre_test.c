/* Обычные шаблоны pcre_compile / pcre_exec. */
#include "myos.h"
#include "stdio.h"
#include "pcre.h"

static int hits(const char *pat, const char *text, int flags) {
    const char *err;
    int off, rc;
    pcre *re = pcre_compile(pat, flags, &err, &off, NULL);
    if (!re) { fprintf(stderr, "compile %s\n", pat); return -1; }
    rc = pcre_exec(re, NULL, text, (int)strlen(text), 0, 0, NULL, 0);
    pcre_free(re);
    return rc;
}

int main(void) {
    if (hits("a.c", "abc", 0) < 1) return 1;
    if (hits("\\d+", "id 42", 0) < 1) return 1;
    if (hits("^z", "az", 0) != PCRE_ERROR_NOMATCH) return 1;
    if (hits("a+", "AAA", PCRE_CASELESS) < 1) return 1;
    if (hits("ab|cd", "xcd", 0) < 1) return 1;
    printf("pcre ok\n");
    return 0;
}
