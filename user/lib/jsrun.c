/* jsrun.c — выполнение JavaScript: обход дерева разбора.
 *
 * Каждая функция-исполнитель возвращает статус: ST_OK, ST_THROW (исключение
 * лежит в js_exc), ST_RETURN / ST_BREAK / ST_CONTINUE (для операторов) или
 * ST_SHORT (a?.b — цепочка оборвалась на null/undefined). */
#include "jsint.h"

enum { ST_OK, ST_THROW, ST_RETURN, ST_BREAK, ST_CONTINUE, ST_SHORT };

int js_max_depth = 500;
int js_cur_line;
const char *js_cur_file;
struct js_frame js_frames[64];                 /* откуда вызвали каждую функцию (для error.stack) */
double js_deadline_ms;              /* после этого времени скрипт прерывается (0 — без срока) */
unsigned js_heap_limit = 48u << 20; /* столько можно набрать мусора за один запуск */
int js_is_construct;                /* для встроенных функций: нас вызвали через new */

static int call_depth;
struct coro;
static struct coro *cur_coro;            /* выполняемая сейчас сопрограмма (генератор / async) */
static int n_suspended;
static unsigned steps;
static jsstr *jump_label;           /* метка break/continue */
static jsval completion;            /* значение последнего выражения (для js_eval) */

static int ev(node *n, jsenv *env, jsval *out);
static int exec_stmt(node *s, jsenv *env, jsval *ret);
static int exec_list(node **l, int n, jsenv *env, jsval *ret);
static int bind_pattern(node *p, jsval v, jsenv *env, int decl);
static int call_closure(jsobj *fo, jsval thisv, int argc, jsval *argv, jsval *ret, int is_new);

/* ------------------------------------------------------------------ очередь заданий (Promise) */

static jsval *jobs;
static int jobs_n, jobs_cap, jobs_head;

void js_enqueue_job(jsval fn, jsval arg) {
    if (jobs_n + 2 > jobs_cap) {
        int live = jobs_n - jobs_head;
        if (jobs_head > 0 && live + 2 <= jobs_cap) {
            memmove(jobs, jobs + jobs_head, sizeof(jsval) * live);
        } else {
            int cap = jobs_cap ? jobs_cap * 2 : 64;
            jsval *p = malloc(sizeof(jsval) * cap);
            if (!p) return;
            if (live) memcpy(p, jobs + jobs_head, sizeof(jsval) * live);
            free(jobs);
            jobs = p;
            jobs_cap = cap;
        }
        jobs_n = live;
        jobs_head = 0;
    }
    jobs[jobs_n++] = fn;
    jobs[jobs_n++] = arg;
}

int js_run_jobs(void) {
    int ran = 0;
    while (jobs_head < jobs_n) {
        jsval fn = jobs[jobs_head], arg = jobs[jobs_head + 1], r;
        jobs_head += 2;
        if (jobs_head == jobs_n) jobs_head = jobs_n = 0;
        if (js_call(fn, js_undef(), 1, &arg, &r) < 0) {
            char buf[200];
            const char *m = js_error_text(js_exc, buf, sizeof(buf));
            js_print("Uncaught (in promise) ", 22);
            js_print(m, strlen(m));
            js_print("\n", 1);
        }
        ran++;
    }
    return ran;
}

void js_mark_runtime(void) {
    for (int i = jobs_head; i < jobs_n; i++) js_mark_val(jobs[i]);
    js_mark_val(completion);
}

void js_run_reset(void) {
    cur_coro = NULL;
    n_suspended = 0;
    free(jobs);
    jobs = NULL;
    jobs_n = jobs_cap = jobs_head = 0;
    call_depth = 0;
    completion = js_undef();
}

/* ------------------------------------------------------------------ мелочи */

static inline jsenv *env_at(jsenv *e, int depth) {
    while (depth-- > 0) e = e->parent;
    return e;
}

static int check_limits(void) {
    if (++steps & 4095) return 0;
    if (js_alloc_since_gc > js_heap_limit) return js_throw("RangeError", "out of memory (script uses too much)");
    if (js_deadline_ms > 0 && js_now_ms() > js_deadline_ms) return js_throw("Error", "script takes too long - stopped");
    return 0;
}

/* ключ свойства из значения: строка (интернированная) или символ */
static int to_key(jsval v, jsstr **out) {
    if (v.tag == JS_STR) { *out = js_intern_str(v.u.s); return 0; }
    if (v.tag == JS_NUM && v.u.n >= 0 && v.u.n < 1e9 && v.u.n == (int)v.u.n) {
        char b[16];
        int n = snprintf(b, sizeof(b), "%d", (int)v.u.n);
        *out = js_intern(b, n);
        return 0;
    }
    jsstr *s;
    if (js_to_string(v, &s) < 0) return -1;
    *out = js_intern_str(s);
    return 0;
}

/* быстрые a[i] для массивов */
static int get_index(jsval o, jsval k, jsval *out) {
    if (o.tag == JS_OBJ && k.tag == JS_NUM) {
        jsobj *a = o.u.o;
        double d = k.u.n;
        if ((a->cls == C_ARRAY || a->cls == C_ARGS) && d >= 0 && d < a->alen && d == (int)d && !a->hc) {
            jsval v = a->arr[(int)d];
            if (v.tag != JS_EMPTY) { *out = v; return 0; }
        }
    } else if (o.tag == JS_STR && k.tag == JS_NUM) {
        double d = k.u.n;
        if (d >= 0 && d < o.u.s->len && d == (int)d) { *out = js_strv(js_intern(o.u.s->s + (int)d, 1)); return 0; }
    }
    if (IS_NULLISH(o)) {
        jsstr *ks = js_to_str_nothrow(k);
        return js_throw("TypeError", "Cannot read properties of %s (reading '%.60s')", o.tag == JS_NULL ? "null" : "undefined", ks->s);
    }
    jsstr *key;
    if (to_key(k, &key) < 0) return -1;
    return js_get(o, key, out);
}

static int set_index(jsval o, jsval k, jsval v) {
    if (o.tag == JS_OBJ && k.tag == JS_NUM) {
        jsobj *a = o.u.o;
        double d = k.u.n;
        if (a->cls == C_ARRAY && d >= 0 && d < a->alen && d == (int)d && !a->hc) { a->arr[(int)d] = v; return 0; }
        if (a->cls == C_ARRAY && d == a->alen && d == (int)d && !a->hc) { js_array_push(a, v); return 0; }
    }
    jsstr *key;
    if (to_key(k, &key) < 0) return -1;
    return js_set(o, key, v);
}

/* ------------------------------------------------------------------ глобальные переменные */

static int get_global(jsstr *name, jsval *out, int quiet) {
    for (jsobj *o = js_global; o; o = o->proto) {
        if (o->hc && o->hc->get) {
            int r = o->hc->get(o, name, out);
            if (r) return r < 0 ? -1 : 0;
        }
        struct prop *p = js_own_prop(o, name);
        if (p) {
            if (p->flags & P_ACCESSOR) return js_call_getter(p->v, js_objv(js_global), out);
            *out = p->v;
            return 0;
        }
    }
    if (quiet) { *out = js_undef(); return 1; }
    return js_throw("ReferenceError", "%.80s is not defined", name->s);
}

/* имя внутри with (obj): есть такое свойство у obj? */
static jsobj *with_obj(node *id, jsenv *env) {
    jsval o = env_at(env, (int)id->num)->v[0];
    if (o.tag != JS_OBJ || !js_has_prop(o.u.o, id->str)) return NULL;
    return o.u.o;
}

static int assign_ident(node *id, jsenv *env, jsval v, int decl) {
    if ((id->flags & F_WITH) && !decl) {
        jsobj *w = with_obj(id, env);
        if (w) return js_set_obj(w, id->str, v, js_objv(w));
    }
    if (id->depth >= 0) {
        if ((id->flags & F_CONST) && !decl) return js_throw("TypeError", "Assignment to constant variable '%.60s'", id->str->s);
        env_at(env, id->depth)->v[id->slot] = v;
        return 0;
    }
    return js_set_obj(js_global, id->str, v, js_objv(js_global));
}

/* ------------------------------------------------------------------ функции */

extern jsobj *js_async_function_proto;
jsobj *js_make_closure(node *fn, jsenv *env) {
    jsobj *f = js_new_object_proto(fn->kind ? js_async_function_proto : js_function_proto);
    f->cls = C_FUNC;
    f->ptr = fn;
    f->env = env;
    f->is_arrow = (fn->flags & F_ARROW) != 0;
    f->is_ctor = !(fn->flags & (F_ARROW | F_METHOD)) || (fn->flags & F_CTOR);
    if (fn->kind) f->is_ctor = 0;                          /* async */
    return f;
}

/* name / length / prototype у функций создаются при первом обращении */
int js_func_lazy(jsobj *o, jsstr *key, jsval *out) {
    node *fn = o->ptr;
    if (key == S_prototype) {
        if (!o->is_ctor) return 0;
        jsobj *p = js_new_object();
        js_define_key(p, S_constructor, js_objv(o), P_WRITE | P_CONF);
        js_define_key(o, S_prototype, js_objv(p), P_WRITE);
        *out = js_objv(p);
        return 1;
    }
    if (key == S_name) { *out = js_strv(fn->str ? fn->str : js_intern("", 0)); return 1; }
    if (key == S_length) {
        int n = 0;
        while (n < fn->n && fn->list[n]->type != N_DEFAULT && fn->list[n]->type != N_REST) n++;
        *out = js_num(n);
        return 1;
    }
    return 0;
}

static jsobj *make_arguments(int argc, jsval *argv, jsobj *callee) {
    jsobj *a = js_new_object();
    a->cls = C_ARGS;
    for (int i = 0; i < argc; i++) js_array_push(a, argv[i]);
    if (!argc) { a->arr = NULL; }
    js_define_key(a, S_length, js_num(argc), P_WRITE | P_CONF);
    js_define_key(a, S_callee, js_objv(callee), P_WRITE | P_CONF);
    return a;
}

