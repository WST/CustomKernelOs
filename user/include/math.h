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
#endif
