/* fputest — проверка, что ядро сохраняет FPU: запусти несколько копий в фоне,
 * результаты должны совпасть до последнего знака. */
#include "myos.h"

int main(int argc, char **argv) {
    int n = argc > 1 ? atoi(argv[1]) : 3000000;
    double s = 0;
    for (int i = 1; i <= n; i++) {
        s += 1.0 / ((double)i * i);
        if ((i & 0xFFFF) == 0) yield();        /* почаще отдаём процессор — пусть FPU «перемешивается» */
    }
    double pi = 0;
    /* sqrt(6*s) без libm: метод Ньютона */
    double x = 6 * s, r = x;
    for (int k = 0; k < 40; k++) r = 0.5 * (r + x / r);
    pi = r;
    printf("[pid %d] sum 1/i^2 = %.9f, pi ~ %.9f\n", getpid(), s, pi);
    return 0;
}
