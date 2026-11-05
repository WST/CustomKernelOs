/* primes [N] — решето Эратосфена; память берётся через malloc -> sbrk. */
#include "myos.h"

int main(int argc, char **argv) {
    unsigned n = argc > 1 ? (unsigned)atoi(argv[1]) : 1000000;
    if (n < 2) n = 2;
    unsigned t0 = ticks();
    char *sieve = calloc(n + 1, 1);
    if (!sieve) { dprintf(STDERR, "primes: not enough memory for %u bytes\n", n + 1); return 1; }

    unsigned count = 0, last = 0;
    for (unsigned i = 2; i <= n; i++) {
        if (sieve[i]) continue;
        count++;
        last = i;
        for (unsigned j = i * 2; j <= n; j += i) sieve[j] = 1;
    }
    free(sieve);
    printf("%u primes below %u (largest %u), %u ms, used %u KB of heap\n",
           count, n, last, ticks() - t0, (n + 1) / 1024);
    return 0;
}
