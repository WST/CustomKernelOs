/* Раздельная компиляция: main.c, lib1.c и lib2.c собираются по-разному (tests/cclink.sh) */
#include <myos.h>
#include "link.h"

static int fails, checks;
#define CHECK(expected, expr) do { int e_ = (expected), v_ = (expr); checks++; \
    if (e_ != v_) { fails++; printf("FAIL %s:%d: %s = %d, expected %d\n", __FILE__, __LINE__, #expr, v_, e_); } } while (0)

static int helper(void) { return 1; }       /* такой же static есть в lib1.c */
int common_counter;                         /* COMMON: объявлен и в lib1.c */
int *table_ptr = &table[2];                 /* указатель с добавкой в массив из другого файла */

struct big main_make(int base) {
    struct big b;
    for (int i = 0; i < 10; i++) b.a[i] = base * i;
    b.tag = 'm';
    return b;
}

/* портит esi/edi (копирование структуры) и зовёт 64-битное деление */
static struct big pattern;
static int callback(int x) {
    struct big tmp = pattern;
    tmp.a[x] += x;
    long long q = (long long)(tmp.a[x] + 1000) * 1000000 / 7;
    return (int)(q % 97) + tmp.a[x];
}

static int stress_reference(int (*f)(int)) {
    int a = 1, b = 2, c = 3, d = 4, e = 5;
    for (int i = 0; i < 10; i++) {
        int r = f(i);
        a += r;
        b ^= r * 3;
        c += a;
        d += b;
        e += c ^ d;
    }
    return a + b + c + d + e;
}

int main(void) {
    CHECK(1, helper());
    CHECK(10, lib1_helper());
    CHECK(42, initialized);
    initialized++;
    CHECK(43, lib1_get_initialized());
    common_counter = 5;
    lib1_bump();
    CHECK(6, common_counter);
    CHECK(30, *table_ptr);
    CHECK(0, strcmp(greeting, "hello from lib1"));
    CHECK(7, ops[0](3, 4));
    CHECK(12, ops[1](3, 4));

    struct big b = lib2_make(5);
    CHECK(5, b.a[0]);
    CHECK(14, b.a[9]);
    CHECK('x', b.tag);
    CHECK(95 + 'x', lib2_sum(b));
    CHECK(27 + 'm', lib2_call_back_make());

    CHECK(-128, lib2_schar());
    CHECK(200, lib2_uchar());
    CHECK(-2, lib2_short());
    CHECK(65535, lib2_ushort());
    CHECK(1, lib2_bool());

    for (int i = 0; i < 10; i++) pattern.a[i] = i * 11;
    CHECK(stress_reference(callback), lib2_stress(callback));
    CHECK(-123456, (int)lib2_div64(-123456789012LL, 1000000));

    printf("link: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
