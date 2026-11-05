/* Тесты cc: double/float, препроцессор, строки и libc */
#include <myos.h>

static int fails, checks;
#define CHECK(expected, expr) do { int e_ = (expected), v_ = (expr); checks++; \
    if (e_ != v_) { fails++; printf("FAIL %s:%d: %s = %d, expected %d\n", __FILE__, __LINE__, #expr, v_, e_); } } while (0)
#define CHECKS(expected, s) do { checks++; if (strcmp(expected, s)) { fails++; printf("FAIL %d: \"%s\" expected \"%s\"\n", __LINE__, s, expected); } } while (0)

double half(double x) { return x / 2; }
float fhalf(float x) { return x / 2; }
double mix(int a, double b, char c, float d) { return a + b + c + d; }
double sum_d(int n, ...) { va_list ap; va_start(ap, n); double s = 0; for (int i = 0; i < n; i++) s += va_arg(ap, double); return s; }
struct vec { double x, y; };
struct vec vadd(struct vec a, struct vec b) { struct vec r = { a.x + b.x, a.y + b.y }; return r; }
double gd[3] = { 1.5, 2.5, -3.25 };

#define SQUARE(x) ((x) * (x))
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a##b
#define VERSION 3
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define LOG(fmt, ...) snprintf(buf, sizeof(buf), fmt, __VA_ARGS__)
#if VERSION > 2 && defined(VERSION)
int ver_ok = 1;
#elif VERSION == 2
int ver_ok = 2;
#else
int ver_ok = 3;
#endif
#ifdef NOT_DEFINED
#error should not happen
#endif
#ifndef __MYOS_CC__
int not_cc = 1;
#endif

int main(void) {
    double d = 3.75;
    CHECK(3, (int)d);
    CHECK(-3, (int)-d);
    CHECK(7, (int)(d * 2));
    CHECK(1, d > 3.7);
    CHECK(0, d < 3.7);
    CHECK(1, d == 3.75);
    CHECK(1, d != 3.7);
    CHECK(1, d >= 3.75 && d <= 3.75);
    CHECK(18, (int)half(37));
    CHECK(18, (int)(fhalf(37.0f) + 0.0));
    CHECK(10, (int)mix(1, 2.5, 3, 3.5f));
    CHECK(6, (int)sum_d(3, 1.0, 2.0, 3.0));
    float f = 0.1f;
    CHECK(1, f != 0.1);                      /* float точнее не бывает */
    CHECK(1, (double)f > 0.0999 && f < 0.1001);
    double z = 0;
    CHECK(1, !z);
    CHECK(0, !d);
    if (z) CHECK(0, 1);
    unsigned big = 3000000000u;
    double db = big;
    CHECK(1, db > 2.9e9);
    CHECK(-1294967296, (int)(unsigned)db);
    int neg = -7;
    double dn = neg;
    CHECK(-7, (int)dn);
    d = 1;
    for (int i = 0; i < 10; i++) d *= 2;
    CHECK(1024, (int)d);
    d /= 3;
    CHECK(341, (int)d);
    d += 0.5; d -= 0.25;
    CHECK(341, (int)d);
    d++;
    CHECK(342, (int)d);
    struct vec a = { 1, 2 }, b = { 0.5, 0.25 };
    struct vec c = vadd(a, b);
    CHECK(150, (int)(c.x * 100));
    CHECK(225, (int)(c.y * 100));
    CHECK(-325, (int)(gd[2] * 100));
    double arr[4];
    for (int i = 0; i < 4; i++) arr[i] = i * 1.5;
    CHECK(45, (int)(arr[3] * 10));
    CHECK(1, -d < 0);
    CHECK(8, sizeof(double));
    CHECK(4, sizeof(float));

    char buf[100];
    snprintf(buf, sizeof(buf), "%d|%5d|%-3d|%x|%s|%c|%u", 42, 7, 1, 255, "ok", 'z', 3000000000u);
    CHECKS("42|    7|1  |ff|ok|z|3000000000", buf);
    snprintf(buf, sizeof(buf), "%.2f", 3.14159);
    CHECKS("3.14", buf);

    CHECK(49, SQUARE(7));
    CHECK(16, SQUARE(1 + 3));
    CHECKS("hello", STR(hello));
    CHECKS("3", XSTR(VERSION));
    int CAT(my, var) = 5;
    CHECK(5, myvar);
    CHECK(9, MAX(4, 9));
    CHECK(1, ver_ok);
    LOG("%d-%d", 1, 2);
    CHECKS("1-2", buf);
    CHECK(__LINE__, __LINE__);

    char s[32];
    strcpy(s, "abc");
    strcat(s, "def");
    CHECKS("abcdef", s);
    CHECK(0, strcmp("abc", "abc"));
    CHECK(1, strcmp("abd", "abc") > 0);
    CHECK('d', *strchr(s, 'd'));
    CHECK(123, atoi("123"));
    CHECK(-45, atoi("-45"));
    char *m = malloc(1000);
    memset(m, 'x', 1000);
    CHECK('x', m[999]);
    m = realloc(m, 5000);
    CHECK('x', m[500]);
    free(m);
    CHECKS("multi" "line" " string", "multiline string");

    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
