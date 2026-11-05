/* Тесты cc: инициализаторы с именами, typedef функций, вложенные структуры, массивы указателей */
#include <myos.h>

static int fails, checks;
#define CHECK(expected, expr) do { int e_ = (expected), v_ = (expr); checks++; \
    if (e_ != v_) { fails++; printf("FAIL %s:%d: %s = %d, expected %d\n", __FILE__, __LINE__, #expr, v_, e_); } } while (0)

struct p { int x, y, z; };
typedef int (*binop)(int, int);
typedef struct { const char *name; binop fn; } op_t;
static int add(int a, int b) { return a + b; }
static int mul(int a, int b) { return a * b; }
static op_t ops[] = { { "add", add }, { "mul", mul } };
struct p gp = { .y = 7 };
int ga[10] = { [3] = 5, [8] = 9 };
struct inner { char tag; short v[3]; };
struct outer { int id; struct inner in; double w; };
typedef struct list list;
struct list { int v; list *next; };

int main(void) {
    struct p q = { .x = 1, .z = 3 };
    CHECK(1, q.x); CHECK(0, q.y); CHECK(3, q.z);
    CHECK(7, gp.y); CHECK(0, gp.x);
    CHECK(5, ga[3]); CHECK(9, ga[8]); CHECK(0, ga[4]);
    int la[] = { [2] = 4, 5 };
    CHECK(16, sizeof(la)); CHECK(5, la[3]);
    CHECK(7, ops[0].fn(3, 4));
    CHECK(12, ops[1].fn(3, 4));
    CHECK('m', ops[1].name[0]);
    binop f = mul;
    CHECK(20, f(4, 5));
    struct outer o = { 1, { 'a', { 1, 2, 3 } }, 2.5 };
    CHECK(3, o.in.v[2]);
    CHECK('a', o.in.tag);
    CHECK(25, (int)(o.w * 10));
    struct outer o2;
    o2 = o;
    o2.in.v[1] = 9;
    CHECK(2, o.in.v[1]);
    CHECK(9, o2.in.v[1]);
    list n2 = { 2, 0 }, n1 = { 1, &n2 };
    CHECK(2, n1.next->v);
    char *words[] = { "alpha", "beta", "gamma", 0 };
    int total = 0;
    for (char **w = words; *w; w++) total += strlen(*w);
    CHECK(14, total);
    char grid[3][4] = { "abc", "def", "ghi" };
    CHECK('e', grid[1][1]);
    CHECK(0, grid[2][3]);
    printf("%d checks, %d failed\n", checks, fails);
    return fails != 0;
}
