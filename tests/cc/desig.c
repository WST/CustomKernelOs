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

/* CPython: _Py_CODEUNIT — объединение, трамплин заполняет не первый член. */
typedef union {
    unsigned short cache;
    struct { unsigned char code; unsigned char arg; } op;
} CU;
static const CU tramp[] = {
    { .op.code = 25, .op.arg = 0 },
    { .op.code = 18, .op.arg = 0 },
    { .op.code = 128, .op.arg = 8 },
    { .cache = 0x0109 },
};

int main(void) {
    if (n.ob_refcnt != 1) return 1;
    if (n.ob_type != &typ) return 2;
    if (n.long_value.lv_tag != 8) return 3;
    if (n.long_value.ob_digit[0] != 11) return 4;
    if (s.a != 0 || s.b != 2 || s.c != 3) return 5;
    if (tramp[0].op.code != 25 || tramp[0].op.arg != 0) return 6;
    if (tramp[1].op.code != 18 || tramp[1].cache != 18) return 7;
    if (tramp[2].op.code != 128 || tramp[2].op.arg != 8) return 8;
    if (tramp[3].op.code != 9 || tramp[3].op.arg != 1) return 9;
    CU local = { .op.code = 7, .op.arg = 4 };
    if (local.op.code != 7 || local.op.arg != 4 || local.cache != 0x0407) return 10;
    return 0;
}
