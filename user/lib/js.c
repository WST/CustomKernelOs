/* js.c — движок JavaScript: память и сборщик мусора, строки, объекты и их
 * свойства, преобразования типов, исключения. Разбор — jsparse.c, выполнение —
 * jsrun.c, стандартная библиотека — jslib.c, регулярные выражения — jsre.c. */
#include "jsint.h"

jsobj *js_global;
jsval js_exc;
int js_steps_limit;

jsobj *js_object_proto, *js_function_proto, *js_array_proto, *js_string_proto,
      *js_number_proto, *js_boolean_proto, *js_error_proto, *js_regexp_proto,
      *js_date_proto, *js_promise_proto, *js_map_proto, *js_set_proto, *js_iter_proto;

jsstr *S_length, *S_prototype, *S_constructor, *S_name, *S_message, *S_toString,
      *S_valueOf, *S_iterator, *S_next, *S_done, *S_value, *S_lastIndex, *S_then,
      *S_index, *S_input, *S_callee, *S_get, *S_set, *S_enumerable, *S_writable,
      *S_configurable, *S_stack, *S_toJSON;

/* ------------------------------------------------------------------ память */

static struct gch *all_objs;        /* все объекты сборщика */
static unsigned heap_bytes;         /* сколько живёт (примерно) */
unsigned js_alloc_since_gc;

static void *xmalloc(int n) {
    void *p = malloc(n);
    if (!p) {
#ifdef JS_HOST
        fprintf(stderr, "js: out of memory\n");
        exit(1);
#else
        dprintf(STDERR, "js: out of memory\n");
        exit(1);
#endif
    }
    return p;
}

static void *gc_alloc(int size, int type) {
    struct gch *h = xmalloc(size);
    memset(h, 0, size);
    h->type = type;
    h->next = all_objs;
    all_objs = h;
    heap_bytes += size;
    js_alloc_since_gc += size;
    return h;
}

unsigned js_heap_used(void) { return heap_bytes; }

/* память, которая живёт до js_reset (дерево разбора и т. п.): кусками по 64 КБ */
struct chunk { struct chunk *next; int used, cap; };
static struct chunk *chunks;

void *js_alloc(int n) {
    n = (n + 7) & ~7;
    if (!chunks || chunks->used + n > chunks->cap) {
        int cap = n > 65536 - 64 ? n : 65536 - 64;
        struct chunk *c = xmalloc(sizeof(struct chunk) + cap);
        c->next = chunks;
        c->used = 0;
        c->cap = cap;
        chunks = c;
    }
    void *p = (char *)(chunks + 1) + chunks->used;
    chunks->used += n;
    memset(p, 0, n);
    return p;
}

/* ------------------------------------------------------------------ строковый буфер */

void jb_add(jsbuf *b, const char *s, int n) {
    if (n <= 0) return;
    if (b->n + n + 1 > b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = xmalloc(cap);
        if (b->n) memcpy(p, b->p, b->n);
        free(b->p);
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
}
void jb_adds(jsbuf *b, const char *s) { jb_add(b, s, strlen(s)); }
void jb_addc(jsbuf *b, char c) { jb_add(b, &c, 1); }
jsstr *jb_str(jsbuf *b) {
    jsstr *s = js_strn(b->p ? b->p : "", b->n);
    jb_free(b);
    return s;
}
void jb_free(jsbuf *b) { free(b->p); b->p = NULL; b->n = b->cap = 0; }

/* ------------------------------------------------------------------ строки */

static unsigned hash_bytes(const char *s, int n) {
    unsigned h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (ju8)s[i]; h *= 16777619u; }
    return h;
}

jsstr *js_strn(const char *s, int n) {
    if (n < 0) n = 0;
    jsstr *r = gc_alloc(sizeof(jsstr) + n + 1, GC_STR);
    r->len = n;
    r->idx = -1;
    if (n) memcpy(r->s, s, n);
    r->s[n] = 0;
    r->hash = hash_bytes(r->s, n);
    return r;
}
jsstr *js_str(const char *s) { return js_strn(s, strlen(s)); }
jsval js_cstr(const char *s) { return js_strv(js_str(s)); }

static jsstr **itab;                /* таблица интернированных строк */
static int itab_cap, itab_n;

