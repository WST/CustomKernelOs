/* jsint.h — внутренности движка JS: дерево разбора (AST) и общие функции
 * между js.c (значения, объекты), jsparse.c (разбор) и jsrun.c (выполнение). */
#ifndef JSINT_H
#define JSINT_H
#include "js.h"

/* виды узлов */
enum {
    /* выражения */
    N_NUM = 1, N_STR, N_TPL, N_TAGGED, N_REGEX, N_IDENT, N_THIS, N_SUPER, N_ARRAY, N_OBJECT,
    N_PROP, N_FUNC, N_CLASS, N_MEMBER, N_INDEX, N_CALL, N_NEW, N_UNARY, N_UPDATE, N_BINARY,
    N_LOGICAL, N_COND, N_ASSIGN, N_SEQ, N_SPREAD, N_OPTCHAIN, N_TRUE, N_FALSE, N_NULL, N_UNDEF,
    N_NEWTARGET,
    /* шаблоны деструктуризации */
    N_ARRPAT, N_OBJPAT, N_DEFAULT, N_REST, N_HOLE,
    /* операторы */
    N_VAR, N_DECL, N_EXPR, N_BLOCK, N_IF, N_FOR, N_FORIN, N_FOROF, N_WHILE, N_DOWHILE,
    N_RETURN, N_BREAK, N_CONTINUE, N_THROW, N_TRY, N_SWITCH, N_CASE, N_LABEL, N_EMPTY,
    N_FUNCDECL, N_CLASSDECL, N_SCRIPT, N_WITH, N_YIELD
};

/* операции (op у N_UNARY/N_BINARY/N_ASSIGN/...) */
enum {
    O_NONE, O_ADD, O_SUB, O_MUL, O_DIV, O_MOD, O_EXP, O_SHL, O_SHR, O_USHR, O_BAND, O_BOR, O_BXOR,
    O_EQ, O_NE, O_SEQ, O_SNE, O_LT, O_GT, O_LE, O_GE, O_IN, O_INSTANCEOF,
    O_AND, O_OR, O_NULLISH,
    O_NOT, O_BNOT, O_NEG, O_PLUS, O_TYPEOF, O_VOID, O_DELETE, O_INC, O_DEC
};

/* флаги */
enum {
    F_ARROW = 1, F_EXPRBODY = 2, F_USES_ARGS = 4, F_METHOD = 8, F_CTOR = 16, F_DERIVED = 32,
    F_PREFIX = 64, F_OPTIONAL = 128, F_STATIC = 256, F_COMPUTED = 512, F_GETTER = 1024,
    F_SETTER = 2048, F_SHORTHAND = 4096, F_CONST = 8192, F_SIMPLE_PARAMS = 16384,
    F_DEFAULT_CTOR = 32768, F_SPREAD_ARGS = 65536, F_WITH = 131072, F_GENERATOR = 262144
};
enum { K_VAR = 1, K_LET, K_CONST };

typedef struct node node;

struct scope {
    int nslots;
    node **funcs;                   /* объявления функций, которые надо создать при входе */
    int nfuncs;
    int self_slot;                  /* слот имени функционального выражения или -1 */
};

struct node {
    ju8 type, op, kind, pad;
    int flags;
    int line;
    node *a, *b, *c, *d;
    node **list;
    int n;
    jsstr *str;                     /* имя, строка, метка */
    jsstr *str2;                    /* «сырая» строка шаблона, флаги regexp */
    double num;
    int depth, slot;                /* найденная переменная: depth -1 — глобальная */
    struct scope *scope;            /* у блоков, функций, циклов */
    const char *file;
};

node *js_parse(const char *src, int len, const char *file, int as_function, char *err, int errmax);
int js_module_compile(const char *src, int len, jsval *out);
jsobj *js_make_closure(node *fn, jsenv *env);
int js_run_script(node *script, jsval *result);
int js_call_depth_check(void);
struct js_frame { const char *file; int line; jsstr *name; };
extern struct js_frame js_frames[64];
extern int js_cur_line;
extern const char *js_cur_file;
void js_stack_text(jsbuf *b);
void js_coro_free(void *c);
void js_coro_mark(void *c);
int  js_coro_suspended(void);
extern jsobj *js_generator_proto;
extern unsigned js_alloc_since_gc;
extern jsstr *js_kw_names[];

/* итерация (for-of, spread, деструктуризация) */
struct js_iter {
    int kind;                       /* 1 массив, 2 строка, 3 общий протокол */
    jsval obj, iter, next;
    int i;
};
int js_iter_open(jsval v, struct js_iter *it);
int js_iter_step(struct js_iter *it, jsval *out, int *done);   /* 0 / -1 */
void js_iter_close(struct js_iter *it);

/* js.c */
jsstr *js_perm(const char *s);
jsstr *js_perm_str(jsstr *s);
jsenv *js_new_env(jsenv *parent, int n);
struct prop *js_define_key(jsobj *o, jsstr *key, jsval v, int flags);
int js_get_obj(jsobj *o, jsstr *key, jsval recv, jsval *out);
int js_set_obj(jsobj *o, jsstr *key, jsval v, jsval recv);
int js_call_getter(jsval getter, jsval recv, jsval *out);
#endif
