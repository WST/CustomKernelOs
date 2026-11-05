/* jsnum.c — числа для движка JS: точное преобразование double <-> текст
 * и математика через сопроцессор x87 (своей libm в MyOS нет). */
#include "jsint.h"

int sprintf_e(char *o, int e);

int js_isnan(double x) { return x != x; }
int js_isinf(double x) { return x == JS_INF || x == -JS_INF; }

/* ------------------------------------------------------------------ x87 */

double js_sqrt(double x) { long double r = x; __asm__("fsqrt" : "+t"(r)); return r; }

static long double reduce_angle(long double x) {    /* fsin/fcos работают при |x| < 2^63 */
    if (x > 1e15 || x < -1e15) return 0;
    return x;
}
double js_sin(double x) { if (js_isinf(x) || js_isnan(x)) return JS_NAN; long double r = reduce_angle(x); __asm__("fsin" : "+t"(r)); return r; }
double js_cos(double x) { if (js_isinf(x) || js_isnan(x)) return JS_NAN; long double r = reduce_angle(x); __asm__("fcos" : "+t"(r)); return r; }
double js_tan(double x) { return js_sin(x) / js_cos(x); }

double js_atan2(double y, double x) {
    long double r, ly = y, lx = x;
    __asm__("fpatan" : "=t"(r) : "0"(lx), "u"(ly) : "st(1)");
    return r;
}

/* 2^x для любого x: целая часть через fscale, дробная через f2xm1 */
static long double exp2l_(long double x) {
    if (x != x) return x;
    if (x > 16384) return JS_INF;
    if (x < -16500) return 0;
    long double i = x, f, r;
    __asm__("frndint" : "+t"(i));
    f = x - i;
    __asm__("f2xm1" : "+t"(f));
    f += 1;
    __asm__("fscale" : "=t"(r) : "0"(f), "u"(i));
    return r;
}

static long double log2l_(long double x) {
    long double r, one = 1;
    __asm__("fyl2x" : "=t"(r) : "0"(x), "u"(one) : "st(1)");
    return r;
}

double js_exp(double x) {
    if (js_isnan(x)) return x;
    if (x == JS_INF) return x;
    if (x == -JS_INF) return 0;
    return exp2l_((long double)x * 1.44269504088896340736L);
}

double js_log(double x) {
    if (js_isnan(x) || x < 0) return JS_NAN;
    if (x == 0) return -JS_INF;
    if (x == JS_INF) return x;
    return log2l_(x) * 0.693147180559945309417L;
}

double js_floor(double x) {
    if (js_isnan(x) || js_isinf(x) || x == 0) return x;
    if (x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    double t = (double)(ji64)x;
    if (t > x) t -= 1;
    if (t == 0 && x < 0) return -0.0;
    return t;
}

double js_ceil(double x) {
    if (js_isnan(x) || js_isinf(x) || x == 0) return x;
    double r = -js_floor(-x);
    return r;
}

double js_fmod(double a, double b) {
    if (js_isnan(a) || js_isnan(b) || js_isinf(a) || b == 0) return JS_NAN;
    if (js_isinf(b)) return a;
    if (a == 0) return a;
    long double x = a, y = b;
    unsigned short sw;
    do {
        __asm__("fprem; fnstsw %%ax" : "+t"(x), "=a"(sw) : "u"(y));
    } while (sw & 0x400);
    return x;
}

double js_pow(double a, double b) {
    if (js_isnan(b)) return JS_NAN;
    if (b == 0) return 1;
    if (js_isnan(a)) return JS_NAN;
    if (b == (double)(ji64)b && b > -1100 && b < 1100) {     /* целая степень — умножением */
        ji64 n = (ji64)b;
        int neg = n < 0;
        if (neg) n = -n;
        long double r = 1, x = a;
        while (n) { if (n & 1) r *= x; x *= x; n >>= 1; }
        return neg ? 1 / r : r;
    }
    if (js_isinf(b)) {
        double m = a < 0 ? -a : a;
        if (m == 1) return JS_NAN;
        return (m > 1) == (b > 0) ? JS_INF : 0;
    }
    if (a < 0) return JS_NAN;
    if (a == 0) return b > 0 ? 0 : JS_INF;
    if (a == JS_INF) return b > 0 ? JS_INF : 0;
    return exp2l_(b * log2l_(a));
}

/* ------------------------------------------------------------------ текст -> число */

double js_parse_float(const char *s, int n, int *used) {
    int i = 0, neg = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    if (n - i >= 8 && !strncmp(s + i, "Infinity", 8)) { *used = i + 8; return neg ? -JS_INF : JS_INF; }
    ji64 mant = 0;
    int digits = 0, exp10 = 0, any = 0;
    while (i < n && isdigit((ju8)s[i])) {
        any = 1;
        if (digits < 18) { mant = mant * 10 + (s[i] - '0'); if (mant) digits++; }
        else exp10++;
        i++;
    }
    if (i < n && s[i] == '.') {
        int j = i + 1;
        while (j < n && isdigit((ju8)s[j])) {
            any = 1;
            if (digits < 18) { mant = mant * 10 + (s[j] - '0'); if (mant) digits++; exp10--; }
            j++;
        }
        if (any) i = j;
    }
    if (!any) { *used = 0; return JS_NAN; }
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        int j = i + 1, eneg = 0, e = 0, ok = 0;
        if (j < n && (s[j] == '+' || s[j] == '-')) { eneg = s[j] == '-'; j++; }
        while (j < n && isdigit((ju8)s[j])) { if (e < 10000) e = e * 10 + (s[j] - '0'); j++; ok = 1; }
        if (ok) { exp10 += eneg ? -e : e; i = j; }
    }
    *used = i;
    long double r = (long double)mant;
    if (mant && exp10) {
        long double p = 1, b = 10;
        int e = exp10 < 0 ? -exp10 : exp10;
        if (e > 5000) e = 5000;
        while (e) { if (e & 1) p *= b; b *= b; e >>= 1; }
        if (exp10 < 0) r /= p; else r *= p;
    }
    double d = (double)r;
    return neg ? -d : d;
}

