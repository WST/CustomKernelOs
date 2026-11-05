/* Тесты cc: условные переходы и короткие пути кодогенератора */
#include <myos.h>

static int fails, checks;
#define CHECK(expected, expr) do { int e_ = (expected), v_ = (expr); checks++; \
    if (e_ != v_) { fails++; printf("FAIL %s:%d: %s = %d, expected %d\n", __FILE__, __LINE__, #expr, v_, e_); } } while (0)

static int br(int c) { if (c) return 1; return 0; }
static int cmp_u(unsigned a, unsigned b) { if (a < b) return -1; if (a > b) return 1; return 0; }
static int cmp_s(int a, int b) { if (a < b) return -1; if (a > b) return 1; return 0; }
static int cmp_c(char a, unsigned char b) { if (a < 0 && b > 127) return 3; if (a == b) return 2; return 0; }
static int logic(int a, int b, int c) {
    int r = 0;
    if (a && b) r |= 1;
    if (a || b) r |= 2;
    if (!(a && c)) r |= 4;
    if (!a || (b && c)) r |= 8;
    if ((a || b) && !c) r |= 16;
    return r;
}
static int dcond(double x) { if (x) return 1; if (!x) return 2; return 3; }
static int dcmp(double a, double b) { if (a < b) return -1; if (a > b) return 1; if (a == b) return 0; return 9; }
static int loop_sum(int n) { int s = 0; for (int i = 0; i < n; i++) if (i % 3 == 0 || i % 5 == 0) s += i; return s; }
static int while_count(unsigned x) { int n = 0; while (x) { x &= x - 1; n++; } return n; }
static int do_count(int x) { int n = 0; do { n++; x /= 2; } while (x > 0); return n; }

int main(void) {
    CHECK(1, br(5)); CHECK(0, br(0));
    CHECK(-1, cmp_u(1, 0xFFFFFFFF)); CHECK(1, cmp_u(0x80000000, 1)); CHECK(0, cmp_u(7, 7));
    CHECK(1, cmp_s(1, -1)); CHECK(-1, cmp_s(-5, 3)); CHECK(0, cmp_s(-2, -2));
    CHECK(3, cmp_c(-1, 255)); CHECK(2, cmp_c(10, 10)); CHECK(0, cmp_c(10, 11));
    CHECK(2 | 4 | 16, logic(1, 0, 0));
    CHECK(1 | 2 | 4 | 16, logic(1, 1, 0));
    CHECK(1 | 2 | 8, logic(1, 1, 1));
    CHECK(4 | 8, logic(0, 0, 0));
    CHECK(2 | 4 | 8, logic(0, 1, 1));
    CHECK(1, dcond(0.5)); CHECK(2, dcond(0.0)); CHECK(1, dcond(-1e-9));
    CHECK(-1, dcmp(1, 2)); CHECK(1, dcmp(2, 1)); CHECK(0, dcmp(3.5, 3.5));
    CHECK(2318, loop_sum(100));
    CHECK(32, while_count(0xFFFFFFFF)); CHECK(1, while_count(0x80000000));
    CHECK(4, do_count(10)); CHECK(1, do_count(0));
    unsigned char b = 250; int i = 0;
    while (b != 4) { b++; i++; }
    CHECK(10, i);
    short sv = -3; int cnt = 0;
    for (; sv < 3; sv++) cnt++;
    CHECK(6, cnt);
    int x = 5, *px = &x;
    if (px && *px == 5) x = 6;
    CHECK(6, x);
    char *s = "abc"; int len = 0;
    while (*s) { s++; len++; }
    CHECK(3, len);
    int y = (x > 3) ? (x < 10 ? 1 : 2) : 3;
    CHECK(1, y);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
