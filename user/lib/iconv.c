/* iconv.c — узкий iconv: UTF-8, UTF-16/32 LE и BE, ISO-8859-1, CP1251, KOI8-R, ASCII.
 * Состояния сдвига нет. Непредставимый знак — EILSEQ, обрыв последовательности — EINVAL. */
#include "myos.h"
#include "stdlib.h"
#include "string.h"
#include "iconv.h"

enum { ENC_UTF8, ENC_UTF16LE, ENC_UTF16BE, ENC_UTF32LE, ENC_UTF32BE, ENC_LATIN1, ENC_CP1251, ENC_KOI8R, ENC_ASCII };

struct iconv_cd { int from, to; };

/* CP1251, байты 0x80..0xFF. 0x98 в кодировке не задан — оставляем U+0098. */
static const unsigned short cp1251_u[128] = {
    0x0402,0x0403,0x201A,0x0453,0x201E,0x2026,0x2020,0x2021,0x20AC,0x2030,0x0409,0x2039,0x040A,0x040C,0x040B,0x040F,
    0x0452,0x2018,0x2019,0x201C,0x201D,0x2022,0x2013,0x2014,0x0098,0x2122,0x0459,0x203A,0x045A,0x045C,0x045B,0x045F,
    0x00A0,0x040E,0x045E,0x0408,0x00A4,0x0490,0x00A6,0x00A7,0x0401,0x00A9,0x0404,0x00AB,0x00AC,0x00AD,0x00AE,0x0407,
    0x00B0,0x00B1,0x0406,0x0456,0x0491,0x00B5,0x00B6,0x00B7,0x0451,0x2116,0x0454,0x00BB,0x0458,0x0405,0x0455,0x0457,
    0x0410,0x0411,0x0412,0x0413,0x0414,0x0415,0x0416,0x0417,0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,0x041F,
    0x0420,0x0421,0x0422,0x0423,0x0424,0x0425,0x0426,0x0427,0x0428,0x0429,0x042A,0x042B,0x042C,0x042D,0x042E,0x042F,
    0x0430,0x0431,0x0432,0x0433,0x0434,0x0435,0x0436,0x0437,0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,0x043F,
    0x0440,0x0441,0x0442,0x0443,0x0444,0x0445,0x0446,0x0447,0x0448,0x0449,0x044A,0x044B,0x044C,0x044D,0x044E,0x044F
};

/* KOI8-R, байты 0x80..0xFF. Буквы 0xC0.. и ё (0xA3/0xB3) — как в RFC 1489. */
static const unsigned short koi8_u[128] = {
    0x2500,0x2502,0x250C,0x2510,0x2514,0x2518,0x251C,0x2524,0x252C,0x2534,0x253C,0x2580,0x2584,0x2588,0x258C,0x2590,
    0x2591,0x2592,0x2593,0x2320,0x25A0,0x2219,0x221A,0x2248,0x2264,0x2265,0x00A0,0x2321,0x00B0,0x00B2,0x00B7,0x00F7,
    0x2550,0x2551,0x2552,0x0451,0x2553,0x2554,0x2555,0x2556,0x2557,0x2558,0x2559,0x255A,0x255B,0x255C,0x255D,0x255E,
    0x255F,0x2560,0x2561,0x0401,0x2562,0x2563,0x2564,0x2565,0x2566,0x2567,0x2568,0x2569,0x256A,0x256B,0x256C,0x00A9,
    0x044E,0x0430,0x0431,0x0446,0x0434,0x0435,0x0444,0x0433,0x0445,0x0438,0x0439,0x043A,0x043B,0x043C,0x043D,0x043E,
    0x043F,0x044F,0x0440,0x0441,0x0442,0x0443,0x0436,0x0432,0x044C,0x044B,0x0437,0x0448,0x044D,0x0449,0x0447,0x044A,
    0x042E,0x0410,0x0411,0x0426,0x0414,0x0415,0x0424,0x0413,0x0425,0x0418,0x0419,0x041A,0x041B,0x041C,0x041D,0x041E,
    0x041F,0x042F,0x0420,0x0421,0x0422,0x0423,0x0416,0x0412,0x042C,0x042B,0x0417,0x0428,0x042D,0x0429,0x0427,0x042A
};

static int name_eq(const char *s, const char *t) {
    while (*s && *t) {
        while (*s == '-' || *s == '_' || *s == ' ') s++;
        while (*t == '-' || *t == '_' || *t == ' ') t++;
        if (!*s || !*t) break;
        if (tolower((unsigned char)*s) != tolower((unsigned char)*t)) return 0;
        s++; t++;
    }
    while (*s == '-' || *s == '_' || *s == ' ') s++;
    while (*t == '-' || *t == '_' || *t == ' ') t++;
    return *s == 0 && *t == 0;
}

