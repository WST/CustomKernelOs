/* gcc -m32 без 32-битного libgcc зовёт эти имена из деления long long.
 * Внутри только 32-битные операции, иначе помощник вызвал бы сам себя. */

typedef union {
    unsigned long long u;
    struct { unsigned lo, hi; } p;
} di_parts;

static void udivmod(unsigned long long n, unsigned long long d,
                    unsigned long long *q, unsigned long long *r) {
    di_parts N, D, Q, R;
    int i;
    N.u = n;
    D.u = d;
    Q.u = 0;
    R.u = 0;
    if (!d) {
        *q = 0;
        *r = n;
        return;
    }
    for (i = 63; i >= 0; i--) {
        unsigned bit = i >= 32 ? (N.p.hi >> (i - 32)) & 1 : (N.p.lo >> i) & 1;
        R.p.hi = (R.p.hi << 1) | (R.p.lo >> 31);
        R.p.lo = (R.p.lo << 1) | bit;
        if (R.p.hi > D.p.hi || (R.p.hi == D.p.hi && R.p.lo >= D.p.lo)) {
            unsigned borrow = R.p.lo < D.p.lo;
            R.p.lo -= D.p.lo;
            R.p.hi = R.p.hi - D.p.hi - borrow;
            if (i >= 32) Q.p.hi |= 1u << (i - 32);
            else Q.p.lo |= 1u << i;
        }
    }
    *q = Q.u;
    *r = R.u;
}

static unsigned long long neg64(unsigned long long u) {
    di_parts x;
    x.u = u;
    x.p.lo = ~x.p.lo + 1;
    x.p.hi = ~x.p.hi + (x.p.lo == 0);
    return x.u;
}

unsigned long long __udivmoddi4(unsigned long long n, unsigned long long d,
                                unsigned long long *rp) {
    unsigned long long q, r;
    udivmod(n, d, &q, &r);
    if (rp) *rp = r;
    return q;
}

long long __divmoddi4(long long a, long long b, long long *rp) {
    di_parts A, B, Q, R;
    int qneg = 0, rneg = 0;
    A.u = (unsigned long long)a;
    B.u = (unsigned long long)b;
    if (A.p.hi & 0x80000000u) { qneg ^= 1; rneg = 1; A.u = neg64(A.u); }
    if (B.p.hi & 0x80000000u) { qneg ^= 1; B.u = neg64(B.u); }
    udivmod(A.u, B.u, &Q.u, &R.u);
    if (qneg) Q.u = neg64(Q.u);
    if (rneg) R.u = neg64(R.u);
    if (rp) *rp = (long long)R.u;
    return (long long)Q.u;
}
