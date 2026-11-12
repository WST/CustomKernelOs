#ifndef MATH_H
#define MATH_H
double __myos_inf(void);
double __myos_nan(void);
#define HUGE_VAL (__myos_inf())
#define INFINITY (__myos_inf())
#define NAN (__myos_nan())

int isnan(double x);
int isinf(double x);
double fabs(double x);
double copysign(double x, double y);
double floor(double x);
double ceil(double x);
double trunc(double x);
double fmod(double x, double y);
double modf(double x, double *iptr);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double sqrt(double x);
double exp(double x);
double log(double x);
double log2(double x);
double log10(double x);
double pow(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);

typedef float float_t;
typedef double double_t;

static inline unsigned __myos_fbits(float f) {
    union { float f; unsigned i; } u;
    u.f = f;
    return u.i;
}
static inline unsigned long long __myos_dbits(double d) {
    union { double d; unsigned long long i; } u;
    u.d = d;
    return u.i;
}
#define signbit(x) (sizeof(x) == sizeof(float) ? (int)(__myos_fbits(x) >> 31) : (int)(__myos_dbits(x) >> 63))
#define isfinite(x) (sizeof(x) == sizeof(float) ? (__myos_fbits(x) & 0x7fffffffu) < 0x7f800000u : (__myos_dbits(x) & 0x7fffffffffffffffULL) < 0x7ff0000000000000ULL)

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define FP_ILOGBNAN (-1 - 0x7fffffff)
#define FP_ILOGB0 FP_ILOGBNAN
int __fpclassify(double);
int __fpclassifyf(float);
#define fpclassify(x) (sizeof(x) == sizeof(float) ? __fpclassifyf(x) : __fpclassify(x))

/* Остальные функции — src/math musl 1.2.5 в libm. Без -lm их нет. */
float acosf(float);
double acosh(double);
float acoshf(float);
float asinf(float);
double asinh(double);
float asinhf(float);
float atanf(float);
float atan2f(float, float);
double atanh(double);
float atanhf(float);
double cbrt(double);
float cbrtf(float);
float ceilf(float);
float copysignf(float, float);
float cosf(float);
float coshf(float);
double erf(double);
float erff(float);
double erfc(double);
float erfcf(float);
float expf(float);
double exp2(double);
float exp2f(float);
double expm1(double);
float expm1f(float);
float fabsf(float);
double fdim(double, double);
float fdimf(float, float);
float floorf(float);
double fma(double, double, double);
float fmaf(float, float, float);
double fmax(double, double);
float fmaxf(float, float);
double fmin(double, double);
float fminf(float, float);
float fmodf(float, float);
float frexpf(float, int *);
double hypot(double, double);
float hypotf(float, float);
int ilogb(double);
int ilogbf(float);
double lgamma(double);
float lgammaf(float);
long long llrint(double);
long long llrintf(float);
long long llround(double);
long long llroundf(float);
double log1p(double);
float log1pf(float);
double logb(double);
float logbf(float);
long lrint(double);
long lrintf(float);
long lround(double);
long lroundf(float);
double nearbyint(double);
float nearbyintf(float);
double nextafter(double, double);
float nextafterf(float, float);
double remainder(double, double);
float remainderf(float, float);
double remquo(double, double, int *);
float remquof(float, float, int *);
double rint(double);
float rintf(float);
double round(double);
float roundf(float);
double scalbn(double, int);
float scalbnf(float, int);
float sinf(float);
float sinhf(float);
float sqrtf(float);
float tanf(float);
float tanhf(float);
double tgamma(double);
float tgammaf(float);
float truncf(float);
extern int signgam;
double j0(double);
double j1(double);
double jn(int, double);
double y0(double);
double y1(double);
double yn(int, double);
float j0f(float);
float j1f(float);
float jnf(int, float);
float y0f(float);
float y1f(float);
float ynf(int, float);
int finite(double);
int finitef(float);
double exp10(double);
float exp10f(float);
double pow10(double);
float pow10f(float);
void sincos(double, double *, double *);
void sincosf(float, float *, float *);
double lgamma_r(double, int *);
float lgammaf_r(float, int *);
double drem(double, double);
float dremf(float, float);
double scalb(double, double);
float scalbf(float, float);
double significand(double);
float significandf(float);
float modff(float, float *);
double scalbln(double, long);
float scalblnf(float, long);
double nan(const char *);
float nanf(const char *);
float logf(float);
float log2f(float);
float log10f(float);
float powf(float, float);

/* musl src/math вызывает weak_alias. У нас это обычные функции в myos_alias.c. */
#define weak_alias(old, new)
#endif
