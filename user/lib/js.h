/* js.h — движок JavaScript для MyOS (user/lib/js.c, jslib.c, jsre.c).
 *
 * Свой интерпретатор: исходник -> токены -> дерево (AST) -> обход дерева.
 * Поддерживается большая часть ES5 и популярное из ES2015+: let/const, стрелочные
 * функции, классы, шаблонные строки, деструктуризация, ...spread, for-of, ?. и ??,
 * Promise, Map/Set, JSON, RegExp. Нет: генераторов, async/await, Proxy, typed arrays.
 *
 * Строки хранятся байтами (UTF-8 как есть): length считает байты, а не символы UTF-16.
 *
 * Память: сборщик мусора «пометить и вымести». Запускается только в безопасной
 * точке — когда никакой JS не выполняется (js_gc() из цикла событий программы),
 * поэтому интерпретатору не надо следить за временными значениями на стеке.
 * Если скрипт за один раз набрал больше JS_HEAP_LIMIT — он прерывается ошибкой.
 *
 * Состояние движка глобальное (один «мир» на процесс); js_reset() выбрасывает всё. */
#ifndef JS_H
#define JS_H

#ifdef JS_HOST
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <stdarg.h>
#else
#include "myos.h"
#endif

typedef unsigned long long ju64;
typedef long long ji64;
typedef unsigned char ju8;

/* ------------------------------------------------------------------ значения */

enum { JS_UNDEF, JS_NULL, JS_BOOL, JS_NUM, JS_STR, JS_OBJ, JS_EMPTY };

typedef struct jsstr jsstr;
typedef struct jsobj jsobj;
typedef struct jsenv jsenv;

typedef struct jsval {
    int tag;
    union { double n; int b; jsstr *s; jsobj *o; char raw[8]; } u;   /* raw: чтобы gcc копировал без x87 */
} jsval;

struct gch { struct gch *next; ju8 type, mark; };        /* заголовок для сборщика */
enum { GC_STR = 1, GC_OBJ, GC_ENV };

struct jsstr {
    struct gch h;
    int len;
    unsigned hash;
    ju8 interned, symbol;
    int idx;                        /* у интернированной: число-индекс "12" -> 12, иначе -1 */
    jsstr *inext;                   /* цепочка в таблице интернированных строк */
    char s[];                       /* всегда с нулём в конце */
};

/* свойство объекта */
enum { P_ENUM = 1, P_WRITE = 2, P_CONF = 4, P_ACCESSOR = 8 };
struct prop {
    jsstr *key;                     /* всегда интернированная строка (или символ) */
    int flags;
    jsval v;                        /* значение, у аксессора — getter */
    jsobj *set;                     /* setter аксессора */
};

/* виды объектов */
enum {
    C_OBJECT, C_ARRAY, C_FUNC, C_NATIVE, C_BOUND, C_ERROR, C_BOOLEAN, C_NUMBER, C_STRING,
    C_DATE, C_REGEXP, C_ARGS, C_MAP, C_SET, C_PROMISE, C_ITER, C_HOST, C_CORO
};

typedef int (*jsnative)(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self);

/* «хозяйские» объекты (DOM): свои getter/setter поверх обычных свойств */
struct js_class {
    const char *name;
    int  (*get)(jsobj *o, jsstr *key, jsval *out);        /* 1 — обработано, 0 — нет, -1 — исключение */
    int  (*set)(jsobj *o, jsstr *key, jsval v);           /* то же */
    void (*mark)(jsobj *o);
    int  (*has)(jsobj *o, jsstr *key);                    /* для Proxy: 1 есть, 0 нет, -1 исключение */
    int  (*del)(jsobj *o, jsstr *key);
    jsobj *(*keys)(jsobj *o);                             /* собственные ключи (массив строк) или NULL */
};