/* ------------------------------------------------------------------ число -> текст */

/* точные десятичные цифры положительного x: x = 0.D * 10^dexp; -> число цифр */
static int exact_digits(double x, char *dig, int max, int *dexp) {
    union { double d; ju64 u; } v;
    v.d = x;
    int be = (int)((v.u >> 52) & 0x7FF);
    ju64 m = v.u & 0xFFFFFFFFFFFFFull;
    if (be) m |= 1ull << 52; else be = 1;
    int e = be - 1075;                                      /* x = m * 2^e */
    static unsigned big[160];                              /* основание 10^8, младшие первыми */
    int nb = 1;
    big[0] = 0;
    for (int b = 52; b >= 0; b--) {                        /* big = m */
        unsigned carry = (m >> b) & 1;
        for (int k = 0; k < nb; k++) { unsigned t = big[k] * 2 + carry; carry = t / 100000000; big[k] = t % 100000000; }
        if (carry) big[nb++] = carry;
    }
    int mul = e >= 0 ? 2 : 5, cnt = e >= 0 ? e : -e;
    while (cnt > 0) {
        unsigned f = mul;
        int steps = 1;
        if (cnt >= 2) { f = mul * mul; steps = 2; }
        unsigned carry = 0;
        for (int k = 0; k < nb; k++) { unsigned t = big[k] * f + carry; carry = t / 100000000; big[k] = t % 100000000; }
        while (carry && nb < 160) { big[nb++] = carry % 100000000; carry /= 100000000; }
        cnt -= steps;
    }
    /* в текст */
    char tmp[1400];
    int n = 0;
    n += snprintf(tmp, sizeof(tmp), "%u", big[nb - 1]);
    for (int k = nb - 2; k >= 0; k--) n += snprintf(tmp + n, sizeof(tmp) - n, "%08u", big[k]);
    int total = n;
    *dexp = e >= 0 ? total : total + e;
    int len = 0;
    for (int k = 0; k < total && len < max; k++) dig[len++] = tmp[k];
    while (len > 1 && dig[len - 1] == '0') len--;
    return len;
}

/* округлить цифры до p знаков (половина — вверх) */
static int round_digits(const char *d, int n, int p, char *out, int *dexp) {
    if (n <= p) { memcpy(out, d, n); return n; }
    memcpy(out, d, p);
    if (d[p] >= '5') {
        int k = p - 1;
        while (k >= 0) {
            if (out[k] == '9') { out[k] = '0'; k--; }
            else { out[k]++; break; }
        }
        if (k < 0) { memmove(out + 1, out, p); out[0] = '1'; (*dexp)++; }
    }
    while (p > 1 && out[p - 1] == '0') p--;
    return p;
}

static void format_js(char *buf, int neg, const char *d, int k, int n) {
    char *o = buf;
    if (neg) *o++ = '-';
    if (k <= n && n <= 21) {
        memcpy(o, d, k); o += k;
        for (int i = 0; i < n - k; i++) *o++ = '0';
    } else if (0 < n && n <= 21) {
        memcpy(o, d, n); o += n;
        *o++ = '.';
        memcpy(o, d + n, k - n); o += k - n;
    } else if (-6 < n && n <= 0) {
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -n; i++) *o++ = '0';
        memcpy(o, d, k); o += k;
    } else {
        *o++ = d[0];
        if (k > 1) { *o++ = '.'; memcpy(o, d + 1, k - 1); o += k - 1; }
        o += sprintf_e(o, n - 1);
    }
    *o = 0;
}

