#include <string.h>

static double frombits(unsigned long long u) {
    double x;
    memcpy(&x, &u, sizeof x);
    return x;
}

static unsigned long long tobits(double x) {
    unsigned long long u;
    memcpy(&u, &x, sizeof u);
    return u;
}

double __myos_inf(void) { return frombits(0x7ff0000000000000ULL); }
double __myos_nan(void) { return frombits(0x7ff8000000000000ULL); }

int isnan(double x) {
    unsigned long long u = tobits(x);
    return ((u >> 52) & 0x7ff) == 0x7ff && (u << 12) != 0;
}

int isinf(double x) {
    unsigned long long u = tobits(x);
    return ((u >> 52) & 0x7ff) == 0x7ff && (u << 12) == 0;
}
