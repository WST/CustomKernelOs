/* После .поля следующее позиционное значение заполняет следующее поле.
 * Так устроены статические маленькие int в CPython. */
typedef struct { unsigned lv_tag; unsigned ob_digit[1]; } V;
typedef struct { int ob_refcnt; void *ob_type; V long_value; } L;

static int typ;
static L n = {
    { 1 },
    &typ,
    .long_value = {
        .lv_tag = 8,
        { 11 },
    },
};
static struct { int a, b, c; } s = { .b = 2, 3 };

int main(void) {
    if (n.ob_refcnt != 1) return 1;
    if (n.ob_type != &typ) return 2;
    if (n.long_value.lv_tag != 8) return 3;
    if (n.long_value.ob_digit[0] != 11) return 4;
    if (s.a != 0 || s.b != 2 || s.c != 3) return 5;
    return 0;
}