static int calc_index(const char *s, int n) {
    if (n == 0 || n > 9) return -1;
    if (s[0] == '0' && n > 1) return -1;
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static void itab_grow(void) {
    int cap = itab_cap ? itab_cap * 2 : 4096;
    jsstr **t = xmalloc(sizeof(jsstr *) * cap);
    memset(t, 0, sizeof(jsstr *) * cap);
    for (int i = 0; i < itab_cap; i++)
        for (jsstr *s = itab[i], *nx; s; s = nx) {
            nx = s->inext;
            s->inext = t[s->hash & (cap - 1)];
            t[s->hash & (cap - 1)] = s;
        }
    free(itab);
    itab = t;
    itab_cap = cap;
}

jsstr *js_intern(const char *s, int n) {
    unsigned h = hash_bytes(s, n);
    if (itab_cap) {
        for (jsstr *p = itab[h & (itab_cap - 1)]; p; p = p->inext)
            if (p->hash == h && p->len == n && !memcmp(p->s, s, n)) return p;
    }
    if (itab_n * 2 >= itab_cap) itab_grow();
    jsstr *r = js_strn(s, n);
    r->interned = 1;
    r->idx = calc_index(s, n);
    r->inext = itab[h & (itab_cap - 1)];
    itab[h & (itab_cap - 1)] = r;
    itab_n++;
    return r;
}

jsstr *js_intern_str(jsstr *s) {
    if (s->interned || s->symbol) return s;
    return js_intern(s->s, s->len);
}

static void itab_remove(jsstr *s) {
    jsstr **pp = &itab[s->hash & (itab_cap - 1)];
    for (; *pp; pp = &(*pp)->inext)
        if (*pp == s) { *pp = s->inext; itab_n--; return; }
}

/* постоянная строка (имена в AST и встроенных объектах): сборщик её не трогает */
jsstr *js_perm(const char *s) {
    jsstr *r = js_intern(s, strlen(s));
    r->h.mark = 2;
    return r;
}

jsstr *js_perm_str(jsstr *s) {
    jsstr *r = js_intern(s->s, s->len);
    r->h.mark = 2;
    return r;
}

jsstr *js_concat(jsstr *a, jsstr *b) {
    if (!a->len) return b;
    if (!b->len) return a;
    jsstr *r = gc_alloc(sizeof(jsstr) + a->len + b->len + 1, GC_STR);
    r->len = a->len + b->len;
    r->idx = -1;
    memcpy(r->s, a->s, a->len);
    memcpy(r->s + a->len, b->s, b->len);
    r->s[r->len] = 0;
    r->hash = hash_bytes(r->s, r->len);
    return r;
}

int js_index_key(jsstr *key) {
    if (key->interned) return key->idx;
    return key->symbol ? -1 : calc_index(key->s, key->len);
}

jsstr *js_num_str(double d) {
    char buf[64];
    js_num_to_str(d, buf);
    return js_str(buf);
}

/* ------------------------------------------------------------------ объекты */

jsobj *js_new_object_proto(jsobj *proto) {
    jsobj *o = gc_alloc(sizeof(jsobj), GC_OBJ);
    o->proto = proto;
    o->extensible = 1;
    return o;
}
jsobj *js_new_object(void) { return js_new_object_proto(js_object_proto); }

jsobj *js_new_array(int n) {
    jsobj *a = js_new_object_proto(js_array_proto);
    a->cls = C_ARRAY;
    if (n > 0) {
        a->arr = xmalloc(sizeof(jsval) * n);
        a->acap = n;
        for (int i = 0; i < n; i++) a->arr[i] = js_undef();
        a->alen = n;
    }
    return a;
}

static void arr_reserve(jsobj *a, int n) {
    if (n <= a->acap) return;
    int cap = a->acap ? a->acap * 2 : 8;
    while (cap < n) cap *= 2;
    jsval *p = xmalloc(sizeof(jsval) * cap);
    if (a->alen) memcpy(p, a->arr, sizeof(jsval) * a->alen);
    free(a->arr);
    heap_bytes += (cap - a->acap) * sizeof(jsval);
    js_alloc_since_gc += (cap - a->acap) * sizeof(jsval);
    a->arr = p;
    a->acap = cap;
}

void js_array_push(jsobj *a, jsval v) {
    arr_reserve(a, a->alen + 1);
    a->arr[a->alen++] = v;
}

static void arr_set_len(jsobj *a, int n) {
    if (n < 0) n = 0;
    if (n > a->alen) {
        arr_reserve(a, n);
        for (int i = a->alen; i < n; i++) a->arr[i].tag = JS_EMPTY;
    }
    a->alen = n;
}

jsobj *js_new_host(const struct js_class *c, void *data, jsobj *proto) {
    jsobj *o = js_new_object_proto(proto ? proto : js_object_proto);
    o->cls = C_HOST;
    o->hc = c;
    o->ptr = data;
    return o;
}

/* --- таблица свойств: массив по порядку добавления + хеш-индекс, когда их > 8 */

static void hidx_build(jsobj *o) {
    int cap = 16;
    while (cap < o->nprops * 2) cap *= 2;
    free(o->hidx);
    o->hidx = xmalloc(sizeof(int) * cap);
    memset(o->hidx, 0, sizeof(int) * cap);
    o->hcap = cap;
    for (int i = 0; i < o->nprops; i++) {
        unsigned h = o->props[i].key->hash & (cap - 1);
        while (o->hidx[h]) h = (h + 1) & (cap - 1);
        o->hidx[h] = i + 1;
    }
}

static int find_prop(jsobj *o, jsstr *key) {
    if (!o->hidx) {
        for (int i = 0; i < o->nprops; i++) if (o->props[i].key == key) return i;
        return -1;
    }
    unsigned h = key->hash & (o->hcap - 1);
    for (;;) {
        int k = o->hidx[h];
        if (!k) return -1;
        if (o->props[k - 1].key == key) return k - 1;
        h = (h + 1) & (o->hcap - 1);
    }
}

struct prop *js_own_prop(jsobj *o, jsstr *key) {
    int i = find_prop(o, key);
    return i < 0 ? NULL : &o->props[i];
}

static struct prop *add_prop(jsobj *o, jsstr *key, int flags) {
    if (o->nprops == o->pcap) {
        int cap = o->pcap ? o->pcap * 2 : 4;
        struct prop *p = xmalloc(sizeof(struct prop) * cap);
        if (o->nprops) memcpy(p, o->props, sizeof(struct prop) * o->nprops);
        free(o->props);
        heap_bytes += (cap - o->pcap) * sizeof(struct prop);
        js_alloc_since_gc += (cap - o->pcap) * sizeof(struct prop);
        o->props = p;
        o->pcap = cap;
    }
    struct prop *p = &o->props[o->nprops++];
    p->key = key;
    p->flags = flags;
    p->v = js_undef();
    p->set = NULL;
    if (o->nprops > 8) {
        if (!o->hidx || o->nprops * 2 > o->hcap) hidx_build(o);
        else {
            unsigned h = key->hash & (o->hcap - 1);
            while (o->hidx[h]) h = (h + 1) & (o->hcap - 1);
            o->hidx[h] = o->nprops;
        }
    }
    return p;
}

void js_define(jsobj *o, const char *key, jsval v, int flags) {
    jsstr *k = js_perm(key);
    struct prop *p = js_own_prop(o, k);
    if (!p) p = add_prop(o, k, flags);
    p->flags = flags;
    p->v = v;
}

/* определить свойство с готовым ключом (для литералов объектов, defineProperty) */
struct prop *js_define_key(jsobj *o, jsstr *key, jsval v, int flags) {
    key = js_intern_str(key);
    if (o->cls == C_ARRAY && !(flags & P_ACCESSOR) && key->idx >= 0 && key->idx < 10000000) {
        if (key->idx >= o->alen) arr_set_len(o, key->idx + 1);
        o->arr[key->idx] = v;
        return NULL;
    }
    struct prop *p = js_own_prop(o, key);
    if (!p) p = add_prop(o, key, flags);
    p->flags = flags;
    p->v = v;
    if (!(flags & P_ACCESSOR)) p->set = NULL;
    return p;
}

jsobj *js_new_native(jsnative fn, const char *name, int nargs) {
    jsobj *f = js_new_object_proto(js_function_proto);
    f->cls = C_NATIVE;
    f->ptr = (void *)fn;
    js_define(f, "name", js_strv(js_perm(name)), P_CONF);
    js_define(f, "length", js_num(nargs), P_CONF);
    return f;
}

void js_defn(jsobj *o, const char *name, jsnative fn, int nargs) {
    js_define(o, name, js_objv(js_new_native(fn, name, nargs)), P_WRITE | P_CONF);
}

void js_def_accessor(jsobj *o, const char *name, jsnative get, jsnative set) {
    jsstr *k = js_perm(name);
    struct prop *p = js_own_prop(o, k);
    if (!p) p = add_prop(o, k, 0);
    p->flags = P_ACCESSOR | P_CONF;
    p->v = get ? js_objv(js_new_native(get, name, 0)) : js_undef();
    p->set = set ? js_new_native(set, name, 1) : NULL;
}

int js_delete(jsobj *o, jsstr *key) {
    key = js_intern_str(key);
    if (o->hc && o->hc->del) { int r = o->hc->del(o, key); if (r < 0) { js_exc = js_undef(); return 0; } return r; }
    if ((o->cls == C_ARRAY || o->cls == C_ARGS) && key->idx >= 0) {
        if (key->idx < o->alen) {
            if (key->idx == o->alen - 1 && 0) o->alen--;
            else o->arr[key->idx].tag = JS_EMPTY;
        }
        return 1;
    }
    int i = find_prop(o, key);
    if (i < 0) return 1;
    if (!(o->props[i].flags & P_CONF)) return 0;
    memmove(&o->props[i], &o->props[i + 1], sizeof(struct prop) * (o->nprops - i - 1));
    o->nprops--;
    if (o->hidx) hidx_build(o);
    return 1;
}

int js_has_prop(jsobj *o, jsstr *key) {
    key = js_intern_str(key);
    for (; o; o = o->proto) {
        if (o->hc && o->hc->has) { int r = o->hc->has(o, key); if (r < 0) { js_exc = js_undef(); return 0; } return r; }
        if (o->hc && o->hc->get) {
            jsval tmp;
            int r = o->hc->get(o, key, &tmp);
            if (r == 1) return 1;
        }
        if ((o->cls == C_ARRAY || o->cls == C_ARGS) && key->idx >= 0 && key->idx < o->alen && o->arr[key->idx].tag != JS_EMPTY) return 1;
        if ((o->cls == C_ARRAY || o->cls == C_ARGS) && key == S_length) return 1;
        if (o->cls == C_STRING && key->idx >= 0 && key->idx < o->iv[0].u.s->len) return 1;
        if (find_prop(o, key) >= 0) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ исключения */

jsobj *js_make_error(const char *type, const char *msg) {
    jsval ctor, proto;
    jsobj *p = js_error_proto;
    if (js_get_c(js_objv(js_global), type, &ctor) == 0 && IS_OBJ(ctor) &&
        js_get(ctor, S_prototype, &proto) == 0 && IS_OBJ(proto)) p = proto.u.o;
    jsobj *e = js_new_object_proto(p);
    e->cls = C_ERROR;
    js_define_key(e, S_message, js_cstr(msg), P_WRITE | P_CONF);
    jsbuf st = { 0 };
    jb_adds(&st, type);
    jb_adds(&st, ": ");
    jb_adds(&st, msg);
    jb_addc(&st, '\n');
    js_stack_text(&st);
    js_define_key(e, S_stack, js_strv(jb_str(&st)), P_WRITE | P_CONF);
    return e;
}

int js_throw(const char *type, const char *fmt, ...) {
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    js_exc = js_objv(js_make_error(type, buf));
    return -1;
}

int js_throw_val(jsval v) { js_exc = v; return -1; }

const char *js_error_text(jsval e, char *buf, int max) {
    jsstr *s = NULL;
    if (IS_OBJ(e)) {
        jsval name, msg, st;
        if (js_get(e, S_name, &name) == 0 && js_get(e, S_message, &msg) == 0 && msg.tag != JS_UNDEF) {
            jsstr *a = js_to_str_nothrow(name), *b = js_to_str_nothrow(msg);
            const char *at = "";
            char where[120] = "";
            if (js_get(e, S_stack, &st) == 0 && st.tag == JS_STR) {     /* первая строка «at ...» */
                at = strstr(st.u.s->s, "\n    at ");
                if (at) { at += 8; int l = 0; while (at[l] && at[l] != '\n' && l < 119) { where[l] = at[l]; l++; } where[l] = 0; }
            }
            at = where;
            snprintf(buf, max, "%s: %s%s%s%s", a->s, b->s, *at ? " (" : "", at, *at ? ")" : "");
            return buf;
        }
    }
    s = js_to_str_nothrow(e);
    snprintf(buf, max, "%s", s->s);
    return buf;
}

/* ------------------------------------------------------------------ чтение и запись свойств */

int js_call_getter(jsval getter, jsval recv, jsval *out) {
    if (!IS_OBJ(getter)) { *out = js_undef(); return 0; }
    return js_call(getter, recv, 0, NULL, out);
}

static int str_char(jsstr *s, int i, jsval *out) {
    *out = js_strv(js_intern(s->s + i, 1));
    return 0;
}

/* свойство объекта o (начиная с o и вверх по прототипам), this = recv */
int js_get_obj(jsobj *o, jsstr *key, jsval recv, jsval *out) {
    for (; o; o = o->proto) {
        if (o->hc && o->hc->get) {
            int r = o->hc->get(o, key, out);
            if (r) return r < 0 ? -1 : 0;
        }
        if (o->cls == C_ARRAY || o->cls == C_ARGS) {
            if (key->idx >= 0) {
                if (key->idx < o->alen && o->arr[key->idx].tag != JS_EMPTY) { *out = o->arr[key->idx]; return 0; }
            } else if (key == S_length && o->cls == C_ARRAY) { *out = js_num(o->alen); return 0; }
        } else if (o->cls == C_STRING) {
            jsstr *s = o->iv[0].u.s;
            if (key == S_length) { *out = js_num(s->len); return 0; }
            if (key->idx >= 0 && key->idx < s->len) return str_char(s, key->idx, out);
        }
        int i = find_prop(o, key);
        if (i < 0 && o->cls == C_FUNC && (key == S_prototype || key == S_name || key == S_length)) {
            extern int js_func_lazy(jsobj *o, jsstr *key, jsval *out);
            if (js_func_lazy(o, key, out)) return 0;
        }
        if (i >= 0) {
            struct prop *p = &o->props[i];
            if (p->flags & P_ACCESSOR) return js_call_getter(p->v, recv, out);
            *out = p->v;
            return 0;
        }
    }
    *out = js_undef();
    return 0;
}

int js_get(jsval v, jsstr *key, jsval *out) {
    key = js_intern_str(key);
    switch (v.tag) {
    case JS_OBJ: return js_get_obj(v.u.o, key, v, out);
    case JS_STR:
        if (key == S_length) { *out = js_num(v.u.s->len); return 0; }
        if (key->idx >= 0) {
            if (key->idx < v.u.s->len) return str_char(v.u.s, key->idx, out);
            *out = js_undef();
            return 0;
        }
        return js_get_obj(js_string_proto, key, v, out);
    case JS_NUM: return js_get_obj(js_number_proto, key, v, out);
    case JS_BOOL: return js_get_obj(js_boolean_proto, key, v, out);
    default:
        return js_throw("TypeError", "Cannot read properties of %s (reading '%.60s')",
                        v.tag == JS_NULL ? "null" : "undefined", key->s);
    }
}

int js_get_c(jsval v, const char *key, jsval *out) { return js_get(v, js_intern(key, strlen(key)), out); }

int js_set_obj(jsobj *o, jsstr *key, jsval v, jsval recv) {
    if (o->hc && o->hc->set) {
        int r = o->hc->set(o, key, v);
        if (r) return r < 0 ? -1 : 0;
    }
    if (o->cls == C_ARRAY) {
        if (key->idx >= 0) {
            if (key->idx < o->alen) { o->arr[key->idx] = v; return 0; }
            if (key->idx < o->alen + 1000000) {
                arr_set_len(o, key->idx + 1);
                o->arr[key->idx] = v;
                return 0;
            }
        } else if (key == S_length) {
            double d;
            if (js_to_number(v, &d) < 0) return -1;
            if (d < 0 || d != js_floor(d) || d > 50000000) return js_throw("RangeError", "Invalid array length");
            arr_set_len(o, (int)d);
            return 0;
        }
    } else if (o->cls == C_ARGS && key->idx >= 0 && key->idx < o->alen) {
        o->arr[key->idx] = v;
        return 0;
    }
    int i = find_prop(o, key);
    if (i >= 0) {
        struct prop *p = &o->props[i];
        if (p->flags & P_ACCESSOR) {
            if (!p->set) return 0;
            jsval r;
            return js_call(js_objv(p->set), recv, 1, &v, &r);
        }
        if (p->flags & P_WRITE) p->v = v;
        return 0;
    }
    for (jsobj *q = o->proto; q; q = q->proto) {             /* сеттер у прототипа? */
        if (q->hc) continue;
        int k = find_prop(q, key);
        if (k >= 0) {
            struct prop *p = &q->props[k];
            if (p->flags & P_ACCESSOR) {
                if (!p->set) return 0;
                jsval r;
                return js_call(js_objv(p->set), recv, 1, &v, &r);
            }
            if (!(p->flags & P_WRITE)) return 0;
            break;
        }
    }
    if (!o->extensible) return 0;
    add_prop(o, key, P_ENUM | P_WRITE | P_CONF)->v = v;
    return 0;
}

int js_set(jsval o, jsstr *key, jsval v) {
    key = js_intern_str(key);
    if (o.tag == JS_OBJ) return js_set_obj(o.u.o, key, v, o);
    if (IS_NULLISH(o))
        return js_throw("TypeError", "Cannot set properties of %s (setting '%.60s')", o.tag == JS_NULL ? "null" : "undefined", key->s);
    return 0;                                               /* у примитивов — молча */
}

int js_set_c(jsval o, const char *key, jsval v) { return js_set(o, js_intern(key, strlen(key)), v); }

/* ------------------------------------------------------------------ преобразования */

int js_truthy(jsval v) {
    switch (v.tag) {
    case JS_BOOL: return v.u.b;
    case JS_NUM: return v.u.n != 0 && !js_isnan(v.u.n);
    case JS_STR: return v.u.s->len > 0;
    case JS_OBJ: return 1;
    default: return 0;
    }
}

int js_is_callable(jsval v) {
    return v.tag == JS_OBJ && (v.u.o->cls == C_FUNC || v.u.o->cls == C_NATIVE || v.u.o->cls == C_BOUND);
}

const char *js_typeof(jsval v) {
    switch (v.tag) {
    case JS_UNDEF: case JS_EMPTY: return "undefined";
    case JS_NULL: return "object";
    case JS_BOOL: return "boolean";
    case JS_NUM: return "number";
    case JS_STR: return v.u.s->symbol ? "symbol" : "string";
    default: return js_is_callable(v) ? "function" : "object";
    }
}

int js_to_primitive(jsval v, int hint_string, jsval *out) {
    if (v.tag != JS_OBJ) { *out = v; return 0; }
    jsobj *o = v.u.o;
    if (o->cls == C_DATE && hint_string < 0) hint_string = 1;
    jsstr *order[2] = { hint_string > 0 ? S_toString : S_valueOf, hint_string > 0 ? S_valueOf : S_toString };
    for (int k = 0; k < 2; k++) {
        jsval f, r;
        if (js_get(v, order[k], &f) < 0) return -1;
        if (js_is_callable(f)) {
            if (js_call(f, v, 0, NULL, &r) < 0) return -1;
            if (r.tag != JS_OBJ) { *out = r; return 0; }
        }
    }
    return js_throw("TypeError", "Cannot convert object to primitive value");
}

double js_str_to_num(const char *s, int n) {
    int i = 0;
    while (i < n && isspace((ju8)s[i])) i++;
    while (n > i && isspace((ju8)s[n - 1])) n--;
    if (i == n) return 0;
    if (n - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B' || s[i + 1] == 'o' || s[i + 1] == 'O')) {
        int base = (s[i + 1] | 32) == 'x' ? 16 : (s[i + 1] | 32) == 'b' ? 2 : 8;
        double v = 0;
        for (int k = i + 2; k < n; k++) {
            int c = tolower((ju8)s[k]), d = isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 99;
            if (d >= base) return JS_NAN;
            v = v * base + d;
        }
        return v;
    }
    int used;
    double v = js_parse_float(s + i, n - i, &used);
    if (used != n - i) return JS_NAN;
    return v;
}

int js_to_number(jsval v, double *out) {
    switch (v.tag) {
    case JS_NUM: *out = v.u.n; return 0;
    case JS_BOOL: *out = v.u.b; return 0;
    case JS_NULL: *out = 0; return 0;
    case JS_STR:
        if (v.u.s->symbol) return js_throw("TypeError", "Cannot convert a Symbol value to a number");
        *out = js_str_to_num(v.u.s->s, v.u.s->len);
        return 0;
    case JS_OBJ: {
        jsval p;
        if (js_to_primitive(v, 0, &p) < 0) return -1;
        return js_to_number(p, out);
    }
    default: *out = JS_NAN; return 0;
    }
}

double js_num_nothrow(jsval v) {
    double d;
    if (js_to_number(v, &d) < 0) { js_exc = js_undef(); return JS_NAN; }
    return d;
}

int js_to_int32(jsval v, int *out) {
    double d;
    if (v.tag == JS_NUM) d = v.u.n;
    else if (js_to_number(v, &d) < 0) return -1;
    if (js_isnan(d) || js_isinf(d)) { *out = 0; return 0; }
    if (d >= -2147483648.0 && d <= 2147483647.0) { *out = (int)d; return 0; }
    d = d < 0 ? -js_floor(-d) : js_floor(d);
    d = js_fmod(d, 4294967296.0);
    if (d < 0) d += 4294967296.0;
    unsigned u = (unsigned)d;
    *out = (int)u;
    return 0;
}

int js_to_string(jsval v, jsstr **out) {
    switch (v.tag) {
    case JS_STR:
        if (v.u.s->symbol) return js_throw("TypeError", "Cannot convert a Symbol value to a string");
        *out = v.u.s;
        return 0;
    case JS_NUM: {
        double d = v.u.n;
        if (d >= 0 && d < 1000000000 && d == (int)d) {
            char b[16];
            int n = snprintf(b, sizeof(b), "%d", (int)d);
            *out = js_intern(b, n);
            return 0;
        }
        *out = js_num_str(d);
        return 0;
    }
    case JS_BOOL: *out = js_intern(v.u.b ? "true" : "false", v.u.b ? 4 : 5); return 0;
    case JS_NULL: *out = js_intern("null", 4); return 0;
    case JS_OBJ: {
        jsval p;
        if (js_to_primitive(v, 1, &p) < 0) return -1;
        return js_to_string(p, out);
    }
    default: *out = js_intern("undefined", 9); return 0;
    }
}

jsstr *js_to_str_nothrow(jsval v) {
    jsstr *s;
    if (v.tag == JS_STR && v.u.s->symbol) {
        jsbuf b = { 0 };
        jb_adds(&b, "Symbol(");
        jb_add(&b, v.u.s->s + 2, v.u.s->len > 2 ? v.u.s->len - 2 : 0);
        jb_adds(&b, ")");
        return jb_str(&b);
    }
    if (js_to_string(v, &s) < 0) { js_exc = js_undef(); return js_intern("[object]", 8); }
    return s;
}

int js_to_object(jsval v, jsobj **out) {
    jsobj *o;
    switch (v.tag) {
    case JS_OBJ: *out = v.u.o; return 0;
    case JS_STR: o = js_new_object_proto(js_string_proto); o->cls = C_STRING; break;
    case JS_NUM: o = js_new_object_proto(js_number_proto); o->cls = C_NUMBER; break;
    case JS_BOOL: o = js_new_object_proto(js_boolean_proto); o->cls = C_BOOLEAN; break;
    default: return js_throw("TypeError", "Cannot convert undefined or null to object");
    }
    o->iv[0] = v;
    *out = o;
    return 0;
}

int js_strict_eq(jsval a, jsval b) {
    if (a.tag != b.tag) return 0;
    switch (a.tag) {
    case JS_UNDEF: case JS_NULL: return 1;
    case JS_BOOL: return a.u.b == b.u.b;
    case JS_NUM: return a.u.n == b.u.n;
    case JS_STR: return a.u.s == b.u.s || (!a.u.s->symbol && !b.u.s->symbol && a.u.s->len == b.u.s->len &&
                                         a.u.s->hash == b.u.s->hash && !memcmp(a.u.s->s, b.u.s->s, a.u.s->len));
    case JS_OBJ: return a.u.o == b.u.o;
    }
    return 0;
}

int js_loose_eq(jsval a, jsval b, int *res) {
    for (;;) {
        if (a.tag == b.tag) { *res = js_strict_eq(a, b); return 0; }
        if (IS_NULLISH(a) && IS_NULLISH(b)) { *res = 1; return 0; }
        if (IS_NULLISH(a) || IS_NULLISH(b)) { *res = 0; return 0; }
        if (a.tag == JS_NUM && b.tag == JS_STR) { *res = a.u.n == js_str_to_num(b.u.s->s, b.u.s->len); return 0; }
        if (a.tag == JS_STR && b.tag == JS_NUM) { *res = b.u.n == js_str_to_num(a.u.s->s, a.u.s->len); return 0; }
        if (a.tag == JS_BOOL) { a = js_num(a.u.b); continue; }
        if (b.tag == JS_BOOL) { b = js_num(b.u.b); continue; }
        if (a.tag == JS_OBJ) { if (js_to_primitive(a, -1, &a) < 0) return -1; continue; }
        if (b.tag == JS_OBJ) { if (js_to_primitive(b, -1, &b) < 0) return -1; continue; }
        *res = 0;
        return 0;
    }
}

int js_array_len(jsval o, int *len) {
    if (o.tag == JS_OBJ && o.u.o->cls == C_ARRAY) { *len = o.u.o->alen; return 0; }
    jsval l;
    if (js_get(o, S_length, &l) < 0) return -1;
    double d;
    if (js_to_number(l, &d) < 0) return -1;
    if (js_isnan(d) || d < 0) d = 0;
    if (d > 50000000) d = 50000000;
    *len = (int)d;
    return 0;
}

/* ------------------------------------------------------------------ сборщик мусора */

#define MAX_ROOTS 64
static jsval *roots[MAX_ROOTS];
static int n_roots;
static void (*mark_hook)(void);

void js_add_root(jsval *v) { if (n_roots < MAX_ROOTS) roots[n_roots++] = v; }
void js_set_mark_hook(void (*f)(void)) { mark_hook = f; }

static struct gch **mstack;         /* стек обхода (без рекурсии) */
static int msp, mcap;

static void push_gc(struct gch *h) {
    if (!h || h->mark) return;
    h->mark = 1;
    if (h->type == GC_STR) return;
    if (msp == mcap) {
        int cap = mcap ? mcap * 2 : 1024;
        struct gch **p = xmalloc(sizeof(void *) * cap);
        if (msp) memcpy(p, mstack, sizeof(void *) * msp);
        free(mstack);
        mstack = p;
        mcap = cap;
    }
    mstack[msp++] = h;
}

void js_mark_val(jsval v) {
    if (v.tag == JS_STR || v.tag == JS_OBJ) push_gc((struct gch *)v.u.o);
}
void js_mark_obj(jsobj *o) { if (o) push_gc(&o->h); }

extern void js_mark_runtime(void);   /* jsrun.c и jslib.c: очередь заданий и т. п. */

static void scan(struct gch *h) {
    if (h->type == GC_ENV) {
        jsenv *e = (jsenv *)h;
        if (e->parent) push_gc(&e->parent->h);
        for (int i = 0; i < e->n; i++) js_mark_val(e->v[i]);
        return;
    }
    jsobj *o = (jsobj *)h;
    if (o->proto) push_gc(&o->proto->h);
    for (int i = 0; i < o->nprops; i++) {
        push_gc(&o->props[i].key->h);
        js_mark_val(o->props[i].v);
        if (o->props[i].set) push_gc(&o->props[i].set->h);
    }
    for (int i = 0; i < o->alen; i++) js_mark_val(o->arr[i]);
    js_mark_val(o->iv[0]);
    js_mark_val(o->iv[1]);
    if (o->env) push_gc(&o->env->h);
    if (o->hc && o->hc->mark) o->hc->mark(o);
    if (o->cls == C_CORO && o->ptr) js_coro_mark(o->ptr);
}

/* стеки приостановленных генераторов и async-функций: на них лежат значения JS,
 * о которых сборщик иначе не знает. Ищем там всё, похожее на адрес объекта. */
extern void js_coro_stack(void *p, void **lo, void **hi);
#if defined(__SANITIZE_ADDRESS__)
__attribute__((no_sanitize_address))
#endif
static void scan_coro_stacks(void) {
    int n = 0;
    for (struct gch *h = all_objs; h; h = h->next) n++;
    int cap = 64;
    while (cap < n * 2) cap *= 2;
    struct gch **set = malloc(sizeof(void *) * cap);
    if (!set) return;
    memset(set, 0, sizeof(void *) * cap);
    for (struct gch *h = all_objs; h; h = h->next) {
        unsigned long k = ((unsigned long)h >> 3) & (cap - 1);
        while (set[k]) k = (k + 1) & (cap - 1);
        set[k] = h;
    }
    for (struct gch *h = all_objs; h; h = h->next) {
        if (h->type != GC_OBJ || ((jsobj *)h)->cls != C_CORO || !((jsobj *)h)->ptr) continue;
        void *lo, *hi;
        js_coro_stack(((jsobj *)h)->ptr, &lo, &hi);
        for (void **w = lo; w && (void *)w < hi; w++) {
            void *v = *w;
            if (!v) continue;
            unsigned long k = ((unsigned long)v >> 3) & (cap - 1);
            while (set[k]) {
                if ((void *)set[k] == v) { push_gc(set[k]); break; }
                k = (k + 1) & (cap - 1);
            }
        }
    }
    free(set);
}

void js_gc(void) {
    if (!js_global) return;
    push_gc(&js_global->h);
    extern jsobj *js_async_function_proto;
    jsobj *protos[] = { js_generator_proto, js_async_function_proto, js_object_proto, js_function_proto, js_array_proto, js_string_proto, js_number_proto,
                        js_boolean_proto, js_error_proto, js_regexp_proto, js_date_proto, js_promise_proto,
                        js_map_proto, js_set_proto, js_iter_proto };
    for (unsigned i = 0; i < sizeof(protos) / sizeof(protos[0]); i++) js_mark_obj(protos[i]);
    js_mark_val(js_exc);
    for (int i = 0; i < n_roots; i++) js_mark_val(*roots[i]);
    if (js_coro_suspended() > 0) scan_coro_stacks();
    js_mark_runtime();
    if (mark_hook) mark_hook();
    while (msp > 0) scan(mstack[--msp]);
    struct gch **pp = &all_objs;
    unsigned live = 0;
    while (*pp) {
        struct gch *h = *pp;
        if (h->mark) {
            if (h->mark == 1) h->mark = 0;
            pp = &h->next;
            if (h->type == GC_STR) live += sizeof(jsstr) + ((jsstr *)h)->len + 1;
            else if (h->type == GC_ENV) live += sizeof(jsenv) + ((jsenv *)h)->n * sizeof(jsval);
            else {
                jsobj *o = (jsobj *)h;
                live += sizeof(jsobj) + o->pcap * sizeof(struct prop) + o->acap * sizeof(jsval);
            }
            continue;
        }
        *pp = h->next;
        if (h->type == GC_STR) {
            if (((jsstr *)h)->interned) itab_remove((jsstr *)h);
        } else if (h->type == GC_OBJ) {
            jsobj *o = (jsobj *)h;
            if (o->cls == C_CORO) js_coro_free(o->ptr);
            free(o->props);
            free(o->hidx);
            free(o->arr);
        }
        free(h);
    }
    heap_bytes = live;
    js_alloc_since_gc = 0;
}

/* ------------------------------------------------------------------ окружения */

jsenv *js_new_env(jsenv *parent, int n) {
    jsenv *e = gc_alloc(sizeof(jsenv) + sizeof(jsval) * n, GC_ENV);
    e->parent = parent;
    e->n = n;
    for (int i = 0; i < n; i++) e->v[i] = js_undef();
    return e;
}

/* ------------------------------------------------------------------ вывод */

static void (*print_fn)(const char *s, int n);
void js_set_print(void (*f)(const char *s, int n)) { print_fn = f; }
void js_print(const char *s, int n) {
    if (print_fn) { print_fn(s, n); return; }
#ifdef JS_HOST
    fwrite(s, 1, n, stdout);
#else
    write(STDOUT, s, n);
#endif
}

/* ------------------------------------------------------------------ большой стек
 * У программ MyOS стек 64 КБ, а интерпретатору (рекурсивный обход дерева)
 * нужно больше. js_big_call выполняет fn(arg) на своём стеке в 2 МБ. */

#if !defined(JS_HOST) && defined(__i386__)
#define BIG_STACK (2 * 1024 * 1024)
static char *big_stack;
static int on_big;
static void (*bc_fn)(void *);
static void *bc_arg, *bc_top;
#endif

void js_big_call(void (*fn)(void *), void *arg) {
#if defined(JS_HOST) || !defined(__i386__)
    fn(arg);
#else
    if (on_big) { fn(arg); return; }
    if (!big_stack) big_stack = malloc(BIG_STACK);
    if (!big_stack) { fn(arg); return; }
    on_big = 1;
    bc_fn = fn;
    bc_arg = arg;
    bc_top = (void *)(((unsigned)(big_stack + BIG_STACK - 64)) & ~15u);
    __asm__ volatile(
        "movl %%esp, %%esi\n\t"
        "movl %2, %%esp\n\t"
        "pushl %%esi\n\t"
        "subl $8, %%esp\n\t"
        "pushl %1\n\t"
        "call *%0\n\t"
        "addl $12, %%esp\n\t"
        "popl %%esp\n\t"
        : : "m"(bc_fn), "m"(bc_arg), "m"(bc_top) : "esi", "eax", "ecx", "edx", "memory", "cc");
    on_big = 0;
#endif
}

/* ------------------------------------------------------------------ начало и конец */

jsstr *js_kw_names[64];
extern void js_parse_init(void);
extern void js_run_reset(void);

void js_init(void) {
    S_length = js_perm("length"); S_prototype = js_perm("prototype"); S_constructor = js_perm("constructor");
    S_name = js_perm("name"); S_message = js_perm("message"); S_toString = js_perm("toString");
    S_valueOf = js_perm("valueOf"); S_next = js_perm("next"); S_done = js_perm("done"); S_value = js_perm("value");
    S_lastIndex = js_perm("lastIndex"); S_then = js_perm("then"); S_index = js_perm("index"); S_input = js_perm("input");
    S_callee = js_perm("callee"); S_get = js_perm("get"); S_set = js_perm("set"); S_enumerable = js_perm("enumerable");
    S_writable = js_perm("writable"); S_configurable = js_perm("configurable"); S_stack = js_perm("stack");
    S_toJSON = js_perm("toJSON");
    S_iterator = js_strn("@@Symbol.iterator", 17);
    S_iterator->symbol = 1;
    S_iterator->h.mark = 2;
    js_parse_init();
    js_lib_init();
}

void js_reset(void) {
    extern void js_re_reset(void);
    js_run_reset();
    js_re_reset();
    for (struct gch *h = all_objs, *nx; h; h = nx) {
        nx = h->next;
        if (h->type == GC_OBJ) {
            jsobj *o = (jsobj *)h;
            if (o->cls == C_CORO) js_coro_free(o->ptr);
            free(o->props);
            free(o->hidx);
            free(o->arr);
        }
        free(h);
    }
    all_objs = NULL;
    heap_bytes = 0;
    js_alloc_since_gc = 0;
    free(itab);
    itab = NULL;
    itab_cap = itab_n = 0;
    while (chunks) { struct chunk *c = chunks; chunks = c->next; free(c); }
    n_roots = 0;
    mark_hook = NULL;
    js_global = NULL;
    js_exc = js_undef();
    js_init();
}
