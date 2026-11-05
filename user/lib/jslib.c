/* jslib.c — встроенные объекты JS (часть 1): Object, Function, Error, Boolean,
 * Number, Symbol, Math, глобальные функции, console. Часть 2 — jslib2.c. */
#include "jsint.h"

#define NATIVE(name) static int name(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self)
#define ARG(i) ((i) < argc ? argv[i] : js_undef())
#define UNUSED (void)thisv; (void)argc; (void)argv; (void)self
#define TRY(x) do { if ((x) < 0) return -1; } while (0)

extern int js_is_construct;
extern int js_binary(int op, jsval a, jsval b, jsval *out);
void js_num_fixed(double x, int f, char *buf, int max);
void js_num_precision(double x, int p, int expo, char *buf);
void js_num_radix(double x, int radix, char *buf);
void js_lib2_init(void);
jsobj *js_own_keys(jsobj *o, int only_enum);

static jsobj *mkproto(jsobj *proto) { return js_new_object_proto(proto); }
static const struct js_class proxy_class;

/* конструктор: функция + её prototype */
jsobj *js_make_ctor(const char *name, jsnative fn, int nargs, jsobj *proto) {
    jsobj *c = js_new_native(fn, name, nargs);
    js_define(c, "prototype", js_objv(proto), 0);
    js_define(proto, "constructor", js_objv(c), P_WRITE | P_CONF);
    js_define(js_global, name, js_objv(c), P_WRITE | P_CONF);
    return c;
}

/* ------------------------------------------------------------------ Object */

static jsstr *idx_str(int i) {
    char b[16];
    int n = snprintf(b, sizeof(b), "%d", i);
    return js_intern(b, n);
}

jsobj *js_own_keys(jsobj *o, int only_enum) {
    if (o->hc && o->hc->keys) { jsobj *k = o->hc->keys(o); if (k) return k; js_exc = js_undef(); return js_new_array(0); }
    jsobj *keys = js_new_array(0);
    if (o->cls == C_ARRAY || o->cls == C_ARGS)
        for (int i = 0; i < o->alen; i++) if (o->arr[i].tag != JS_EMPTY) js_array_push(keys, js_strv(idx_str(i)));
    if (o->cls == C_STRING) for (int i = 0; i < o->iv[0].u.s->len; i++) js_array_push(keys, js_strv(idx_str(i)));
    for (int i = 0; i < o->nprops; i++) {
        struct prop *p = &o->props[i];
        if (p->key->symbol) continue;
        if (only_enum && !(p->flags & P_ENUM)) continue;
        js_array_push(keys, js_strv(p->key));
    }
    if (!only_enum && o->cls == C_ARRAY) js_array_push(keys, js_strv(S_length));
    return keys;
}

NATIVE(Object_ctor) {
    UNUSED;
    jsval v = ARG(0);
    if (IS_NULLISH(v)) {
        if (js_is_construct && IS_OBJ(thisv)) { *ret = thisv; return 0; }
        *ret = js_objv(js_new_object());
        return 0;
    }
    jsobj *o;
    TRY(js_to_object(v, &o));
    *ret = js_objv(o);
    return 0;
}

static int obj_arg(int argc, jsval *argv, jsobj **o) { return js_to_object(ARG(0), o); }

NATIVE(Object_keys) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    *ret = js_objv(js_own_keys(o, 1));
    return 0;
}

NATIVE(Object_values) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    jsobj *k = js_own_keys(o, 1), *r = js_new_array(0);
    for (int i = 0; i < k->alen; i++) { jsval v; TRY(js_get(js_objv(o), k->arr[i].u.s, &v)); js_array_push(r, v); }
    *ret = js_objv(r);
    return 0;
}

NATIVE(Object_entries) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    jsobj *k = js_own_keys(o, 1), *r = js_new_array(0);
    for (int i = 0; i < k->alen; i++) {
        jsval v;
        TRY(js_get(js_objv(o), k->arr[i].u.s, &v));
        jsobj *pair = js_new_array(0);
        js_array_push(pair, k->arr[i]);
        js_array_push(pair, v);
        js_array_push(r, js_objv(pair));
    }
    *ret = js_objv(r);
    return 0;
}

NATIVE(Object_assign) {
    UNUSED;
    jsobj *t;
    TRY(obj_arg(argc, argv, &t));
    for (int a = 1; a < argc; a++) {
        if (!IS_OBJ(argv[a])) continue;
        jsobj *s = argv[a].u.o;
        jsobj *k = js_own_keys(s, 1);
        for (int i = 0; i < s->nprops; i++)                    /* и ключи-символы */
            if (s->props[i].key->symbol && (s->props[i].flags & P_ENUM)) js_array_push(k, js_strv(s->props[i].key));
        for (int i = 0; i < k->alen; i++) {
            jsval v;
            TRY(js_get(argv[a], k->arr[i].u.s, &v));
            TRY(js_set(js_objv(t), k->arr[i].u.s, v));
        }
    }
    *ret = js_objv(t);
    return 0;
}

static int define_from_desc(jsobj *o, jsstr *key, jsval desc) {
    if (!IS_OBJ(desc)) return js_throw("TypeError", "Property description must be an object");
    jsval g, s, v, e, w, c;
    TRY(js_get(desc, S_get, &g));
    TRY(js_get(desc, S_set, &s));
    TRY(js_get(desc, S_value, &v));
    TRY(js_get(desc, S_enumerable, &e));
    TRY(js_get(desc, S_writable, &w));
    TRY(js_get(desc, S_configurable, &c));
    key = js_intern_str(key);
    struct prop *old = js_own_prop(o, key);
    int flags = 0;
    if (old) flags = old->flags & (P_ENUM | P_CONF | P_WRITE);
    if (e.tag != JS_UNDEF) flags = js_truthy(e) ? flags | P_ENUM : flags & ~P_ENUM;
    if (c.tag != JS_UNDEF) flags = js_truthy(c) ? flags | P_CONF : flags & ~P_CONF;
    if (g.tag != JS_UNDEF || s.tag != JS_UNDEF) {
        struct prop *p = js_define_key(o, key, g, (flags & ~P_WRITE) | P_ACCESSOR);
        if (!p) return 0;
        if (g.tag == JS_UNDEF && old && (old->flags & P_ACCESSOR)) p->v = old->v;
        p->set = IS_OBJ(s) ? s.u.o : (old && (old->flags & P_ACCESSOR) && s.tag == JS_UNDEF ? p->set : NULL);
        return 0;
    }
    if (w.tag != JS_UNDEF) flags = js_truthy(w) ? flags | P_WRITE : flags & ~P_WRITE;
    if (v.tag == JS_UNDEF && old && !(old->flags & P_ACCESSOR)) v = old->v;
    if (o->cls == C_ARRAY && key == S_length) return js_set(js_objv(o), key, v);
    js_define_key(o, key, v, flags);
    return 0;
}

static int prop_key(jsval v, jsstr **k) {
    if (v.tag == JS_STR) { *k = js_intern_str(v.u.s); return 0; }
    jsstr *s;
    TRY(js_to_string(v, &s));
    *k = js_intern_str(s);
    return 0;
}

NATIVE(Object_defineProperty) {
    UNUSED;
    if (!IS_OBJ(ARG(0))) return js_throw("TypeError", "Object.defineProperty called on non-object");
    jsstr *k;
    TRY(prop_key(ARG(1), &k));
    TRY(define_from_desc(argv[0].u.o, k, ARG(2)));
    *ret = argv[0];
    return 0;
}

NATIVE(Object_defineProperties) {
    UNUSED;
    if (!IS_OBJ(ARG(0)) || !IS_OBJ(ARG(1))) return js_throw("TypeError", "Object.defineProperties called on non-object");
    jsobj *k = js_own_keys(argv[1].u.o, 1);
    for (int i = 0; i < k->alen; i++) {
        jsval d;
        TRY(js_get(argv[1], k->arr[i].u.s, &d));
        TRY(define_from_desc(argv[0].u.o, k->arr[i].u.s, d));
    }
    *ret = argv[0];
    return 0;
}

NATIVE(Object_create) {
    UNUSED;
    jsval p = ARG(0);
    if (!IS_OBJ(p) && p.tag != JS_NULL) return js_throw("TypeError", "Object prototype may only be an Object or null");
    jsobj *o = js_new_object_proto(IS_OBJ(p) ? p.u.o : NULL);
    if (IS_OBJ(ARG(1))) {
        jsval a[2] = { js_objv(o), argv[1] }, r;
        TRY(Object_defineProperties(js_undef(), 2, a, &r, self));
    }
    *ret = js_objv(o);
    return 0;
}

NATIVE(Object_getPrototypeOf) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    if (ARG(0).tag == JS_STR) { *ret = js_objv(js_string_proto); return 0; }
    *ret = o->proto ? js_objv(o->proto) : js_null();
    return 0;
}

NATIVE(Object_setPrototypeOf) {
    UNUSED;
    if (IS_OBJ(ARG(0))) {
        if (IS_OBJ(ARG(1))) argv[0].u.o->proto = argv[1].u.o;
        else if (ARG(1).tag == JS_NULL) argv[0].u.o->proto = NULL;
    }
    *ret = ARG(0);
    return 0;
}

NATIVE(Object_getOwnPropertyNames) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    *ret = js_objv(js_own_keys(o, 0));
    return 0;
}

