typedef union { unsigned bits; } Ref;

static Ref make(void *p) {
    Ref r;
    r.bits = (unsigned)p | 1;
    return r;
}

static Ref wrap(void *p) {
    Ref ref = make(p);
    return ref;
}

static Ref id(Ref r) {
    Ref c = r;
    return c;
}

static Ref lit(void *p) {
    Ref ref = (Ref){ .bits = (unsigned)p | 1 };
    return ref;
}

static const Ref one = { .bits = 1 };

static Ref pick(int cond, void *p) {
    Ref r = cond ? make(p) : one;
    return r;
}

int main(void) {
    void *p = (void *)0x12345678;
    Ref o;
    Ref w;

    o = make(p);
    if (o.bits != 0x12345679) return 1;
    w = wrap(p);
    if (w.bits != 0x12345679) return 2;
    if (id(w).bits != 0x12345679) return 3;
    if (lit(p).bits != 0x12345679) return 4;
    if (pick(1, p).bits != 0x12345679) return 5;
    if (pick(0, p).bits != 1) return 6;
    return 0;
}