static void instantiate_funcs(struct scope *sc, jsenv *env) {
    for (int i = 0; i < sc->nfuncs; i++) {
        node *d = sc->funcs[i];
        jsobj *f = js_make_closure(d->a, env);
        if (env && d->slot >= 0) env->v[d->slot] = js_objv(f);
        else js_set_obj(js_global, d->str, js_objv(f), js_objv(js_global));
    }
}


/* ------------------------------------------------------------------ сопрограммы
 * Генератор или async-функция выполняется на своём стеке. yield / await
 * переключают стек обратно на вызвавшего (js_coro_swap — несколько команд
 * ассемблера: сохранить регистры, поменять указатель стека). */

#if defined(__x86_64__)
__asm__(".text\n.globl js_coro_swap\njs_coro_swap:\n"
        "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
        "  movq %rsp, (%rdi)\n  movq %rsi, %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n  ret\n");
#define CORO_REGS 6
#elif defined(__i386__)
__asm__(".text\n.globl js_coro_swap\njs_coro_swap:\n"
        "  pushl %ebp\n  pushl %ebx\n  pushl %esi\n  pushl %edi\n"
        "  movl 20(%esp), %eax\n  movl 24(%esp), %edx\n"
        "  movl %esp, (%eax)\n  movl %edx, %esp\n"
        "  popl %edi\n  popl %esi\n  popl %ebx\n  popl %ebp\n  ret\n");
#define CORO_REGS 4
#endif
void js_coro_swap(void **save, void *to);

#ifdef JS_HOST
#define CORO_STACK (512 * 1024)
#define CORO_DEPTH 150
#else
#define CORO_STACK (160 * 1024)
#define CORO_DEPTH 130
#endif

enum { CR_YIELD, CR_DONE, CR_THROW, CR_AWAIT };

struct coro {
    void *sp, *caller_sp;
    char *stack;
    int state;                      /* 0 не начат, 1 приостановлен, 2 работает, 3 закончен */
    int resume_kind;                /* 0 next, 1 throw, 2 return */
    int result_kind;                /* CR_* — почему вернулись к вызвавшему */
    int base, inner, limit;         /* глубина вызовов: своя часть и предел */
    jsobj *fo, *holder, *promise;   /* функция, объект-держатель (для сборщика), promise async-функции */
    jsenv *env;
    jsval transfer;
    int is_async;
    struct coro *prev;
};

static jsobj *return_marker;        /* «исключение», которым return() выходит из генератора */
jsobj *js_generator_proto;

int js_coro_suspended(void) { return n_suspended; }

void js_coro_mark(void *p) {
    struct coro *c = p;
    js_mark_obj(c->fo);
    js_mark_obj(c->promise);
    js_mark_val(c->transfer);
    if (c->env) js_mark_val(js_objv((jsobj *)c->env));     /* env — тоже объект сборщика */
}

void js_coro_free(void *p) {
    struct coro *c = p;
    if (!c) return;
    if (c->state == 1) n_suspended--;
    free(c->stack);
    free(c);
}

/* стек приостановленной сопрограммы: от sp до вершины (для сборщика) */
void js_coro_stack(void *p, void **lo, void **hi) {
    struct coro *c = p;
    *lo = *hi = NULL;
    if (c->state != 1 || !c->stack) return;
    *lo = c->sp;
    *hi = c->stack + CORO_STACK;
}

static int call_body(jsobj *fo, node *fn, jsenv *e, jsval *ret);

static void coro_start(void) {
    struct coro *c = cur_coro;
    jsval r = js_undef();
    int st = call_body(c->fo, c->fo->ptr, c->env, &r);
    if (st == ST_THROW && IS_OBJ(js_exc) && js_exc.u.o->proto == return_marker) {   /* return() у генератора */
        r = js_exc.u.o->iv[0];
        js_exc = js_undef();
        st = ST_OK;
    }
    c->transfer = st == ST_THROW ? js_exc : r;
    if (st == ST_THROW) js_exc = js_undef();
    c->result_kind = st == ST_THROW ? CR_THROW : CR_DONE;
    c->state = 3;
    c->inner = call_depth - c->base;
    js_coro_swap(&c->sp, c->caller_sp);
    for (;;) {}                                                  /* сюда не возвращаемся */
}

/* продолжить сопрограмму: -> CR_* и значение */
static int coro_resume(struct coro *c, int kind, jsval v, jsval *out) {
    if (c->state == 3) { *out = js_undef(); return CR_DONE; }
    if (c->state == 2) { js_throw("TypeError", "Generator is already running"); *out = js_exc; js_exc = js_undef(); return CR_THROW; }
    if (call_depth >= js_max_depth - 20) { js_throw("RangeError", "Maximum call stack size exceeded"); *out = js_exc; js_exc = js_undef(); return CR_THROW; }
    if (c->state == 0) {
        if (kind != 0) { c->state = 3; *out = v; return kind == 1 ? CR_THROW : CR_DONE; }
        c->stack = malloc(CORO_STACK);
        if (!c->stack) { js_throw("RangeError", "out of memory"); *out = js_exc; js_exc = js_undef(); return CR_THROW; }
        void **top = (void **)(((unsigned long)(c->stack + CORO_STACK)) & ~15ul);
        *--top = 0;                                             /* «адрес возврата» для coro_start */
        *--top = (void *)coro_start;
        for (int i = 0; i < CORO_REGS; i++) *--top = 0;
        c->sp = top;
        c->inner = 0;
    } else n_suspended--;
    c->resume_kind = kind;
    c->transfer = v;
    c->prev = cur_coro;
    int save_depth = call_depth, save_line = js_cur_line;
    const char *save_file = js_cur_file;
    jsstr *save_label = jump_label;
    c->base = call_depth;
    call_depth = c->base + c->inner;
    c->limit = c->base + CORO_DEPTH;
    cur_coro = c;
    c->state = 2;
    js_coro_swap(&c->caller_sp, c->sp);
    /* вернулись: yield, await, return или исключение */
    cur_coro = c->prev;
    call_depth = save_depth;
    js_cur_line = save_line;
    js_cur_file = save_file;
    jump_label = save_label;
    *out = c->transfer;
    c->transfer = js_undef();
    if (c->state == 3) { free(c->stack); c->stack = NULL; }
    else { c->state = 1; n_suspended++; }
    return c->result_kind;
}

/* изнутри сопрограммы: отдать значение и ждать продолжения */
static int coro_suspend(int why, jsval v, jsval *in) {
    struct coro *c = cur_coro;
    c->transfer = v;
    c->result_kind = why;
    c->inner = call_depth - c->base;
    js_coro_swap(&c->sp, c->caller_sp);
    /* продолжили */
    *in = c->transfer;
    c->transfer = js_undef();
    if (c->resume_kind == 1) { js_exc = *in; return ST_THROW; }
    if (c->resume_kind == 2) {
        jsobj *m = js_new_object_proto(return_marker);
        m->iv[0] = *in;
        js_exc = js_objv(m);
        return ST_THROW;
    }
    return ST_OK;
}

static struct coro *new_coro(jsobj *fo, jsenv *env, int is_async) {
    struct coro *c = malloc(sizeof(struct coro));
    memset(c, 0, sizeof(*c));
    c->fo = fo;
    c->env = env;
    c->is_async = is_async;
    c->transfer = js_undef();
    c->holder = js_new_object_proto(is_async ? js_object_proto : js_generator_proto);
    c->holder->cls = C_CORO;
    c->holder->ptr = c;
    return c;
}

/* --- генераторы: next / throw / return */

static jsval iter_result_obj(jsval v, int done) {
    jsobj *r = js_new_object();
    js_define_key(r, S_value, v, P_ENUM | P_WRITE | P_CONF);
    js_define_key(r, S_done, js_bool(done), P_ENUM | P_WRITE | P_CONF);
    return js_objv(r);
}

static int gen_step(jsval thisv, int kind, jsval v, jsval *ret) {
    if (!IS_OBJ(thisv) || thisv.u.o->cls != C_CORO || !thisv.u.o->ptr) return js_throw("TypeError", "next method called on incompatible receiver");
    struct coro *c = thisv.u.o->ptr;
    jsval out;
    int r = coro_resume(c, kind, v, &out);
    if (r == CR_THROW) return js_throw_val(out);
    *ret = iter_result_obj(out, r != CR_YIELD);
    return 0;
}

static int Gen_next(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self) { (void)self; return gen_step(thisv, 0, argc ? argv[0] : js_undef(), ret); }
static int Gen_throw(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self) { (void)self; return gen_step(thisv, 1, argc ? argv[0] : js_undef(), ret); }
static int Gen_return(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self) { (void)self; return gen_step(thisv, 2, argc ? argv[0] : js_undef(), ret); }
static int Gen_iter(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self) { (void)argc; (void)argv; (void)self; *ret = thisv; return 0; }

/* --- async-функции: await подписывается на promise и отдаёт управление */

static int Async_resume(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self) {
    (void)thisv;
    struct coro *c = self->iv[0].u.o->ptr;
    jsval out;
    int r = coro_resume(c, self->is_arrow ? 1 : 0, argc ? argv[0] : js_undef(), &out);
    if (r == CR_DONE) js_promise_resolve_fn(c->promise, out);
    else if (r == CR_THROW) js_promise_reject_fn(c->promise, out);
    *ret = js_undef();
    return 0;
}

static int start_async(jsobj *fo, jsenv *env, jsval *ret) {
    struct coro *c = new_coro(fo, env, 1);
    c->promise = js_new_promise();
    jsval out;
    jsval hold = js_objv(c->holder);
    int r = coro_resume(c, 0, js_undef(), &out);
    if (r == CR_DONE) js_promise_resolve_fn(c->promise, out);
    else if (r == CR_THROW) js_promise_reject_fn(c->promise, out);
    (void)hold;
    *ret = js_objv(c->promise);
    return ST_OK;
}

