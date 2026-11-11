/* libm: двойная точность для cc -lm. Тот же файл подключается в libc.c,
 * поэтому sin и pow находятся и без -lm. Целые степени считаются умножением,
 * остальные функции — рядами после приведения аргумента. Точности хватает Lua. */
#include "myos.h"
#include "math.h"

static unsigned long long dbits(double x) {
    unsigned long long u;
    memcpy(&u, &x, 8);
    return u;
}

static double dfrom(unsigned long long u) {
    double x;
    memcpy(&x, &u, 8);
    return x;
}

double __myos_inf(void) { return dfrom(0x7ff0000000000000ULL); }
double __myos_nan(void) { return dfrom(0x7ff8000000000000ULL); }

int isnan(double x) {
    unsigned long long u = dbits(x);
    return ((u >> 52) & 0x7ff) == 0x7ff && (u << 12) != 0;
}

int isinf(double x) {
    unsigned long long u = dbits(x);
    return ((u >> 52) & 0x7ff) == 0x7ff && (u << 12) == 0;
}

double fabs(double x) { return dfrom(dbits(x) & ~(1ULL << 63)); }

double copysign(double x, double y) {
    unsigned long long u = (dbits(x) & ~(1ULL << 63)) | (dbits(y) & (1ULL << 63));
    return dfrom(u);
}

static int finite(double x) { return !isnan(x) && !isinf(x); }

double frexp(double x, int *exp) {
    unsigned long long u = dbits(x);
    int be = (int)((u >> 52) & 0x7ff);
    unsigned long long mag = u & ((1ULL << 63) - 1);
    if (mag == 0 || be == 0x7ff) { *exp = 0; return x; }
    if (be == 0) {
        x = frexp(x * 18014398509481984.0, exp); /* 2^54, вытащить денормаль */
        *exp -= 54;
        return x;
    }
    *exp = be - 1022;
    u = (u & ((1ULL << 63) | ((1ULL << 52) - 1))) | (1022ULL << 52);
    return dfrom(u);
}

double ldexp(double x, int n) {
    unsigned long long u;
    int be;
    if (x == 0 || !finite(x)) return x;
    u = dbits(x);
    be = (int)((u >> 52) & 0x7ff);
    if (be == 0) {
        x = ldexp(x * 18014398509481984.0, n - 54);
        return x;
    }
    be += n;
    if (be >= 0x7ff) return copysign(__myos_inf(), x);
    if (be <= 0) {
        if (be < -52) return copysign(0.0, x);
        /* мантисса (1.frac) сдвигается в денормаль */
        unsigned long long frac = (u & ((1ULL << 52) - 1)) | (1ULL << 52);
        int shift = 1 - be;
        frac >>= shift;
        u = (u & (1ULL << 63)) | frac;
        return dfrom(u);
    }
    u = (u & ~(((1ULL << 11) - 1) << 52)) | ((unsigned long long)be << 52);
    return dfrom(u);
}