struct jsobj {
    struct gch h;
    ju8 cls, extensible, is_ctor, is_arrow;
    jsobj *proto;
    struct prop *props;
    int nprops, pcap;
    int *hidx, hcap;                /* хеш-индекс свойств (когда их много) */
    jsval *arr;                     /* элементы массива (плотно) */
    int alen, acap;
    jsval iv[2];                    /* внутренние значения (примитив обёртки, дата, ...) */
    void *ptr;                      /* функция: узел AST / jsnative; хозяйский объект: данные */
    jsenv *env;                     /* замыкание */
    const struct js_class *hc;
};

struct jsenv {
    struct gch h;
    jsenv *parent;
    int n;
    jsval v[];
};

/* ------------------------------------------------------------------ API */

extern jsobj *js_global;            /* глобальный объект (window) */
extern jsval js_exc;                /* брошенное исключение (когда функция вернула -1) */
extern int js_steps_limit;          /* защита от вечных циклов: 0 — без ограничения */
extern int (*js_poll_hook)(void);   /* ~раз в 100 мс работы скрипта (окну — обработать события); не 0 — прервать скрипт */

void   js_init(void);
void   js_reset(void);              /* выбросить всё и начать заново */
int    js_eval(const char *src, int len, const char *file, jsval *result);   /* 0 / -1 */
int    js_call(jsval f, jsval thisv, int argc, jsval *argv, jsval *ret);       /* 0 / -1 */
int    js_construct(jsval f, int argc, jsval *argv, jsval *ret);
void   js_gc(void);
void   js_add_root(jsval *v);       /* дополнительный корень для сборщика */
void   js_set_mark_hook(void (*f)(void));
void   js_mark_val(jsval v);
void   js_mark_obj(jsobj *o);
int    js_run_jobs(void);           /* выполнить очередь микрозадач (Promise) */
const char *js_error_text(jsval e, char *buf, int max);   /* исключение -> текст */
void   js_set_print(void (*f)(const char *s, int n));     /* куда пишет console.log */
unsigned js_heap_used(void);
void   js_big_call(void (*fn)(void *), void *arg);   /* выполнить на большом стеке (2 МБ) */

/* значения */
static inline jsval js_undef(void) { jsval v; v.tag = JS_UNDEF; v.u.o = 0; return v; }
static inline jsval js_null(void) { jsval v; v.tag = JS_NULL; v.u.o = 0; return v; }
static inline jsval js_bool(int b) { jsval v; v.tag = JS_BOOL; v.u.b = !!b; return v; }
static inline jsval js_num(double d) { jsval v; v.tag = JS_NUM; v.u.n = d; return v; }
static inline jsval js_strv(jsstr *s) { jsval v; v.tag = JS_STR; v.u.s = s; return v; }
static inline jsval js_objv(jsobj *o) { jsval v; v.tag = JS_OBJ; v.u.o = o; return v; }
#define IS_OBJ(v) ((v).tag == JS_OBJ)
#define IS_STR(v) ((v).tag == JS_STR)
#define IS_NUM(v) ((v).tag == JS_NUM)
#define IS_NULLISH(v) ((v).tag == JS_UNDEF || (v).tag == JS_NULL)

jsstr *js_str(const char *s);
jsstr *js_strn(const char *s, int n);
jsstr *js_intern(const char *s, int n);
jsstr *js_intern_str(jsstr *s);
jsval  js_cstr(const char *s);      /* js_strv(js_str(s)) */

jsobj *js_new_object(void);
jsobj *js_new_object_proto(jsobj *proto);
jsobj *js_new_array(int n);
jsobj *js_new_native(jsnative fn, const char *name, int nargs);
jsobj *js_new_host(const struct js_class *c, void *data, jsobj *proto);
void   js_array_push(jsobj *a, jsval v);

int    js_get(jsval o, jsstr *key, jsval *out);             /* 0 / -1 */
int    js_get_c(jsval o, const char *key, jsval *out);
int    js_set(jsval o, jsstr *key, jsval v);
int    js_set_c(jsval o, const char *key, jsval v);
void   js_define(jsobj *o, const char *key, jsval v, int flags); /* без проверок */
void   js_defn(jsobj *o, const char *name, jsnative fn, int nargs);  /* метод (не перечисляемый) */
void   js_def_accessor(jsobj *o, const char *name, jsnative get, jsnative set);
struct prop *js_own_prop(jsobj *o, jsstr *key);
int    js_has_prop(jsobj *o, jsstr *key);
int    js_delete(jsobj *o, jsstr *key);