static int coro_await(jsval v, jsval *out) {
    struct coro *c = cur_coro;
    jsobj *p;
    if (IS_OBJ(v) && v.u.o->cls == C_PROMISE) p = v.u.o;
    else { p = js_new_promise(); js_promise_resolve_fn(p, v); }
    jsobj *ok = js_new_native(Async_resume, "", 1), *bad = js_new_native(Async_resume, "", 1);
    ok->iv[0] = bad->iv[0] = js_objv(c->holder);
    bad->is_arrow = 1;
    jsval f, r, args[2] = { js_objv(ok), js_objv(bad) };
    if (js_get(js_objv(p), S_then, &f) < 0) return ST_THROW;
    if (js_call(f, js_objv(p), 2, args, &r) < 0) return ST_THROW;
    return coro_suspend(CR_AWAIT, js_undef(), out);
}

void coro_init_protos(void) {
    js_generator_proto = js_new_object_proto(js_iter_proto);
    js_defn(js_generator_proto, "next", Gen_next, 1);
    js_defn(js_generator_proto, "throw", Gen_throw, 1);
    js_defn(js_generator_proto, "return", Gen_return, 1);
    js_define_key(js_generator_proto, S_iterator, js_objv(js_new_native(Gen_iter, "[Symbol.iterator]", 0)), P_WRITE | P_CONF);
    return_marker = js_new_object();
    js_define(js_generator_proto, "%return", js_objv(return_marker), 0);
}

static int call_body(jsobj *fo, node *fn, jsenv *e, jsval *ret) {
    if (fn->flags & F_EXPRBODY) {
        int st = ev(fn->a, e, ret);
        return st == ST_THROW ? ST_THROW : ST_OK;
    }
    *ret = js_undef();
    int st = exec_list(fn->a->list, fn->a->n, e, ret);
    (void)fo;
    if (st == ST_THROW) return ST_THROW;
    if (st != ST_RETURN) *ret = js_undef();
    return ST_OK;
}

static int init_fields(jsobj *ctor, jsval thisv) {
    if (ctor->iv[1].tag != JS_OBJ) return 0;
    jsval r;
    return js_call(ctor->iv[1], thisv, 0, NULL, &r);
}

static int construct_with_this(jsval parent, jsval thisv, int argc, jsval *argv) {
    if (!IS_OBJ(parent)) return js_throw("TypeError", "Super constructor is not a constructor");
    jsobj *p = parent.u.o;
    jsval r;
    if (p->cls == C_FUNC) return call_closure(p, thisv, argc, argv, &r, 1) == ST_THROW ? -1 : 0;
    if (p->cls == C_NATIVE) {
        js_is_construct = 1;
        int res = ((jsnative)p->ptr)(thisv, argc, argv, &r, p);
        js_is_construct = 0;
        if (res < 0) return -1;
        if (IS_OBJ(r) && r.u.o != thisv.u.o && IS_OBJ(thisv)) {   /* встроенный сделал новый объект: перенесём его суть */
            jsobj *t = thisv.u.o, *s = r.u.o;
            if (t->cls == C_OBJECT) {
                t->cls = s->cls;
                t->iv[0] = s->iv[0];
                t->iv[1] = s->iv[1];
                t->ptr = s->ptr;
                for (int i = 0; i < s->nprops; i++) js_define_key(t, s->props[i].key, s->props[i].v, s->props[i].flags);
                for (int i = 0; i < s->alen; i++) js_array_push(t, s->arr[i]);
            }
        }
        return 0;
    }
    if (p->cls == C_BOUND) return construct_with_this(p->iv[0], thisv, argc, argv);
    return js_throw("TypeError", "Super constructor is not a constructor");
}


static int call_closure(jsobj *fo, jsval thisv, int argc, jsval *argv, jsval *ret, int is_new) {
    node *fn = fo->ptr;
    struct scope *sc = fn->scope;
    if (call_depth >= js_max_depth || (cur_coro && call_depth >= cur_coro->limit)) return js_throw("RangeError", "Maximum call stack size exceeded") ? ST_THROW : ST_THROW;
    if (check_limits() < 0) return ST_THROW;
    if ((fn->flags & F_CTOR) && !is_new) return js_throw("TypeError", "Class constructor %.60s cannot be invoked without 'new'", fn->str ? fn->str->s : "") ? ST_THROW : ST_THROW;
    call_depth++;
    jsenv *e = js_new_env(fo->env, sc->nslots);
    if (!(fn->flags & F_ARROW)) {
        if (IS_NULLISH(thisv)) thisv = js_objv(js_global);
        e->v[0] = thisv;
        e->v[2] = js_objv(fo);
        if (fn->flags & F_USES_ARGS) e->v[1] = js_objv(make_arguments(argc, argv, fo));
    }
    if (sc->self_slot >= 0) e->v[sc->self_slot] = js_objv(fo);
    int st = ST_OK;
    if (fn->flags & F_SIMPLE_PARAMS) {
        for (int i = 0; i < fn->n; i++) e->v[fn->list[i]->slot] = i < argc ? argv[i] : js_undef();
    } else {
        for (int i = 0; i < fn->n && st == ST_OK; i++) {
            node *p = fn->list[i];
            if (p->type == N_REST) {
                jsobj *rest = js_new_array(0);
                for (int k = i; k < argc; k++) js_array_push(rest, argv[k]);
                st = bind_pattern(p->a, js_objv(rest), e, 1);
            } else st = bind_pattern(p, i < argc ? argv[i] : js_undef(), e, 1);
        }
    }
    if (st == ST_OK) {
        instantiate_funcs(sc, e);
        if (fn->flags & F_CTOR) {
            if (fn->flags & F_DERIVED) {
                if (fn->flags & F_DEFAULT_CTOR) {
                    if (construct_with_this(js_objv(fo->proto), thisv, argc, argv) < 0 || init_fields(fo, thisv) < 0) st = ST_THROW;
                }
            } else if (init_fields(fo, thisv) < 0) st = ST_THROW;
        }
    }
    if (st == ST_OK) {
        int line = js_cur_line;
        const char *file = js_cur_file;
        if (call_depth <= 64) { js_frames[call_depth - 1].file = file; js_frames[call_depth - 1].line = line; js_frames[call_depth - 1].name = fn->str; }
        if (fn->flags & F_GENERATOR) { struct coro *c = new_coro(fo, e, 0); *ret = js_objv(c->holder); }
        else if (fn->kind) st = start_async(fo, e, ret);
        else st = call_body(fo, fn, e, ret);
        if (st != ST_THROW) { js_cur_line = line; js_cur_file = file; }
    }
    call_depth--;
    return st;
}

/* вызов функции из bind(): свои аргументы + переданные */
static __attribute__((noinline)) int call_bound(jsobj *fo, int argc, jsval *argv, jsval *ret, int construct) {
    int n = fo->alen + argc;
    jsval tmp[16], *args = n <= 16 ? tmp : malloc(sizeof(jsval) * n);
    for (int i = 0; i < fo->alen; i++) args[i] = fo->arr[i];
    for (int i = 0; i < argc; i++) args[fo->alen + i] = argv[i];
    int r = construct ? js_construct(fo->iv[0], n, args, ret) : js_call(fo->iv[0], fo->iv[1], n, args, ret);
    if (args != tmp) free(args);
    return r;
}

int js_call(jsval f, jsval thisv, int argc, jsval *argv, jsval *ret) {
    *ret = js_undef();
    if (f.tag != JS_OBJ) return js_throw("TypeError", "%s is not a function", js_typeof(f));
    jsobj *fo = f.u.o;
    switch (fo->cls) {
    case C_FUNC:
        return call_closure(fo, thisv, argc, argv, ret, 0) == ST_THROW ? -1 : 0;
    case C_NATIVE: {
        js_is_construct = 0;
        if (call_depth >= js_max_depth) return js_throw("RangeError", "Maximum call stack size exceeded");
        call_depth++;
        if (call_depth <= 64) { js_frames[call_depth - 1].file = js_cur_file; js_frames[call_depth - 1].line = js_cur_line; js_frames[call_depth - 1].name = NULL; }
        int r = ((jsnative)fo->ptr)(thisv, argc, argv, ret, fo);
        call_depth--;
        return r < 0 ? -1 : 0;
    }
    case C_BOUND: return call_bound(fo, argc, argv, ret, 0);
    }
    return js_throw("TypeError", "object is not a function");
}

int js_construct(jsval f, int argc, jsval *argv, jsval *ret) {
    if (f.tag != JS_OBJ) return js_throw("TypeError", "%s is not a constructor", js_typeof(f));
    jsobj *fo = f.u.o;
    if (fo->cls == C_BOUND) return call_bound(fo, argc, argv, ret, 1);
    if (fo->cls != C_FUNC && fo->cls != C_NATIVE) return js_throw("TypeError", "object is not a constructor");
    if (fo->cls == C_FUNC && !fo->is_ctor) return js_throw("TypeError", "%.60s is not a constructor",
                                                          ((node *)fo->ptr)->str ? ((node *)fo->ptr)->str->s : "function");
    jsval protov;
    if (js_get(f, S_prototype, &protov) < 0) return -1;
    jsobj *obj = js_new_object_proto(IS_OBJ(protov) ? protov.u.o : js_object_proto);
    jsval r;
    if (fo->cls == C_FUNC) {
        if (call_closure(fo, js_objv(obj), argc, argv, &r, 1) == ST_THROW) return -1;
    } else {
        js_is_construct = 1;
        call_depth++;
        int res = ((jsnative)fo->ptr)(js_objv(obj), argc, argv, &r, fo);
        call_depth--;
        js_is_construct = 0;
        if (res < 0) return -1;
    }
    *ret = IS_OBJ(r) ? r : js_objv(obj);
    return 0;
}

/* ------------------------------------------------------------------ итерация */