double floor(double x) {
    double t;
    if (!finite(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    t = (double)(long long)x;
    if (x >= 0 || t == x) return t;
    return t - 1.0;
}

double ceil(double x) {
    double t;
    if (!finite(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    t = (double)(long long)x;
    if (x <= 0 || t == x) return t;
    return t + 1.0;
}

double trunc(double x) {
    if (!finite(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    return (double)(long long)x;
}

double modf(double x, double *ip) {
    double i = x < 0 ? ceil(x) : floor(x);
    *ip = i;
    return x == i ? copysign(0.0, x) : x - i;
}

double fmod(double x, double y) {
    double ax, ay;
    if (y == 0 || !finite(x) || isnan(y)) return __myos_nan();
    if (isinf(y)) return x;
    ax = fabs(x);
    ay = fabs(y);
    if (ax < ay) return x;
    while (ax >= ay) {
        int ex, ey, k;
        double sub;
        frexp(ax, &ex);
        frexp(ay, &ey);
        k = ex - ey;
        sub = ldexp(ay, k);
        if (sub > ax) sub = ldexp(ay, k - 1);
        if (!(sub > 0) || sub > ax) break;
        ax -= sub;
    }
    return copysign(ax, x);
}

double sqrt(double x) {
    int e, i;
    double m, y;
    if (isnan(x) || x == 0) return x;
    if (x < 0) return __myos_nan();
    if (isinf(x)) return x;
    m = frexp(x, &e);
    if (e & 1) { m *= 2; e--; }
    y = 0.41731 + 0.59016 * m;
    y = ldexp(y, e / 2);
    for (i = 0; i < 6; i++) y = 0.5 * (y + x / y);
    return y;
}

static const double LN2 = 0.693147180559945309417;
static const double LN2_HI = 0.6931471805599453;
static const double LN2_LO = 2.3190468138462996e-17;

double exp(double x) {
    double n, r, p;
    int k;
    if (isnan(x)) return x;
    if (x > 709.782712893384) return __myos_inf();
    if (x < -745.1332191019412) return 0;
    n = floor(x / LN2 + (x >= 0 ? 0.5 : -0.5));
    r = (x - n * LN2_HI) - n * LN2_LO;
    p = 1;
    {
        double t = 1;
        for (k = 1; k <= 14; k++) { t *= r / k; p += t; }
    }
    return ldexp(p, (int)n);
}

double log(double x) {
    int e;
    double m, f, z, z2, s, t;
    int i;
    if (isnan(x)) return x;
    if (x < 0) return __myos_nan();
    if (x == 0) return -__myos_inf();
    if (isinf(x)) return x;
    m = frexp(x, &e);
    if (m < 0.7071067811865476) { m *= 2; e--; }
    f = m - 1;
    z = f / (2 + f);
    z2 = z * z;
    s = 0;
    t = z;
    for (i = 0; i < 14; i++) { s += t / (2 * i + 1); t *= z2; }
    return 2 * s + e * LN2;
}

double log2(double x) { return log(x) / LN2; }
double log10(double x) { return log(x) / 2.30258509299404568402; }

double pow(double x, double y) {
    long long n, bit;
    double r, b;
    int neg;
    if (y == 0) return 1;
    if (x == 1) return 1;
    if (isnan(x) || isnan(y)) return __myos_nan();
    if (x == 0) {
        if (y < 0) return copysign(__myos_inf(), y == floor(y) && fmod(y, 2) ? x : 1);
        return y == floor(y) && fmod(y, 2) ? x : 0;
    }
    if (x < 0) {
        if (y != floor(y) || fabs(y) > 9007199254740992.0) return __myos_nan();
        n = (long long)y;
        neg = (int)(n & 1);
        r = pow(-x, y);
        return neg ? -r : r;
    }
    if (y == floor(y) && fabs(y) <= 1024) {
        n = (long long)y;
        neg = n < 0;
        if (neg) n = -n;
        r = 1;
        b = x;
        for (bit = n; bit; bit >>= 1) {
            if (bit & 1) r *= b;
            b *= b;
        }
        if (!neg) return r;
        return r == 0 ? __myos_inf() : 1 / r;
    }
    if (isinf(y)) {
        if (x > 1) return y > 0 ? y : 0;
        if (x == 1) return 1;
        return y > 0 ? 0 : __myos_inf();
    }
    return exp(y * log(x));
}

static const double PIO2_HI = 1.5707963267948966;
static const double PIO2_LO = 6.123233995736766e-17;
static const double INVPIO2 = 0.6366197723675814;

static double sin_poly(double r) {
    double z = r * r;
    return r * (1 + z * (-1.6666666666666632e-1 + z * (8.3333333333224895e-3 +
        z * (-1.9841269829857949e-4 + z * (2.7557313707070068e-6 +
        z * (-2.5050760253406863e-8 + z * 1.5896909952115501e-10))))));
}

static double cos_poly(double r) {
    double z = r * r;
    return 1 + z * (-0.5 + z * (4.1666666666666602e-2 + z * (-1.3888888888874109e-3 +
        z * (2.4801587289476730e-5 + z * (-2.7557314351390663e-7 +
        z * 2.0875723212981750e-9)))));
}

static double rempio2(double x, int *n) {
    double fn = floor(x * INVPIO2 + 0.5);
    *n = (int)fn;
    return (x - fn * PIO2_HI) - fn * PIO2_LO;
}

static double sin_reduced(double x) {
    int n;
    double r;
    if (!finite(x)) return __myos_nan();
    if (fabs(x) > 1e16) x = fmod(x, 6.283185307179586);
    r = rempio2(x, &n);
    switch (n & 3) {
    case 0: return sin_poly(r);
    case 1: return cos_poly(r);
    case 2: return -sin_poly(r);
    default: return -cos_poly(r);
    }
}

double sin(double x) { return sin_reduced(x); }

double cos(double x) {
    int n;
    double r;
    if (!finite(x)) return __myos_nan();
    if (fabs(x) > 1e16) x = fmod(x, 6.283185307179586);
    r = rempio2(x, &n);
    switch (n & 3) {
    case 0: return cos_poly(r);
    case 1: return -sin_poly(r);
    case 2: return -cos_poly(r);
    default: return sin_poly(r);
    }
}

double tan(double x) {
    double c = cos(x);
    if (c == 0) return copysign(__myos_inf(), sin(x));
    return sin(x) / c;
}

static double atan_series(double x) {
    double z2 = x * x, s = 0, t = x;
    int i;
    for (i = 0; i < 20; i++) {
        s += (i & 1 ? -t : t) / (2 * i + 1);
        t *= z2;
        if (fabs(t) < 1e-18) break;
    }
    return s;
}

double atan(double x) {
    int complement = 0, neg;
    double s;
    if (isnan(x)) return x;
    if (isinf(x)) return copysign(PIO2_HI, x);
    neg = x < 0;
    x = fabs(x);
    if (x > 1) { x = 1 / x; complement = 1; }
    if (x > 0.5) {
        x = (x - 1) / (x + 1);
        s = 0.7853981633974483 + atan_series(x);
    } else {
        s = atan_series(x);
    }
    if (complement) s = PIO2_HI - s;
    return neg ? -s : s;
}

double atan2(double y, double x) {
    double a;
    if (isnan(x) || isnan(y)) return __myos_nan();
    if (x == 0) return y == 0 ? copysign(0.0, y) : copysign(PIO2_HI, y);
    a = atan(y / x);
    if (x < 0) a += y >= 0 ? 2 * PIO2_HI : -2 * PIO2_HI;
    return a;
}

double asin(double x) {
    double d;
    if (fabs(x) > 1) return __myos_nan();
    d = sqrt(1 - x * x);
    return atan2(x, d);
}

double acos(double x) {
    if (fabs(x) > 1) return __myos_nan();
    return atan2(sqrt(1 - x * x), x);
}

double sinh(double x) {
    double a = fabs(x);
    double e;
    if (a > 20) return copysign(exp(a) / 2, x);
    e = exp(a);
    return copysign((e - 1 / e) / 2, x);
}

double cosh(double x) {
    double a = fabs(x);
    double e;
    if (a > 20) return exp(a) / 2;
    e = exp(a);
    return (e + 1 / e) / 2;
}

double tanh(double x) {
    double s, c;
    if (fabs(x) > 20) return copysign(1, x);
    s = sinh(x);
    c = cosh(x);
    return s / c;
}