static int enc_of(const char *s) {
    if (!s) return -1;
    if (name_eq(s, "utf8") || name_eq(s, "utf-8")) return ENC_UTF8;
    if (name_eq(s, "utf16le") || name_eq(s, "utf-16le")) return ENC_UTF16LE;
    if (name_eq(s, "utf16be") || name_eq(s, "utf-16be")) return ENC_UTF16BE;
    if (name_eq(s, "utf32le") || name_eq(s, "utf-32le")) return ENC_UTF32LE;
    if (name_eq(s, "utf32be") || name_eq(s, "utf-32be")) return ENC_UTF32BE;
    if (name_eq(s, "iso88591") || name_eq(s, "latin1") || name_eq(s, "iso-8859-1")) return ENC_LATIN1;
    if (name_eq(s, "cp1251") || name_eq(s, "windows1251") || name_eq(s, "cp-1251")) return ENC_CP1251;
    if (name_eq(s, "koi8r") || name_eq(s, "koi8-r")) return ENC_KOI8R;
    if (name_eq(s, "ascii") || name_eq(s, "usascii") || name_eq(s, "us-ascii")) return ENC_ASCII;
    return -1;
}

static int from_byte(int enc, unsigned char b, unsigned *cp) {
    if (b < 0x80) { *cp = b; return b == 0 || enc != ENC_ASCII || b < 128; }
    if (enc == ENC_ASCII) return 0;
    if (enc == ENC_LATIN1) { *cp = b; return 1; }
    if (enc == ENC_CP1251) { *cp = cp1251_u[b - 0x80]; return 1; }
    if (enc == ENC_KOI8R) { *cp = koi8_u[b - 0x80]; return 1; }
    return 0;
}

static int to_byte(int enc, unsigned cp, unsigned char *b) {
    int i;
    if (cp < 0x80) { *b = (unsigned char)cp; return 1; }
    if (enc == ENC_ASCII) return 0;
    if (enc == ENC_LATIN1) { if (cp > 255) return 0; *b = (unsigned char)cp; return 1; }
    if (enc == ENC_CP1251) {
        for (i = 0; i < 128; i++) if (cp1251_u[i] == cp) { *b = (unsigned char)(0x80 + i); return 1; }
        return 0;
    }
    if (enc == ENC_KOI8R) {
        for (i = 0; i < 128; i++) if (koi8_u[i] == cp) { *b = (unsigned char)(0x80 + i); return 1; }
        return 0;
    }
    return 0;
}

static int utf8_get(const unsigned char *p, int n, unsigned *cp, int *used) {
    unsigned c, need, min, i;
    if (n < 1) return 0;
    c = p[0];
    if (c < 0x80) { *cp = c; *used = 1; return 1; }
    if ((c & 0xe0) == 0xc0) { need = 2; c &= 0x1f; min = 0x80; }
    else if ((c & 0xf0) == 0xe0) { need = 3; c &= 0x0f; min = 0x800; }
    else if ((c & 0xf8) == 0xf0) { need = 4; c &= 0x07; min = 0x10000; }
    else return -1;
    if (n < (int)need) return 0;
    for (i = 1; i < need; i++) {
        if ((p[i] & 0xc0) != 0x80) return -1;
        c = (c << 6) | (p[i] & 0x3f);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -1;
    *cp = c; *used = (int)need; return 1;
}

static int iconv_utf8_put(unsigned cp, unsigned char *p, int n) {
    if (cp < 0x80) { if (n < 1) return 0; p[0] = (unsigned char)cp; return 1; }
    if (cp < 0x800) { if (n < 2) return 0; p[0] = (unsigned char)(0xc0 | (cp >> 6)); p[1] = (unsigned char)(0x80 | (cp & 0x3f)); return 2; }
    if (cp < 0x10000) {
        if (n < 3) return 0;
        p[0] = (unsigned char)(0xe0 | (cp >> 12));
        p[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
        p[2] = (unsigned char)(0x80 | (cp & 0x3f));
        return 3;
    }
    if (n < 4) return 0;
    p[0] = (unsigned char)(0xf0 | (cp >> 18));
    p[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3f));
    p[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3f));
    p[3] = (unsigned char)(0x80 | (cp & 0x3f));
    return 4;
}