int js_iter_open(jsval v, struct js_iter *it) {
    memset(it, 0, sizeof(*it));
    it->obj = v;
    if (v.tag == JS_OBJ && (v.u.o->cls == C_ARRAY || v.u.o->cls == C_ARGS) && !js_own_prop(v.u.o, S_iterator)) { it->kind = 1; return 0; }
    if (v.tag == JS_STR) { it->kind = 2; return 0; }
    jsval f;
    if (IS_NULLISH(v)) return js_throw("TypeError", "%s is not iterable", v.tag == JS_NULL ? "null" : "undefined");
    if (js_get(v, S_iterator, &f) < 0) return -1;
    if (!js_is_callable(f)) return js_throw("TypeError", "object is not iterable");
    if (js_call(f, v, 0, NULL, &it->iter) < 0) return -1;
    if (js_get(it->iter, S_next, &it->next) < 0) return -1;
    it->kind = 3;
    return 0;
}

int js_iter_step(struct js_iter *it, jsval *out, int *done) {
    *done = 0;
    if (it->kind == 1) {
        jsobj *a = it->obj.u.o;
        if (it->i >= a->alen) { *done = 1; return 0; }
        *out = a->arr[it->i++];
        if (out->tag == JS_EMPTY) *out = js_undef();
        return 0;
    }
    if (it->kind == 2) {
        jsstr *s = it->obj.u.s;
        if (it->i >= s->len) { *done = 1; return 0; }
        int st = it->i, c = (ju8)s->s[st], l = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        if (st + l > s->len) l = s->len - st;
        it->i += l;
        *out = js_strv(js_strn(s->s + st, l));
        return 0;
    }
    jsval r, d;
    if (js_call(it->next, it->iter, 0, NULL, &r) < 0) return -1;
    if (!IS_OBJ(r)) return js_throw("TypeError", "iterator result is not an object");
    if (js_get(r, S_done, &d) < 0) return -1;
    if (js_truthy(d)) { *done = 1; return 0; }
    return js_get(r, S_value, out);
}

void js_iter_close(struct js_iter *it) {
    if (it->kind != 3) return;
    jsval f, r;
    if (js_get_c(it->iter, "return", &f) == 0 && js_is_callable(f)) {
        jsval save = js_exc;
        js_call(f, it->iter, 0, NULL, &r);
        js_exc = save;
    }
}

/* ------------------------------------------------------------------ деструктуризация */

static __attribute__((noinline)) int bind_objpat(node *p, jsval v, jsenv *env, int decl) {
        if (IS_NULLISH(v)) return js_throw("TypeError", "Cannot destructure '%s' as it is %s", v.tag == JS_NULL ? "null" : "undefined",
                                            v.tag == JS_NULL ? "null" : "undefined") ? ST_THROW : ST_THROW;
        jsstr *used[64];
        int nused = 0;
        for (int i = 0; i < p->n; i++) {
            node *e = p->list[i];
            if (e->type == N_REST) {
                jsobj *rest = js_new_object(), *src;
                if (js_to_object(v, &src) < 0) return ST_THROW;
                if (src->cls == C_ARRAY) for (int k = 0; k < src->alen; k++) {
                    char b[16];
                    int l = snprintf(b, sizeof(b), "%d", k);
                    js_define_key(rest, js_intern(b, l), src->arr[k], P_ENUM | P_WRITE | P_CONF);
                }
                for (int k = 0; k < src->nprops; k++) {
                    struct prop *pr = &src->props[k];
                    if (!(pr->flags & P_ENUM) || pr->key->symbol) continue;
                    int skip = 0;
                    for (int u = 0; u < nused; u++) if (used[u] == pr->key) skip = 1;
                    if (skip) continue;
                    jsval x;
                    if (js_get(v, pr->key, &x) < 0) return ST_THROW;
                    js_define_key(rest, pr->key, x, P_ENUM | P_WRITE | P_CONF);
                }
                int st = bind_pattern(e->a, js_objv(rest), env, decl);
                if (st) return st;
                continue;
            }
            jsstr *key = e->str;
            if (e->flags & F_COMPUTED) {
                jsval kv;
                int st = ev(e->a, env, &kv);
                if (st) return st;
                if (to_key(kv, &key) < 0) return ST_THROW;
            }
            if (nused < 64) used[nused++] = key;
            jsval x;
            if (js_get(v, key, &x) < 0) return ST_THROW;
            int st = bind_pattern(e->b, x, env, decl);
            if (st) return st;
        }
        return ST_OK;
}

static int bind_pattern(node *p, jsval v, jsenv *env, int decl) {
    switch (p->type) {
    case N_IDENT:
        return assign_ident(p, env, v, decl) < 0 ? ST_THROW : ST_OK;
    case N_MEMBER: case N_INDEX: {
        jsval o, k;
        int st = ev(p->a, env, &o);
        if (st) return st;
        if (p->type == N_MEMBER) return js_set(o, p->str, v) < 0 ? ST_THROW : ST_OK;
        if ((st = ev(p->b, env, &k))) return st;
        return set_index(o, k, v) < 0 ? ST_THROW : ST_OK;
    }
    case N_DEFAULT:
        if (v.tag == JS_UNDEF) {
            int st = ev(p->b, env, &v);
            if (st) return st;
            if (v.tag == JS_OBJ && v.u.o->cls == C_FUNC && p->a->type == N_IDENT && !((node *)v.u.o->ptr)->str)
                ((node *)v.u.o->ptr)->str = p->a->str;
        }
        return bind_pattern(p->a, v, env, decl);
    case N_HOLE: return ST_OK;
    case N_ARRPAT: {
        struct js_iter it;
        if (js_iter_open(v, &it) < 0) return ST_THROW;
        int done = 0;
        for (int i = 0; i < p->n; i++) {
            node *e = p->list[i];
            if (e->type == N_REST) {
                jsobj *rest = js_new_array(0);
                while (!done) {
                    jsval x;
                    if (js_iter_step(&it, &x, &done) < 0) return ST_THROW;
                    if (!done) js_array_push(rest, x);
                }
                int st = bind_pattern(e->a, js_objv(rest), env, decl);
                if (st) return st;
                continue;
            }
            jsval x = js_undef();
            if (!done && js_iter_step(&it, &x, &done) < 0) return ST_THROW;
            if (done) x = js_undef();
            int st = bind_pattern(e, x, env, decl);
            if (st) return st;
        }
        if (!done) js_iter_close(&it);
        return ST_OK;
    }
    case N_OBJPAT: return bind_objpat(p, v, env, decl);
    }
    return js_throw("SyntaxError", "invalid destructuring target") ? ST_THROW : ST_THROW;
}

/* ------------------------------------------------------------------ операции */

static double num_mod(double a, double b) { return js_fmod(a, b); }

int js_binary(int op, jsval a, jsval b, jsval *out) {
    double x, y;
    int i, k;
    switch (op) {
    case O_ADD:
        if (a.tag == JS_NUM && b.tag == JS_NUM) { *out = js_num(a.u.n + b.u.n); return 0; }
        if (a.tag == JS_STR && b.tag == JS_STR) { *out = js_strv(js_concat(a.u.s, b.u.s)); return 0; }
        if (js_to_primitive(a, -1, &a) < 0 || js_to_primitive(b, -1, &b) < 0) return -1;
        if (a.tag == JS_STR || b.tag == JS_STR) {
            jsstr *s1, *s2;
            if (js_to_string(a, &s1) < 0 || js_to_string(b, &s2) < 0) return -1;
            *out = js_strv(js_concat(s1, s2));
            return 0;
        }
        if (js_to_number(a, &x) < 0 || js_to_number(b, &y) < 0) return -1;
        *out = js_num(x + y);
        return 0;
    case O_SUB: case O_MUL: case O_DIV: case O_MOD: case O_EXP:
        if (a.tag == JS_NUM) x = a.u.n; else if (js_to_number(a, &x) < 0) return -1;
        if (b.tag == JS_NUM) y = b.u.n; else if (js_to_number(b, &y) < 0) return -1;
        *out = js_num(op == O_SUB ? x - y : op == O_MUL ? x * y : op == O_DIV ? x / y : op == O_MOD ? num_mod(x, y) : js_pow(x, y));
        return 0;
    case O_BAND: case O_BOR: case O_BXOR: case O_SHL: case O_SHR:
        if (js_to_int32(a, &i) < 0 || js_to_int32(b, &k) < 0) return -1;
        switch (op) {
        case O_BAND: i &= k; break;
        case O_BOR: i |= k; break;
        case O_BXOR: i ^= k; break;
        case O_SHL: i = (int)((unsigned)i << (k & 31)); break;
        case O_SHR: i >>= (k & 31); break;
        }
        *out = js_num(i);
        return 0;
    case O_USHR:
        if (js_to_int32(a, &i) < 0 || js_to_int32(b, &k) < 0) return -1;
        *out = js_num((double)((unsigned)i >> (k & 31)));
        return 0;
    case O_EQ: case O_NE: {
        int r;
        if (js_loose_eq(a, b, &r) < 0) return -1;
        *out = js_bool(op == O_EQ ? r : !r);
        return 0;
    }
    case O_SEQ: *out = js_bool(js_strict_eq(a, b)); return 0;
    case O_SNE: *out = js_bool(!js_strict_eq(a, b)); return 0;
    case O_LT: case O_GT: case O_LE: case O_GE: {
        if (!(a.tag == JS_NUM && b.tag == JS_NUM)) {
            if (js_to_primitive(a, 0, &a) < 0 || js_to_primitive(b, 0, &b) < 0) return -1;
        }
        if (a.tag == JS_STR && b.tag == JS_STR) {
            int l = a.u.s->len < b.u.s->len ? a.u.s->len : b.u.s->len;
            int c = memcmp(a.u.s->s, b.u.s->s, l);
            if (!c) c = a.u.s->len - b.u.s->len;
            *out = js_bool(op == O_LT ? c < 0 : op == O_GT ? c > 0 : op == O_LE ? c <= 0 : c >= 0);
            return 0;
        }
        if (js_to_number(a, &x) < 0 || js_to_number(b, &y) < 0) return -1;
        *out = js_bool(op == O_LT ? x < y : op == O_GT ? x > y : op == O_LE ? x <= y : x >= y);
        return 0;
    }
    case O_IN: {
        if (b.tag != JS_OBJ) return js_throw("TypeError", "Cannot use 'in' operator to search for a key in a non-object");
        jsstr *key;
        if (to_key(a, &key) < 0) return -1;
        *out = js_bool(js_has_prop(b.u.o, key));
        return 0;
    }
    case O_INSTANCEOF: {
        if (!js_is_callable(b)) return js_throw("TypeError", "Right-hand side of 'instanceof' is not callable");
        jsobj *f = b.u.o;
        while (f->cls == C_BOUND && IS_OBJ(f->iv[0])) f = f->iv[0].u.o;
        jsval proto;
        if (js_get(js_objv(f), S_prototype, &proto) < 0) return -1;
        int r = 0;
        if (a.tag == JS_OBJ && IS_OBJ(proto))
            for (jsobj *o = a.u.o->proto; o; o = o->proto) if (o == proto.u.o) { r = 1; break; }
        *out = js_bool(r);
        return 0;
    }
    }
    return js_throw("Error", "bad operator");
}