int sprintf_e(char *o, int e) {
    char t[16];
    int l = snprintf(t, sizeof(t), "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    memcpy(o, t, l);
    return l;
}

/* кратчайшие цифры, которые читаются обратно в то же число */
int js_shortest_digits(double x, char *out, int *dexp) {
    char exact[800];
    int e0;
    int n = exact_digits(x, exact, sizeof(exact), &e0);
    for (int p = 1; p <= 17; p++) {
        int e = e0;
        int k = round_digits(exact, n, p, out, &e);
        char tmp[48];
        int l = 0;
        tmp[l++] = '.';
        memcpy(tmp + l, out, k); l += k;
        l += snprintf(tmp + l, sizeof(tmp) - l, "e%d", e);
        int used;
        if (js_parse_float(tmp, l, &used) == x || p == 17) { *dexp = e; return k; }
    }
    return 0;
}

void js_num_to_str(double d, char *buf) {
    if (js_isnan(d)) { strcpy(buf, "NaN"); return; }
    if (d == 0) { strcpy(buf, "0"); return; }
    if (js_isinf(d)) { strcpy(buf, d > 0 ? "Infinity" : "-Infinity"); return; }
    int neg = d < 0;
    if (neg) d = -d;
    if (d < 9007199254740992.0 && d == (double)(ji64)d) {
        double v = d;
        char t[24];
        int n = 0;
        while (v >= 1) { double q = js_floor(v / 10); t[n++] = '0' + (int)(v - q * 10); v = q; }
        char *o = buf;
        if (neg) *o++ = '-';
        while (n) *o++ = t[--n];
        *o = 0;
        return;
    }
    char dig[24];
    int e;
    int k = js_shortest_digits(d, dig, &e);
    format_js(buf, neg, dig, k, e);
}

/* toFixed(f): f знаков после точки */
void js_num_fixed(double x, int f, char *buf, int max) {
    if (js_isnan(x)) { snprintf(buf, max, "NaN"); return; }
    if (x >= 1e21 || x <= -1e21) { js_num_to_str(x, buf); return; }
    int neg = x < 0;
    if (neg) x = -x;
    char exact[800], out[820];
    int e, n = x == 0 ? 1 : exact_digits(x, exact, sizeof(exact), &e);
    if (x == 0) { exact[0] = '0'; e = 1; }
    int p = e + f;                                         /* сколько цифр оставить */
    int k, ee = e;
    if (p < 0 || (p == 0 && exact[0] < '5')) { k = 0; ee = 0; }
    else if (p == 0) { out[0] = '1'; k = 1; ee = e + 1; }
    else {
        k = round_digits(exact, n, p, out, &ee);
    }
    /* собрать: целая часть + f знаков */
    char *o = buf;
    if (neg) *o++ = '-';
    int ip = ee;                                           /* цифр в целой части */
    if (ip <= 0) *o++ = '0';
    for (int i = 0; i < ip; i++) *o++ = i < k ? out[i] : '0';
    if (f > 0) {
        *o++ = '.';
        for (int i = 0; i < f; i++) {
            int idx = ip + i;
            *o++ = (idx >= 0 && idx < k) ? out[idx] : '0';
        }
    }
    *o = 0;
    (void)max;
}

/* toPrecision(p) / toExponential(f) */
void js_num_precision(double x, int p, int expo, char *buf) {
    if (js_isnan(x)) { strcpy(buf, "NaN"); return; }
    if (js_isinf(x)) { strcpy(buf, x > 0 ? "Infinity" : "-Infinity"); return; }
    int neg = x < 0;
    if (neg) x = -x;
    char exact[800], out[64];
    int e, n, k;
    if (x == 0) { exact[0] = '0'; n = 1; e = 1; }
    else n = exact_digits(x, exact, sizeof(exact), &e);
    if (p < 1) p = 1;
    if (p > 40) p = 40;
    k = round_digits(exact, n, p, out, &e);
    while (k < p) out[k++] = '0';
    char *o = buf;
    if (neg) *o++ = '-';
    if (expo || e - 1 < -6 || e - 1 >= p) {
        *o++ = out[0];
        if (p > 1) { *o++ = '.'; memcpy(o, out + 1, p - 1); o += p - 1; }
        o += sprintf_e(o, x == 0 ? 0 : e - 1);
    } else if (e <= 0) {
        *o++ = '0'; *o++ = '.';
        for (int i = 0; i < -e; i++) *o++ = '0';
        memcpy(o, out, p); o += p;
    } else {
        memcpy(o, out, e); o += e;
        if (p > e) { *o++ = '.'; memcpy(o, out + e, p - e); o += p - e; }
    }
    *o = 0;
}

/* toString(radix) */
void js_num_radix(double x, int radix, char *buf) {
    if (radix == 10 || js_isnan(x) || js_isinf(x)) { js_num_to_str(x, buf); return; }
    int neg = x < 0;
    if (neg) x = -x;
    double ip = js_floor(x), fp = x - ip;
    char t[1100];
    int n = 0;
    if (ip == 0) t[n++] = '0';
    while (ip >= 1 && n < 1100) {
        double q = js_floor(ip / radix);
        int d = (int)(ip - q * radix);
        t[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[d];
        ip = q;
    }
    char *o = buf;
    if (neg) *o++ = '-';
    while (n) *o++ = t[--n];
    if (fp > 0) {
        *o++ = '.';
        for (int i = 0; i < 20 && fp > 0; i++) {
            fp *= radix;
            int d = (int)fp;
            *o++ = "0123456789abcdefghijklmnopqrstuvwxyz"[d];
            fp -= d;
        }
    }
    *o = 0;
}