NATIVE(Object_getOwnPropertySymbols) {
    UNUSED;
    jsobj *a = js_new_array(0);
    if (IS_OBJ(ARG(0))) {
        jsobj *o = argv[0].u.o;
        for (int i = 0; i < o->nprops; i++) if (o->props[i].key->symbol) js_array_push(a, js_strv(o->props[i].key));
    }
    *ret = js_objv(a);
    return 0;
}

static jsval make_desc(jsobj *o, jsstr *key) {
    while (o->hc == &proxy_class && IS_OBJ(o->iv[0])) o = o->iv[0].u.o;   /* у Proxy — дескриптор цели */
    jsobj *d = js_new_object();
    if ((o->cls == C_ARRAY || o->cls == C_ARGS) && key->idx >= 0) {
        if (key->idx >= o->alen || o->arr[key->idx].tag == JS_EMPTY) return js_undef();
        js_define_key(d, S_value, o->arr[key->idx], P_ENUM | P_WRITE | P_CONF);
        js_define_key(d, S_writable, js_bool(1), P_ENUM | P_WRITE | P_CONF);
        js_define_key(d, S_enumerable, js_bool(1), P_ENUM | P_WRITE | P_CONF);
        js_define_key(d, S_configurable, js_bool(1), P_ENUM | P_WRITE | P_CONF);
        return js_objv(d);
    }
    struct prop *p = js_own_prop(o, key);
    if (!p) {
        if (o->cls == C_ARRAY && key == S_length) {
            js_define_key(d, S_value, js_num(o->alen), P_ENUM | P_WRITE | P_CONF);
            js_define_key(d, S_writable, js_bool(1), P_ENUM | P_WRITE | P_CONF);
            return js_objv(d);
        }
        return js_undef();
    }
    if (p->flags & P_ACCESSOR) {
        js_define_key(d, S_get, p->v, P_ENUM | P_WRITE | P_CONF);
        js_define_key(d, S_set, p->set ? js_objv(p->set) : js_undef(), P_ENUM | P_WRITE | P_CONF);
    } else {
        js_define_key(d, S_value, p->v, P_ENUM | P_WRITE | P_CONF);
        js_define_key(d, S_writable, js_bool(p->flags & P_WRITE), P_ENUM | P_WRITE | P_CONF);
    }
    js_define_key(d, S_enumerable, js_bool(p->flags & P_ENUM), P_ENUM | P_WRITE | P_CONF);
    js_define_key(d, S_configurable, js_bool(p->flags & P_CONF), P_ENUM | P_WRITE | P_CONF);
    return js_objv(d);
}

NATIVE(Object_getOwnPropertyDescriptor) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    jsstr *k;
    TRY(prop_key(ARG(1), &k));
    *ret = make_desc(o, k);
    return 0;
}

NATIVE(Object_getOwnPropertyDescriptors) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    jsobj *k = js_own_keys(o, 0), *r = js_new_object();
    for (int i = 0; i < k->alen; i++) js_define_key(r, k->arr[i].u.s, make_desc(o, k->arr[i].u.s), P_ENUM | P_WRITE | P_CONF);
    *ret = js_objv(r);
    return 0;
}

NATIVE(Object_freeze) {
    UNUSED;
    if (IS_OBJ(ARG(0))) {
        jsobj *o = argv[0].u.o;
        for (int i = 0; i < o->nprops; i++) o->props[i].flags &= ~(P_CONF | ((o->props[i].flags & P_ACCESSOR) ? 0 : P_WRITE));
        o->extensible = 0;
    }
    *ret = ARG(0);
    return 0;
}

NATIVE(Object_seal) {
    UNUSED;
    if (IS_OBJ(ARG(0))) {
        jsobj *o = argv[0].u.o;
        for (int i = 0; i < o->nprops; i++) o->props[i].flags &= ~P_CONF;
        o->extensible = 0;
    }
    *ret = ARG(0);
    return 0;
}

NATIVE(Object_preventExtensions) { UNUSED; if (IS_OBJ(ARG(0))) argv[0].u.o->extensible = 0; *ret = ARG(0); return 0; }

NATIVE(Object_isFrozen) {
    UNUSED;
    if (!IS_OBJ(ARG(0))) { *ret = js_bool(1); return 0; }
    jsobj *o = argv[0].u.o;
    int r = !o->extensible;
    for (int i = 0; i < o->nprops && r; i++) if (o->props[i].flags & (P_CONF | P_WRITE)) r = 0;
    *ret = js_bool(r);
    return 0;
}

NATIVE(Object_isSealed) { UNUSED; *ret = js_bool(!IS_OBJ(ARG(0)) || !argv[0].u.o->extensible); return 0; }
NATIVE(Object_isExtensible) { UNUSED; *ret = js_bool(IS_OBJ(ARG(0)) && argv[0].u.o->extensible); return 0; }

NATIVE(Object_is) {
    UNUSED;
    jsval a = ARG(0), b = ARG(1);
    if (a.tag == JS_NUM && b.tag == JS_NUM) {
        double x = a.u.n, y = b.u.n;
        if (js_isnan(x) && js_isnan(y)) { *ret = js_bool(1); return 0; }
        if (x == 0 && y == 0) { *ret = js_bool((1 / x > 0) == (1 / y > 0)); return 0; }
    }
    *ret = js_bool(js_strict_eq(a, b));
    return 0;
}

NATIVE(Object_fromEntries) {
    UNUSED;
    jsobj *o = js_new_object();
    struct js_iter it;
    TRY(js_iter_open(ARG(0), &it));
    for (;;) {
        jsval e, k, v;
        int done;
        TRY(js_iter_step(&it, &e, &done));
        if (done) break;
        TRY(js_get(e, idx_str(0), &k));
        TRY(js_get(e, idx_str(1), &v));
        jsstr *ks;
        TRY(prop_key(k, &ks));
        js_define_key(o, ks, v, P_ENUM | P_WRITE | P_CONF);
    }
    *ret = js_objv(o);
    return 0;
}

NATIVE(Object_hasOwn) {
    UNUSED;
    jsobj *o;
    TRY(obj_arg(argc, argv, &o));
    jsstr *k;
    TRY(prop_key(ARG(1), &k));
    *ret = js_bool(make_desc(o, k).tag != JS_UNDEF);
    return 0;
}

NATIVE(ObjectProto_hasOwnProperty) {
    UNUSED;
    jsobj *o;
    TRY(js_to_object(thisv, &o));
    jsstr *k;
    TRY(prop_key(ARG(0), &k));
    if (o->hc && o->hc->get && o->hc != &proxy_class) { jsval tmp; if (o->hc->get(o, k, &tmp) == 1) { *ret = js_bool(1); return 0; } }
    if (o->cls == C_STRING && (k == S_length || (k->idx >= 0 && k->idx < o->iv[0].u.s->len))) { *ret = js_bool(1); return 0; }
    if (o->cls == C_FUNC && k == S_prototype) { jsval tmp; TRY(js_get(thisv, k, &tmp)); }
    *ret = js_bool(make_desc(o, k).tag != JS_UNDEF);
    return 0;
}

NATIVE(ObjectProto_isPrototypeOf) {
    UNUSED;
    int r = 0;
    if (IS_OBJ(ARG(0)) && IS_OBJ(thisv))
        for (jsobj *o = argv[0].u.o->proto; o; o = o->proto) if (o == thisv.u.o) { r = 1; break; }
    *ret = js_bool(r);
    return 0;
}

NATIVE(ObjectProto_propertyIsEnumerable) {
    UNUSED;
    jsobj *o;
    TRY(js_to_object(thisv, &o));
    jsstr *k;
    TRY(prop_key(ARG(0), &k));
    struct prop *p = js_own_prop(o, k);
    *ret = js_bool(p ? (p->flags & P_ENUM) : ((o->cls == C_ARRAY && k->idx >= 0 && k->idx < o->alen)));
    return 0;
}

const char *js_class_name(jsval v) {
    if (v.tag == JS_UNDEF) return "Undefined";
    if (v.tag == JS_NULL) return "Null";
    if (v.tag == JS_NUM) return "Number";
    if (v.tag == JS_STR) return "String";
    if (v.tag == JS_BOOL) return "Boolean";
    switch (v.u.o->cls) {
    case C_ARRAY: return "Array";
    case C_FUNC: case C_NATIVE: case C_BOUND: return "Function";
    case C_ERROR: return "Error";
    case C_BOOLEAN: return "Boolean";
    case C_NUMBER: return "Number";
    case C_STRING: return "String";
    case C_DATE: return "Date";
    case C_REGEXP: return "RegExp";
    case C_ARGS: return "Arguments";
    case C_MAP: return "Map";
    case C_SET: return "Set";
    case C_PROMISE: return "Promise";
    case C_HOST: return v.u.o->hc->name;
    }
    return "Object";
}

NATIVE(ObjectProto_toString) {
    UNUSED;
    char b[64];
    snprintf(b, sizeof(b), "[object %s]", js_class_name(thisv));
    *ret = js_cstr(b);
    return 0;
}

NATIVE(ObjectProto_toLocaleString) {
    UNUSED;
    jsval f;
    TRY(js_get(thisv, S_toString, &f));
    return js_call(f, thisv, 0, NULL, ret);
}