/* ------------------------------------------------------------------ выражения */

#define EV(n, out) do { int st_ = ev((n), env, (out)); if (st_) return st_; } while (0)
#define TRY(x) do { if ((x) < 0) return ST_THROW; } while (0)

/* аргументы вызова (с ...spread) — во временный массив */
static int eval_args(node *call, jsenv *env, jsval **args, int *argc, jsval *small, int nsmall) {
    if (!(call->flags & F_SPREAD_ARGS)) {
        jsval *a = call->n <= nsmall ? small : malloc(sizeof(jsval) * call->n);
        for (int i = 0; i < call->n; i++) {
            int st = ev(call->list[i], env, &a[i]);
            if (st) { if (a != small) free(a); return st; }
        }
        *args = a;
        *argc = call->n;
        return ST_OK;
    }
    jsobj *tmp = js_new_array(0);
    for (int i = 0; i < call->n; i++) {
        node *e = call->list[i];
        jsval v;
        if (e->type == N_SPREAD) {
            int st = ev(e->a, env, &v);
            if (st) return st;
            struct js_iter it;
            TRY(js_iter_open(v, &it));
            for (;;) {
                int done;
                jsval x;
                TRY(js_iter_step(&it, &x, &done));
                if (done) break;
                js_array_push(tmp, x);
            }
        } else {
            int st = ev(e, env, &v);
            if (st) return st;
            js_array_push(tmp, v);
        }
    }
    jsval *a = tmp->alen <= nsmall ? small : malloc(sizeof(jsval) * tmp->alen);
    for (int i = 0; i < tmp->alen; i++) a[i] = tmp->arr[i];
    *args = a;
    *argc = tmp->alen;
    return ST_OK;
}

static jsobj *home_of(node *sup, jsenv *env, jsval *thisv) {
    if (sup->depth < 0) return NULL;
    jsenv *e = env_at(env, sup->depth);
    *thisv = e->v[0];
    jsval callee = e->v[sup->slot];
    if (!IS_OBJ(callee) || !IS_OBJ(callee.u.o->iv[0])) return NULL;
    return callee.u.o->iv[0].u.o;
}

static int eval_class(node *c, jsenv *env, jsval *out);
static int eval_object(node *n, jsenv *env, jsval *out);

/* await: ждём Promise, прокручивая очередь заданий (простое решение без сопрограмм) */
extern int js_promise_state(jsobj *p, jsval *v);   /* jslib.c: 0 ждёт, 1 выполнен, 2 отклонён */
void (*js_await_hook)(void);

static int do_await(jsval v, jsval *out) {
    if (v.tag == JS_OBJ && v.u.o->cls == C_PROMISE) {
        jsval r;
        for (int guard = 0; guard < 100000; guard++) {
            int s = js_promise_state(v.u.o, &r);
            if (s == 1) { *out = r; return ST_OK; }
            if (s == 2) { js_exc = r; return ST_THROW; }
            if (!js_run_jobs()) {
                if (js_await_hook) js_await_hook();
                if (js_promise_state(v.u.o, &r) == 0 && !js_run_jobs()) break;
            }
        }
        *out = js_undef();
        return ST_OK;
    }
    *out = v;
    return ST_OK;
}

static __attribute__((noinline)) int ev_tpl(node *n, jsenv *env, jsval *out) {
        jsbuf b = { 0 };
        for (int i = 0; i < n->n; i++) {
            node *p = n->list[i];
            if (p->type == N_STR && !(i & 1)) { jb_add(&b, p->str->s, p->str->len); continue; }
            jsval v;
            jsstr *s;
            int st = ev(p, env, &v);
            if (st) { jb_free(&b); return st; }
            if (js_to_string(v, &s) < 0) { jb_free(&b); return ST_THROW; }
            jb_add(&b, s->s, s->len);
        }
        *out = js_strv(jb_str(&b));
        return ST_OK;
}

static __attribute__((noinline)) int ev_tagged(node *n, jsenv *env, jsval *out) {
        jsobj *strs = js_new_array(0), *raw = js_new_array(0);
        jsval args[33];
        int argc = 1;
        for (int i = 0; i < n->n; i++) {
            node *p = n->list[i];
            if (!(i & 1)) { js_array_push(strs, js_strv(p->str)); js_array_push(raw, js_strv(p->str2)); }
            else if (argc < 33) EV(p, &args[argc++]);
        }
        js_define(strs, "raw", js_objv(raw), 0);
        args[0] = js_objv(strs);
        jsval f, thisv = js_undef();
        if (n->a->type == N_MEMBER) { EV(n->a->a, &thisv); TRY(js_get(thisv, n->a->str, &f)); }
        else EV(n->a, &f);
        TRY(js_call(f, thisv, argc, args, out));
        return ST_OK;
}

static __attribute__((noinline)) int ev_array(node *n, jsenv *env, jsval *out) {
        jsobj *a = js_new_array(0);
        for (int i = 0; i < n->n; i++) {
            node *e = n->list[i];
            jsval v;
            if (e->type == N_HOLE) { v.tag = JS_EMPTY; js_array_push(a, v); continue; }
            if (e->type == N_SPREAD) {
                EV(e->a, &v);
                struct js_iter it;
                TRY(js_iter_open(v, &it));
                for (;;) {
                    int done;
                    jsval x;
                    TRY(js_iter_step(&it, &x, &done));
                    if (done) break;
                    js_array_push(a, x);
                }
                continue;
            }
            EV(e, &v);
            js_array_push(a, v);
        }
        *out = js_objv(a);
        return ST_OK;
}

static __attribute__((noinline)) int ev_call(node *n, jsenv *env, jsval *out) {
        jsval f, thisv = js_undef();
        node *c = n->a;
        if (c->type == N_SUPER) {                            /* super(...) */
            jsval small[8], *args;
            int argc;
            int st = eval_args(n, env, &args, &argc, small, 8);
            if (st) return st;
            if (c->depth < 0) return js_throw("SyntaxError", "'super' keyword unexpected here") ? ST_THROW : ST_THROW;
            jsenv *e = env_at(env, c->depth);
            jsval callee = e->v[c->slot];
            thisv = e->v[0];
            int r = construct_with_this(js_objv(callee.u.o->proto), thisv, argc, args);
            if (args != small) free(args);
            if (r < 0 || init_fields(callee.u.o, thisv) < 0) return ST_THROW;
            *out = js_undef();
            return ST_OK;
        }
        if (c->type == N_MEMBER) {
            if (c->a->type == N_SUPER) {
                jsobj *home = home_of(c->a, env, &thisv);
                f = js_undef();
                if (home && home->proto) TRY(js_get_obj(home->proto, c->str, thisv, &f));
            } else {
                EV(c->a, &thisv);
                if ((c->flags & F_OPTIONAL) && IS_NULLISH(thisv)) return ST_SHORT;
                TRY(js_get(thisv, c->str, &f));
            }
        } else if (c->type == N_INDEX) {
            jsval k;
            EV(c->a, &thisv);
            if ((c->flags & F_OPTIONAL) && IS_NULLISH(thisv)) return ST_SHORT;
            EV(c->b, &k);
            TRY(get_index(thisv, k, &f));
        } else {
            EV(c, &f);
            if (c->type == N_IDENT && (c->flags & F_WITH)) { jsobj *w = with_obj(c, env); if (w) thisv = js_objv(w); }
        }
        if ((n->flags & F_OPTIONAL) && IS_NULLISH(f)) return ST_SHORT;
        if (!js_is_callable(f)) {
            const char *what = c->type == N_IDENT ? c->str->s : c->type == N_MEMBER ? c->str->s : "expression";
            return js_throw("TypeError", "%.60s is not a function", what) ? ST_THROW : ST_THROW;
        }
        jsval small[8], *args;
        int argc;
        int st = eval_args(n, env, &args, &argc, small, 8);
        if (st) return st;
        int r = js_call(f, thisv, argc, args, out);
        if (args != small) free(args);
        return r < 0 ? ST_THROW : ST_OK;
}

static __attribute__((noinline)) int ev_new(node *n, jsenv *env, jsval *out) {
        jsval f;
        EV(n->a, &f);
        jsval small[8], *args;
        int argc;
        int st = eval_args(n, env, &args, &argc, small, 8);
        if (st) return st;
        if (!IS_OBJ(f) || (f.u.o->cls != C_FUNC && f.u.o->cls != C_NATIVE && f.u.o->cls != C_BOUND)) {
            if (args != small) free(args);
            const char *what = n->a->type == N_IDENT ? n->a->str->s : n->a->type == N_MEMBER ? n->a->str->s : "expression";
            return js_throw("TypeError", "%.60s is not a constructor", what) ? ST_THROW : ST_THROW;
        }
        int r = js_construct(f, argc, args, out);
        if (args != small) free(args);
        return r < 0 ? ST_THROW : ST_OK;
}

