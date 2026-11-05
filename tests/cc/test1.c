/* Тесты компилятора cc: каждый CHECK сравнивает результат с ожидаемым. */
#include <myos.h>

static int fails, checks;
#define CHECK(expected, expr) do { int e_ = (expected), v_ = (expr); checks++; \
    if (e_ != v_) { fails++; printf("FAIL %s:%d: %s = %d, expected %d\n", __FILE__, __LINE__, #expr, v_, e_); } } while (0)

/* ---- функции ---- */
int add2(int a, int b) { return a + b; }
int sub3(int a, int b, int c) { return a - b - c; }
int fact(int n) { return n <= 1 ? 1 : n * fact(n - 1); }
int sum_va(int n, ...) { va_list ap; va_start(ap, n); int s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, int); va_end(ap); return s; }
char ret_char(int x) { return x; }
unsigned char ret_uchar(int x) { return x; }
short ret_short(int x) { return x; }
int char_param(char c) { return c; }
int apply(int (*f)(int, int), int a, int b) { return f(a, b); }

/* ---- структуры ---- */
struct point { int x, y; };
struct rect { struct point a, b; char name[8]; };
typedef struct node { int val; struct node *next; } node_t;
union u { int i; char c[4]; unsigned short s[2]; };
enum color { RED, GREEN = 5, BLUE };

struct point make_point(int x, int y) { struct point p; p.x = x; p.y = y; return p; }
int point_sum(struct point p) { return p.x + p.y; }
int rect_area(struct rect r) { return (r.b.x - r.a.x) * (r.b.y - r.a.y); }
struct rect grow(struct rect r, int d) { r.b.x += d; r.b.y += d; return r; }

/* ---- глобальные ---- */
int g_int = 42;
int g_arr[5] = { 1, 2, 3 };
char g_str[] = "hello";
char *g_ptr = "world";
int *g_pint = &g_arr[2];
struct point g_pts[] = { {1, 2}, {3, 4}, {5, 6} };
const char *g_names[] = { "zero", "one", "two" };
int (*g_fn)(int, int) = add2;
int g_bss[100];
static int s_counter;
double g_d = 2.5;
float g_f = 1.25f;

int counter(void) { static int n = 10; return n++; }

int main(void) {
    /* арифметика */
    CHECK(7, 3 + 4);
    CHECK(-1, 3 - 4);
    CHECK(42, 6 * 7);
    CHECK(3, 10 / 3);
    CHECK(-3, -10 / 3);
    CHECK(1, 10 % 3);
    CHECK(-1, -10 % 3);
    CHECK(16, 1 << 4);
    CHECK(-4, -16 >> 2);
    CHECK(0x3FFFFFFC, (int)(0xFFFFFFF0u >> 2));
    CHECK(1, (unsigned)-1 > 1);
    CHECK(0, -1 > 1u ? 0 : 1);
    CHECK(1, -1 < 0);
    CHECK(5, 0x0F & 0x35);
    CHECK(0x3F, 0x0F | 0x30);
    CHECK(0x3A, 0x0F ^ 0x35);
    CHECK(-6, ~5);
    CHECK(1, !0);
    CHECK(0, !7);
    CHECK(1, 3 < 4 && 4 < 5);
    CHECK(1, 0 || 2);
    CHECK(0, 0 && (1 / 0));
    CHECK(12, 012 + 2);
    CHECK(255, 0xff);
    CHECK(5, 0b101);
    CHECK(97, 'a');
    CHECK(10, '\n');
    CHECK(27, '\033');
    CHECK(-1, '\xff');
    CHECK(3, (1, 2, 3));
    CHECK(7, 1 ? 7 : 8);
    CHECK(8, 0 ? 7 : 8);
    unsigned u = 4000000000u;
    CHECK(2000000000, (int)(u / 2));
    CHECK(1, u > 3000000000u);
    CHECK(0, (int)(u % 1000));

    /* char / short */
    char c = 200;
    CHECK(-56, c);
    unsigned char uc = 200;
    CHECK(200, uc);
    uc += 100;
    CHECK(44, uc);
    short sh = 40000;
    CHECK(-25536, sh);
    unsigned short us = 70000;
    CHECK(4464, us);
    CHECK(-56, ret_char(200));
    CHECK(200, ret_uchar(200));
    CHECK(-25536, ret_short(40000));
    CHECK(-56, char_param(200));
    c = 'A'; c++;
    CHECK('B', c);
    bool b = 5;
    CHECK(1, b);
    b = 0.5;
    CHECK(1, b);

    /* функции */
    CHECK(5, add2(2, 3));
    CHECK(5, sub3(10, 3, 2));
    CHECK(120, fact(5));
    CHECK(15, sum_va(5, 1, 2, 3, 4, 5));
    CHECK(9, apply(add2, 4, 5));
    int (*fp)(int, int) = add2;
    CHECK(11, fp(5, 6));
    CHECK(11, (*fp)(5, 6));
    CHECK(3, g_fn(1, 2));

    /* указатели и массивы */
    int a[10];
    for (int i = 0; i < 10; i++) a[i] = i * i;
    int *p = a;
    CHECK(16, *(p + 4));
    CHECK(16, p[4]);
    CHECK(16, 4[p]);
    p += 3;
    CHECK(9, *p++);
    CHECK(16, *p);
    CHECK(4, p - a);
    CHECK(25, *++p);
    CHECK(25, *p--);
    CHECK(16, *p);
    CHECK(40, sizeof(a));
    CHECK(4, sizeof(p));
    CHECK(10, sizeof(a) / sizeof(a[0]));
    int m[3][4];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 4; j++) m[i][j] = i * 10 + j;
    CHECK(23, m[2][3]);
    CHECK(12, m[1][2]);
    CHECK(48, sizeof(m));
    CHECK(16, sizeof(m[0]));
    int *pm = &m[1][0];
    CHECK(13, pm[3]);
    char s[] = "abc";
    CHECK(4, sizeof(s));
    CHECK('c', s[2]);
    CHECK(0, s[3]);
    char *ps = "xyz";
    CHECK('y', ps[1]);
    CHECK(3, strlen(ps));
    int la[5] = { 1, 2 };
    CHECK(2, la[1]);
    CHECK(0, la[4]);
    int lb[] = { 5, 6, 7 };
    CHECK(12, sizeof(lb));
    void *vp = a;
    CHECK(4, (int)((char *)vp + 4 - (char *)a));

    /* структуры */
    struct point pt = { 3, 4 };
    CHECK(7, pt.x + pt.y);
    struct point *ppt = &pt;
    ppt->x = 10;
    CHECK(10, pt.x);
    struct point pt2 = pt;
    pt2.y = 100;
    CHECK(4, pt.y);
    CHECK(100, pt2.y);
    pt2 = pt;
    CHECK(4, pt2.y);
    struct point mp = make_point(7, 8);
    CHECK(15, mp.x + mp.y);
    CHECK(15, make_point(7, 8).x + 8);
    CHECK(14, point_sum(pt));
    struct rect r = { {0, 0}, {4, 5}, "box" };
    CHECK(20, rect_area(r));
    struct rect r2 = grow(r, 1);
    CHECK(30, rect_area(r2));
    CHECK(20, rect_area(r));
    CHECK('b', r2.name[0]);
    CHECK(24, sizeof(struct rect));
    struct { char c; int i; } st;
    CHECK(8, sizeof(st));
    union u un;
    un.i = 0x11223344;
    CHECK(0x44, un.c[0]);
    CHECK(0x1122, un.s[1]);
    CHECK(4, sizeof(un));
    CHECK(0, RED);
    CHECK(6, BLUE);
    enum color col = GREEN;
    CHECK(5, col);
    node_t n3 = { 3, NULL }, n2 = { 2, &n3 }, n1 = { 1, &n2 };
    int sum = 0;
    for (node_t *n = &n1; n; n = n->next) sum += n->val;
    CHECK(6, sum);
    node_t *list = NULL;
    for (int i = 1; i <= 100; i++) { node_t *nn = malloc(sizeof(node_t)); nn->val = i; nn->next = list; list = nn; }
    sum = 0;
    while (list) { node_t *nx = list->next; sum += list->val; free(list); list = nx; }
    CHECK(5050, sum);
    struct point arr_pt[3] = { {1, 1}, {2, 2} };
    CHECK(2, arr_pt[1].y);
    CHECK(0, arr_pt[2].x);

    /* глобальные */
    CHECK(42, g_int);
    CHECK(3, g_arr[2]);
    CHECK(0, g_arr[4]);
    CHECK('l', g_str[3]);
    CHECK('w', g_ptr[0]);
    CHECK(3, *g_pint);
    CHECK(6, g_pts[2].y);
    CHECK('t', g_names[2][0]);
    CHECK(0, g_bss[99]);
    g_bss[99] = 5;
    CHECK(5, g_bss[99]);
    s_counter += 3;
    CHECK(3, s_counter);
    CHECK(10, counter());
    CHECK(11, counter());

    /* управление */
    int x = 0;
    for (int i = 0; i < 10; i++) { if (i == 3) continue; if (i == 7) break; x += i; }
    CHECK(18, x);
    x = 0;
    do x++; while (x < 5);
    CHECK(5, x);
    x = 0;
    while (1) { if (++x == 8) break; }
    CHECK(8, x);
    int sw = 0;
    for (int i = 0; i < 6; i++) {
        switch (i) {
        case 0: sw += 1; break;
        case 1: case 2: sw += 10; break;
        case 3: sw += 100;               /* проваливается */
        case 4: sw += 1000; break;
        default: sw += 10000;
        }
    }
    CHECK(12121, sw);
    switch (7) { case 5 ... 9: x = 99; break; default: x = 0; }
    CHECK(99, x);
    x = 0;
    goto skip;
    x = 1;
skip:
    CHECK(0, x);
    x = 5;
    x += 3; CHECK(8, x);
    x -= 2; CHECK(6, x);
    x *= 4; CHECK(24, x);
    x /= 5; CHECK(4, x);
    x %= 3; CHECK(1, x);
    x <<= 4; CHECK(16, x);
    x >>= 2; CHECK(4, x);
    x |= 3; CHECK(7, x);
    x &= 5; CHECK(5, x);
    x ^= 1; CHECK(4, x);
    CHECK(4, x++);
    CHECK(6, ++x);
    CHECK(6, x--);
    CHECK(4, --x);
    CHECK(10, ({ int t = 4; t + 6; }));

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