NATIVE(ObjectProto_valueOf) { UNUSED; jsobj *o; TRY(js_to_object(thisv, &o)); *ret = js_objv(o); return 0; }

NATIVE(ObjectProto_proto_get) {
    UNUSED;
    jsobj *o;
    TRY(js_to_object(thisv, &o));
    *ret = o->proto ? js_objv(o->proto) : js_null();
    return 0;
}
NATIVE(ObjectProto_proto_set) {
    UNUSED;
    if (IS_OBJ(thisv)) {
        if (IS_OBJ(ARG(0))) thisv.u.o->proto = argv[0].u.o;
        else if (ARG(0).tag == JS_NULL) thisv.u.o->proto = NULL;
    }
    return 0;
}

/* ------------------------------------------------------------------ Function */

NATIVE(Function_noop) { UNUSED; *ret = js_undef(); return 0; }

jsobj *js_async_function_proto;

NATIVE(Function_ctor) {
    UNUSED;
    jsbuf b = { 0 };
    jb_adds(&b, self->is_arrow ? "(async function anonymous(" : "(function anonymous(");
    for (int i = 0; i + 1 < argc; i++) {
        jsstr *s;
        if (js_to_string(argv[i], &s) < 0) { jb_free(&b); return -1; }
        if (i) jb_addc(&b, ',');
        jb_add(&b, s->s, s->len);
    }
    jb_adds(&b, "\n) {\n");
    if (argc > 0) {
        jsstr *s;
        if (js_to_string(argv[argc - 1], &s) < 0) { jb_free(&b); return -1; }
        jb_add(&b, s->s, s->len);
    }
    jb_adds(&b, "\n})");
    int r = js_eval(b.p, b.n, "Function", ret);
    jb_free(&b);
    return r;
}

NATIVE(FunctionProto_call) {
    UNUSED;
    return js_call(thisv, ARG(0), argc > 0 ? argc - 1 : 0, argv + 1, ret);
}

NATIVE(FunctionProto_apply) {
    UNUSED;
    jsval a = ARG(1);
    if (IS_NULLISH(a)) return js_call(thisv, ARG(0), 0, NULL, ret);
    if (!IS_OBJ(a)) return js_throw("TypeError", "CreateListFromArrayLike called on non-object");
    int n;
    TRY(js_array_len(a, &n));
    jsval tmp[16], *args = n <= 16 ? tmp : malloc(sizeof(jsval) * n);
    for (int i = 0; i < n; i++) {
        jsobj *o = a.u.o;
        if ((o->cls == C_ARRAY || o->cls == C_ARGS) && i < o->alen) { args[i] = o->arr[i].tag == JS_EMPTY ? js_undef() : o->arr[i]; continue; }
        if (js_get(a, idx_str(i), &args[i]) < 0) { if (args != tmp) free(args); return -1; }
    }
    int r = js_call(thisv, ARG(0), n, args, ret);
    if (args != tmp) free(args);
    return r;
}

NATIVE(FunctionProto_bind) {
    UNUSED;
    if (!js_is_callable(thisv)) return js_throw("TypeError", "Bind must be called on a function");
    jsobj *b = js_new_object_proto(js_function_proto);
    b->cls = C_BOUND;
    b->iv[0] = thisv;
    b->iv[1] = ARG(0);
    for (int i = 1; i < argc; i++) js_array_push(b, argv[i]);
    jsval name;
    TRY(js_get(thisv, S_name, &name));
    jsstr *ns = js_to_str_nothrow(name);
    jsbuf nb = { 0 };
    jb_adds(&nb, "bound ");
    jb_add(&nb, ns->s, ns->len);
    js_define_key(b, S_name, js_strv(jb_str(&nb)), P_CONF);
    *ret = js_objv(b);
    return 0;
}

