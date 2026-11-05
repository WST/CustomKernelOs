/* count [N] [ms] — считает до N с паузой. Запусти в фоне: count 10 500 & */
#include "myos.h"

int main(int argc, char **argv) {
    int n  = argc > 1 ? atoi(argv[1]) : 10;
    int ms = argc > 2 ? atoi(argv[2]) : 1000;
    for (int i = 1; i <= n; i++) {
        printf("[count pid %d] %d/%d\n", getpid(), i, n);
        sleep_ms(ms);
    }
    return 0;
}
