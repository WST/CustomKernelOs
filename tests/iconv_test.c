/* Кириллица туда и обратно: UTF-8, CP1251, KOI8-R. */
#include "myos.h"
#include "stdio.h"
#include "string.h"
#include "iconv.h"

static int fails;

static int convert(const char *to, const char *from, const char *in, int n, char *out, int cap) {
    iconv_t cd = iconv_open(to, from);
    char *ip = (char *)in, *op = out;
    size_t il = (size_t)n, ol = (size_t)cap, r;
    if (cd == (iconv_t)-1) return -1;
    r = iconv(cd, &ip, &il, &op, &ol);
    iconv_close(cd);
    if (r == (size_t)-1 || il) return -1;
    return cap - (int)ol;
}

int main(void) {
    const char *utf = "Привет";
    char cp[16], back[16], koi[16];
    int n = convert("CP1251", "UTF-8", utf, (int)strlen(utf), cp, sizeof cp);
    int b, k;
    static const unsigned char expect_cp[] = { 0xcf, 0xf0, 0xe8, 0xe2, 0xe5, 0xf2 };
    static const unsigned char expect_koi[] = { 0xf0, 0xd2, 0xc9, 0xd7, 0xc5, 0xd4 };
    if (n != 6 || memcmp(cp, expect_cp, 6)) { fprintf(stderr, "cp1251\n"); fails++; }
    b = convert("UTF-8", "CP1251", cp, n, back, sizeof back);
    if (b != (int)strlen(utf) || memcmp(back, utf, (size_t)b)) { fprintf(stderr, "utf\n"); fails++; }
    k = convert("KOI8-R", "UTF-8", utf, (int)strlen(utf), koi, sizeof koi);
    if (k != 6 || memcmp(koi, expect_koi, 6)) { fprintf(stderr, "koi\n"); fails++; }
    if (iconv_open("NO-SUCH", "UTF-8") != (iconv_t)-1) { fprintf(stderr, "bad open\n"); fails++; }
    if (fails) return 1;
    printf("iconv ok\n");
    return 0;
}