static int unit_get(int enc, const unsigned char *p, int n, unsigned *cp, int *used) {
    unsigned u;
    if (enc == ENC_UTF8) return utf8_get(p, n, cp, used);
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
        if (n < 2) return 0;
        u = enc == ENC_UTF16LE ? (unsigned)p[0] | ((unsigned)p[1] << 8) : ((unsigned)p[0] << 8) | p[1];
        if (u >= 0xd800 && u <= 0xdbff) {
            unsigned v;
            if (n < 4) return 0;
            v = enc == ENC_UTF16LE ? (unsigned)p[2] | ((unsigned)p[3] << 8) : ((unsigned)p[2] << 8) | p[3];
            if (v < 0xdc00 || v > 0xdfff) return -1;
            *cp = 0x10000 + (((u - 0xd800) << 10) | (v - 0xdc00));
            *used = 4;
            return 1;
        }
        if (u >= 0xdc00 && u <= 0xdfff) return -1;
        *cp = u; *used = 2; return 1;
    }
    if (enc == ENC_UTF32LE || enc == ENC_UTF32BE) {
        if (n < 4) return 0;
        if (enc == ENC_UTF32LE) u = (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
        else u = ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
        if (u > 0x10ffff || (u >= 0xd800 && u <= 0xdfff)) return -1;
        *cp = u; *used = 4; return 1;
    }
    if (n < 1) return 0;
    if (!from_byte(enc, p[0], cp)) return -1;
    *used = 1;
    return 1;
}

static int unit_put(int enc, unsigned cp, unsigned char *p, int n) {
    if (cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return -1;
    if (enc == ENC_UTF8) return iconv_utf8_put(cp, p, n);
    if (enc == ENC_UTF16LE || enc == ENC_UTF16BE) {
        unsigned hi, lo;
        int k = cp >= 0x10000 ? 4 : 2;
        if (n < k) return 0;
        if (cp < 0x10000) { hi = cp; lo = 0; }
        else { cp -= 0x10000; hi = 0xd800 + (cp >> 10); lo = 0xdc00 + (cp & 0x3ff); }
        if (enc == ENC_UTF16LE) { p[0] = (unsigned char)hi; p[1] = (unsigned char)(hi >> 8); }
        else { p[0] = (unsigned char)(hi >> 8); p[1] = (unsigned char)hi; }
        if (k == 4) {
            if (enc == ENC_UTF16LE) { p[2] = (unsigned char)lo; p[3] = (unsigned char)(lo >> 8); }
            else { p[2] = (unsigned char)(lo >> 8); p[3] = (unsigned char)lo; }
        }
        return k;
    }
    if (enc == ENC_UTF32LE || enc == ENC_UTF32BE) {
        if (n < 4) return 0;
        if (enc == ENC_UTF32LE) { p[0] = (unsigned char)cp; p[1] = (unsigned char)(cp >> 8); p[2] = (unsigned char)(cp >> 16); p[3] = (unsigned char)(cp >> 24); }
        else { p[0] = (unsigned char)(cp >> 24); p[1] = (unsigned char)(cp >> 16); p[2] = (unsigned char)(cp >> 8); p[3] = (unsigned char)cp; }
        return 4;
    }
    if (n < 1) return 0;
    if (!to_byte(enc, cp, p)) return -1;
    return 1;
}

iconv_t iconv_open(const char *tocode, const char *fromcode) {
    struct iconv_cd *cd;
    int from = enc_of(fromcode), to = enc_of(tocode);
    if (from < 0 || to < 0) { errno = EINVAL; return (iconv_t)-1; }
    cd = malloc(sizeof *cd);
    if (!cd) { errno = ENOMEM; return (iconv_t)-1; }
    cd->from = from;
    cd->to = to;
    return cd;
}

size_t iconv(iconv_t cd, char **inbuf, size_t *inbytesleft, char **outbuf, size_t *outbytesleft) {
    struct iconv_cd *c = cd;
    if (cd == (iconv_t)-1 || !c) { errno = EINVAL; return (size_t)-1; }
    if (!inbuf || !*inbuf) return 0;
    if (!inbytesleft || !outbuf || !*outbuf || !outbytesleft) { errno = EINVAL; return (size_t)-1; }
    while (*inbytesleft) {
        unsigned cp;
        int used = 0, put, rc;
        rc = unit_get(c->from, (const unsigned char *)*inbuf, (int)*inbytesleft, &cp, &used);
        if (rc == 0) { errno = EINVAL; return (size_t)-1; }
        if (rc < 0) { errno = EILSEQ; return (size_t)-1; }
        put = unit_put(c->to, cp, (unsigned char *)*outbuf, (int)*outbytesleft);
        if (put == 0) { errno = E2BIG; return (size_t)-1; }
        if (put < 0) { errno = EILSEQ; return (size_t)-1; }
        *inbuf += used;
        *inbytesleft -= (size_t)used;
        *outbuf += put;
        *outbytesleft -= (size_t)put;
    }
    return 0;
}

int iconv_close(iconv_t cd) {
    if (cd == (iconv_t)-1 || !cd) { errno = EINVAL; return -1; }
    free(cd);
    return 0;
}
