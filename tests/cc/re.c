/* regcomp/regexec: литерал, точка, звёздочка, класс, якоря, регистр, ссылка назад. */
#include <regex.h>
#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static int match(const char *pat, const char *s, int flags) {
    regex_t re;
    int rc = regcomp(&re, pat, flags);
    int m;
    if (rc) { regfree(&re); return -rc; }
    m = regexec(&re, s, 0, NULL, 0);
    regfree(&re);
    return m;
}

int main(void) {
    regex_t re;
    regmatch_t rm[3];
    char err[64];

    CHECK(match("cat", "a cat!", 0) == 0);
    CHECK(match("dog", "a cat!", 0) == REG_NOMATCH);
    CHECK(match("c.t", "cat", 0) == 0);
    CHECK(match("c.t", "ct", 0) == REG_NOMATCH);
    CHECK(match("a*", "bbb", 0) == 0);
    CHECK(match("ab*c", "ac", 0) == 0);
    CHECK(match("ab*c", "abbbc", 0) == 0);
    CHECK(match("ab*c", "ab", 0) == REG_NOMATCH);
    CHECK(match("[abc]", "xby", 0) == 0);
    CHECK(match("[^abc]", "b", 0) == REG_NOMATCH);
    CHECK(match("[^abc]", "z", 0) == 0);
    CHECK(match("^cat", "cat", 0) == 0);
    CHECK(match("^cat", "a cat", 0) == REG_NOMATCH);
    CHECK(match("cat$", "the cat", 0) == 0);
    CHECK(match("cat$", "cat!", 0) == REG_NOMATCH);
    CHECK(match("Cat", "cat", REG_ICASE) == 0);
    CHECK(match("Cat", "cat", 0) == REG_NOMATCH);

    CHECK(regcomp(&re, "\\(ab\\)\\1", 0) == 0);
    CHECK(regexec(&re, "abab", 3, rm, 0) == 0);
    CHECK(rm[0].rm_so == 0 && rm[0].rm_eo == 4);
    CHECK(rm[1].rm_so == 0 && rm[1].rm_eo == 2);
    CHECK(regexec(&re, "abba", 0, NULL, 0) == REG_NOMATCH);
    regfree(&re);

    CHECK(regcomp(&re, "a|ab", REG_EXTENDED) == 0);
    CHECK(regexec(&re, "ab", 0, NULL, 0) == 0);
    regfree(&re);
    {
        char buf[96];
        int i;
        for (i = 0; i < 80; i++) buf[i] = 'a';
        buf[80] = 0;
        CHECK(match("a*", buf, 0) == 0);
        CHECK(match("a*b", buf, 0) == REG_NOMATCH);
        buf[70] = 'b';
        buf[71] = 0;
        CHECK(match("a*b", buf, 0) == 0);
        CHECK(match("foo.*bar", "foo and then bar", 0) == 0);
        CHECK(match("^a.*z$", "abcxyz", 0) == 0);
        CHECK(match("^a.*z$", "abcxy", 0) == REG_NOMATCH);
    }
    CHECK(regcomp(&re, "\\(", 0) != 0);
    CHECK(regerror(REG_BADPAT, NULL, err, sizeof err) > 0 && err[0]);

    return fails != 0;
}
