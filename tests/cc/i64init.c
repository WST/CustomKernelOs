/* Вычисленный инициализатор long long не обрезается до int. */
#define LLONG_MAX 9223372036854775807LL
const long long lim = LLONG_MAX / 1000;
static long long neg = -((LLONG_MAX / 1000));

int main(void) {
    long long us = 250000;
    if (lim != 9223372036854775LL) return 1;
    if (us > lim) return 2;
    if (neg != -9223372036854775LL) return 3;
    if (neg > 0) return 4;
    return 0;
}