static __attribute__((noinline)) int ev_unary(node *n, jsenv *env, jsval *out) {
        jsval v;
        if (n->op == O_TYPEOF && n->a->type == N_IDENT && n->a->depth < 0 && !((n->a->flags & F_WITH) && with_obj(n->a, env))) {
            int r = get_global(n->a->str, &v, 1);
            if (r < 0) return ST_THROW;
            *out = js_strv(js_intern(js_typeof(v), strlen(js_typeof(v))));
            return ST_OK;
        }
        if (n->op == O_DELETE) {
            node *t = n->a;
            if (t->type == N_MEMBER || t->type == N_INDEX) {
                jsval o, k;
                jsstr *key;
                EV(t->a, &o);
                if (t->type == N_MEMBER) key = t->str;
                else { EV(t->b, &k); TRY(to_key(k, &key)); }
                if (o.tag != JS_OBJ) { *out = js_bool(1); return ST_OK; }
                *out = js_bool(js_delete(o.u.o, key));
                return ST_OK;
            }
            if (t->type == N_IDENT && t->depth < 0) { *out = js_bool(js_delete(js_global, t->str)); return ST_OK; }
            *out = js_bool(1);
            return ST_OK;
        }
        EV(n->a, &v);
        double d;
        int i;
        switch (n->op) {
        case O_NONE: return cur_coro && cur_coro->is_async ? coro_await(v, out) : do_await(v, out);
        case O_NOT: *out = js_bool(!js_truthy(v)); return ST_OK;
        case O_NEG: TRY(js_to_number(v, &d)); *out = js_num(-d); return ST_OK;
        case O_PLUS: TRY(js_to_number(v, &d)); *out = js_num(d); return ST_OK;
        case O_BNOT: TRY(js_to_int32(v, &i)); *out = js_num(~i); return ST_OK;
        case O_TYPEOF: { const char *t = js_typeof(v); *out = js_strv(js_intern(t, strlen(t))); return ST_OK; }
        case O_VOID: *out = js_undef(); return ST_OK;
        }
        return ST_OK;
}

static __attribute__((noinline)) int ev_update(node *n, jsenv *env, jsval *out) {
        node *t = n->a;
        jsval old, o, k;
        double d;
        if (t->type == N_IDENT) EV(t, &old);
        else if (t->type == N_MEMBER) { EV(t->a, &o); TRY(js_get(o, t->str, &old)); }
        else if (t->type == N_INDEX) { EV(t->a, &o); EV(t->b, &k); TRY(get_index(o, k, &old)); }
        else return js_throw("SyntaxError", "Invalid left-hand side expression") ? ST_THROW : ST_THROW;
        if (old.tag == JS_NUM) d = old.u.n; else TRY(js_to_number(old, &d));
        jsval nv = js_num(n->op == O_INC ? d + 1 : d - 1);
        if (t->type == N_IDENT) TRY(assign_ident(t, env, nv, 0));
        else if (t->type == N_MEMBER) TRY(js_set(o, t->str, nv));
        else TRY(set_index(o, k, nv));
        *out = (n->flags & F_PREFIX) ? nv : js_num(d);
        return ST_OK;
}

static __attribute__((noinline)) int ev_assign(node *n, jsenv *env, jsval *out) {
        node *t = n->a;
        jsval o, k, v, old;
        if (t->type == N_ARRPAT || t->type == N_OBJPAT) {
            EV(n->b, &v);
            int st = bind_pattern(t, v, env, 0);
            if (st) return st;
            *out = v;
            return ST_OK;
        }
        if (t->type == N_MEMBER) EV(t->a, &o);
        else if (t->type == N_INDEX) { EV(t->a, &o); EV(t->b, &k); }
        if (n->op != O_NONE) {
            if (t->type == N_IDENT) EV(t, &old);
            else if (t->type == N_MEMBER) TRY(js_get(o, t->str, &old));
            else TRY(get_index(o, k, &old));
            if (n->op == O_AND || n->op == O_OR || n->op == O_NULLISH) {
                if (n->op == O_AND ? !js_truthy(old) : n->op == O_OR ? js_truthy(old) : !IS_NULLISH(old)) { *out = old; return ST_OK; }
                EV(n->b, &v);
            } else {
                jsval r;
                EV(n->b, &r);
                TRY(js_binary(n->op, old, r, &v));
            }
        } else {
            EV(n->b, &v);
            if (v.tag == JS_OBJ && v.u.o->cls == C_FUNC && t->type == N_IDENT && (n->b->type == N_FUNC || n->b->type == N_CLASS) && !((node *)v.u.o->ptr)->str)
                ((node *)v.u.o->ptr)->str = t->str;
        }
        if (t->type == N_IDENT) TRY(assign_ident(t, env, v, 0));
        else if (t->type == N_MEMBER) {
            if (o.tag == JS_OBJ) TRY(js_set_obj(o.u.o, t->str, v, o));
            else TRY(js_set(o, t->str, v));
        } else TRY(set_index(o, k, v));
        *out = v;
        return ST_OK;
}

static __attribute__((noinline)) int ev_regex(node *n, jsenv *env, jsval *out) {
        jsobj *re;
        TRY(js_regexp_create(n->str, n->str2, &re));
        *out = js_objv(re);
        return ST_OK;
}

static __attribute__((noinline)) int ev_member(node *n, jsenv *env, jsval *out) {
        jsval o;
        if (n->a->type == N_SUPER) {
            jsval thisv;
            jsobj *home = home_of(n->a, env, &thisv);
            if (!home || !home->proto) { *out = js_undef(); return ST_OK; }
            TRY(js_get_obj(home->proto, n->str, thisv, out));
            return ST_OK;
        }
        EV(n->a, &o);
        if ((n->flags & F_OPTIONAL) && IS_NULLISH(o)) return ST_SHORT;
        TRY(js_get(o, n->str, out));
        return ST_OK;
}

static __attribute__((noinline)) int ev_index(node *n, jsenv *env, jsval *out) {
        jsval o, k;
        EV(n->a, &o);
        if ((n->flags & F_OPTIONAL) && IS_NULLISH(o)) return ST_SHORT;
        EV(n->b, &k);
        TRY(get_index(o, k, out));
        return ST_OK;
}

static int ev(node *n, jsenv *env, jsval *out) {
    switch (n->type) {
    case N_NUM: *out = js_num(n->num); return ST_OK;
    case N_STR: *out = js_strv(n->str); return ST_OK;
    case N_TRUE: *out = js_bool(1); return ST_OK;
    case N_FALSE: *out = js_bool(0); return ST_OK;
    case N_NULL: *out = js_null(); return ST_OK;
    case N_UNDEF: case N_NEWTARGET: *out = js_undef(); return ST_OK;
    case N_IDENT:
        if (n->flags & F_WITH) {
            jsobj *w = with_obj(n, env);
            if (w) return js_get_obj(w, n->str, js_objv(w), out) < 0 ? ST_THROW : ST_OK;
        }
        if (n->depth >= 0) { *out = env_at(env, n->depth)->v[n->slot]; return ST_OK; }
        return get_global(n->str, out, 0) < 0 ? ST_THROW : ST_OK;
    case N_THIS:
        if (n->depth >= 0) { *out = env_at(env, n->depth)->v[n->slot]; return ST_OK; }
        *out = js_objv(js_global);
        return ST_OK;
    case N_TPL: return ev_tpl(n, env, out);
    case N_TAGGED: return ev_tagged(n, env, out);
    case N_REGEX: return ev_regex(n, env, out);
    case N_ARRAY: return ev_array(n, env, out);
    case N_OBJECT: return eval_object(n, env, out);
    case N_FUNC: *out = js_objv(js_make_closure(n, env)); return ST_OK;
    case N_CLASS: return eval_class(n, env, out);
    case N_MEMBER: return ev_member(n, env, out);
    case N_INDEX: return ev_index(n, env, out);
    case N_OPTCHAIN: {
        int st = ev(n->a, env, out);
        if (st == ST_SHORT) { *out = js_undef(); return ST_OK; }
        return st;
    }
    case N_CALL: return ev_call(n, env, out);
    case N_NEW: return ev_new(n, env, out);
    case N_UNARY: return ev_unary(n, env, out);
    case N_UPDATE: return ev_update(n, env, out);
    case N_BINARY: {
        jsval a, b;
        EV(n->a, &a);
        EV(n->b, &b);
        if (a.tag == JS_NUM && b.tag == JS_NUM) {             /* частые случаи — сразу */
            double x = a.u.n, y = b.u.n;
            switch (n->op) {
            case O_ADD: *out = js_num(x + y); return ST_OK;
            case O_SUB: *out = js_num(x - y); return ST_OK;
            case O_MUL: *out = js_num(x * y); return ST_OK;
            case O_LT: *out = js_bool(x < y); return ST_OK;
            case O_GT: *out = js_bool(x > y); return ST_OK;
            case O_LE: *out = js_bool(x <= y); return ST_OK;
            case O_GE: *out = js_bool(x >= y); return ST_OK;
            case O_SEQ: case O_EQ: *out = js_bool(x == y); return ST_OK;
            case O_SNE: case O_NE: *out = js_bool(x != y); return ST_OK;
            }
        }
        TRY(js_binary(n->op, a, b, out));
        return ST_OK;
    }
    case N_LOGICAL: {
        EV(n->a, out);
        if (n->op == O_AND ? !js_truthy(*out) : n->op == O_OR ? js_truthy(*out) : !IS_NULLISH(*out)) return ST_OK;
        return ev(n->b, env, out);
    }
    case N_COND: {
        jsval c;
        EV(n->a, &c);
        return ev(js_truthy(c) ? n->b : n->c, env, out);
    }
    case N_SEQ:
        for (int i = 0; i < n->n; i++) EV(n->list[i], out);
        return ST_OK;
    case N_ASSIGN: return ev_assign(n, env, out);
    case N_YIELD: {
        jsval v = js_undef();
        if (n->a) EV(n->a, &v);
        if (!cur_coro || cur_coro->is_async) return js_throw("SyntaxError", "yield outside of a generator") ? ST_THROW : ST_THROW;
        if (n->op == 1) {                                     /* yield* — отдать всё из другого итератора */
            struct js_iter it;
            TRY(js_iter_open(v, &it));
            for (;;) {
                jsval x, in;
                int done;
                TRY(js_iter_step(&it, &x, &done));
                if (done) { *out = js_undef(); return ST_OK; }
                int st = coro_suspend(CR_YIELD, x, &in);
                if (st) { js_iter_close(&it); return st; }
            }
        }
        return coro_suspend(CR_YIELD, v, out);
    }
    case N_SUPER:
        return js_throw("SyntaxError", "'super' keyword unexpected here") ? ST_THROW : ST_THROW;
    }
    return js_throw("SyntaxError", "unsupported expression") ? ST_THROW : ST_THROW;
}

