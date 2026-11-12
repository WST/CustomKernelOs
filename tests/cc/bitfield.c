struct S {
    unsigned a : 3;
    unsigned b : 5;
    unsigned c : 8;
    int d;
};

struct Z {
    unsigned a : 1;
    unsigned : 0;
    unsigned b : 1;
};

static struct S gs = { 5, 17, 200, 9 };

int main(void) {
    struct S s;
    struct { int x : 3; } t;
    struct Z z;
    s.a = 5;
    s.b = 17;
    s.c = 200;
    s.d = 3;
    if (s.a != 5 || s.b != 17 || s.c != 200 || s.d != 3) return 1;
    s.a += 1;
    if (s.a != 6) return 2;
    s.b++;
    if (s.b != 18) return 3;
    t.x = -1;
    if (t.x != -1) return 4;
    z.a = 1;
    z.b = 1;
    if (z.a != 1 || z.b != 1) return 5;
    if (gs.a != 5 || gs.b != 17 || gs.c != 200 || gs.d != 9) return 6;
    if (sizeof(struct S) != 8) return 7;
    return 0;
}