int    js_to_string(jsval v, jsstr **out);   /* 0 / -1 */
jsstr *js_to_str_nothrow(jsval v);
int    js_to_number(jsval v, double *out);
double js_num_nothrow(jsval v);
int    js_truthy(jsval v);
int    js_to_object(jsval v, jsobj **out);
int    js_is_callable(jsval v);
int    js_strict_eq(jsval a, jsval b);
int    js_loose_eq(jsval a, jsval b, int *res);
int    js_to_primitive(jsval v, int hint_string, jsval *out);
int    js_to_int32(jsval v, int *out);
const char *js_typeof(jsval v);
void   js_num_to_str(double d, char *buf);   /* как Number.prototype.toString(): буфер >= 32 */
double js_str_to_num(const char *s, int n);  /* как Number("...") */
double js_parse_float(const char *s, int n, int *used);

int    js_throw(const char *type, const char *fmt, ...);   /* -> -1 */
int    js_throw_val(jsval v);

/* служебное для jslib.c / jsre.c / dom */
extern jsobj *js_object_proto, *js_function_proto, *js_array_proto, *js_string_proto,
             *js_number_proto, *js_boolean_proto, *js_error_proto, *js_regexp_proto,
             *js_date_proto, *js_promise_proto, *js_map_proto, *js_set_proto, *js_iter_proto;
extern jsstr *S_length, *S_prototype, *S_constructor, *S_name, *S_message, *S_toString,
             *S_valueOf, *S_iterator, *S_next, *S_done, *S_value, *S_lastIndex, *S_then,
             *S_index, *S_input, *S_callee, *S_get, *S_set, *S_enumerable, *S_writable,
             *S_configurable, *S_stack, *S_toJSON;
void   js_lib_init(void);           /* jslib.c */
int    js_array_len(jsval o, int *len);
int    js_iterate(jsval it, int (*cb)(jsval v, void *ud), void *ud);   /* for-of */
jsobj *js_make_error(const char *type, const char *msg);
int    js_index_key(jsstr *key);    /* "123" -> 123, иначе -1 */
jsstr *js_concat(jsstr *a, jsstr *b);
jsstr *js_num_str(double d);
void  *js_alloc(int n);             /* память под AST и прочее, что живёт до js_reset */
int    js_regexp_create(jsstr *src, jsstr *flags, jsobj **out);   /* jsre.c */
int    js_regexp_exec(jsobj *re, jsstr *s, int start, int *caps, int ncap_max);  /* -> число групп или 0 */
int    js_regexp_ngroups(jsobj *re);
void   js_promise_resolve_fn(jsobj *p, jsval v);
void   js_promise_reject_fn(jsobj *p, jsval v);
jsobj *js_new_promise(void);
void   js_enqueue_job(jsval fn, jsval arg);
double js_now_ms(void);
void   js_print(const char *s, int n);
int    js_json_stringify(jsval v, jsstr **out);
int    js_json_parse(const char *s, int n, jsval *out);

/* строковый буфер */
typedef struct { char *p; int n, cap; } jsbuf;
void   jb_add(jsbuf *b, const char *s, int n);
void   jb_adds(jsbuf *b, const char *s);
void   jb_addc(jsbuf *b, char c);
jsstr *jb_str(jsbuf *b);            /* и освободить буфер */
void   jb_free(jsbuf *b);

/* x87: математика без libm */
double js_floor(double x);
double js_ceil(double x);
double js_fmod(double a, double b);
double js_sqrt(double x);
double js_pow(double a, double b);
double js_exp(double x);
double js_log(double x);
double js_sin(double x);
double js_cos(double x);
double js_tan(double x);
double js_atan2(double y, double x);
int    js_isnan(double x);
int    js_isinf(double x);
#define JS_NAN (__builtin_nan(""))
#define JS_INF (__builtin_inf())

#endif