static int eval_object(node *n, jsenv *env, jsval *out) {
    jsobj *o = js_new_object();
    for (int i = 0; i < n->n; i++) {
        node *p = n->list[i];
        jsval v;
        if (p->type == N_SPREAD) {
            EV(p->a, &v);
            if (v.tag == JS_OBJ) {
                jsobj *s = v.u.o;
                if (s->cls == C_ARRAY) for (int k = 0; k < s->alen; k++) {
                    char b[16];
                    int l = snprintf(b, sizeof(b), "%d", k);
                    if (s->arr[k].tag != JS_EMPTY) js_define_key(o, js_intern(b, l), s->arr[k], P_ENUM | P_WRITE | P_CONF);
                }
                for (int k = 0; k < s->nprops; k++) {
                    if (!(s->props[k].flags & P_ENUM)) continue;
                    jsval x;
                    TRY(js_get(v, s->props[k].key, &x));
                    js_define_key(o, s->props[k].key, x, P_ENUM | P_WRITE | P_CONF);
                }
            } else if (v.tag == JS_STR) {
                for (int k = 0; k < v.u.s->len; k++) {
                    char b[16];
                    int l = snprintf(b, sizeof(b), "%d", k);
                    js_define_key(o, js_intern(b, l), js_strv(js_intern(v.u.s->s + k, 1)), P_ENUM | P_WRITE | P_CONF);
                }
            }
            continue;
        }
        jsstr *key = p->str;
        if (p->flags & F_COMPUTED) {
            jsval kv;
            EV(p->a, &kv);
            TRY(to_key(kv, &key));
        }
        if (p->flags & (F_GETTER | F_SETTER)) {
            jsobj *f = js_make_closure(p->b, env);
            f->iv[0] = js_objv(o);
            struct prop *pr = js_own_prop(o, key);
            if (!pr || !(pr->flags & P_ACCESSOR)) { pr = js_define_key(o, key, js_undef(), P_ACCESSOR | P_ENUM | P_CONF); pr->set = NULL; }
            if (p->flags & F_GETTER) pr->v = js_objv(f);
            else pr->set = f;
            continue;
        }
        EV(p->b, &v);
        if (v.tag == JS_OBJ && v.u.o->cls == C_FUNC && (p->b->type == N_FUNC || p->b->type == N_CLASS)) {
            if (p->b->flags & F_METHOD) v.u.o->iv[0] = js_objv(o);
            if (!((node *)v.u.o->ptr)->str) ((node *)v.u.o->ptr)->str = key;
        }
        if (!(p->flags & (F_COMPUTED | F_SHORTHAND)) && !(p->b->flags & F_METHOD) && !strcmp(key->s, "__proto__")) {
            if (v.tag == JS_OBJ) o->proto = v.u.o;
            else if (v.tag == JS_NULL) o->proto = NULL;
            continue;
        }
        js_define_key(o, key, v, P_ENUM | P_WRITE | P_CONF);
    }
    *out = js_objv(o);
    return ST_OK;
}

static int eval_class(node *c, jsenv *env, jsval *out) {
    jsobj *proto_parent = js_object_proto, *ctor_parent = js_function_proto;
    if (c->a) {
        jsval pv;
        EV(c->a, &pv);
        if (pv.tag == JS_NULL) proto_parent = NULL;
        else {
            if (!js_is_callable(pv)) return js_throw("TypeError", "Class extends value is not a constructor") ? ST_THROW : ST_THROW;
            jsval pp;
            TRY(js_get(pv, S_prototype, &pp));
            proto_parent = IS_OBJ(pp) ? pp.u.o : NULL;
            ctor_parent = pv.u.o;
        }
    }
    jsobj *proto = js_new_object_proto(proto_parent);
    jsobj *ctor = js_make_closure(c->b, env);
    ctor->is_ctor = 1;
    ctor->proto = ctor_parent;
    ctor->iv[0] = js_objv(proto);
    js_define_key(ctor, S_prototype, js_objv(proto), 0);
    js_define_key(proto, S_constructor, js_objv(ctor), P_WRITE | P_CONF);
    for (int i = 0; i < c->n; i++) {
        node *p = c->list[i];
        jsobj *target = (p->flags & F_STATIC) ? ctor : proto;
        jsstr *key = p->str;
        if (p->flags & F_COMPUTED) {
            jsval kv;
            EV(p->a, &kv);
            TRY(to_key(kv, &key));
        }
        jsobj *f = js_make_closure(p->b, env);
        f->iv[0] = js_objv(target);
        if (p->flags & (F_GETTER | F_SETTER)) {
            struct prop *pr = js_own_prop(target, key);
            if (!pr || !(pr->flags & P_ACCESSOR)) { pr = js_define_key(target, key, js_undef(), P_ACCESSOR | P_CONF); pr->set = NULL; }
            if (p->flags & F_GETTER) pr->v = js_objv(f);
            else pr->set = f;
        } else js_define_key(target, key, js_objv(f), P_WRITE | P_CONF);
    }
    if (c->c) {
        jsobj *fi = js_make_closure(c->c, env);
        fi->iv[0] = js_objv(proto);
        ctor->iv[1] = js_objv(fi);
    }
    if (c->d) {
        jsobj *sf = js_make_closure(c->d, env);
        sf->iv[0] = js_objv(ctor);
        jsval r;
        TRY(js_call(js_objv(sf), js_objv(ctor), 0, NULL, &r));
    }
    *out = js_objv(ctor);
    return ST_OK;
}

/* ------------------------------------------------------------------ операторы */

static jsenv *enter_scope(struct scope *sc, jsenv *env) {
    if (!sc || !sc->nslots) return env;
    jsenv *e = js_new_env(env, sc->nslots);
    instantiate_funcs(sc, e);
    return e;
}

static int loop_ctl(int st, jsstr *label, int *stop) {
    /* -> что делать циклу: 0 — продолжать, иначе вернуть st; *stop — выйти (break) */
    if (st == ST_BREAK) {
        if (!jump_label || jump_label == label) { jump_label = NULL; *stop = 1; return 0; }
        return st;
    }
    if (st == ST_CONTINUE) {
        if (!jump_label || jump_label == label) { jump_label = NULL; return 0; }
        return st;
    }
    return st;
}

/* собрать перечисляемые ключи для for-in */
static jsobj *for_in_keys(jsval v) {
    jsobj *keys = js_new_array(0);
    if (v.tag == JS_STR) {
        for (int i = 0; i < v.u.s->len; i++) js_array_push(keys, js_strv(js_num_str(i)));
        return keys;
    }
    if (v.tag != JS_OBJ) return keys;
    if (v.u.o->hc && v.u.o->hc->keys) {
        extern jsobj *js_own_keys(jsobj *o, int only_enum);
        return js_own_keys(v.u.o, 1);
    }
    for (jsobj *o = v.u.o; o; o = o->proto) {
        if (o->cls == C_ARRAY || o->cls == C_ARGS)
            for (int i = 0; i < o->alen; i++) if (o->arr[i].tag != JS_EMPTY) js_array_push(keys, js_strv(js_num_str(i)));
        if (o->cls == C_STRING) for (int i = 0; i < o->iv[0].u.s->len; i++) js_array_push(keys, js_strv(js_num_str(i)));
        for (int i = 0; i < o->nprops; i++) {
            struct prop *p = &o->props[i];
            if (!(p->flags & P_ENUM) || p->key->symbol) continue;
            int dup = 0;
            if (o != v.u.o) for (int k = 0; k < keys->alen; k++) if (keys->arr[k].u.s == p->key) { dup = 1; break; }
            if (!dup) js_array_push(keys, js_strv(p->key));
        }
    }
    return keys;
}

