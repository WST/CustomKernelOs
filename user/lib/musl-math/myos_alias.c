#include <math.h>
#include "libm.h"

double pow10(double x) { return exp10(x); }
float pow10f(float x) { return exp10f(x); }
double lgamma_r(double x, int *sign) { return __lgamma_r(x, sign); }
float lgammaf_r(float x, int *sign) { return __lgammaf_r(x, sign); }
double drem(double x, double y) { return remainder(x, y); }
float dremf(float x, float y) { return remainderf(x, y); }
