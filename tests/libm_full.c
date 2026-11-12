#include <math.h>
#include <stdio.h>

static int fails;

static void near(double got, double want, double tol, const char *msg) {
    double d = got - want;
    if (d < 0) d = -d;
    if (d > tol) { printf("fail %s got %.17g\n", msg, got); fails++; }
}

int main(void) {
    near(log1p(0), 0, 0, "log1p0");
    near(log1p(1e-8), 9.9999999500000003e-9, 1e-18, "log1p");
    near(expm1(0), 0, 0, "expm10");
    near(expm1(1e-8), 1.0000000050000000e-8, 1e-18, "expm1");
    near(hypot(3, 4), 5, 0, "hypot");
    near(erf(0), 0, 0, "erf0");
    near(erf(1), 0.8427007929497148, 1e-12, "erf1");
    near(lgamma(5), 3.1780538303479458, 1e-12, "lgamma");
    near(tgamma(5), 24, 1e-9, "tgamma");
    near(acosh(1), 0, 0, "acosh");
    near(asinh(0), 0, 0, "asinh");
    near(atanh(0), 0, 0, "atanh");
    if (!(nextafter(1.0, 2.0) > 1.0)) { printf("fail nextafter\n"); fails++; }
    if (sinf(0) != 0) { printf("fail sinf\n"); fails++; }
    near(sinf(0.5f), 0.479425538604203, 1e-6, "sinf");
    if (!fails) printf("libm full: ok\n");
    return fails != 0;
}