static int exec_loop(node *s, jsenv *env, jsval *ret, jsstr *label) {
    int st, stop = 0;
    switch (s->type) {
    case N_WHILE:
        for (;;) {
            jsval c;
            if (check_limits() < 0) return ST_THROW;
            if ((st = ev(s->a, env, &c))) return st;
            if (!js_truthy(c)) return ST_OK;
            st = exec_stmt(s->b, env, ret);
            if (st && (st = loop_ctl(st, label, &stop))) return st;
            if (stop) return ST_OK;
        }
    case N_DOWHILE:
        for (;;) {
            jsval c;
            if (check_limits() < 0) return ST_THROW;
            st = exec_stmt(s->b, env, ret);
            if (st && (st = loop_ctl(st, label, &stop))) return st;
            if (stop) return ST_OK;
            if ((st = ev(s->a, env, &c))) return st;
            if (!js_truthy(c)) return ST_OK;
        }
    case N_FOR: {
        jsenv *e = enter_scope(s->scope, env);
        if (s->a) {
            jsval tmp;
            if (s->a->type == N_VAR) st = exec_stmt(s->a, e, ret);
            else st = ev(s->a, e, &tmp);
            if (st) return st;
        }
        for (;;) {
            if (check_limits() < 0) return ST_THROW;
            if (s->b) {
                jsval c;
                if ((st = ev(s->b, e, &c))) return st;
                if (!js_truthy(c)) return ST_OK;
            }
            st = exec_stmt(s->d, e, ret);
            if (st && (st = loop_ctl(st, label, &stop))) return st;
            if (stop) return ST_OK;
            if (s->scope && s->scope->nslots) {                /* у каждой итерации свои let */
                jsenv *ne = js_new_env(env, e->n);
                for (int i = 0; i < e->n; i++) ne->v[i] = e->v[i];
                e = ne;
            }
            if (s->c) {
                jsval tmp;
                if ((st = ev(s->c, e, &tmp))) return st;
            }
        }
    }
    case N_FORIN: case N_FOROF: {
        jsval obj;
        if ((st = ev(s->b, env, &obj))) return st;
        int has_scope = s->scope && s->scope->nslots;
        if (s->type == N_FORIN) {
            if (IS_NULLISH(obj)) return ST_OK;
            jsobj *keys = for_in_keys(obj);
            for (int i = 0; i < keys->alen; i++) {
                if (check_limits() < 0) return ST_THROW;
                jsval k = keys->arr[i];
                if (obj.tag == JS_OBJ && !js_has_prop(obj.u.o, k.u.s)) continue;   /* удалили по ходу */
                jsenv *e = has_scope ? js_new_env(env, s->scope->nslots) : env;
                if ((st = bind_pattern(s->a, k, e, 1))) return st;
                st = exec_stmt(s->c, e, ret);
                if (st && (st = loop_ctl(st, label, &stop))) return st;
                if (stop) return ST_OK;
            }
            return ST_OK;
        }
        struct js_iter it;
        if (js_iter_open(obj, &it) < 0) return ST_THROW;
        for (;;) {
            if (check_limits() < 0) return ST_THROW;
            jsval v;
            int done;
            if (js_iter_step(&it, &v, &done) < 0) return ST_THROW;
            if (done) return ST_OK;
            jsenv *e = has_scope ? js_new_env(env, s->scope->nslots) : env;
            if ((st = bind_pattern(s->a, v, e, 1))) { js_iter_close(&it); return st; }
            st = exec_stmt(s->c, e, ret);
            if (st && (st = loop_ctl(st, label, &stop))) { if (st != ST_THROW) js_iter_close(&it); return st; }
            if (stop) { js_iter_close(&it); return ST_OK; }
        }
    }
    }
    return ST_OK;
}

static int exec_list(node **l, int n, jsenv *env, jsval *ret) {
    for (int i = 0; i < n; i++) {
        int st = exec_stmt(l[i], env, ret);
        if (st) return st;
    }
    return ST_OK;
}

static int exec_stmt(node *s, jsenv *env, jsval *ret) {
    int st;
    js_cur_line = s->line;
    js_cur_file = s->file;
    switch (s->type) {
    case N_EXPR: {
        jsval v;
        st = ev(s->a, env, &v);
        if (st == ST_SHORT) st = ST_OK;
        if (!st) completion = v;
        return st;
    }
    case N_VAR:
        for (int i = 0; i < s->n; i++) {
            node *d = s->list[i];
            jsval v = js_undef();
            if (d->b) {
                if ((st = ev(d->b, env, &v))) return st;
                if (v.tag == JS_OBJ && v.u.o->cls == C_FUNC && d->a->type == N_IDENT && (d->b->type == N_FUNC || d->b->type == N_CLASS) && !((node *)v.u.o->ptr)->str)
                    ((node *)v.u.o->ptr)->str = d->a->str;
            } else if (s->kind == K_VAR) continue;
            if ((st = bind_pattern(d->a, v, env, 1))) return st;
        }
        return ST_OK;
    case N_FUNCDECL: case N_EMPTY: return ST_OK;
    case N_CLASSDECL: {
        jsval c;
        if ((st = eval_class(s->a, env, &c))) return st;
        if (s->depth >= 0) env_at(env, s->depth)->v[s->slot] = c;
        else if (js_set_obj(js_global, s->str, c, js_objv(js_global)) < 0) return ST_THROW;
        return ST_OK;
    }
    case N_BLOCK:
        return exec_list(s->list, s->n, enter_scope(s->scope, env), ret);
    case N_IF: {
        jsval c;
        if ((st = ev(s->a, env, &c))) return st == ST_SHORT ? ST_OK : st;
        if (js_truthy(c)) return exec_stmt(s->b, env, ret);
        return s->c ? exec_stmt(s->c, env, ret) : ST_OK;
    }
    case N_WHILE: case N_DOWHILE: case N_FOR: case N_FORIN: case N_FOROF:
        return exec_loop(s, env, ret, NULL);
    case N_WITH: {
        jsval o;
        jsobj *ob;
        if ((st = ev(s->a, env, &o))) return st;
        if (js_to_object(o, &ob) < 0) return ST_THROW;
        jsenv *e = js_new_env(env, 1);
        e->v[0] = js_objv(ob);
        return exec_stmt(s->b, e, ret);
    }
    case N_LABEL: {
        node *t = s->a;
        if (t->type == N_WHILE || t->type == N_DOWHILE || t->type == N_FOR || t->type == N_FORIN || t->type == N_FOROF)
            st = exec_loop(t, env, ret, s->str);
        else st = exec_stmt(t, env, ret);
        if (st == ST_BREAK && jump_label == s->str) { jump_label = NULL; return ST_OK; }
        return st;
    }
    case N_RETURN:
        *ret = js_undef();
        if (s->a && (st = ev(s->a, env, ret))) return st;
        return ST_RETURN;
    case N_BREAK: jump_label = s->str; return ST_BREAK;
    case N_CONTINUE: jump_label = s->str; return ST_CONTINUE;
    case N_THROW: {
        jsval v;
        if ((st = ev(s->a, env, &v))) return st;
        js_exc = v;
        return ST_THROW;
    }
    case N_TRY: {
        int depth_save = call_depth;
        st = exec_stmt(s->a, env, ret);
        if (st == ST_THROW && s->c) {
            call_depth = depth_save;
            jsval e = js_exc;
            js_exc = js_undef();
            jsenv *ce = (s->scope && s->scope->nslots) ? js_new_env(env, s->scope->nslots) : env;
            st = ST_OK;
            if (s->b) st = bind_pattern(s->b, e, ce, 1);
            if (!st) st = exec_stmt(s->c, ce, ret);
        }
        if (s->d) {
            jsval saved_ret = *ret, saved_exc = js_exc;
            jsstr *saved_label = jump_label;
            int st2 = exec_stmt(s->d, env, ret);
            if (st2) return st2;
            *ret = saved_ret;
            js_exc = saved_exc;
            jump_label = saved_label;
        }
        return st;
    }
    case N_SWITCH: {
        jsval d;
        if ((st = ev(s->a, env, &d))) return st;
        jsenv *e = enter_scope(s->scope, env);
        int start = -1, def = -1;
        for (int i = 0; i < s->n; i++) {
            node *c = s->list[i];
            if (!c->a) { def = i; continue; }
            jsval v;
            if ((st = ev(c->a, e, &v))) return st;
            if (js_strict_eq(d, v)) { start = i; break; }
        }
        if (start < 0) start = def;
        if (start < 0) return ST_OK;
        for (int i = start; i < s->n; i++) {
            st = exec_list(s->list[i]->list, s->list[i]->n, e, ret);
            if (st == ST_BREAK && !jump_label) return ST_OK;
            if (st) return st;
        }
        return ST_OK;
    }
    }
    return ST_OK;
}

/* ------------------------------------------------------------------ сценарий целиком */

int js_run_script(node *sc, jsval *result) {
    /* var и function верхнего уровня — свойства глобального объекта */
    for (int i = 0; i < sc->a->n; i++) {
        jsstr *name = sc->a->list[i]->str;
        if (!js_has_prop(js_global, name)) js_define_key(js_global, name, js_undef(), P_ENUM | P_WRITE);
    }
    instantiate_funcs(sc->scope, NULL);
    completion = js_undef();
    jsval ret = js_undef();
    int st = exec_list(sc->list, sc->n, NULL, &ret);
    *result = completion;
    if (st == ST_THROW) return -1;
    if (st == ST_BREAK || st == ST_CONTINUE) return js_throw("SyntaxError", "Illegal break statement");
    if (st == ST_RETURN) return js_throw("SyntaxError", "Illegal return statement");
    return 0;
}

int js_eval(const char *src, int len, const char *file, jsval *result) {
    char err[200];
    *result = js_undef();
    node *sc = js_parse(src, len, file, 0, err, sizeof(err));
    if (!sc) {
        const char *m = err;
        if (!strncmp(m, "SyntaxError: ", 13)) m += 13;
        return js_throw("SyntaxError", "%s", m);
    }
    int depth = call_depth;
    steps = 0;
    int r = js_run_script(sc, result);
    call_depth = depth;
    return r;
}

int js_call_depth_check(void) { return call_depth; }

/* стек вызовов текстом: «    at file:line» для текущего места и каждого вызова */
void js_stack_text(jsbuf *b) {
    char t[200];
    int n = call_depth < 64 ? call_depth : 64;
    /* кадр k выполняет функцию frames[k].name; где он сейчас — место вызова кадра k+1 */
    const char *file = js_cur_file;
    int line = js_cur_line;
    int shown = 0;
    for (int k = n - 1; k >= -1 && shown < 16; k--) {
        const char *name = k >= 0 && js_frames[k].name ? js_frames[k].name->s : k >= 0 ? "<native>" : NULL;
        if (shown) jb_addc(b, '\n');
        if (name) snprintf(t, sizeof(t), "    at %.60s (%.100s:%d)", name, file ? file : "?", line);
        else snprintf(t, sizeof(t), "    at %.100s:%d", file ? file : "?", line);
        jb_adds(b, t);
        shown++;
        if (k < 0) break;
        file = js_frames[k].file;
        line = js_frames[k].line;
        if (!file) break;
    }
}