NATIVE(FunctionProto_toString) {
    UNUSED;
    jsval name = js_undef();
    if (IS_OBJ(thisv)) js_get(thisv, S_name, &name);
    jsstr *n = name.tag == JS_STR ? name.u.s : js_intern("", 0);
    jsbuf b = { 0 };
    jb_adds(&b, "function ");
    jb_add(&b, n->s, n->len);
    jb_adds(&b, "() { [native code] }");
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(FunctionProto_hasInstance) {
    UNUSED;
    return js_binary(O_INSTANCEOF, ARG(0), thisv, ret);
}

/* ------------------------------------------------------------------ Error */

static const char *err_names[] = { "Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", "EvalError", "URIError", "AggregateError" };
static jsobj *err_protos[8];

static int make_err(int kind, jsval thisv, int argc, jsval *argv, jsval *ret) {
    jsobj *e;
    if (js_is_construct && IS_OBJ(thisv)) e = thisv.u.o;
    else e = js_new_object_proto(err_protos[kind]);
    js_is_construct = 0;
    e->cls = C_ERROR;
    if (argc > 0 && argv[0].tag != JS_UNDEF) {
        jsstr *m;
        TRY(js_to_string(argv[0], &m));
        js_define_key(e, S_message, js_strv(m), P_WRITE | P_CONF);
    }
    if (argc > 1 && IS_OBJ(argv[1])) {
        jsval c;
        TRY(js_get_c(argv[1], "cause", &c));
        if (c.tag != JS_UNDEF) js_define(e, "cause", c, P_WRITE | P_CONF);
    }
    jsval mv = js_undef();
    js_get(js_objv(e), S_message, &mv);
    jsbuf st = { 0 };
    jb_adds(&st, err_names[kind]);
    if (mv.tag == JS_STR && mv.u.s->len) { jb_adds(&st, ": "); jb_add(&st, mv.u.s->s, mv.u.s->len); }
    jb_addc(&st, '\n');
    js_stack_text(&st);
    js_define_key(e, S_stack, js_strv(jb_str(&st)), P_WRITE | P_CONF);
    *ret = js_objv(e);
    return 0;
}

NATIVE(Error_ctor) { UNUSED; return make_err(0, thisv, argc, argv, ret); }
NATIVE(TypeError_ctor) { UNUSED; return make_err(1, thisv, argc, argv, ret); }
NATIVE(RangeError_ctor) { UNUSED; return make_err(2, thisv, argc, argv, ret); }
NATIVE(SyntaxError_ctor) { UNUSED; return make_err(3, thisv, argc, argv, ret); }
NATIVE(ReferenceError_ctor) { UNUSED; return make_err(4, thisv, argc, argv, ret); }
NATIVE(EvalError_ctor) { UNUSED; return make_err(5, thisv, argc, argv, ret); }
NATIVE(URIError_ctor) { UNUSED; return make_err(6, thisv, argc, argv, ret); }
NATIVE(AggregateError_ctor) {
    UNUSED;
    jsval a[1] = { ARG(1) };
    TRY(make_err(7, thisv, argc > 1 ? 1 : 0, a, ret));
    jsobj *errs = js_new_array(0);
    if (!IS_NULLISH(ARG(0))) {
        struct js_iter it;
        TRY(js_iter_open(ARG(0), &it));
        for (;;) { jsval v; int done; TRY(js_iter_step(&it, &v, &done)); if (done) break; js_array_push(errs, v); }
    }
    js_define(ret->u.o, "errors", js_objv(errs), P_WRITE | P_CONF);
    return 0;
}

NATIVE(ErrorProto_toString) {
    UNUSED;
    jsval n, m;
    TRY(js_get(thisv, S_name, &n));
    TRY(js_get(thisv, S_message, &m));
    jsstr *ns = n.tag == JS_UNDEF ? js_intern("Error", 5) : js_to_str_nothrow(n);
    jsstr *ms = m.tag == JS_UNDEF ? js_intern("", 0) : js_to_str_nothrow(m);
    if (!ms->len) { *ret = js_strv(ns); return 0; }
    if (!ns->len) { *ret = js_strv(ms); return 0; }
    jsbuf b = { 0 };
    jb_add(&b, ns->s, ns->len);
    jb_adds(&b, ": ");
    jb_add(&b, ms->s, ms->len);
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(Error_captureStackTrace) { UNUSED; *ret = js_undef(); return 0; }

/* ------------------------------------------------------------------ Boolean */

NATIVE(Boolean_ctor) {
    UNUSED;
    jsval b = js_bool(js_truthy(ARG(0)));
    if (js_is_construct && IS_OBJ(thisv)) { thisv.u.o->cls = C_BOOLEAN; thisv.u.o->iv[0] = b; *ret = thisv; return 0; }
    *ret = b;
    return 0;
}

static int this_bool(jsval t, int *b) {
    if (t.tag == JS_BOOL) { *b = t.u.b; return 0; }
    if (IS_OBJ(t) && t.u.o->cls == C_BOOLEAN) { *b = t.u.o->iv[0].u.b; return 0; }
    return js_throw("TypeError", "not a boolean");
}
NATIVE(BooleanProto_toString) { UNUSED; int b = 0; TRY(this_bool(thisv, &b)); *ret = js_cstr(b ? "true" : "false"); return 0; }
NATIVE(BooleanProto_valueOf) { UNUSED; int b = 0; TRY(this_bool(thisv, &b)); *ret = js_bool(b); return 0; }

/* ------------------------------------------------------------------ Number */

NATIVE(Number_ctor) {
    UNUSED;
    double d = 0;
    if (argc > 0) TRY(js_to_number(argv[0], &d));
    if (js_is_construct && IS_OBJ(thisv)) { thisv.u.o->cls = C_NUMBER; thisv.u.o->iv[0] = js_num(d); *ret = thisv; return 0; }
    *ret = js_num(d);
    return 0;
}

static int this_num(jsval t, double *d) {
    if (t.tag == JS_NUM) { *d = t.u.n; return 0; }
    if (IS_OBJ(t) && t.u.o->cls == C_NUMBER) { *d = t.u.o->iv[0].u.n; return 0; }
    return js_throw("TypeError", "not a number");
}

NATIVE(NumberProto_toString) {
    UNUSED;
    double d = 0;
    TRY(this_num(thisv, &d));
    int radix = 10;
    if (argc > 0 && argv[0].tag != JS_UNDEF) { double r; TRY(js_to_number(argv[0], &r)); radix = (int)r; }
    if (radix < 2 || radix > 36) return js_throw("RangeError", "toString() radix must be between 2 and 36");
    char b[1200];
    js_num_radix(d, radix, b);
    *ret = js_cstr(b);
    return 0;
}

NATIVE(NumberProto_toLocaleString) {
    UNUSED;
    double d = 0;
    TRY(this_num(thisv, &d));
    char b[64], o[96];
    if (d == js_floor(d) && d < 1e21 && d > -1e21) {
        js_num_to_str(d, b);
        int n = strlen(b), k = 0, st = b[0] == '-';
        if (st) o[k++] = '-';
        for (int i = st; i < n; i++) {
            o[k++] = b[i];
            int left = n - i - 1;
            if (left > 0 && left % 3 == 0) o[k++] = ',';
        }
        o[k] = 0;
        *ret = js_cstr(o);
        return 0;
    }
    js_num_fixed(d, 3, b, sizeof(b));
    int n = strlen(b);
    while (n > 0 && b[n - 1] == '0') b[--n] = 0;
    if (n > 0 && b[n - 1] == '.') b[--n] = 0;
    *ret = js_cstr(b);
    return 0;
}

NATIVE(NumberProto_valueOf) { UNUSED; double d = 0; TRY(this_num(thisv, &d)); *ret = js_num(d); return 0; }

NATIVE(NumberProto_toFixed) {
    UNUSED;
    double d = 0, f = 0;
    TRY(this_num(thisv, &d));
    if (argc > 0) TRY(js_to_number(argv[0], &f));
    if (js_isnan(f)) f = 0;
    if (f < 0 || f > 100) return js_throw("RangeError", "toFixed() digits argument must be between 0 and 100");
    char b[1300];
    js_num_fixed(d, (int)f, b, sizeof(b));
    *ret = js_cstr(b);
    return 0;
}

NATIVE(NumberProto_toPrecision) {
    UNUSED;
    double d = 0, p = 0;
    TRY(this_num(thisv, &d));
    if (argc == 0 || argv[0].tag == JS_UNDEF) { char b[64]; js_num_to_str(d, b); *ret = js_cstr(b); return 0; }
    TRY(js_to_number(argv[0], &p));
    if (p < 1 || p > 100) return js_throw("RangeError", "toPrecision() argument must be between 1 and 100");
    char b[200];
    js_num_precision(d, (int)p, 0, b);
    *ret = js_cstr(b);
    return 0;
}

NATIVE(NumberProto_toExponential) {
    UNUSED;
    double d = 0, f = -1;
    TRY(this_num(thisv, &d));
    if (argc > 0 && argv[0].tag != JS_UNDEF) TRY(js_to_number(argv[0], &f));
    char b[200];
    if (f < 0) {
        extern int js_shortest_digits(double x, char *out, int *dexp);
        char dig[24];
        int e, k = d == 0 ? 1 : js_shortest_digits(d < 0 ? -d : d, dig, &e);
        js_num_precision(d, k, 1, b);
    } else js_num_precision(d, (int)f + 1, 1, b);
    *ret = js_cstr(b);
    return 0;
}

NATIVE(Number_isFinite) { UNUSED; jsval v = ARG(0); *ret = js_bool(v.tag == JS_NUM && !js_isnan(v.u.n) && !js_isinf(v.u.n)); return 0; }
NATIVE(Number_isNaN) { UNUSED; jsval v = ARG(0); *ret = js_bool(v.tag == JS_NUM && js_isnan(v.u.n)); return 0; }
NATIVE(Number_isInteger) { UNUSED; jsval v = ARG(0); *ret = js_bool(v.tag == JS_NUM && !js_isinf(v.u.n) && v.u.n == js_floor(v.u.n)); return 0; }
NATIVE(Number_isSafeInteger) {
    UNUSED;
    jsval v = ARG(0);
    *ret = js_bool(v.tag == JS_NUM && v.u.n == js_floor(v.u.n) && v.u.n <= 9007199254740991.0 && v.u.n >= -9007199254740991.0);
    return 0;
}

/* ------------------------------------------------------------------ глобальные функции */

NATIVE(g_parseInt) {
    UNUSED;
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    double r = 0;
    int radix = 0;
    if (argc > 1 && argv[1].tag != JS_UNDEF) { int x; TRY(js_to_int32(argv[1], &x)); radix = x; }
    const char *p = s->s, *e = s->s + s->len;
    while (p < e && isspace((ju8)*p)) p++;
    int neg = 0;
    if (p < e && (*p == '+' || *p == '-')) { neg = *p == '-'; p++; }
    if (radix == 0) {
        radix = 10;
        if (e - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) { radix = 16; p += 2; }
    } else if (radix == 16 && e - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    if (radix < 2 || radix > 36) { *ret = js_num(JS_NAN); return 0; }
    int any = 0;
    while (p < e) {
        int c = tolower((ju8)*p), d = isdigit(c) ? c - '0' : (c >= 'a' && c <= 'z') ? c - 'a' + 10 : 99;
        if (d >= radix) break;
        r = r * radix + d;
        any = 1;
        p++;
    }
    *ret = js_num(any ? (neg ? -r : r) : JS_NAN);
    return 0;
}

NATIVE(g_parseFloat) {
    UNUSED;
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    int i = 0, used;
    while (i < s->len && isspace((ju8)s->s[i])) i++;
    double d = js_parse_float(s->s + i, s->len - i, &used);
    *ret = js_num(used ? d : JS_NAN);
    return 0;
}

NATIVE(g_isNaN) { UNUSED; double d; TRY(js_to_number(ARG(0), &d)); *ret = js_bool(js_isnan(d)); return 0; }
NATIVE(g_isFinite) { UNUSED; double d; TRY(js_to_number(ARG(0), &d)); *ret = js_bool(!js_isnan(d) && !js_isinf(d)); return 0; }

NATIVE(g_eval) {
    UNUSED;
    if (ARG(0).tag != JS_STR) { *ret = ARG(0); return 0; }
    return js_eval(argv[0].u.s->s, argv[0].u.s->len, "eval", ret);
}

static int uri_encode(jsval v, const char *keep, jsval *ret) {
    jsstr *s;
    TRY(js_to_string(v, &s));
    jsbuf b = { 0 };
    static const char hex[] = "0123456789ABCDEF";
    for (int i = 0; i < s->len; i++) {
        ju8 c = s->s[i];
        if (isalnum(c) || (c < 128 && strchr(keep, c) && c)) jb_addc(&b, c);
        else { char t[3] = { '%', hex[c >> 4], hex[c & 15] }; jb_add(&b, t, 3); }
    }
    *ret = js_strv(jb_str(&b));
    return 0;
}

static int uri_decode(jsval v, const char *reserved, jsval *ret) {
    jsstr *s;
    TRY(js_to_string(v, &s));
    jsbuf b = { 0 };
    for (int i = 0; i < s->len; i++) {
        char c = s->s[i];
        if (c == '%' && i + 2 < s->len && isalnum((ju8)s->s[i + 1]) && isalnum((ju8)s->s[i + 2])) {
            int h = tolower((ju8)s->s[i + 1]), l = tolower((ju8)s->s[i + 2]);
            int hv = isdigit(h) ? h - '0' : h - 'a' + 10, lv = isdigit(l) ? l - '0' : l - 'a' + 10;
            if (hv < 16 && lv < 16) {
                char d = hv * 16 + lv;
                if (d && strchr(reserved, d)) jb_add(&b, s->s + i, 3);
                else jb_addc(&b, d);
                i += 2;
                continue;
            }
        }
        if (c == '%' && i + 5 < s->len && s->s[i + 1] == 'u') {          /* %uXXXX (escape) */
            unsigned cp = 0;
            int ok = 1;
            for (int k = 0; k < 4; k++) {
                int h = tolower((ju8)s->s[i + 2 + k]);
                int hv = isdigit(h) ? h - '0' : (h >= 'a' && h <= 'f') ? h - 'a' + 10 : -1;
                if (hv < 0) ok = 0;
                cp = cp * 16 + (hv < 0 ? 0 : hv);
            }
            if (ok) {
                char t[3];
                if (cp < 0x80) { t[0] = cp; jb_add(&b, t, 1); }
                else if (cp < 0x800) { t[0] = 0xC0 | (cp >> 6); t[1] = 0x80 | (cp & 63); jb_add(&b, t, 2); }
                else { t[0] = 0xE0 | (cp >> 12); t[1] = 0x80 | ((cp >> 6) & 63); t[2] = 0x80 | (cp & 63); jb_add(&b, t, 3); }
                i += 5;
                continue;
            }
        }
        jb_addc(&b, c);
    }
    *ret = js_strv(jb_str(&b));
    return 0;
}

NATIVE(g_encodeURIComponent) { UNUSED; return uri_encode(ARG(0), "-_.!~*'()", ret); }
NATIVE(g_encodeURI) { UNUSED; return uri_encode(ARG(0), "-_.!~*'();/?:@&=+$,#", ret); }
NATIVE(g_decodeURIComponent) { UNUSED; return uri_decode(ARG(0), "", ret); }
NATIVE(g_decodeURI) { UNUSED; return uri_decode(ARG(0), ";/?:@&=+$,#", ret); }
NATIVE(g_escape) { UNUSED; return uri_encode(ARG(0), "@*_+-./", ret); }
NATIVE(g_unescape) { UNUSED; return uri_decode(ARG(0), "", ret); }

/* ------------------------------------------------------------------ Symbol */

static unsigned sym_counter;
static jsobj *sym_registry;

static jsstr *new_symbol(jsstr *desc) {
    jsbuf b = { 0 };
    jb_adds(&b, "@@");
    if (desc) jb_add(&b, desc->s, desc->len);
    jsstr *s = jb_str(&b);
    s->symbol = 1;
    s->hash = 0x9E3779B9u * ++sym_counter;
    return s;
}

NATIVE(Symbol_ctor) {
    UNUSED;
    if (js_is_construct) return js_throw("TypeError", "Symbol is not a constructor");
    jsstr *d = NULL;
    if (argc > 0 && argv[0].tag != JS_UNDEF) TRY(js_to_string(argv[0], &d));
    *ret = js_strv(new_symbol(d));
    return 0;
}

NATIVE(Symbol_for) {
    UNUSED;
    jsstr *k;
    TRY(js_to_string(ARG(0), &k));
    k = js_intern_str(k);
    struct prop *p = js_own_prop(sym_registry, k);
    if (p) { *ret = p->v; return 0; }
    jsval s = js_strv(new_symbol(k));
    js_define_key(sym_registry, k, s, P_WRITE);
    *ret = s;
    return 0;
}

NATIVE(SymbolProto_toString) {
    UNUSED;
    *ret = js_strv(js_to_str_nothrow(thisv));
    return 0;
}

/* ------------------------------------------------------------------ Math */

static int num_arg(int argc, jsval *argv, int i, double *d) {
    if (i >= argc) { *d = JS_NAN; return 0; }
    if (argv[i].tag == JS_NUM) { *d = argv[i].u.n; return 0; }
    return js_to_number(argv[i], d);
}

#define MATH1(name, expr) NATIVE(Math_##name) { UNUSED; double x; TRY(num_arg(argc, argv, 0, &x)); *ret = js_num(expr); return 0; }
MATH1(abs, x < 0 ? -x : x == 0 ? 0 : x)
MATH1(floor, js_floor(x))
MATH1(ceil, js_ceil(x))
MATH1(round, js_isnan(x) || js_isinf(x) ? x : js_floor(x + 0.5))
MATH1(trunc, x < 0 ? js_ceil(x) : js_floor(x))
MATH1(sign, js_isnan(x) ? x : x > 0 ? 1 : x < 0 ? -1 : x)
MATH1(sqrt, x < 0 ? JS_NAN : js_sqrt(x))
MATH1(cbrt, x < 0 ? -js_pow(-x, 1.0 / 3) : js_pow(x, 1.0 / 3))
MATH1(exp, js_exp(x))
MATH1(expm1, js_exp(x) - 1)
MATH1(log, js_log(x))
MATH1(log1p, js_log(1 + x))
MATH1(log2, js_log(x) / 0.6931471805599453)
MATH1(log10, js_log(x) / 2.302585092994046)
MATH1(sin, js_sin(x))
MATH1(cos, js_cos(x))
MATH1(tan, js_tan(x))
MATH1(atan, js_atan2(x, 1))
MATH1(asin, x < -1 || x > 1 ? JS_NAN : js_atan2(x, js_sqrt(1 - x * x)))
MATH1(acos, x < -1 || x > 1 ? JS_NAN : js_atan2(js_sqrt(1 - x * x), x))
MATH1(sinh, (js_exp(x) - js_exp(-x)) / 2)
MATH1(cosh, (js_exp(x) + js_exp(-x)) / 2)
MATH1(tanh, x > 20 ? 1 : x < -20 ? -1 : (js_exp(2 * x) - 1) / (js_exp(2 * x) + 1))
MATH1(fround, x)
MATH1(clz32, (double)({ unsigned u = (unsigned)(ji64)x; int n = 0; if (!u) n = 32; else while (!(u & 0x80000000u)) { u <<= 1; n++; } n; }))

NATIVE(Math_atan2) { UNUSED; double y, x; TRY(num_arg(argc, argv, 0, &y)); TRY(num_arg(argc, argv, 1, &x)); *ret = js_num(js_atan2(y, x)); return 0; }
NATIVE(Math_pow) { UNUSED; double a, b; TRY(num_arg(argc, argv, 0, &a)); TRY(num_arg(argc, argv, 1, &b)); *ret = js_num(js_pow(a, b)); return 0; }
NATIVE(Math_imul) {
    UNUSED;
    int a, b;
    TRY(js_to_int32(ARG(0), &a));
    TRY(js_to_int32(ARG(1), &b));
    *ret = js_num((int)((unsigned)a * (unsigned)b));
    return 0;
}

NATIVE(Math_max) {
    UNUSED;
    double r = -JS_INF;
    for (int i = 0; i < argc; i++) {
        double x;
        TRY(num_arg(argc, argv, i, &x));
        if (js_isnan(x)) r = x;
        else if (!js_isnan(r) && x > r) r = x;
    }
    *ret = js_num(r);
    return 0;
}

NATIVE(Math_min) {
    UNUSED;
    double r = JS_INF;
    for (int i = 0; i < argc; i++) {
        double x;
        TRY(num_arg(argc, argv, i, &x));
        if (js_isnan(x)) r = x;
        else if (!js_isnan(r) && x < r) r = x;
    }
    *ret = js_num(r);
    return 0;
}

NATIVE(Math_hypot) {
    UNUSED;
    double s = 0;
    for (int i = 0; i < argc; i++) { double x; TRY(num_arg(argc, argv, i, &x)); s += x * x; }
    *ret = js_num(js_sqrt(s));
    return 0;
}

static unsigned rnd_s[4];
NATIVE(Math_random) {
    UNUSED;
    if (!rnd_s[0]) {
        double t = js_now_ms();
        rnd_s[0] = (unsigned)js_fmod(t, 4294967296.0) | 1;
        rnd_s[1] = 0x9E3779B9u ^ (unsigned)(ji64)(t / 7);
        rnd_s[2] = 0x7F4A7C15u;
        rnd_s[3] = 0xF39CC060u;
    }
    unsigned t = rnd_s[3];                                 /* xorshift128 */
    unsigned s = rnd_s[0];
    rnd_s[3] = rnd_s[2]; rnd_s[2] = rnd_s[1]; rnd_s[1] = s;
    t ^= t << 11; t ^= t >> 8;
    rnd_s[0] = t ^ s ^ (s >> 19);
    unsigned hi = rnd_s[0] >> 5, lo = rnd_s[1] >> 6;
    *ret = js_num((hi * 67108864.0 + lo) / 9007199254740992.0);
    return 0;
}

/* ------------------------------------------------------------------ console */

static void inspect(jsbuf *b, jsval v, int depth, int top);

static void inspect_key(jsbuf *b, jsstr *k) {
    int plain = k->len > 0 && !isdigit((ju8)k->s[0]);
    for (int i = 0; i < k->len && plain; i++) if (!isalnum((ju8)k->s[i]) && k->s[i] != '_' && k->s[i] != '$') plain = 0;
    if (plain) jb_add(b, k->s, k->len);
    else { jb_addc(b, '\''); jb_add(b, k->s, k->len); jb_addc(b, '\''); }
}

static void inspect(jsbuf *b, jsval v, int depth, int top) {
    char t[64];
    switch (v.tag) {
    case JS_STR:
        if (v.u.s->symbol) { jsstr *s = js_to_str_nothrow(v); jb_add(b, s->s, s->len); return; }
        if (top) { jb_add(b, v.u.s->s, v.u.s->len); return; }
        jb_addc(b, '\''); jb_add(b, v.u.s->s, v.u.s->len); jb_addc(b, '\'');
        return;
    case JS_OBJ: break;
    default: { jsstr *s = js_to_str_nothrow(v); jb_add(b, s->s, s->len); return; }
    }
    jsobj *o = v.u.o;
    if (js_is_callable(v)) {
        jsval n = js_undef();
        js_get(v, S_name, &n);
        jsstr *ns = n.tag == JS_STR ? n.u.s : NULL;
        jb_adds(b, (o->cls == C_FUNC && ((struct node *)o->ptr)->flags & F_CTOR) ? "[class " : "[Function");
        if (ns && ns->len) { if (b->p[b->n - 1] != ' ') jb_addc(b, ':'); jb_addc(b, ' '); jb_add(b, ns->s, ns->len); }
        else if (b->p[b->n - 1] == ' ') b->n--;
        jb_addc(b, ']');
        return;
    }
    if (o->cls == C_ERROR || o->cls == C_DATE || o->cls == C_REGEXP || o->cls == C_HOST) {
        jsstr *s = js_to_str_nothrow(v);
        if (o->cls == C_HOST && !top) { jb_addc(b, '['); jb_add(b, s->s, s->len); jb_addc(b, ']'); }
        else jb_add(b, s->s, s->len);
        return;
    }
    if (depth > 2) { jb_adds(b, o->cls == C_ARRAY ? "[Array]" : "[Object]"); return; }
    if (o->cls == C_ARRAY) {
        jb_addc(b, '[');
        for (int i = 0; i < o->alen && i < 100; i++) {
            if (i) jb_adds(b, ", ");
            if (o->arr[i].tag == JS_EMPTY) jb_adds(b, "<empty>");
            else inspect(b, o->arr[i], depth + 1, 0);
        }
        if (o->alen > 100) jb_adds(b, ", ...");
        jb_addc(b, ']');
        return;
    }
    if (o->cls == C_MAP || o->cls == C_SET) {
        snprintf(t, sizeof(t), "%s(%d) {", o->cls == C_MAP ? "Map" : "Set", (int)o->iv[0].u.n);
        jb_adds(b, t);
        int first = 1, step = o->cls == C_MAP ? 2 : 1;
        for (int i = 0; i + step - 1 < o->alen; i += step) {
            if (o->arr[i].tag == JS_EMPTY) continue;
            jb_adds(b, first ? " " : ", ");
            first = 0;
            inspect(b, o->arr[i], depth + 1, 0);
            if (step == 2) { jb_adds(b, " => "); inspect(b, o->arr[i + 1], depth + 1, 0); }
        }
        jb_adds(b, first ? "}" : " }");
        return;
    }
    if (o->cls == C_PROMISE) { jb_adds(b, "Promise {}"); return; }
    jb_addc(b, '{');
    int first = 1;
    for (int i = 0; i < o->nprops && i < 50; i++) {
        struct prop *p = &o->props[i];
        if (!(p->flags & P_ENUM) || p->key->symbol) continue;
        jb_adds(b, first ? " " : ", ");
        first = 0;
        inspect_key(b, p->key);
        jb_adds(b, ": ");
        if (p->flags & P_ACCESSOR) jb_adds(b, "[Getter]");
        else inspect(b, p->v, depth + 1, 0);
    }
    jb_adds(b, first ? "}" : " }");
}

void js_inspect(jsbuf *b, jsval v, int top) { inspect(b, v, 0, top); }

NATIVE(console_log) {
    UNUSED;
    jsbuf b = { 0 };
    for (int i = 0; i < argc; i++) {
        if (i) jb_addc(&b, ' ');
        inspect(&b, argv[i], 0, 1);
    }
    jb_addc(&b, '\n');
    js_print(b.p, b.n);
    jb_free(&b);
    *ret = js_undef();
    return 0;
}

NATIVE(console_assert) {
    UNUSED;
    if (!js_truthy(ARG(0))) {
        js_print("Assertion failed", 16);
        if (argc > 1) { js_print(": ", 2); }
        return console_log(thisv, argc > 1 ? argc - 1 : 0, argv + 1, ret, self);
    }
    *ret = js_undef();
    return 0;
}

/* ------------------------------------------------------------------ Proxy и Reflect */

/* Proxy: iv[0] — цель, iv[1] — обработчик; ловушки вызываются из «хозяйских» хуков */
static int trap(jsobj *p, const char *name, jsval *fn) {
    if (!IS_OBJ(p->iv[1])) return js_throw("TypeError", "Cannot perform '%s' on a proxy that has been revoked", name);
    return js_get_c(p->iv[1], name, fn);
}

static jsval key_val(jsstr *k) { return js_strv(k); }

static int proxy_get(jsobj *p, jsstr *key, jsval *out) {
    jsval f;
    if (trap(p, "get", &f) < 0) return -1;
    if (!js_is_callable(f)) return js_get_obj(p->iv[0].u.o, key, js_objv(p), out) < 0 ? -1 : 1;
    jsval a[3] = { p->iv[0], key_val(key), js_objv(p) };
    return js_call(f, p->iv[1], 3, a, out) < 0 ? -1 : 1;
}

static int proxy_set(jsobj *p, jsstr *key, jsval v) {
    jsval f, r;
    if (trap(p, "set", &f) < 0) return -1;
    if (!js_is_callable(f)) return js_set(p->iv[0], key, v) < 0 ? -1 : 1;
    jsval a[4] = { p->iv[0], key_val(key), v, js_objv(p) };
    return js_call(f, p->iv[1], 4, a, &r) < 0 ? -1 : 1;
}

static int proxy_has(jsobj *p, jsstr *key) {
    jsval f, r;
    if (trap(p, "has", &f) < 0) return -1;
    if (!js_is_callable(f)) return js_has_prop(p->iv[0].u.o, key);
    jsval a[2] = { p->iv[0], key_val(key) };
    if (js_call(f, p->iv[1], 2, a, &r) < 0) return -1;
    return js_truthy(r);
}

static int proxy_del(jsobj *p, jsstr *key) {
    jsval f, r;
    if (trap(p, "deleteProperty", &f) < 0) return -1;
    if (!js_is_callable(f)) return js_delete(p->iv[0].u.o, key);
    jsval a[2] = { p->iv[0], key_val(key) };
    if (js_call(f, p->iv[1], 2, a, &r) < 0) return -1;
    return js_truthy(r);
}

static jsobj *proxy_keys(jsobj *p) {
    jsval f, r;
    if (trap(p, "ownKeys", &f) < 0) return NULL;
    if (!js_is_callable(f)) return js_own_keys(p->iv[0].u.o, 1);
    jsval a[1] = { p->iv[0] };
    if (js_call(f, p->iv[1], 1, a, &r) < 0) return NULL;
    jsobj *k = js_new_array(0);
    int n;
    if (js_array_len(r, &n) < 0) return NULL;
    for (int i = 0; i < n; i++) {
        jsval v;
        if (js_get(r, idx_str(i), &v) < 0) return NULL;
        if (v.tag == JS_STR && !v.u.s->symbol) js_array_push(k, v);
    }
    return k;
}

static const struct js_class proxy_class = { "Object", proxy_get, proxy_set, NULL, proxy_has, proxy_del, proxy_keys };

int js_is_array(jsval v) {
    while (IS_OBJ(v) && v.u.o->hc == &proxy_class) v = v.u.o->iv[0];
    return IS_OBJ(v) && v.u.o->cls == C_ARRAY;
}

NATIVE(Proxy_call) {                                       /* вызов прокси функции: apply / construct */
    UNUSED;
    jsval f;
    int construct = js_is_construct;
    js_is_construct = 0;
    if (trap(self, construct ? "construct" : "apply", &f) < 0) return -1;
    if (js_is_callable(f)) {
        jsobj *args = js_new_array(0);
        for (int i = 0; i < argc; i++) js_array_push(args, argv[i]);
        jsval a[3] = { self->iv[0], construct ? js_objv(args) : thisv, construct ? js_objv(self) : js_objv(args) };
        if (construct) { a[1] = js_objv(args); a[2] = js_objv(self); }
        return js_call(f, self->iv[1], 3, a, ret);
    }
    if (construct) return js_construct(self->iv[0], argc, argv, ret);
    return js_call(self->iv[0], thisv, argc, argv, ret);
}

static jsobj *make_proxy(jsval target, jsval handler) {
    jsobj *t = target.u.o;
    jsobj *p = js_new_object_proto(t->proto);
    if (js_is_callable(target)) { p->cls = C_NATIVE; p->ptr = (void *)Proxy_call; }
    else if (t->cls == C_ARRAY) p->cls = C_OBJECT;
    p->hc = &proxy_class;
    p->iv[0] = target;
    p->iv[1] = handler;
    return p;
}

NATIVE(Proxy_ctor) {
    UNUSED;
    if (!js_is_construct) return js_throw("TypeError", "Constructor Proxy requires 'new'");
    js_is_construct = 0;
    if (!IS_OBJ(ARG(0)) || !IS_OBJ(ARG(1))) return js_throw("TypeError", "Cannot create proxy with a non-object as target or handler");
    *ret = js_objv(make_proxy(argv[0], argv[1]));
    return 0;
}

NATIVE(Proxy_revoke) { UNUSED; if (IS_OBJ(self->iv[0])) self->iv[0].u.o->iv[1] = js_null(); *ret = js_undef(); return 0; }

NATIVE(Proxy_revocable) {
    UNUSED;
    if (!IS_OBJ(ARG(0)) || !IS_OBJ(ARG(1))) return js_throw("TypeError", "Cannot create proxy with a non-object as target or handler");
    jsobj *p = make_proxy(argv[0], argv[1]), *r = js_new_object(), *rv = js_new_native(Proxy_revoke, "revoke", 0);
    rv->iv[0] = js_objv(p);
    js_define(r, "proxy", js_objv(p), P_ENUM | P_WRITE | P_CONF);
    js_define(r, "revoke", js_objv(rv), P_ENUM | P_WRITE | P_CONF);
    *ret = js_objv(r);
    return 0;
}

static int need_obj(jsval v, const char *fn) { return IS_OBJ(v) ? 0 : js_throw("TypeError", "Reflect.%s called on non-object", fn); }

NATIVE(Reflect_get) {
    UNUSED;
    TRY(need_obj(ARG(0), "get"));
    jsstr *k;
    TRY(prop_key(ARG(1), &k));
    return js_get_obj(argv[0].u.o, k, argc > 2 ? argv[2] : argv[0], ret);
}
NATIVE(Reflect_set) {
    UNUSED;
    TRY(need_obj(ARG(0), "set"));
    jsstr *k;
    TRY(prop_key(ARG(1), &k));
    jsval recv = argc > 3 ? argv[3] : argv[0];
    if (IS_OBJ(recv) && recv.u.o != argv[0].u.o && !argv[0].u.o->hc) {
        /* сеттер цели с другим this (так делает Vue): ищем аксессор, иначе пишем в получателя */
        for (jsobj *o = argv[0].u.o; o; o = o->proto) {
            struct prop *p = js_own_prop(o, k);
            if (p) {
                if (p->flags & P_ACCESSOR) { jsval r; if (p->set) TRY(js_call(js_objv(p->set), recv, 1, &argv[2], &r)); *ret = js_bool(p->set != NULL); return 0; }
                break;
            }
        }
        TRY(js_set_obj(recv.u.o->hc ? argv[0].u.o : recv.u.o, k, ARG(2), recv));
        *ret = js_bool(1);
        return 0;
    }
    TRY(js_set_obj(argv[0].u.o, k, ARG(2), recv));
    *ret = js_bool(1);
    return 0;
}
NATIVE(Reflect_has) { UNUSED; TRY(need_obj(ARG(0), "has")); jsstr *k; TRY(prop_key(ARG(1), &k)); *ret = js_bool(js_has_prop(argv[0].u.o, k)); return 0; }
NATIVE(Reflect_deleteProperty) { UNUSED; TRY(need_obj(ARG(0), "deleteProperty")); jsstr *k; TRY(prop_key(ARG(1), &k)); *ret = js_bool(js_delete(argv[0].u.o, k)); return 0; }
NATIVE(Reflect_ownKeys) {
    UNUSED;
    TRY(need_obj(ARG(0), "ownKeys"));
    jsobj *o = argv[0].u.o, *k = js_own_keys(o, 0);
    for (int i = 0; i < o->nprops; i++) if (o->props[i].key->symbol) js_array_push(k, js_strv(o->props[i].key));
    *ret = js_objv(k);
    return 0;
}
NATIVE(Reflect_apply) {
    UNUSED;
    jsval a[2] = { ARG(1), ARG(2) };
    return FunctionProto_apply(ARG(0), 2, a, ret, self);
}
NATIVE(Reflect_construct) {
    UNUSED;
    int n;
    TRY(js_array_len(ARG(1), &n));
    jsval tmp[16], *args = n <= 16 ? tmp : malloc(sizeof(jsval) * n);
    for (int i = 0; i < n; i++) if (js_get(argv[1], idx_str(i), &args[i]) < 0) { if (args != tmp) free(args); return -1; }
    int r = js_construct(ARG(0), n, args, ret);
    if (r == 0 && argc > 2 && IS_OBJ(argv[2]) && IS_OBJ(*ret)) {   /* new.target: прототип из него */
        jsval pr;
        if (js_get(argv[2], S_prototype, &pr) == 0 && IS_OBJ(pr)) ret->u.o->proto = pr.u.o;
    }
    if (args != tmp) free(args);
    return r;
}
NATIVE(Reflect_defineProperty) {
    UNUSED;
    jsval r;
    if (Object_defineProperty(thisv, argc, argv, &r, self) < 0) { js_exc = js_undef(); *ret = js_bool(0); return 0; }
    *ret = js_bool(1);
    return 0;
}
NATIVE(Reflect_isExtensible) { UNUSED; *ret = js_bool(IS_OBJ(ARG(0)) && argv[0].u.o->extensible); return 0; }
NATIVE(Reflect_preventExtensions) { UNUSED; if (IS_OBJ(ARG(0))) argv[0].u.o->extensible = 0; *ret = js_bool(1); return 0; }
NATIVE(Reflect_setPrototypeOf) { UNUSED; jsval r; TRY(Object_setPrototypeOf(thisv, argc, argv, &r, self)); *ret = js_bool(1); return 0; }

/* WeakRef — просто держит объект */
NATIVE(WeakRef_ctor) {
    UNUSED;
    jsobj *o = IS_OBJ(thisv) ? thisv.u.o : js_new_object();
    o->iv[0] = ARG(0);
    *ret = js_objv(o);
    return 0;
}
NATIVE(WeakRef_deref) { UNUSED; *ret = IS_OBJ(thisv) ? thisv.u.o->iv[0] : js_undef(); return 0; }

/* ------------------------------------------------------------------ сборка */

jsval js_symbol_iterator_v(void) { return js_strv(S_iterator); }

void js_lib_init(void) {
    js_object_proto = js_new_object_proto(NULL);
    js_function_proto = js_new_object_proto(js_object_proto);
    js_function_proto->cls = C_NATIVE;
    js_function_proto->ptr = (void *)Function_noop;
    js_global = js_new_object_proto(js_object_proto);
    js_define(js_global, "globalThis", js_objv(js_global), P_WRITE | P_CONF);

    /* Object */
    jsobj *op = js_object_proto;
    jsobj *O = js_make_ctor("Object", Object_ctor, 1, op);
    js_defn(O, "keys", Object_keys, 1);
    js_defn(O, "values", Object_values, 1);
    js_defn(O, "entries", Object_entries, 1);
    js_defn(O, "assign", Object_assign, 2);
    js_defn(O, "create", Object_create, 2);
    js_defn(O, "defineProperty", Object_defineProperty, 3);
    js_defn(O, "defineProperties", Object_defineProperties, 2);
    js_defn(O, "getPrototypeOf", Object_getPrototypeOf, 1);
    js_defn(O, "setPrototypeOf", Object_setPrototypeOf, 2);
    js_defn(O, "getOwnPropertyNames", Object_getOwnPropertyNames, 1);
    js_defn(O, "getOwnPropertySymbols", Object_getOwnPropertySymbols, 1);
    js_defn(O, "getOwnPropertyDescriptor", Object_getOwnPropertyDescriptor, 2);
    js_defn(O, "getOwnPropertyDescriptors", Object_getOwnPropertyDescriptors, 1);
    js_defn(O, "freeze", Object_freeze, 1);
    js_defn(O, "seal", Object_seal, 1);
    js_defn(O, "preventExtensions", Object_preventExtensions, 1);
    js_defn(O, "isFrozen", Object_isFrozen, 1);
    js_defn(O, "isSealed", Object_isSealed, 1);
    js_defn(O, "isExtensible", Object_isExtensible, 1);
    js_defn(O, "is", Object_is, 2);
    js_defn(O, "fromEntries", Object_fromEntries, 1);
    js_defn(O, "hasOwn", Object_hasOwn, 2);
    js_defn(op, "hasOwnProperty", ObjectProto_hasOwnProperty, 1);
    js_defn(op, "isPrototypeOf", ObjectProto_isPrototypeOf, 1);
    js_defn(op, "propertyIsEnumerable", ObjectProto_propertyIsEnumerable, 1);
    js_defn(op, "toString", ObjectProto_toString, 0);
    js_defn(op, "toLocaleString", ObjectProto_toLocaleString, 0);
    js_defn(op, "valueOf", ObjectProto_valueOf, 0);
    js_def_accessor(op, "__proto__", ObjectProto_proto_get, ObjectProto_proto_set);

    /* Function */
    jsobj *fp = js_function_proto;
    js_make_ctor("Function", Function_ctor, 1, fp);
    js_define(fp, "name", js_cstr(""), P_CONF);
    js_define(fp, "length", js_num(0), P_CONF);
    js_defn(fp, "call", FunctionProto_call, 1);
    js_defn(fp, "apply", FunctionProto_apply, 2);
    js_defn(fp, "bind", FunctionProto_bind, 1);
    js_defn(fp, "toString", FunctionProto_toString, 0);
    js_async_function_proto = mkproto(fp);                   /* AsyncFunction (не глобальный, как в браузерах) */
    jsobj *AF = js_new_native(Function_ctor, "AsyncFunction", 1);
    AF->is_arrow = 1;
    js_define(AF, "prototype", js_objv(js_async_function_proto), 0);
    js_define(js_async_function_proto, "constructor", js_objv(AF), P_WRITE | P_CONF);
    (void)FunctionProto_hasInstance;

    /* Error */
    js_error_proto = mkproto(op);
    jsnative ectors[] = { Error_ctor, TypeError_ctor, RangeError_ctor, SyntaxError_ctor, ReferenceError_ctor, EvalError_ctor, URIError_ctor, AggregateError_ctor };
    for (int i = 0; i < 8; i++) {
        jsobj *p = i ? mkproto(js_error_proto) : js_error_proto;
        err_protos[i] = p;
        jsobj *c = js_make_ctor(err_names[i], ectors[i], 1, p);
        js_define(p, "name", js_cstr(err_names[i]), P_WRITE | P_CONF);
        js_define(p, "message", js_cstr(""), P_WRITE | P_CONF);
        if (!i) {
            js_defn(p, "toString", ErrorProto_toString, 0);
            js_defn(c, "captureStackTrace", Error_captureStackTrace, 1);
            js_define(c, "stackTraceLimit", js_num(10), P_WRITE | P_CONF | P_ENUM);
        }
    }
    for (int i = 1; i < 8; i++) {                           /* TypeError.__proto__ === Error */
        jsval c, e;
        js_get_c(js_objv(js_global), err_names[i], &c);
        js_get_c(js_objv(js_global), "Error", &e);
        c.u.o->proto = e.u.o;
    }

    /* Boolean, Number */
    js_boolean_proto = mkproto(op);
    js_boolean_proto->cls = C_BOOLEAN;
    js_boolean_proto->iv[0] = js_bool(0);
    js_make_ctor("Boolean", Boolean_ctor, 1, js_boolean_proto);
    js_defn(js_boolean_proto, "toString", BooleanProto_toString, 0);
    js_defn(js_boolean_proto, "valueOf", BooleanProto_valueOf, 0);

    js_number_proto = mkproto(op);
    js_number_proto->cls = C_NUMBER;
    js_number_proto->iv[0] = js_num(0);
    jsobj *N = js_make_ctor("Number", Number_ctor, 1, js_number_proto);
    js_defn(js_number_proto, "toString", NumberProto_toString, 1);
    js_defn(js_number_proto, "toLocaleString", NumberProto_toLocaleString, 0);
    js_defn(js_number_proto, "valueOf", NumberProto_valueOf, 0);
    js_defn(js_number_proto, "toFixed", NumberProto_toFixed, 1);
    js_defn(js_number_proto, "toPrecision", NumberProto_toPrecision, 1);
    js_defn(js_number_proto, "toExponential", NumberProto_toExponential, 1);
    js_defn(N, "isFinite", Number_isFinite, 1);
    js_defn(N, "isNaN", Number_isNaN, 1);
    js_defn(N, "isInteger", Number_isInteger, 1);
    js_defn(N, "isSafeInteger", Number_isSafeInteger, 1);
    js_define(N, "MAX_SAFE_INTEGER", js_num(9007199254740991.0), 0);
    js_define(N, "MIN_SAFE_INTEGER", js_num(-9007199254740991.0), 0);
    js_define(N, "EPSILON", js_num(2.220446049250313e-16), 0);
    js_define(N, "MAX_VALUE", js_num(1.7976931348623157e308), 0);
    js_define(N, "MIN_VALUE", js_num(5e-324), 0);
    js_define(N, "POSITIVE_INFINITY", js_num(JS_INF), 0);
    js_define(N, "NEGATIVE_INFINITY", js_num(-JS_INF), 0);
    js_define(N, "NaN", js_num(JS_NAN), 0);

    /* глобальные */
    jsobj *g = js_global;
    js_define(g, "NaN", js_num(JS_NAN), 0);
    js_define(g, "Infinity", js_num(JS_INF), 0);
    js_define(g, "undefined", js_undef(), 0);
    js_defn(g, "parseInt", g_parseInt, 2);
    js_defn(g, "parseFloat", g_parseFloat, 1);
    js_define(N, "parseInt", js_own_prop(g, js_perm("parseInt"))->v, P_WRITE | P_CONF);
    js_define(N, "parseFloat", js_own_prop(g, js_perm("parseFloat"))->v, P_WRITE | P_CONF);
    js_defn(g, "isNaN", g_isNaN, 1);
    js_defn(g, "isFinite", g_isFinite, 1);
    js_defn(g, "eval", g_eval, 1);
    js_defn(g, "encodeURIComponent", g_encodeURIComponent, 1);
    js_defn(g, "encodeURI", g_encodeURI, 1);
    js_defn(g, "decodeURIComponent", g_decodeURIComponent, 1);
    js_defn(g, "decodeURI", g_decodeURI, 1);
    js_defn(g, "escape", g_escape, 1);
    js_defn(g, "unescape", g_unescape, 1);

    /* Symbol */
    jsobj *symp = mkproto(op);
    jsobj *Sy = js_make_ctor("Symbol", Symbol_ctor, 0, symp);
    sym_registry = js_new_object_proto(NULL);
    js_define(Sy, "%registry", js_objv(sym_registry), 0);
    js_defn(Sy, "for", Symbol_for, 1);
    js_define(Sy, "iterator", js_strv(S_iterator), 0);
    const char *wk[] = { "asyncIterator", "hasInstance", "toStringTag", "toPrimitive", "species", "isConcatSpreadable", "match", "replace", "search", "split", "unscopables" };
    for (unsigned i = 0; i < sizeof(wk) / sizeof(wk[0]); i++) js_define(Sy, wk[i], js_strv(new_symbol(js_perm(wk[i]))), 0);
    js_defn(symp, "toString", SymbolProto_toString, 0);

    /* Math */
    jsobj *M = js_new_object();
    js_define(g, "Math", js_objv(M), P_WRITE | P_CONF);
    js_define(M, "PI", js_num(3.141592653589793), 0);
    js_define(M, "E", js_num(2.718281828459045), 0);
    js_define(M, "LN2", js_num(0.6931471805599453), 0);
    js_define(M, "LN10", js_num(2.302585092994046), 0);
    js_define(M, "LOG2E", js_num(1.4426950408889634), 0);
    js_define(M, "LOG10E", js_num(0.4342944819032518), 0);
    js_define(M, "SQRT2", js_num(1.4142135623730951), 0);
    js_define(M, "SQRT1_2", js_num(0.7071067811865476), 0);
    struct { const char *n; jsnative f; int a; } mf[] = {
        { "abs", Math_abs, 1 }, { "floor", Math_floor, 1 }, { "ceil", Math_ceil, 1 }, { "round", Math_round, 1 },
        { "trunc", Math_trunc, 1 }, { "sign", Math_sign, 1 }, { "sqrt", Math_sqrt, 1 }, { "cbrt", Math_cbrt, 1 },
        { "exp", Math_exp, 1 }, { "expm1", Math_expm1, 1 }, { "log", Math_log, 1 }, { "log1p", Math_log1p, 1 },
        { "log2", Math_log2, 1 }, { "log10", Math_log10, 1 }, { "sin", Math_sin, 1 }, { "cos", Math_cos, 1 },
        { "tan", Math_tan, 1 }, { "atan", Math_atan, 1 }, { "asin", Math_asin, 1 }, { "acos", Math_acos, 1 },
        { "sinh", Math_sinh, 1 }, { "cosh", Math_cosh, 1 }, { "tanh", Math_tanh, 1 }, { "fround", Math_fround, 1 },
        { "clz32", Math_clz32, 1 }, { "atan2", Math_atan2, 2 }, { "pow", Math_pow, 2 }, { "imul", Math_imul, 2 },
        { "max", Math_max, 2 }, { "min", Math_min, 2 }, { "hypot", Math_hypot, 2 }, { "random", Math_random, 0 },
    };
    for (unsigned i = 0; i < sizeof(mf) / sizeof(mf[0]); i++) js_defn(M, mf[i].n, mf[i].f, mf[i].a);

    /* console */
    jsobj *con = js_new_object();
    js_define(g, "console", js_objv(con), P_WRITE | P_CONF);
    const char *cn[] = { "log", "info", "warn", "error", "debug", "trace", "dir", "table", "group", "groupCollapsed" };
    for (unsigned i = 0; i < sizeof(cn) / sizeof(cn[0]); i++) js_defn(con, cn[i], console_log, 0);
    const char *nn[] = { "groupEnd", "time", "timeEnd", "count", "clear", "profile", "profileEnd", "timeLog" };
    for (unsigned i = 0; i < sizeof(nn) / sizeof(nn[0]); i++) js_defn(con, nn[i], Function_noop, 0);
    js_defn(con, "assert", console_assert, 1);

    /* Proxy, Reflect */
    jsobj *Px = js_new_native(Proxy_ctor, "Proxy", 2);
    js_define(g, "Proxy", js_objv(Px), P_WRITE | P_CONF);
    js_defn(Px, "revocable", Proxy_revocable, 2);
    jsobj *Rf = js_new_object();
    js_define(g, "Reflect", js_objv(Rf), P_WRITE | P_CONF);
    struct { const char *n; jsnative f; int a; } rf[] = {
        { "get", Reflect_get, 2 }, { "set", Reflect_set, 3 }, { "has", Reflect_has, 2 }, { "deleteProperty", Reflect_deleteProperty, 2 },
        { "ownKeys", Reflect_ownKeys, 1 }, { "apply", Reflect_apply, 3 }, { "construct", Reflect_construct, 2 },
        { "defineProperty", Reflect_defineProperty, 3 }, { "getPrototypeOf", Object_getPrototypeOf, 1 },
        { "setPrototypeOf", Reflect_setPrototypeOf, 2 }, { "getOwnPropertyDescriptor", Object_getOwnPropertyDescriptor, 2 },
        { "isExtensible", Reflect_isExtensible, 1 }, { "preventExtensions", Reflect_preventExtensions, 1 },
    };
    for (unsigned i = 0; i < sizeof(rf) / sizeof(rf[0]); i++) js_defn(Rf, rf[i].n, rf[i].f, rf[i].a);
    jsobj *wrp = mkproto(op);
    js_make_ctor("WeakRef", WeakRef_ctor, 1, wrp);
    js_defn(wrp, "deref", WeakRef_deref, 0);

    js_lib2_init();
}
