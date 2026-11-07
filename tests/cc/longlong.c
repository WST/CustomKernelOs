/* long long: сдвиги, умножение, деление, сравнение, аргументы. */
#include <myos.h>

static int fails;

static unsigned half(long long v, int hi) {
    union { long long v; unsigned p[2]; } u;
    u.v = v;
    return u.p[hi];
}

#define CHECK(cond) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static long long addll(long long a, long long b) { return a + b; }
static long long divll(long long a, long long b) { return a / b; }

long long g = 10000000000LL;

int main(void) {
    CHECK(half(g, 0) == 0x540BE400u);
    CHECK(half(g, 1) == 2);

    long long a = 0xFFFFFFFFLL;
    long long sum = a + 2;
    CHECK(half(sum, 0) == 1);
    CHECK(half(sum, 1) == 1);
    CHECK(sum - 2 == a);

    long long one = 1;
    CHECK((one << 0) == 1);
    CHECK((one << 32) == 4294967296LL);
    CHECK(half(one << 40, 0) == 0);
    CHECK(half(one << 40, 1) == 256);
    CHECK((one << 63) < 0);

    unsigned long long u = ~0ULL;
    CHECK((u >> 1) == 0x7FFFFFFFFFFFFFFFULL);
    CHECK((u >> 63) == 1);
    long long neg = -1;
    CHECK((neg >> 1) == -1);
    CHECK((neg >> 40) == -1);
    unsigned long long logical = ~0ULL;
    logical >>= 40;
    CHECK(logical == 0xFFFFFFULL);

    CHECK(addll(10000000000LL, 5) == 10000000005LL);
    CHECK(100000LL * 100000LL == 10000000000LL);
    CHECK(divll(10000000000LL, 3) == 3333333333LL);
    CHECK(10000000000LL % 3 == 1);
    CHECK((-10000000000LL) / 3 == -3333333333LL);
    CHECK((-10000000000LL) % 3 == -1);
    CHECK((unsigned long long)-1 > 1ULL);
    CHECK((int)10000000000LL == (int)0x540BE400);
    CHECK(-g == -10000000000LL);

    long long i, acc = 0;
    for (i = 0; i < 1000; i++) acc += i;
    CHECK(acc == 499500);

    return fails != 0;
}
