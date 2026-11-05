/* domjs.c — объекты DOM для JavaScript: document, элементы, window, location,
 * события, таймеры, сеть (__myos.fetch для XMLHttpRequest/fetch из прелюдии),
 * cookie. Здесь же — запуск JS на отдельном большом стеке (у программ MyOS
 * стек всего 64 КБ, а интерпретатору с рекурсией нужно больше). */
#include "domint.h"

#define NATIVE(name) static int name(jsval thisv, int argc, jsval *argv, jsval *ret, jsobj *self)
#define ARG(i) ((i) < argc ? argv[i] : js_undef())
#define UNUSED (void)thisv; (void)argc; (void)argv; (void)self
#define TRY(x) do { if ((x) < 0) return -1; } while (0)

extern double js_deadline_ms;
extern int js_max_depth;
extern unsigned js_heap_limit;
void js_inspect(jsbuf *b, jsval v, int top);
jsobj *js_make_ctor(const char *name, jsnative fn, int nargs, jsobj *proto);
void dom_parse_document(const char *s, int n, int run_scripts);
void dom_free_all(void);

static const char prelude_src[] =
#include "dom_prelude.h"
;

static jsobj *node_proto, *element_proto, *html_proto, *text_proto, *doc_proto, *frag_proto, *comment_proto,
             *style_proto, *classlist_proto, *nodelist_proto, *loc_proto, *et_proto;
static jsobj *loc_obj;
static dnode *current_script;
static int n_scripts, n_errors;
static char last_error[200];
static char pending_nav[1024];
static int nav_requested;
static char storage[2][8192];                    /* localStorage / sessionStorage (JSON) */

typedef void (*bigfn)(void *);
static void big_call(bigfn fn, void *arg) { js_big_call(fn, arg); }

/* ------------------------------------------------------------------ адреса */

static void url_split(const char *u, char *proto, char *host, char *path) {   /* "https:", "a.b:8080", "/p?q#h" */
    proto[0] = host[0] = path[0] = 0;
    const char *c = strstr(u, "://");
    if (!c) {
        const char *col = strchr(u, ':');
        if (col && col - u < 16) { snprintf(proto, 16, "%.*s", (int)(col - u + 1), u); snprintf(path, 1024, "%s", col + 1); }
        else snprintf(path, 1024, "%s", u);
        return;
    }
    snprintf(proto, 16, "%.*s", (int)(c - u + 1), u);
    const char *h = c + 3, *e = h;
    while (*e && *e != '/' && *e != '?' && *e != '#') e++;
    snprintf(host, 256, "%.*s", (int)(e - h), h);
    snprintf(path, 1024, "%s", *e ? e : "/");
}

void dn_resolve(const char *base, const char *rel, char *out, int max) {
    while (*rel == ' ' || *rel == '\n' || *rel == '\t') rel++;
    const char *c = rel;
    while (*c && (isalnum((ju8)*c) || *c == '+' || *c == '-' || *c == '.')) c++;
    if (*c == ':' && c > rel && (c[1] == '/' || !strncmp(rel, "about:", 6) || !strncmp(rel, "javascript:", 11) ||
                                  !strncmp(rel, "mailto:", 7) || !strncmp(rel, "data:", 5) || !strncmp(rel, "file:", 5) || !strncmp(rel, "blob:", 5))) {
        snprintf(out, max, "%s", rel);
        return;
    }
    char proto[16], host[256], path[1024];
    url_split(base, proto, host, path);
    if (rel[0] == '/' && rel[1] == '/') { snprintf(out, max, "%s%s", proto, rel); return; }
    char prefix[300];
    if (host[0]) snprintf(prefix, sizeof(prefix), "%s//%s", proto, host);
    else snprintf(prefix, sizeof(prefix), "%s", proto);
    if (!rel[0]) { snprintf(out, max, "%s", base); char *h = strchr(out, '#'); if (h) *h = 0; return; }
    if (rel[0] == '/') { snprintf(out, max, "%s%s", prefix, rel); return; }
    char *h = strchr(path, '#');
    if (rel[0] == '#') { if (h) *h = 0; snprintf(out, max, "%s%s%s", prefix, path, rel); return; }
    if (h) *h = 0;
    char *q = strchr(path, '?');
    if (rel[0] == '?') { if (q) *q = 0; snprintf(out, max, "%s%s%s", prefix, path, rel); return; }
    if (q) *q = 0;
    char *sl = strrchr(path, '/');
    if (sl) sl[1] = 0; else strcpy(path, "/");
    char res[2048];
    snprintf(res, sizeof(res), "%s%s", path, rel);
    /* убрать ./ и ../ */
    char *segs[256];
    int ns = 0;
    char *p = res + (res[0] == '/');
    char tail[1024] = "";
    char *qs = p;
    while (*qs && *qs != '?' && *qs != '#') qs++;
    if (!*qs) qs = NULL;
    if (qs) { snprintf(tail, sizeof(tail), "%s", qs); *qs = 0; }
    int trailing = res[strlen(res) - 1] == '/' || !strcmp(res + strlen(res) - 2 > res ? res + strlen(res) - 2 : res, "/.") ;
    while (p && *p && ns < 256) {
        char *e = strchr(p, '/');
        if (e) *e = 0;
        if (!strcmp(p, "..")) { if (ns) ns--; trailing = 1; }
        else if (strcmp(p, ".") && *p) { segs[ns++] = p; trailing = 0; }
        else if (!strcmp(p, ".")) trailing = 1;
        p = e ? e + 1 : NULL;
        if (e && !*p) trailing = 1;
    }
    int o = snprintf(out, max, "%s", prefix);
    for (int i = 0; i < ns && o < max; i++) o += snprintf(out + o, max - o, "/%s", segs[i]);
    if ((trailing || !ns) && o < max) o += snprintf(out + o, max - o, "/");
    if (o < max) snprintf(out + o, max - o, "%s", tail);
}

/* ------------------------------------------------------------------ обёртки узлов */

static int node_get(jsobj *o, jsstr *key, jsval *out);
static int node_set(jsobj *o, jsstr *key, jsval v);
static void node_mark(jsobj *o) { (void)o; }
static const struct js_class node_class = { "HTMLElement", node_get, node_set, node_mark };

/* свой прототип для каждого вида элементов: div -> HTMLDivElement.prototype и т. д. */
static const struct { const char *tags, *name; } elem_kinds[] = {
    { " div ", "HTMLDivElement" }, { " span ", "HTMLSpanElement" }, { " a ", "HTMLAnchorElement" },
    { " input ", "HTMLInputElement" }, { " form ", "HTMLFormElement" }, { " img ", "HTMLImageElement" },
    { " script ", "HTMLScriptElement" }, { " button ", "HTMLButtonElement" }, { " select ", "HTMLSelectElement" },
    { " textarea ", "HTMLTextAreaElement" }, { " iframe ", "HTMLIFrameElement" }, { " canvas ", "HTMLCanvasElement" },
    { " video ", "HTMLVideoElement" }, { " audio ", "HTMLAudioElement" }, { " link ", "HTMLLinkElement" },
    { " style ", "HTMLStyleElement" }, { " meta ", "HTMLMetaElement" }, { " head ", "HTMLHeadElement" },
    { " body ", "HTMLBodyElement" }, { " html ", "HTMLHtmlElement" }, { " table ", "HTMLTableElement" },
    { " tr ", "HTMLTableRowElement" }, { " td th ", "HTMLTableCellElement" }, { " p ", "HTMLParagraphElement" },
    { " h1 h2 h3 h4 h5 h6 ", "HTMLHeadingElement" }, { " li ", "HTMLLIElement" }, { " ul ", "HTMLUListElement" },
    { " ol ", "HTMLOListElement" }, { " option ", "HTMLOptionElement" }, { " label ", "HTMLLabelElement" },
    { " pre ", "HTMLPreElement" }, { " template ", "HTMLTemplateElement" }, { " br ", "HTMLBRElement" },
    { " hr ", "HTMLHRElement" }, { " title ", "HTMLTitleElement" }, { " picture ", "HTMLPictureElement" },
    { " source ", "HTMLSourceElement" }, { " details ", "HTMLDetailsElement" }, { " dialog ", "HTMLDialogElement" },
    { " fieldset ", "HTMLFieldSetElement" }, { " legend ", "HTMLLegendElement" }, { " object ", "HTMLObjectElement" },
    { " embed ", "HTMLEmbedElement" }, { " slot ", "HTMLSlotElement" }, { " time ", "HTMLTimeElement" },
    { " output ", "HTMLOutputElement" }, { " progress ", "HTMLProgressElement" }, { " meter ", "HTMLMeterElement" },
    { " data ", "HTMLDataElement" }, { " thead tbody tfoot ", "HTMLTableSectionElement" }, { " optgroup ", "HTMLOptGroupElement" },
    { " base ", "HTMLBaseElement" }, { " noscript section article header footer nav main aside b i em strong small code ", "%HTMLElement" },
};
#define N_KINDS (sizeof(elem_kinds) / sizeof(elem_kinds[0]))
static jsobj *kind_proto[N_KINDS + 1];
static jsobj *svg_proto;

static jsobj *proto_for_tag(const char *tag) {
    char key[40];
    snprintf(key, sizeof(key), " %s ", tag);
    for (unsigned i = 0; i < N_KINDS; i++) if (strstr(elem_kinds[i].tags, key)) return kind_proto[i] ? kind_proto[i] : html_proto;
    if (strstr(" svg path g circle rect line polyline polygon use symbol defs text tspan ellipse clippath mask lineargradient stop ", key)) return svg_proto;
    return html_proto;
}

jsval dn_wrap(dnode *n) {
    if (!n) return js_null();
    if (n->type == DN_WINDOW) return js_objv(js_global);
    if (n->wrap) return js_objv(n->wrap);
    jsobj *p = n->type == DN_ELEMENT ? proto_for_tag(n->tag) : n->type == DN_TEXT ? text_proto : n->type == DN_DOCUMENT ? doc_proto :
               n->type == DN_COMMENT ? comment_proto : frag_proto;
    n->wrap = js_new_host(&node_class, n, p);
    return js_objv(n->wrap);
}

dnode *dn_of(jsval v) {
    if (!IS_OBJ(v)) return NULL;
    if (v.u.o == js_global) return d_window;
    if (v.u.o->hc != &node_class) return NULL;
    return v.u.o->ptr;
}

static void mark_timers(void);
void domjs_mark(void) {
    mark_timers();
    for (int i = 0; i < d_count; i++) {
        dnode *n = d_nodes[i];
        if (n->wrap) js_mark_obj(n->wrap);
        for (int k = 0; k < n->nls; k++) { js_mark_val(n->ls[k].fn); js_mark_val(js_strv(n->ls[k].type)); }
    }
}

static jsobj *node_list(dnode **v, int n) {
    jsobj *a = js_new_array(0);
    a->proto = nodelist_proto;
    for (int i = 0; i < n; i++) js_array_push(a, dn_wrap(v[i]));
    return a;
}

static jsval cstr_or_null(const char *s) { return s ? js_cstr(s) : js_null(); }

/* строковые свойства, которые просто отражают атрибуты */
static const char *attr_props[] = { "id", "name", "type", "title", "alt", "rel", "target", "placeholder", "lang", "dir",
                                    "method", "role", "slot", "accept", "autocomplete", "pattern", "min", "max", "step",
                                    "media", "charset", "content", "httpEquiv", "enctype", "download", "srcset", "sizes",
                                    "crossOrigin", "integrity", "referrerPolicy", "loading", "decoding", "nonce", "as", "label", NULL };

static const char *prop_to_attr(const char *p, char *buf) {
    if (!strcmp(p, "className")) return "class";
    if (!strcmp(p, "htmlFor")) return "for";
    if (!strcmp(p, "httpEquiv")) return "http-equiv";
    if (!strcmp(p, "crossOrigin")) return "crossorigin";
    if (!strcmp(p, "referrerPolicy")) return "referrerpolicy";
    int o = 0;
    for (const char *s = p; *s && o < 60; s++) buf[o++] = tolower((ju8)*s);
    buf[o] = 0;
    return buf;
}

static int is_bool_attr(const char *p) {
    static const char *b[] = { "checked", "disabled", "selected", "hidden", "readOnly", "required", "multiple", "autofocus",
                               "defer", "async", "noModule", "open", "autoplay", "controls", "loop", "muted", "noValidate", NULL };
    for (int i = 0; b[i]; i++) if (!strcmp(b[i], p)) return 1;
    return 0;
}

static dnode *elem_sibling(dnode *n, int next) {
    for (n = next ? n->next : n->prev; n; n = next ? n->next : n->prev) if (n->type == DN_ELEMENT) return n;
    return NULL;
}

static dnode *elem_child(dnode *n, int last) {
    for (dnode *k = last ? n->last : n->first; k; k = last ? k->prev : k->next) if (k->type == DN_ELEMENT) return k;
    return NULL;
}

static void collect(dnode *root, int (*pred)(dnode *, const void *), const void *arg, dnode ***out, int *cnt) {
    int cap = 0;
    *out = NULL;
    *cnt = 0;
    dnode *stack[512];
    int sp = 0;
    for (dnode *k = root->last; k; k = k->prev) if (sp < 512) stack[sp++] = k;
    while (sp) {
        dnode *n = stack[--sp];
        if (n->type == DN_ELEMENT && pred(n, arg)) {
            if (*cnt == cap) {
                cap = cap ? cap * 2 : 16;
                dnode **p = malloc(sizeof(dnode *) * cap);
                if (*cnt) memcpy(p, *out, sizeof(dnode *) * *cnt);
                free(*out);
                *out = p;
            }
            (*out)[(*cnt)++] = n;
        }
        for (dnode *k = n->last; k; k = k->prev) if (sp < 512) stack[sp++] = k;
    }
}

static int pred_tag(dnode *n, const void *t) { return !strcmp((const char *)t, "*") || !strcmp(n->tag, (const char *)t); }
static int pred_form_field(dnode *n, const void *t) {
    (void)t;
    return !strcmp(n->tag, "input") || !strcmp(n->tag, "select") || !strcmp(n->tag, "textarea") || !strcmp(n->tag, "button");
}

static jsobj *tag_list(dnode *root, const char *tag) {
    dnode **v;
    int c;
    collect(root, pred_tag, tag, &v, &c);
    jsobj *a = node_list(v, c);
    free(v);
    return a;
}

/* стиль: объект, который читает и пишет атрибут style="..." */
static int style_get(jsobj *o, jsstr *key, jsval *out);
static int style_set(jsobj *o, jsstr *key, jsval v);
static const struct js_class style_class = { "CSSStyleDeclaration", style_get, style_set, NULL };
static int classlist_get(jsobj *o, jsstr *key, jsval *out);
static const struct js_class classlist_class = { "DOMTokenList", classlist_get, NULL, NULL };
static int dataset_get(jsobj *o, jsstr *key, jsval *out);
static int dataset_set(jsobj *o, jsstr *key, jsval v);
static const struct js_class dataset_class = { "DOMStringMap", dataset_get, dataset_set, NULL };

static jsval sub_object(jsobj *wrap, const char *slot, const struct js_class *c, jsobj *proto, dnode *n) {
    jsstr *k = js_perm(slot);
    struct prop *p = js_own_prop(wrap, k);
    if (p) return p->v;
    jsobj *o = js_new_host(c, n, proto ? proto : js_object_proto);
    js_define_key(wrap, k, js_objv(o), 0);
    return js_objv(o);
}

static void url_part(const char *href, const char *part, jsval *out) {
    char proto[16], host[256], path[1024];
    url_split(href, proto, host, path);
    char *hash = strchr(path, '#');
    char hs[1024] = "";
    if (hash) { snprintf(hs, sizeof(hs), "%s", hash); *hash = 0; }
    char *q = strchr(path, '?');
    char qs[1024] = "";
    if (q) { snprintf(qs, sizeof(qs), "%s", q); *q = 0; }
    char hn[256];
    snprintf(hn, sizeof(hn), "%s", host);
    char *colon = strrchr(hn, ':');
    char port[16] = "";
    if (colon) { snprintf(port, sizeof(port), "%s", colon + 1); *colon = 0; }
    char b[1300];
    if (!strcmp(part, "protocol")) *out = js_cstr(proto);
    else if (!strcmp(part, "host")) *out = js_cstr(host);
    else if (!strcmp(part, "hostname")) *out = js_cstr(hn);
    else if (!strcmp(part, "port")) *out = js_cstr(port);
    else if (!strcmp(part, "pathname")) *out = js_cstr(path);
    else if (!strcmp(part, "search")) *out = js_cstr(!strcmp(qs, "?") ? "" : qs);
    else if (!strcmp(part, "hash")) *out = js_cstr(!strcmp(hs, "#") ? "" : hs);
    else { snprintf(b, sizeof(b), "%s//%s", proto, host); *out = js_cstr(b); }   /* origin */
}

static const char *url_parts[] = { "protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin", NULL };

static int is_doc_prop(const char *k, dnode *n, jsval *out);

static int node_get(jsobj *o, jsstr *key, jsval *out) {
    dnode *n = o->ptr;
    const char *k = key->s;
    if (key->symbol || key->len > 40) return 0;
    if (key->idx >= 0) return 0;
    char c0 = k[0];
    jsbuf b = { 0 };
    char tmp[64];
    if (n->type == DN_DOCUMENT && is_doc_prop(k, n, out)) return 1;
    switch (c0) {
    case 'a':
        if (!strcmp(k, "attributes")) {
            jsobj *a = js_new_array(0);
            for (int i = 0; i < n->nattr; i++) {
                jsobj *at = js_new_object();
                js_define(at, "name", js_cstr(n->attr[2 * i]), P_ENUM | P_WRITE);
                js_define(at, "value", js_cstr(n->attr[2 * i + 1]), P_ENUM | P_WRITE);
                js_define(at, "nodeName", js_cstr(n->attr[2 * i]), P_ENUM | P_WRITE);
                js_define(at, "nodeValue", js_cstr(n->attr[2 * i + 1]), P_ENUM | P_WRITE);
                js_array_push(a, js_objv(at));
            }
            *out = js_objv(a);
            return 1;
        }
        if (!strcmp(k, "action") && n->type == DN_ELEMENT) {
            const char *v = dn_get_attr(n, "action");
            char full[1024];
            dn_resolve(d_url, v ? v : "", full, sizeof(full));
            *out = js_cstr(full);
            return 1;
        }
        break;
    case 'c':
        if (!strcmp(k, "childNodes")) {
            jsobj *a = js_new_array(0);
            a->proto = nodelist_proto;
            for (dnode *x = n->first; x; x = x->next) js_array_push(a, dn_wrap(x));
            *out = js_objv(a);
            return 1;
        }
        if (!strcmp(k, "children")) {
            jsobj *a = js_new_array(0);
            a->proto = nodelist_proto;
            for (dnode *x = n->first; x; x = x->next) if (x->type == DN_ELEMENT) js_array_push(a, dn_wrap(x));
            *out = js_objv(a);
            return 1;
        }
        if (!strcmp(k, "childElementCount")) { int c = 0; for (dnode *x = n->first; x; x = x->next) c += x->type == DN_ELEMENT; *out = js_num(c); return 1; }
        if (!strcmp(k, "className") && n->type == DN_ELEMENT) { const char *v = dn_get_attr(n, "class"); *out = js_cstr(v ? v : ""); return 1; }
        if (!strcmp(k, "classList") && n->type == DN_ELEMENT) { *out = sub_object(o, "%classList", &classlist_class, classlist_proto, n); return 1; }
        if (!strcmp(k, "checked")) { *out = js_bool(dn_get_attr(n, "checked") != NULL); return 1; }
        if ((!strcmp(k, "clientWidth") || !strcmp(k, "clientHeight")) && n->type == DN_ELEMENT) { *out = js_num(!dn_visible(n) ? 0 : k[6] == 'W' ? 760 : 400); return 1; }
        if (!strcmp(k, "content") && n->type == DN_ELEMENT && !strcmp(n->tag, "template")) { *out = dn_wrap(n); return 1; }
        break;
    case 'd':
        if (!strcmp(k, "dataset") && n->type == DN_ELEMENT) { *out = sub_object(o, "%dataset", &dataset_class, NULL, n); return 1; }
        if (!strcmp(k, "data") && (n->type == DN_TEXT || n->type == DN_COMMENT)) { *out = js_strv(js_strn(n->text, n->tlen)); return 1; }
        if (!strcmp(k, "disabled")) { *out = js_bool(dn_get_attr(n, "disabled") != NULL); return 1; }
        break;
    case 'e':
        if (!strcmp(k, "elements") && n->type == DN_ELEMENT) {
            dnode **v;
            int c;
            collect(n, pred_form_field, NULL, &v, &c);
            jsobj *a = node_list(v, c);
            for (int i = 0; i < c; i++) {                      /* form.elements.q */
                const char *nm = dn_get_attr(v[i], "name");
                if (nm && *nm && !isdigit((ju8)nm[0])) js_define_key(a, js_intern(nm, strlen(nm)), dn_wrap(v[i]), P_WRITE | P_CONF);
            }
            free(v);
            *out = js_objv(a);
            return 1;
        }
        break;
    case 'f':
        if (!strcmp(k, "firstChild")) { *out = dn_wrap(n->first); return 1; }
        if (!strcmp(k, "firstElementChild")) { *out = dn_wrap(elem_child(n, 0)); return 1; }
        if (!strcmp(k, "form") && n->type == DN_ELEMENT) { dnode *p = n->parent; while (p && !(p->type == DN_ELEMENT && !strcmp(p->tag, "form"))) p = p->parent; *out = dn_wrap(p); return 1; }
        break;
    case 'h':
        if ((!strcmp(k, "href") || !strcmp(k, "hostname") || !strcmp(k, "host") || !strcmp(k, "hash")) && n->type == DN_ELEMENT &&
            (!strcmp(n->tag, "a") || !strcmp(n->tag, "link") || !strcmp(n->tag, "area") || !strcmp(n->tag, "base"))) {
            const char *v = dn_get_attr(n, "href");
            char full[1024];
            if (v) dn_resolve(d_url, v, full, sizeof(full)); else full[0] = 0;
            if (k[1] == 'r') *out = js_cstr(full);
            else url_part(full, k, out);
            return 1;
        }
        if (!strcmp(k, "hidden")) { *out = js_bool(dn_get_attr(n, "hidden") != NULL); return 1; }
        if (!strcmp(k, "height") && n->type == DN_ELEMENT) { const char *v = dn_get_attr(n, "height"); *out = js_num(v ? atoi(v) : 0); return 1; }
        break;
    case 'i':
        if (!strcmp(k, "innerHTML")) { html_serialize(&b, n, 0); *out = js_strv(jb_str(&b)); return 1; }
        if (!strcmp(k, "innerText")) { dn_text_content(n, &b, 1); *out = js_strv(jb_str(&b)); return 1; }
        if (!strcmp(k, "isConnected")) { *out = js_bool(dn_connected(n)); return 1; }
        break;
    case 'l':
        if (!strcmp(k, "lastChild")) { *out = dn_wrap(n->last); return 1; }
        if (!strcmp(k, "lastElementChild")) { *out = dn_wrap(elem_child(n, 1)); return 1; }
        if (!strcmp(k, "localName") && n->type == DN_ELEMENT) { *out = js_cstr(n->tag); return 1; }
        if (!strcmp(k, "length") && n->type == DN_ELEMENT && (!strcmp(n->tag, "select") || !strcmp(n->tag, "form"))) {
            dnode **v;
            int c;
            if (!strcmp(n->tag, "select")) collect(n, pred_tag, "option", &v, &c); else collect(n, pred_form_field, NULL, &v, &c);
            free(v);
            *out = js_num(c);
            return 1;
        }
        if (!strcmp(k, "length") && (n->type == DN_TEXT || n->type == DN_COMMENT)) { *out = js_num(n->tlen); return 1; }
        break;
    case 'n':
        if (!strcmp(k, "nodeType")) { *out = js_num(n->type); return 1; }
        if (!strcmp(k, "nodeName")) {
            if (n->type == DN_ELEMENT) { int i = 0; for (; n->tag[i] && i < 63; i++) tmp[i] = toupper((ju8)n->tag[i]); tmp[i] = 0; *out = js_cstr(tmp); }
            else *out = js_cstr(n->type == DN_TEXT ? "#text" : n->type == DN_COMMENT ? "#comment" : n->type == DN_DOCUMENT ? "#document" : "#document-fragment");
            return 1;
        }
        if (!strcmp(k, "nodeValue")) { *out = (n->type == DN_TEXT || n->type == DN_COMMENT) ? js_strv(js_strn(n->text, n->tlen)) : js_null(); return 1; }
        if (!strcmp(k, "nextSibling")) { *out = dn_wrap(n->next); return 1; }
        if (!strcmp(k, "nextElementSibling")) { *out = dn_wrap(elem_sibling(n, 1)); return 1; }
        break;
    case 'o':
        if (!strcmp(k, "outerHTML")) { html_serialize(&b, n, 1); *out = js_strv(jb_str(&b)); return 1; }
        if (!strcmp(k, "ownerDocument")) { *out = n->type == DN_DOCUMENT ? js_null() : dn_wrap(d_document); return 1; }
        if (!strcmp(k, "offsetParent")) { *out = n->parent ? dn_wrap(n->parent) : js_null(); return 1; }
        if (!strncmp(k, "offset", 6) && n->type == DN_ELEMENT) {
            int hidden = !dn_visible(n);
            *out = js_num(hidden ? 0 : !strcmp(k, "offsetWidth") ? 760 : !strcmp(k, "offsetHeight") ? 20 : 0);
            return 1;
        }
        if (!strcmp(k, "options") && n->type == DN_ELEMENT) { *out = js_objv(tag_list(n, "option")); return 1; }
        break;
    case 'p':
        if (!strcmp(k, "parentNode")) { *out = dn_wrap(n->parent); return 1; }
        if (!strcmp(k, "parentElement")) { *out = n->parent && n->parent->type == DN_ELEMENT ? dn_wrap(n->parent) : js_null(); return 1; }
        if (!strcmp(k, "previousSibling")) { *out = dn_wrap(n->prev); return 1; }
        if (!strcmp(k, "previousElementSibling")) { *out = dn_wrap(elem_sibling(n, 0)); return 1; }
        if ((!strcmp(k, "pathname") || !strcmp(k, "protocol") || !strcmp(k, "port")) && n->type == DN_ELEMENT && !strcmp(n->tag, "a")) {
            const char *v = dn_get_attr(n, "href");
            char full[1024];
            dn_resolve(d_url, v ? v : "", full, sizeof(full));
            url_part(full, k, out);
            return 1;
        }
        break;
    case 's':
        if (!strcmp(k, "style") && n->type == DN_ELEMENT) { *out = sub_object(o, "%style", &style_class, style_proto, n); return 1; }
        if (!strcmp(k, "src") && n->type == DN_ELEMENT) {
            const char *v = dn_get_attr(n, "src");
            char full[1024];
            if (v) dn_resolve(d_url, v, full, sizeof(full)); else full[0] = 0;
            *out = js_cstr(full);
            return 1;
        }
        if (!strcmp(k, "selected")) { *out = js_bool(dn_get_attr(n, "selected") != NULL); return 1; }
        if (!strcmp(k, "selectedIndex") && n->type == DN_ELEMENT) {
            dnode **v;
            int c, r = c = 0;
            collect(n, pred_tag, "option", &v, &c);
            r = c ? 0 : -1;
            for (int i = 0; i < c; i++) if (dn_get_attr(v[i], "selected")) r = i;
            free(v);
            *out = js_num(r);
            return 1;
        }
        if (!strncmp(k, "scroll", 6) && n->type == DN_ELEMENT && (!strcmp(k + 6, "Top") || !strcmp(k + 6, "Left") || !strcmp(k + 6, "Width") || !strcmp(k + 6, "Height"))) {
            *out = js_num(k[6] == 'W' ? 760 : k[6] == 'H' ? 400 : 0);
            return 1;
        }
        if (!strcmp(k, "search") && n->type == DN_ELEMENT && !strcmp(n->tag, "a")) {
            const char *v = dn_get_attr(n, "href");
            char full[1024];
            dn_resolve(d_url, v ? v : "", full, sizeof(full));
            url_part(full, k, out);
            return 1;
        }
        break;
    case 't':
        if (!strcmp(k, "tagName") && n->type == DN_ELEMENT) { int i = 0; for (; n->tag[i] && i < 63; i++) tmp[i] = toupper((ju8)n->tag[i]); tmp[i] = 0; *out = js_cstr(tmp); return 1; }
        if (!strcmp(k, "textContent")) {
            if (n->type == DN_DOCUMENT) { *out = js_null(); return 1; }
            dn_text_content(n, &b, 0);
            *out = js_strv(jb_str(&b));
            return 1;
        }
        if (!strcmp(k, "text") && n->type == DN_ELEMENT && (!strcmp(n->tag, "script") || !strcmp(n->tag, "a") || !strcmp(n->tag, "option") || !strcmp(n->tag, "title"))) {
            dn_text_content(n, &b, 0);
            *out = js_strv(jb_str(&b));
            return 1;
        }
        if (!strcmp(k, "type") && n->type == DN_ELEMENT && !strcmp(n->tag, "input")) { const char *v = dn_get_attr(n, "type"); *out = js_cstr(v ? v : "text"); return 1; }
        if (!strcmp(k, "tabIndex")) { const char *v = dn_get_attr(n, "tabindex"); *out = js_num(v ? atoi(v) : -1); return 1; }
        break;
    case 'v':
        if (!strcmp(k, "value") && n->type == DN_ELEMENT) {
            if (!strcmp(n->tag, "textarea") && !dn_get_attr(n, "value")) { dn_text_content(n, &b, 0); *out = js_strv(jb_str(&b)); return 1; }
            if (!strcmp(n->tag, "select")) {
                dnode **v;
                int c;
                collect(n, pred_tag, "option", &v, &c);
                dnode *sel = c ? v[0] : NULL;
                for (int i = 0; i < c; i++) if (dn_get_attr(v[i], "selected")) sel = v[i];
                free(v);
                if (!sel) { *out = js_cstr(""); return 1; }
                n = sel;
            }
            const char *v = dn_get_attr(n, "value");
            if (!v && !strcmp(n->tag, "option")) { dn_text_content(n, &b, 0); *out = js_strv(jb_str(&b)); return 1; }
            *out = js_cstr(v ? v : (!strcmp(n->tag, "input") && dn_get_attr(n, "type") && !strcmp(dn_get_attr(n, "type"), "checkbox")) ? "on" : "");
            return 1;
        }
        break;
    case 'w':
        if (!strcmp(k, "width") && n->type == DN_ELEMENT) { const char *v = dn_get_attr(n, "width"); *out = js_num(v ? atoi(v) : 0); return 1; }
        break;
    }
    if (n->type == DN_ELEMENT) {
        for (int i = 0; attr_props[i]; i++)
            if (!strcmp(k, attr_props[i])) { const char *v = dn_get_attr(n, prop_to_attr(k, tmp)); *out = js_cstr(v ? v : ""); return 1; }
        if (!strcmp(k, "htmlFor")) { const char *v = dn_get_attr(n, "for"); *out = js_cstr(v ? v : ""); return 1; }
        if (is_bool_attr(k)) { *out = js_bool(dn_get_attr(n, prop_to_attr(k, tmp)) != NULL); return 1; }
    }
    return 0;
}

static void set_inner_html(dnode *n, jsstr *s) {
    while (n->first) dn_remove(n->first);
    html_parse_into(n, s->s, s->len);
}

static void navigate_to(const char *u) {
    dn_resolve(d_url, u, pending_nav, sizeof(pending_nav));
    nav_requested = 1;
}

static int doc_set(dnode *n, const char *k, jsval v);

static int node_set(jsobj *o, jsstr *key, jsval v) {
    dnode *n = o->ptr;
    const char *k = key->s;
    if (key->symbol || key->len > 40 || key->idx >= 0) return 0;
    char tmp[64];
    if (n->type == DN_DOCUMENT) { int r = doc_set(n, k, v); if (r) return r; }
    if (!strcmp(k, "innerHTML") || !strcmp(k, "outerHTML")) {
        jsstr *s;
        TRY(js_to_string(v, &s));
        if (k[0] == 'i') set_inner_html(n, s);
        else if (n->parent) {
            dnode *frag = dn_new(DN_FRAGMENT, NULL);
            html_parse_into(frag, s->s, s->len);
            dnode *p = n->parent;
            dn_insert_before(p, frag, n);
            dn_remove(n);
        }
        return 1;
    }
    if (!strcmp(k, "textContent") || !strcmp(k, "innerText") || !strcmp(k, "nodeValue") || !strcmp(k, "data") || (!strcmp(k, "text") && n->type == DN_ELEMENT && !strcmp(n->tag, "script"))) {
        if (n->type == DN_DOCUMENT) return 1;
        jsstr *s = js_intern("", 0);
        if (!IS_NULLISH(v)) TRY(js_to_string(v, &s));
        if ((k[0] == 'n' || k[0] == 'd') && n->type == DN_ELEMENT) return 1;
        dn_set_text(n, s->s, s->len);
        return 1;
    }
    if (n->type != DN_ELEMENT) return 0;
    if (!strcmp(k, "value")) {
        jsstr *s;
        TRY(js_to_string(v, &s));
        if (!strcmp(n->tag, "select")) {
            dnode **opts;
            int c;
            collect(n, pred_tag, "option", &opts, &c);
            for (int i = 0; i < c; i++) {
                const char *ov = dn_get_attr(opts[i], "value");
                if (ov && !strcmp(ov, s->s)) dn_set_attr(opts[i], "selected", ""); else dn_del_attr(opts[i], "selected");
            }
            free(opts);
            return 1;
        }
        dn_set_attr(n, "value", s->s);
        return 1;
    }
    if (!strcmp(k, "style")) {
        jsstr *s;
        TRY(js_to_string(v, &s));
        dn_set_attr(n, "style", s->s);
        return 1;
    }
    if (!strcmp(k, "className")) { jsstr *s; TRY(js_to_string(v, &s)); dn_set_attr(n, "class", s->s); return 1; }
    if (!strcmp(k, "htmlFor")) { jsstr *s; TRY(js_to_string(v, &s)); dn_set_attr(n, "for", s->s); return 1; }
    if (!strcmp(k, "href") || !strcmp(k, "src") || !strcmp(k, "action")) {
        jsstr *s;
        TRY(js_to_string(v, &s));
        dn_set_attr(n, k, s->s);
        if (!strcmp(k, "src") && !strcmp(n->tag, "script") && dn_connected(n) && !n->script_done) domjs_script_inserted(n);
        return 1;
    }
    if (!strcmp(k, "selectedIndex")) {
        double d = js_num_nothrow(v);
        dnode **opts;
        int c;
        collect(n, pred_tag, "option", &opts, &c);
        for (int i = 0; i < c; i++) { if (i == (int)d) dn_set_attr(opts[i], "selected", ""); else dn_del_attr(opts[i], "selected"); }
        free(opts);
        return 1;
    }
    if (is_bool_attr(k)) {
        if (js_truthy(v)) dn_set_attr(n, prop_to_attr(k, tmp), "");
        else dn_del_attr(n, prop_to_attr(k, tmp));
        return 1;
    }
    for (int i = 0; attr_props[i]; i++)
        if (!strcmp(k, attr_props[i])) {
            jsstr *s;
            TRY(js_to_string(v, &s));
            dn_set_attr(n, prop_to_attr(k, tmp), s->s);
            return 1;
        }
    if (!strcmp(k, "width") || !strcmp(k, "height") || !strcmp(k, "tabIndex")) {
        jsstr *s;
        TRY(js_to_string(v, &s));
        dn_set_attr(n, prop_to_attr(k, tmp), s->s);
        return 1;
    }
    return 0;
}

/* --- document */

static const char *host_of_url(char *buf) {
    char proto[16], path[1024];
    url_split(d_url, proto, buf, path);
    char *c = strchr(buf, ':');
    if (c) *c = 0;
    return buf;
}

static int pred_has_attr(dnode *n, const void *a) { return dn_get_attr(n, (const char *)a) != NULL; }
static int pred_link(dnode *n, const void *a) { (void)a; return (!strcmp(n->tag, "a") || !strcmp(n->tag, "area")) && dn_get_attr(n, "href"); }

static int is_doc_prop(const char *k, dnode *n, jsval *out) {
    char buf[4096];
    if (!strcmp(k, "documentElement")) { *out = dn_wrap(dn_find_tag(n, "html")); return 1; }
    if (!strcmp(k, "body")) { *out = dn_wrap(dn_find_tag(n, "body")); return 1; }
    if (!strcmp(k, "head")) { *out = dn_wrap(dn_find_tag(n, "head")); return 1; }
    if (n != d_document) {                                  /* документ из createHTMLDocument */
        if (!strcmp(k, "location") || !strcmp(k, "defaultView")) { *out = js_null(); return 1; }
        if (!strcmp(k, "implementation") || !strcmp(k, "cookie")) return 0;
    }
    if (!strcmp(k, "title")) { dom_title(buf, sizeof(buf)); *out = js_cstr(buf); return 1; }
    if (!strcmp(k, "cookie")) { char h[256]; cookie_get(host_of_url(h), buf, sizeof(buf)); *out = js_cstr(buf); return 1; }
    if (!strcmp(k, "readyState")) { *out = js_cstr(d_parsing ? "loading" : "complete"); return 1; }
    if (!strcmp(k, "URL") || !strcmp(k, "documentURI") || !strcmp(k, "baseURI")) { *out = js_cstr(d_url); return 1; }
    if (!strcmp(k, "location")) { *out = js_objv(loc_obj); return 1; }
    if (!strcmp(k, "domain")) { char h[256]; *out = js_cstr(host_of_url(h)); return 1; }
    if (!strcmp(k, "referrer") || !strcmp(k, "lastModified")) { *out = js_cstr(""); return 1; }
    if (!strcmp(k, "forms")) { *out = js_objv(tag_list(d_document, "form")); return 1; }
    if (!strcmp(k, "images")) { *out = js_objv(tag_list(d_document, "img")); return 1; }
    if (!strcmp(k, "scripts")) { *out = js_objv(tag_list(d_document, "script")); return 1; }
    if (!strcmp(k, "all")) { *out = js_objv(tag_list(d_document, "*")); return 1; }
    if (!strcmp(k, "links")) { dnode **v; int c; collect(d_document, pred_link, NULL, &v, &c); *out = js_objv(node_list(v, c)); free(v); return 1; }
    if (!strcmp(k, "activeElement")) { *out = dn_wrap(dn_find_tag(d_document, "body")); return 1; }
    if (!strcmp(k, "defaultView")) { *out = js_objv(js_global); return 1; }
    if (!strcmp(k, "characterSet") || !strcmp(k, "charset") || !strcmp(k, "inputEncoding")) { *out = js_cstr("UTF-8"); return 1; }
    if (!strcmp(k, "compatMode")) { *out = js_cstr("CSS1Compat"); return 1; }
    if (!strcmp(k, "visibilityState")) { *out = js_cstr("visible"); return 1; }
    if (!strcmp(k, "hidden")) { *out = js_bool(0); return 1; }
    if (!strcmp(k, "currentScript")) { *out = dn_wrap(current_script); return 1; }
    if (!strcmp(k, "styleSheets")) { *out = js_objv(js_new_array(0)); return 1; }
    if (!strcmp(k, "contentType")) { *out = js_cstr("text/html"); return 1; }
    if (!strcmp(k, "designMode")) { *out = js_cstr("off"); return 1; }
    if (!strcmp(k, "scrollingElement")) { *out = dn_wrap(dn_find_tag(d_document, "html")); return 1; }
    (void)pred_has_attr;
    return 0;
}

static int doc_set(dnode *n, const char *k, jsval v) {
    (void)n;
    if (!strcmp(k, "title")) {
        jsstr *s;
        TRY(js_to_string(v, &s));
        dnode *t = dn_find_tag(d_document, "title");
        if (!t) { dnode *h = dn_find_tag(d_document, "head"); if (!h) return 1; t = dn_new(DN_ELEMENT, "title"); dn_append(h, t); }
        dn_set_text(t, s->s, s->len);
        return 1;
    }
    if (!strcmp(k, "cookie")) {
        jsstr *s;
        TRY(js_to_string(v, &s));
        char h[256];
        cookie_set(host_of_url(h), s->s);
        return 1;
    }
    if (!strcmp(k, "location")) { jsstr *s; TRY(js_to_string(v, &s)); navigate_to(s->s); return 1; }
    if (!strcmp(k, "body") || !strcmp(k, "readyState") || !strcmp(k, "URL")) return 1;
    return 0;
}

/* --- стиль */

static void camel_to_kebab(const char *p, char *out) {
    int o = 0;
    if (!strncmp(p, "webkit", 6) || !strncmp(p, "Webkit", 6) || !strncmp(p, "moz", 3) || !strncmp(p, "ms", 2)) out[o++] = '-';
    for (; *p && o < 60; p++) {
        if (isupper((ju8)*p)) { out[o++] = '-'; out[o++] = tolower((ju8)*p); }
        else out[o++] = *p;
    }
    out[o] = 0;
    if (!strcmp(out, "css-float")) strcpy(out, "float");
}

/* найти объявление prop в style="..." -> начало значения и длину */
int strncasecmp_(const char *a, const char *b, int n);
static int style_find(const char *st, const char *prop, int *vs, int *ve, int *ds, int *de) {
    int pl = strlen(prop);
    const char *p = st;
    while (*p) {
        while (*p == ' ' || *p == ';' || *p == '\n' || *p == '\t') p++;
        if (!*p) break;
        const char *ns = p;
        while (*p && *p != ':' && *p != ';') p++;
        const char *ne = p;
        while (ne > ns && ne[-1] == ' ') ne--;
        if (*p != ':') continue;
        p++;
        while (*p == ' ') p++;
        const char *v0 = p;
        while (*p && *p != ';') p++;
        const char *v1 = p;
        while (v1 > v0 && v1[-1] == ' ') v1--;
        if (ne - ns == pl && !strncasecmp_(ns, prop, pl)) {
            *vs = v0 - st; *ve = v1 - st; *ds = ns - st; *de = (*p == ';' ? p + 1 : p) - st;
            return 1;
        }
    }
    return 0;
}

int strncasecmp_(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) { int x = tolower((ju8)a[i]), y = tolower((ju8)b[i]); if (x != y) return x - y; if (!x) return 0; }
    return 0;
}

static int style_get(jsobj *o, jsstr *key, jsval *out) {
    dnode *n = o->ptr;
    if (key->symbol || key->idx >= 0) return 0;
    const char *st = dn_get_attr(n, "style");
    if (!strcmp(key->s, "cssText")) { *out = js_cstr(st ? st : ""); return 1; }
    if (!strcmp(key->s, "length")) { int c = 0; for (const char *p = st ? st : ""; *p; p++) c += *p == ':'; *out = js_num(c); return 1; }
    if (!isalpha((ju8)key->s[0]) || key->len > 40) return 0;
    struct prop *pr = js_own_prop(style_proto, key);
    if (pr) return 0;                                         /* метод getPropertyValue и т. п. */
    char kebab[64];
    camel_to_kebab(key->s, kebab);
    int vs, ve, ds, de;
    if (st && style_find(st, kebab, &vs, &ve, &ds, &de)) *out = js_strv(js_strn(st + vs, ve - vs));
    else *out = js_cstr("");
    return 1;
}

static void style_put(dnode *n, const char *kebab, const char *val) {
    const char *st = dn_get_attr(n, "style");
    jsbuf b = { 0 };
    int vs, ve, ds, de;
    if (st && style_find(st, kebab, &vs, &ve, &ds, &de)) {
        jb_add(&b, st, ds);
        if (val && *val) { jb_adds(&b, kebab); jb_adds(&b, ": "); jb_adds(&b, val); jb_adds(&b, "; "); }
        jb_adds(&b, st + de);
    } else {
        if (st) { jb_adds(&b, st); if (b.n && b.p[b.n - 1] != ';' && b.p[b.n - 1] != ' ') jb_adds(&b, "; "); }
        if (val && *val) { jb_adds(&b, kebab); jb_adds(&b, ": "); jb_adds(&b, val); jb_addc(&b, ';'); }
    }
    jb_addc(&b, 0);
    dn_set_attr(n, "style", b.p);
    jb_free(&b);
}

static int style_set(jsobj *o, jsstr *key, jsval v) {
    dnode *n = o->ptr;
    if (key->symbol || key->idx >= 0 || key->len > 40) return 0;
    jsstr *s = js_intern("", 0);
    if (!IS_NULLISH(v)) TRY(js_to_string(v, &s));
    if (!strcmp(key->s, "cssText")) { dn_set_attr(n, "style", s->s); return 1; }
    char kebab[64];
    camel_to_kebab(key->s, kebab);
    style_put(n, kebab, s->s);
    return 1;
}

NATIVE(Style_getPropertyValue) {
    UNUSED;
    if (!IS_OBJ(thisv) || thisv.u.o->hc != &style_class) { *ret = js_cstr(""); return 0; }
    jsstr *p;
    TRY(js_to_string(ARG(0), &p));
    const char *st = dn_get_attr(thisv.u.o->ptr, "style");
    int vs, ve, ds, de;
    if (st && style_find(st, p->s, &vs, &ve, &ds, &de)) *ret = js_strv(js_strn(st + vs, ve - vs));
    else *ret = js_cstr("");
    return 0;
}

NATIVE(Style_setProperty) {
    UNUSED;
    if (!IS_OBJ(thisv) || thisv.u.o->hc != &style_class) return 0;
    jsstr *p, *v = js_intern("", 0);
    TRY(js_to_string(ARG(0), &p));
    if (!IS_NULLISH(ARG(1))) TRY(js_to_string(argv[1], &v));
    style_put(thisv.u.o->ptr, p->s, v->s);
    *ret = js_undef();
    return 0;
}

NATIVE(Style_removeProperty) {
    UNUSED;
    if (!IS_OBJ(thisv) || thisv.u.o->hc != &style_class) return 0;
    jsstr *p;
    TRY(js_to_string(ARG(0), &p));
    style_put(thisv.u.o->ptr, p->s, "");
    *ret = js_cstr("");
    return 0;
}

/* --- classList */

static int classlist_get(jsobj *o, jsstr *key, jsval *out) {
    dnode *n = o->ptr;
    const char *c = dn_get_attr(n, "class");
    if (key == S_length || key->idx >= 0) {
        int cnt = 0;
        const char *p = c ? c : "";
        while (*p) {
            while (*p && isspace((ju8)*p)) p++;
            const char *st = p;
            while (*p && !isspace((ju8)*p)) p++;
            if (p > st) {
                if (key->idx == cnt) { *out = js_strv(js_strn(st, p - st)); return 1; }
                cnt++;
            }
        }
        if (key->idx >= 0) { *out = js_undef(); return 1; }
        *out = js_num(cnt);
        return 1;
    }
    if (!strcmp(key->s, "value")) { *out = js_cstr(c ? c : ""); return 1; }
    return 0;
}

static dnode *cl_node(jsval t) { return IS_OBJ(t) && t.u.o->hc == &classlist_class ? t.u.o->ptr : NULL; }

static void cl_remove(dnode *n, const char *cls, int l) {
    const char *c = dn_get_attr(n, "class");
    if (!c) return;
    jsbuf b = { 0 };
    const char *p = c;
    while (*p) {
        while (*p && isspace((ju8)*p)) p++;
        const char *st = p;
        while (*p && !isspace((ju8)*p)) p++;
        if (p > st && !(p - st == l && !strncmp(st, cls, l))) { if (b.n) jb_addc(&b, ' '); jb_add(&b, st, p - st); }
    }
    jb_addc(&b, 0);
    dn_set_attr(n, "class", b.p);
    jb_free(&b);
}

static void cl_add(dnode *n, const char *cls, int l) {
    if (dn_has_class(n, cls, l)) return;
    const char *c = dn_get_attr(n, "class");
    jsbuf b = { 0 };
    if (c && *c) { jb_adds(&b, c); jb_addc(&b, ' '); }
    jb_add(&b, cls, l);
    jb_addc(&b, 0);
    dn_set_attr(n, "class", b.p);
    jb_free(&b);
}

NATIVE(ClassList_add) {
    UNUSED;
    dnode *n = cl_node(thisv);
    if (!n) return 0;
    for (int i = 0; i < argc; i++) { jsstr *s; TRY(js_to_string(argv[i], &s)); cl_add(n, s->s, s->len); }
    *ret = js_undef();
    return 0;
}
NATIVE(ClassList_remove) {
    UNUSED;
    dnode *n = cl_node(thisv);
    if (!n) return 0;
    for (int i = 0; i < argc; i++) { jsstr *s; TRY(js_to_string(argv[i], &s)); cl_remove(n, s->s, s->len); }
    *ret = js_undef();
    return 0;
}
NATIVE(ClassList_contains) {
    UNUSED;
    dnode *n = cl_node(thisv);
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    *ret = js_bool(n && dn_has_class(n, s->s, s->len));
    return 0;
}
NATIVE(ClassList_toggle) {
    UNUSED;
    dnode *n = cl_node(thisv);
    if (!n) return 0;
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    int has = dn_has_class(n, s->s, s->len), want = argc > 1 && argv[1].tag != JS_UNDEF ? js_truthy(argv[1]) : !has;
    if (want && !has) cl_add(n, s->s, s->len);
    if (!want && has) cl_remove(n, s->s, s->len);
    *ret = js_bool(want);
    return 0;
}
NATIVE(ClassList_replace) {
    UNUSED;
    dnode *n = cl_node(thisv);
    jsstr *a, *b;
    TRY(js_to_string(ARG(0), &a));
    TRY(js_to_string(ARG(1), &b));
    int has = n && dn_has_class(n, a->s, a->len);
    if (has) { cl_remove(n, a->s, a->len); cl_add(n, b->s, b->len); }
    *ret = js_bool(has);
    return 0;
}
NATIVE(ClassList_item) {
    UNUSED;
    jsval r;
    char b[16];
    int idx = (int)js_num_nothrow(ARG(0));
    snprintf(b, sizeof(b), "%d", idx < 0 ? 999999 : idx);
    TRY(js_get(thisv, js_intern(b, strlen(b)), &r));
    *ret = r.tag == JS_UNDEF ? js_null() : r;
    return 0;
}
NATIVE(ClassList_toString) { UNUSED; dnode *n = cl_node(thisv); const char *c = n ? dn_get_attr(n, "class") : NULL; *ret = js_cstr(c ? c : ""); return 0; }

/* --- dataset: data-* атрибуты */

static void dataset_attr(const char *k, char *out) {
    int o = snprintf(out, 8, "data-");
    for (; *k && o < 60; k++) {
        if (isupper((ju8)*k)) { out[o++] = '-'; out[o++] = tolower((ju8)*k); }
        else out[o++] = *k;
    }
    out[o] = 0;
}
static int dataset_get(jsobj *o, jsstr *key, jsval *out) {
    if (key->symbol || key->len > 50) return 0;
    char a[72];
    dataset_attr(key->s, a);
    const char *v = dn_get_attr(o->ptr, a);
    if (!v) return 0;
    *out = js_cstr(v);
    return 1;
}
static int dataset_set(jsobj *o, jsstr *key, jsval v) {
    if (key->symbol || key->len > 50) return 0;
    char a[72];
    dataset_attr(key->s, a);
    jsstr *s;
    TRY(js_to_string(v, &s));
    dn_set_attr(o->ptr, a, s->s);
    return 1;
}

/* ------------------------------------------------------------------ методы узлов */

static int need_node(jsval v, dnode **n) {
    *n = dn_of(v);
    if (!*n || (*n)->type == DN_WINDOW) return js_throw("TypeError", "Illegal invocation");
    return 0;
}

static int arg_node(jsval v, dnode **n) {
    *n = dn_of(v);
    if (!*n || (*n)->type == DN_WINDOW) return js_throw("TypeError", "parameter is not of type 'Node'");
    return 0;
}

/* после вставки: скрипты, которые теперь в документе, — выполнить */
static void after_insert(dnode *c) {
    if (!dn_connected(c)) return;
    dnode *stack[256];
    int sp = 0;
    stack[sp++] = c;
    while (sp) {
        dnode *n = stack[--sp];
        if (n->type == DN_ELEMENT && !strcmp(n->tag, "script") && !n->script_done) domjs_script_inserted(n);
        for (dnode *k = n->last; k; k = k->prev) if (sp < 256) stack[sp++] = k;
    }
}

NATIVE(Node_appendChild) {
    UNUSED;
    dnode *p, *c;
    TRY(need_node(thisv, &p));
    TRY(arg_node(ARG(0), &c));
    if (c->type == DN_FRAGMENT) {
        dnode *first = c->first;
        dn_append(p, c);
        for (dnode *k = first; k; k = k->next) after_insert(k);
    } else { dn_append(p, c); after_insert(c); }
    *ret = argv[0];
    return 0;
}

NATIVE(Node_insertBefore) {
    UNUSED;
    dnode *p, *c, *ref = dn_of(ARG(1));
    TRY(need_node(thisv, &p));
    TRY(arg_node(ARG(0), &c));
    if (ref && ref->parent != p) return js_throw("NotFoundError", "The node before which the new node is to be inserted is not a child of this node.");
    dnode *first = c->type == DN_FRAGMENT ? c->first : c;
    dn_insert_before(p, c, ref);
    for (dnode *k = first; k && k != ref; k = k->next) { after_insert(k); if (first == c) break; }
    *ret = argv[0];
    return 0;
}

NATIVE(Node_removeChild) {
    UNUSED;
    dnode *p, *c;
    TRY(need_node(thisv, &p));
    TRY(arg_node(ARG(0), &c));
    if (c->parent != p) return js_throw("NotFoundError", "The node to be removed is not a child of this node.");
    dn_remove(c);
    *ret = argv[0];
    return 0;
}

NATIVE(Node_replaceChild) {
    UNUSED;
    dnode *p, *nw, *old;
    TRY(need_node(thisv, &p));
    TRY(arg_node(ARG(0), &nw));
    TRY(arg_node(ARG(1), &old));
    if (old->parent != p) return js_throw("NotFoundError", "The node to be replaced is not a child of this node.");
    dn_insert_before(p, nw, old);
    dn_remove(old);
    after_insert(nw);
    *ret = argv[1];
    return 0;
}

NATIVE(Node_cloneNode) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    *ret = dn_wrap(dn_clone(n, js_truthy(ARG(0))));
    return 0;
}

NATIVE(Node_contains) {
    UNUSED;
    dnode *n, *c = dn_of(ARG(0));
    TRY(need_node(thisv, &n));
    int r = 0;
    for (; c; c = c->parent) if (c == n) { r = 1; break; }
    *ret = js_bool(r);
    return 0;
}

NATIVE(Node_hasChildNodes) { UNUSED; dnode *n; TRY(need_node(thisv, &n)); *ret = js_bool(n->first != NULL); return 0; }
NATIVE(Node_getRootNode) { UNUSED; dnode *n; TRY(need_node(thisv, &n)); while (n->parent) n = n->parent; *ret = dn_wrap(n); return 0; }
NATIVE(Node_isSameNode) { UNUSED; *ret = js_bool(dn_of(thisv) == dn_of(ARG(0))); return 0; }
NATIVE(Node_normalize) { UNUSED; *ret = js_undef(); return 0; }
NATIVE(Node_compareDocumentPosition) {
    UNUSED;
    dnode *a, *b = dn_of(ARG(0));
    TRY(need_node(thisv, &a));
    if (!b || a == b) { *ret = js_num(0); return 0; }
    for (dnode *p = b->parent; p; p = p->parent) if (p == a) { *ret = js_num(20); return 0; }   /* CONTAINED_BY|FOLLOWING */
    for (dnode *p = a->parent; p; p = p->parent) if (p == b) { *ret = js_num(10); return 0; }   /* CONTAINS|PRECEDING */
    *ret = js_num(a->id < b->id ? 4 : 2);
    return 0;
}

NATIVE(El_getAttribute) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *k;
    TRY(js_to_string(ARG(0), &k));
    char low[128];
    snprintf(low, sizeof(low), "%s", k->s);
    for (char *p = low; *p; p++) *p = tolower((ju8)*p);
    *ret = cstr_or_null(dn_get_attr(n, low));
    return 0;
}

NATIVE(El_setAttribute) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *k, *v;
    TRY(js_to_string(ARG(0), &k));
    TRY(js_to_string(ARG(1), &v));
    dn_set_attr(n, k->s, v->s);
    if (!strcmp(n->tag, "script") && !strcmp(k->s, "src") && dn_connected(n) && !n->script_done) domjs_script_inserted(n);
    *ret = js_undef();
    return 0;
}

NATIVE(El_removeAttribute) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *k;
    TRY(js_to_string(ARG(0), &k));
    char low[128];
    snprintf(low, sizeof(low), "%s", k->s);
    for (char *p = low; *p; p++) *p = tolower((ju8)*p);
    dn_del_attr(n, low);
    *ret = js_undef();
    return 0;
}

NATIVE(El_hasAttribute) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *k;
    TRY(js_to_string(ARG(0), &k));
    char low[128];
    snprintf(low, sizeof(low), "%s", k->s);
    for (char *p = low; *p; p++) *p = tolower((ju8)*p);
    *ret = js_bool(dn_get_attr(n, low) != NULL);
    return 0;
}

NATIVE(El_toggleAttribute) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *k;
    TRY(js_to_string(ARG(0), &k));
    int has = dn_get_attr(n, k->s) != NULL, want = argc > 1 ? js_truthy(argv[1]) : !has;
    if (want && !has) dn_set_attr(n, k->s, "");
    if (!want && has) dn_del_attr(n, k->s);
    *ret = js_bool(want);
    return 0;
}

NATIVE(El_hasAttributes) { UNUSED; dnode *n; TRY(need_node(thisv, &n)); *ret = js_bool(n->nattr > 0); return 0; }
NATIVE(El_getAttributeNames) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsobj *a = js_new_array(0);
    for (int i = 0; i < n->nattr; i++) js_array_push(a, js_cstr(n->attr[2 * i]));
    *ret = js_objv(a);
    return 0;
}

static int query(jsval thisv, jsval sel, int first, jsval *ret) {
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *s;
    TRY(js_to_string(sel, &s));
    struct selector *se = sel_parse(s->s);
    if (!se) return js_throw("SyntaxError", "'%.80s' is not a valid selector", s->s);
    dnode **v;
    int c;
    sel_query(n, se, first, &v, &c);
    sel_free(se);
    if (first) *ret = c ? dn_wrap(v[0]) : js_null();
    else *ret = js_objv(node_list(v, c));
    free(v);
    return 0;
}

NATIVE(El_querySelector) { UNUSED; return query(thisv, ARG(0), 1, ret); }
NATIVE(El_querySelectorAll) { UNUSED; return query(thisv, ARG(0), 0, ret); }

NATIVE(El_getElementsByTagName) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *t;
    TRY(js_to_string(ARG(0), &t));
    char low[64];
    snprintf(low, sizeof(low), "%s", t->s);
    for (char *p = low; *p; p++) *p = tolower((ju8)*p);
    *ret = js_objv(tag_list(n, low));
    return 0;
}

static int pred_classes(dnode *n, const void *arg) {
    const char *p = arg;
    while (*p) {
        while (*p == ' ') p++;
        const char *st = p;
        while (*p && *p != ' ') p++;
        if (p > st && !dn_has_class(n, st, p - st)) return 0;
    }
    return 1;
}

NATIVE(El_getElementsByClassName) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *t;
    TRY(js_to_string(ARG(0), &t));
    dnode **v;
    int c;
    collect(n, pred_classes, t->s, &v, &c);
    *ret = js_objv(node_list(v, c));
    free(v);
    return 0;
}

static int pred_name(dnode *n, const void *arg) { const char *v = dn_get_attr(n, "name"); return v && !strcmp(v, (const char *)arg); }
NATIVE(Doc_getElementsByName) {
    UNUSED;
    jsstr *t;
    TRY(js_to_string(ARG(0), &t));
    dnode **v;
    int c;
    collect(d_document, pred_name, t->s, &v, &c);
    *ret = js_objv(node_list(v, c));
    free(v);
    return 0;
}

NATIVE(El_closest) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    struct selector *se = sel_parse(s->s);
    if (!se) return js_throw("SyntaxError", "'%.80s' is not a valid selector", s->s);
    for (; n && n->type == DN_ELEMENT; n = n->parent) if (sel_match(se, n)) break;
    sel_free(se);
    *ret = n && n->type == DN_ELEMENT ? dn_wrap(n) : js_null();
    return 0;
}

NATIVE(El_matches) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    struct selector *se = sel_parse(s->s);
    if (!se) return js_throw("SyntaxError", "'%.80s' is not a valid selector", s->s);
    *ret = js_bool(sel_match(se, n));
    sel_free(se);
    return 0;
}

NATIVE(El_remove) { UNUSED; dnode *n; TRY(need_node(thisv, &n)); dn_remove(n); *ret = js_undef(); return 0; }

/* append/prepend/before/after: узлы или строки */
static int node_args(int argc, jsval *argv, dnode **out) {
    dnode *frag = dn_new(DN_FRAGMENT, NULL);
    for (int i = 0; i < argc; i++) {
        dnode *c = dn_of(argv[i]);
        if (c && c->type != DN_WINDOW) dn_append(frag, c);
        else { jsstr *s; TRY(js_to_string(argv[i], &s)); dn_append(frag, dn_text(s->s, s->len)); }
    }
    *out = frag;
    return 0;
}

static void insert_frag(dnode *p, dnode *frag, dnode *ref) {
    dnode *kids[256];
    int nk = 0;
    for (dnode *k = frag->first; k && nk < 256; k = k->next) kids[nk++] = k;
    dn_insert_before(p, frag, ref);
    for (int i = 0; i < nk; i++) after_insert(kids[i]);
}

NATIVE(El_append) { UNUSED; dnode *n, *f; TRY(need_node(thisv, &n)); TRY(node_args(argc, argv, &f)); insert_frag(n, f, NULL); *ret = js_undef(); return 0; }
NATIVE(El_prepend) { UNUSED; dnode *n, *f; TRY(need_node(thisv, &n)); TRY(node_args(argc, argv, &f)); insert_frag(n, f, n->first); *ret = js_undef(); return 0; }
NATIVE(El_before) { UNUSED; dnode *n, *f; TRY(need_node(thisv, &n)); TRY(node_args(argc, argv, &f)); if (n->parent) insert_frag(n->parent, f, n); *ret = js_undef(); return 0; }
NATIVE(El_after) { UNUSED; dnode *n, *f; TRY(need_node(thisv, &n)); TRY(node_args(argc, argv, &f)); if (n->parent) insert_frag(n->parent, f, n->next); *ret = js_undef(); return 0; }
NATIVE(El_replaceWith) {
    UNUSED;
    dnode *n, *f;
    TRY(need_node(thisv, &n));
    TRY(node_args(argc, argv, &f));
    if (n->parent) { dnode *p = n->parent, *nx = n->next; dn_remove(n); insert_frag(p, f, nx); }
    *ret = js_undef();
    return 0;
}

NATIVE(El_insertAdjacentHTML) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    jsstr *pos, *html;
    TRY(js_to_string(ARG(0), &pos));
    TRY(js_to_string(ARG(1), &html));
    dnode *frag = dn_new(DN_FRAGMENT, NULL);
    html_parse_into(frag, html->s, html->len);
    if (!strcmp(pos->s, "beforebegin") && n->parent) dn_insert_before(n->parent, frag, n);
    else if (!strcmp(pos->s, "afterbegin")) dn_insert_before(n, frag, n->first);
    else if (!strcmp(pos->s, "beforeend")) dn_append(n, frag);
    else if (!strcmp(pos->s, "afterend") && n->parent) dn_insert_before(n->parent, frag, n->next);
    *ret = js_undef();
    return 0;
}

NATIVE(El_insertAdjacentElement) {
    UNUSED;
    dnode *n, *c;
    TRY(need_node(thisv, &n));
    jsstr *pos;
    TRY(js_to_string(ARG(0), &pos));
    TRY(arg_node(ARG(1), &c));
    if (!strcmp(pos->s, "beforebegin") && n->parent) dn_insert_before(n->parent, c, n);
    else if (!strcmp(pos->s, "afterbegin")) dn_insert_before(n, c, n->first);
    else if (!strcmp(pos->s, "beforeend")) dn_append(n, c);
    else if (!strcmp(pos->s, "afterend") && n->parent) dn_insert_before(n->parent, c, n->next);
    after_insert(c);
    *ret = ARG(1);
    return 0;
}

NATIVE(El_click) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    int prevented;
    domjs_fire(n, "click", 1, 1, &prevented);
    if (!prevented && !strcmp(n->tag, "a")) {
        const char *h = dn_get_attr(n, "href");
        if (h && strncmp(h, "javascript:", 11) && h[0] != '#') navigate_to(h);
    }
    *ret = js_undef();
    return 0;
}

NATIVE(El_noop) { UNUSED; *ret = js_undef(); return 0; }

NATIVE(El_getBoundingClientRect) {
    UNUSED;
    jsobj *r = js_new_object();
    const char *k[] = { "x", "y", "top", "left", "right", "bottom", "width", "height" };
    double v[] = { 0, 0, 0, 0, 760, 20, 760, 20 };
    for (int i = 0; i < 8; i++) js_define(r, k[i], js_num(v[i]), P_ENUM | P_WRITE);
    *ret = js_objv(r);
    return 0;
}

NATIVE(El_getClientRects) {
    UNUSED;
    jsval r;
    TRY(El_getBoundingClientRect(thisv, 0, NULL, &r, self));
    jsobj *a = js_new_array(0);
    dnode *n = dn_of(thisv);
    if (n && dn_visible(n)) js_array_push(a, r);
    *ret = js_objv(a);
    return 0;
}

NATIVE(Form_submit) {
    UNUSED;
    dnode *n;
    TRY(need_node(thisv, &n));
    const char *a = dn_get_attr(n, "action");
    char base[1024];
    dn_resolve(d_url, a && *a ? a : d_url, base, sizeof(base));
    char *q = strchr(base, '?');
    if (q) *q = 0;
    jsbuf b = { 0 };
    jb_adds(&b, base);
    dnode **v;
    int c;
    collect(n, pred_form_field, NULL, &v, &c);
    int first = 1;
    for (int i = 0; i < c; i++) {
        const char *nm = dn_get_attr(v[i], "name"), *ty = dn_get_attr(v[i], "type");
        if (!nm || (ty && (!strcmp(ty, "submit") || !strcmp(ty, "button") || !strcmp(ty, "reset")))) continue;
        if (ty && (!strcmp(ty, "checkbox") || !strcmp(ty, "radio")) && !dn_get_attr(v[i], "checked")) continue;
        jsval val;
        TRY(js_get_c(dn_wrap(v[i]), "value", &val));
        jsval enc, a1 = val;
        jsval f;
        TRY(js_get_c(js_objv(js_global), "encodeURIComponent", &f));
        TRY(js_call(f, js_undef(), 1, &a1, &enc));
        jb_addc(&b, first ? '?' : '&');
        first = 0;
        jb_adds(&b, nm);
        jb_addc(&b, '=');
        jb_add(&b, enc.u.s->s, enc.u.s->len);
    }
    free(v);
    jb_addc(&b, 0);
    navigate_to(b.p);
    jb_free(&b);
    *ret = js_undef();
    return 0;
}

NATIVE(Form_reset) { UNUSED; *ret = js_undef(); return 0; }

/* --- document */

NATIVE(Doc_getElementById) {
    UNUSED;
    jsstr *id;
    TRY(js_to_string(ARG(0), &id));
    dnode *root = dn_of(thisv);
    *ret = dn_wrap(dn_by_id(root && root->type != DN_WINDOW ? root : d_document, id->s));
    return 0;
}

NATIVE(Doc_createElement) {
    UNUSED;
    jsstr *t;
    TRY(js_to_string(ARG(argc > 1 && self->is_arrow ? 1 : 0), &t));
    char tag[64];
    snprintf(tag, sizeof(tag), "%s", t->s);
    char *c = strchr(tag, ':');
    *ret = dn_wrap(dn_new(DN_ELEMENT, c ? c + 1 : tag));
    return 0;
}

NATIVE(Doc_createTextNode) { UNUSED; jsstr *t; TRY(js_to_string(ARG(0), &t)); *ret = dn_wrap(dn_text(t->s, t->len)); return 0; }
NATIVE(Doc_createComment) {
    UNUSED;
    jsstr *t;
    TRY(js_to_string(ARG(0), &t));
    dnode *n = dn_new(DN_COMMENT, NULL);
    n->text = malloc(t->len + 1);
    memcpy(n->text, t->s, t->len + 1);
    n->tlen = t->len;
    *ret = dn_wrap(n);
    return 0;
}
NATIVE(Doc_createDocumentFragment) { UNUSED; *ret = dn_wrap(dn_new(DN_FRAGMENT, NULL)); return 0; }

NATIVE(Doc_write) {
    UNUSED;
    jsbuf b = { 0 };
    for (int i = 0; i < argc; i++) { jsstr *s; if (js_to_string(argv[i], &s) < 0) { jb_free(&b); return -1; } jb_add(&b, s->s, s->len); }
    if (self->is_arrow) jb_addc(&b, '\n');                    /* writeln */
    if (d_parsing) html_write(b.p, b.n);
    else {                                                     /* после загрузки: дописываем в body */
        dnode *body = dn_find_tag(d_document, "body");
        if (body && b.n) html_parse_into(body, b.p, b.n);
    }
    jb_free(&b);
    *ret = js_undef();
    return 0;
}

NATIVE(Doc_hasFocus) { UNUSED; *ret = js_bool(1); return 0; }
NATIVE(Doc_execCommand) { UNUSED; *ret = js_bool(0); return 0; }
NATIVE(Doc_importNode) { UNUSED; dnode *n; TRY(arg_node(ARG(0), &n)); *ret = dn_wrap(dn_clone(n, js_truthy(ARG(1)))); return 0; }
NATIVE(Doc_adoptNode) { UNUSED; dnode *n; TRY(arg_node(ARG(0), &n)); dn_remove(n); *ret = ARG(0); return 0; }
NATIVE(Doc_elementFromPoint) { UNUSED; *ret = js_null(); return 0; }

NATIVE(Doc_createHTMLDocument) {
    UNUSED;
    dnode *d = dn_new(DN_DOCUMENT, NULL), *html = dn_new(DN_ELEMENT, "html"), *head = dn_new(DN_ELEMENT, "head"), *body = dn_new(DN_ELEMENT, "body");
    dn_append(d, html);
    dn_append(html, head);
    dn_append(html, body);
    if (argc && argv[0].tag != JS_UNDEF) {
        jsstr *t;
        TRY(js_to_string(argv[0], &t));
        dnode *title = dn_new(DN_ELEMENT, "title");
        dn_append(head, title);
        dn_append(title, dn_text(t->s, t->len));
    }
    *ret = dn_wrap(d);
    return 0;
}

/* ------------------------------------------------------------------ события */

static jsstr *S_type_, *S_target, *S_currentTarget, *S_defaultPrevented, *S_cancelBubble, *S_stopNow, *S_handleEvent, *S_srcElement, *S_eventPhase;

static int add_listener(dnode *n, jsstr *type, jsval fn, int capture, int once) {
    for (int i = 0; i < n->nls; i++)
        if (n->ls[i].type == type && js_strict_eq(n->ls[i].fn, fn) && n->ls[i].capture == capture) return 0;
    if (n->nls == n->lcap) {
        int cap = n->lcap ? n->lcap * 2 : 4;
        struct listener *l = malloc(sizeof(struct listener) * cap);
        if (n->nls) memcpy(l, n->ls, sizeof(struct listener) * n->nls);
        free(n->ls);
        n->ls = l;
        n->lcap = cap;
    }
    n->ls[n->nls].type = type;
    n->ls[n->nls].fn = fn;
    n->ls[n->nls].capture = capture;
    n->ls[n->nls].once = once;
    n->nls++;
    if (!strcmp(type->s, "click")) dom_dirty = 1;              /* элемент стал «щёлкабельным» */
    return 0;
}

static dnode *et_node(jsval thisv) {
    if (IS_OBJ(thisv) && thisv.u.o == js_global) return d_window;
    return dn_of(thisv);
}

NATIVE(ET_addEventListener) {
    UNUSED;
    dnode *n = et_node(thisv);
    if (!n || argc < 2 || (!js_is_callable(argv[1]) && !IS_OBJ(argv[1]))) { *ret = js_undef(); return 0; }
    jsstr *t;
    TRY(js_to_string(argv[0], &t));
    int capture = 0, once = 0;
    if (argc > 2) {
        if (IS_OBJ(argv[2])) {
            jsval c, o;
            TRY(js_get_c(argv[2], "capture", &c));
            TRY(js_get_c(argv[2], "once", &o));
            capture = js_truthy(c);
            once = js_truthy(o);
        } else capture = js_truthy(argv[2]);
    }
    add_listener(n, js_perm_str(t), argv[1], capture, once);
    *ret = js_undef();
    return 0;
}

NATIVE(ET_removeEventListener) {
    UNUSED;
    dnode *n = et_node(thisv);
    if (!n || argc < 2) { *ret = js_undef(); return 0; }
    jsstr *t;
    TRY(js_to_string(argv[0], &t));
    t = js_intern_str(t);
    for (int i = 0; i < n->nls; i++)
        if (n->ls[i].type == t && js_strict_eq(n->ls[i].fn, argv[1])) {
            memmove(&n->ls[i], &n->ls[i + 1], sizeof(struct listener) * (n->nls - i - 1));
            n->nls--;
            break;
        }
    *ret = js_undef();
    return 0;
}

/* обработчик из атрибута onclick="..." -> функция (кэш в обёртке) */
static int attr_handler(dnode *n, jsstr *type, jsval *fn) {
    char an[64];
    snprintf(an, sizeof(an), "on%s", type->s);
    fn->tag = JS_UNDEF;
    if (n->type == DN_WINDOW) {
        dnode *body = dn_find_tag(d_document, "body");
        jsval v;
        TRY(js_get_c(js_objv(js_global), an, &v));
        if (js_is_callable(v)) { *fn = v; return 0; }
        if (body && (!strcmp(type->s, "load") || !strcmp(type->s, "unload") || !strcmp(type->s, "beforeunload") || !strcmp(type->s, "resize"))) n = body;
        else return 0;
    }
    jsval w = dn_wrap(n);
    struct prop *p = js_own_prop(w.u.o, js_intern(an, strlen(an)));
    if (p && js_is_callable(p->v)) { *fn = p->v; return 0; }
    const char *src = n->type == DN_ELEMENT ? dn_get_attr(n, an) : NULL;
    if (!src || !*src) return 0;
    char cache[72];
    snprintf(cache, sizeof(cache), "%%%s=%u", an, (unsigned)strlen(src));
    struct prop *c = js_own_prop(w.u.o, js_intern(cache, strlen(cache)));
    if (c) { *fn = c->v; return 0; }
    jsbuf b = { 0 };
    jb_adds(&b, "(function(event){");
    jb_adds(&b, src);
    jb_adds(&b, "\n})");
    int r = js_eval(b.p, b.n, an, fn);
    jb_free(&b);
    if (r < 0) return -1;
    js_define(w.u.o, cache, *fn, 0);
    return 0;
}

int domjs_has_click(dnode *n) {
    if (n->type != DN_ELEMENT) return 0;
    for (int i = 0; i < n->nls; i++) if (!strcmp(n->ls[i].type->s, "click") || !strcmp(n->ls[i].type->s, "mousedown")) return 1;
    if (dn_get_attr(n, "onclick")) return 1;
    if (n->wrap) {
        struct prop *p = js_own_prop(n->wrap, js_perm("onclick"));
        if (p && js_is_callable(p->v)) return 1;
    }
    return 0;
}

static void report_error(const char *where) {
    char buf[200];
    js_error_text(js_exc, buf, sizeof(buf));
    n_errors++;
    snprintf(last_error, sizeof(last_error), "%s", buf);
    char line[300];
    int l = snprintf(line, sizeof(line), "JS error (%s): %s\n", where, buf);
    if (d_host && d_host->console) d_host->console(line, l);
    js_exc = js_undef();
}

static void call_handler(jsval fn, jsval thisv, jsval ev, jsval *res) {
    *res = js_undef();
    if (js_is_callable(fn)) { if (js_call(fn, thisv, 1, &ev, res) < 0) report_error("event"); return; }
    if (IS_OBJ(fn)) {
        jsval h;
        if (js_get(fn, S_handleEvent, &h) == 0 && js_is_callable(h)) { if (js_call(h, fn, 1, &ev, res) < 0) report_error("event"); }
    }
}

static int dispatch(dnode *target, jsval ev) {
    jsval tv;
    if (js_get(ev, S_type_, &tv) < 0) return 0;
    jsstr *type = js_intern_str(js_to_str_nothrow(tv));
    jsval bub;
    js_get_c(ev, "bubbles", &bub);
    int bubbles = js_truthy(bub);
    js_set(ev, S_target, dn_wrap(target));
    js_set(ev, S_srcElement, dn_wrap(target));
    dnode *path[256];
    int np = 0;
    for (dnode *p = target; p && np < 255; p = p->parent) path[np++] = p;
    if (np && path[np - 1] == d_document && target != d_window) path[np++] = d_window;
    if (!bubbles) np = 1;
    /* сначала «перехватчики» (capture) от окна к цели, потом обычные — от цели наверх */
    for (int phase = 0; phase < 2; phase++) {
        for (int k = 0; k < np; k++) {
            int i = phase == 0 ? np - 1 - k : k;
            dnode *n = path[i];
            if (phase == 0 && i == 0) continue;
            js_set(ev, S_currentTarget, dn_wrap(n));
            js_set(ev, S_eventPhase, js_num(i == 0 ? 2 : phase == 0 ? 1 : 3));
            int cnt = n->nls;
            struct listener tmp[64];
            int nt = 0;
            for (int l = 0; l < cnt && nt < 64; l++)
                if (n->ls[l].type == type && (phase == 0 ? n->ls[l].capture : (i == 0 || !n->ls[l].capture))) tmp[nt++] = n->ls[l];
            for (int l = 0; l < nt; l++) {
                if (tmp[l].once) {
                    for (int q = 0; q < n->nls; q++)
                        if (n->ls[q].type == type && js_strict_eq(n->ls[q].fn, tmp[l].fn)) {
                            memmove(&n->ls[q], &n->ls[q + 1], sizeof(struct listener) * (n->nls - q - 1));
                            n->nls--;
                            break;
                        }
                }
                jsval r;
                call_handler(tmp[l].fn, dn_wrap(n), ev, &r);
                jsval stop;
                js_get(ev, S_stopNow, &stop);
                if (js_truthy(stop)) break;
            }
            if (phase == 1) {
                jsval h, r;
                if (attr_handler(n, type, &h) < 0) report_error("handler");
                else if (h.tag != JS_UNDEF) {
                    call_handler(h, dn_wrap(n), ev, &r);
                    if (r.tag == JS_BOOL && !r.u.b) js_set(ev, S_defaultPrevented, js_bool(1));
                }
            }
            jsval cb;
            js_get(ev, S_cancelBubble, &cb);
            if (js_truthy(cb)) goto done;
        }
    }
done:;
    jsval dp;
    js_get(ev, S_defaultPrevented, &dp);
    js_set(ev, S_currentTarget, js_null());
    return js_truthy(dp);
}

static jsval make_event(const char *type, int bubbles, int cancelable, const char *klass) {
    jsval ctor, init, ev;
    if (js_get_c(js_objv(js_global), klass, &ctor) < 0 || !js_is_callable(ctor)) js_get_c(js_objv(js_global), "Event", &ctor);
    jsobj *io = js_new_object();
    js_define(io, "bubbles", js_bool(bubbles), P_ENUM | P_WRITE);
    js_define(io, "cancelable", js_bool(cancelable), P_ENUM | P_WRITE);
    init = js_objv(io);
    jsval args[2] = { js_cstr(type), init };
    if (!js_is_callable(ctor) || js_construct(ctor, 2, args, &ev) < 0) { js_exc = js_undef(); ev = js_objv(js_new_object()); js_set(ev, S_type_, js_cstr(type)); }
    js_set_c(ev, "isTrusted", js_bool(1));
    return ev;
}

void domjs_fire(dnode *target, const char *type, int bubbles, int cancelable, int *prevented) {
    jsval ev = make_event(type, bubbles, cancelable, !strcmp(type, "click") ? "MouseEvent" : !strncmp(type, "key", 3) ? "KeyboardEvent" : "Event");
    *prevented = dispatch(target, ev);
}

NATIVE(ET_dispatchEvent) {
    UNUSED;
    dnode *n = et_node(thisv);
    if (!n || !IS_OBJ(ARG(0))) return js_throw("TypeError", "parameter 1 is not of type 'Event'");
    *ret = js_bool(!dispatch(n, argv[0]));
    return 0;
}

/* ------------------------------------------------------------------ таймеры */

struct timer { int id; double due, interval; jsval fn; jsobj *args; };
static struct timer timers[256];
static int n_timers, next_timer_id = 1;

static void mark_timers(void) {
    for (int i = 0; i < n_timers; i++) { js_mark_val(timers[i].fn); js_mark_obj(timers[i].args); }
    js_mark_obj(loc_obj);
}

static int add_timer(jsval fn, double ms, int repeat, int argc, jsval *argv) {
    if (n_timers >= 256) return 0;
    if (js_isnan(ms) || ms < 0) ms = 0;
    if (repeat && ms < 10) ms = 10;
    struct timer *t = &timers[n_timers++];
    t->id = next_timer_id++;
    t->due = js_now_ms() + ms;
    t->interval = repeat ? ms : 0;
    t->fn = fn;
    t->args = js_new_array(0);
    for (int i = 0; i < argc; i++) js_array_push(t->args, argv[i]);
    return t->id;
}

NATIVE(W_setTimeout) {
    UNUSED;
    double ms = argc > 1 ? js_num_nothrow(argv[1]) : 0;
    *ret = js_num(add_timer(ARG(0), ms, self->is_arrow, argc > 2 ? argc - 2 : 0, argv + 2));
    return 0;
}

NATIVE(W_clearTimeout) {
    UNUSED;
    int id = (int)js_num_nothrow(ARG(0));
    for (int i = 0; i < n_timers; i++) if (timers[i].id == id) { timers[i] = timers[--n_timers]; break; }
    *ret = js_undef();
    return 0;
}

int dom_next_timer(void) {
    if (!n_timers || !dom_scripting) return -1;
    double now = js_now_ms(), best = 1e18;
    for (int i = 0; i < n_timers; i++) if (timers[i].due < best) best = timers[i].due;
    double d = best - now;
    return d < 0 ? 0 : d > 60000 ? 60000 : (int)d;
}

static void run_timers_body(void *arg) {
    (void)arg;
    double now = js_now_ms();
    js_deadline_ms = now + 3000;
    int ran = 0;
    for (int pass = 0; pass < 2 && ran < 64; pass++) {
        for (int i = 0; i < n_timers && ran < 64; i++) {
            if (timers[i].due > now) continue;
            struct timer t = timers[i];
            if (t.interval > 0) timers[i].due = now + t.interval;
            else { timers[i] = timers[--n_timers]; i--; }
            ran++;
            jsval r;
            if (t.fn.tag == JS_STR) { if (js_eval(t.fn.u.s->s, t.fn.u.s->len, "timeout", &r) < 0) report_error("timer"); }
            else if (js_is_callable(t.fn) && js_call(t.fn, js_undef(), t.args->alen, t.args->arr, &r) < 0) report_error("timer");
            js_run_jobs();
        }
    }
    js_deadline_ms = 0;
}

void dom_run_timers(void) {
    if (!dom_scripting) return;
    big_call(run_timers_body, NULL);
    if (js_alloc_since_gc > (4u << 20)) js_gc();
}

/* ------------------------------------------------------------------ скрипты */

static int js_type_ok(dnode *e) {
    const char *t = dn_get_attr(e, "type");
    if (!t || !*t) return 1;
    char low[64];
    snprintf(low, sizeof(low), "%s", t);
    for (char *p = low; *p; p++) *p = tolower((ju8)*p);
    return strstr(low, "javascript") || strstr(low, "ecmascript") || !strcmp(low, "text/jscript") || !strcmp(low, "text/livescript");
}

static unsigned total_js_bytes;

static int script_nesting;

void domjs_run_script(const char *src, int len, const char *name, dnode *script) {
    dnode *save = current_script;
    current_script = script;
    n_scripts++;
    double save_deadline = js_deadline_ms;
    if (!save_deadline) js_deadline_ms = js_now_ms() + 8000;
    jsval r;
    script_nesting++;
    if (js_eval(src, len, name, &r) < 0) report_error(name);
    js_run_jobs();
    script_nesting--;
    js_deadline_ms = save_deadline;
    current_script = save;
    /* между скриптами страницы JS не выполняется — можно собрать мусор */
    if (!script_nesting && js_call_depth_check() == 0 && js_alloc_since_gc > (8u << 20)) js_gc();
}

/* внешний скрипт: скачать и выполнить */
static void run_external(dnode *e) {
    const char *src = dn_get_attr(e, "src");
    char url[1024];
    dn_resolve(d_url, src, url, sizeof(url));
    char *data = NULL, ctype[64];
    int len = 0, status = 0;
    char msg[160];
    snprintf(msg, sizeof(msg), "Loading script %.120s", url);
    if (d_host->status) d_host->status(msg);
    int prevented;
    if (total_js_bytes > (6u << 20) || d_host->fetch(url, "GET", NULL, 0, &data, &len, &status, ctype, sizeof(ctype)) < 0 || status >= 400) {
        free(data);
        n_errors++;
        snprintf(last_error, sizeof(last_error), "cannot load %.150s", url);
        domjs_fire(e, "error", 0, 0, &prevented);
        return;
    }
    total_js_bytes += len;
    const char *nm = strrchr(url, '/');
    domjs_run_script(data, len, nm ? nm + 1 : url, e);
    free(data);
    domjs_fire(e, "load", 0, 0, &prevented);
}

static dnode *deferred[128];
static int n_deferred;

static struct timer *timer_for_script(dnode *e);

void domjs_script_inserted(dnode *e) {
    if (e->script_done && !e->parser_inserted) return;
    e->script_done = 1;
    if (!dom_scripting || !js_type_ok(e)) return;
    if (dn_get_attr(e, "src")) {
        if (e->parser_inserted && !dn_get_attr(e, "defer") && !dn_get_attr(e, "async")) { run_external(e); return; }
        if (e->parser_inserted && n_deferred < 128) { deferred[n_deferred++] = e; return; }
        timer_for_script(e);                                   /* вставили из скрипта — выполним чуть позже */
        return;
    }
    jsbuf b = { 0 };
    dn_text_content(e, &b, 0);
    if (b.n) domjs_run_script(b.p, b.n, "inline script", e);
    jb_free(&b);
}

NATIVE(W_runScriptLater) {
    UNUSED;
    dnode *e = dn_of(ARG(0));
    if (e) run_external(e);
    *ret = js_undef();
    return 0;
}

static jsobj *run_later_fn;
static struct timer *timer_for_script(dnode *e) {
    jsval a = dn_wrap(e);
    add_timer(js_objv(run_later_fn), 0, 0, 1, &a);
    return NULL;
}

/* ------------------------------------------------------------------ window */

static int loc_get(jsobj *o, jsstr *key, jsval *out) {
    (void)o;
    const char *k = key->s;
    if (key->symbol) return 0;
    if (!strcmp(k, "href")) { *out = js_cstr(d_url); return 1; }
    for (int i = 0; url_parts[i]; i++) if (!strcmp(k, url_parts[i])) { url_part(d_url, k, out); return 1; }
    return 0;
}

static int loc_set(jsobj *o, jsstr *key, jsval v) {
    (void)o;
    const char *k = key->s;
    jsstr *s;
    if (!strcmp(k, "href")) { TRY(js_to_string(v, &s)); navigate_to(s->s); return 1; }
    if (!strcmp(k, "hash")) {
        TRY(js_to_string(v, &s));
        char u[1024];
        snprintf(u, sizeof(u), "%s", d_url);
        char *h = strchr(u, '#');
        if (h) *h = 0;
        int l = strlen(u);
        snprintf(u + l, sizeof(u) - l, "%s%s", s->s[0] == '#' ? "" : "#", s->s);
        snprintf(d_url, sizeof(d_url), "%s", u);
        return 1;
    }
    if (!strcmp(k, "search") || !strcmp(k, "pathname")) {
        TRY(js_to_string(v, &s));
        char u[1024];
        snprintf(u, sizeof(u), "%s%s", k[0] == 's' && s->s[0] != '?' ? "?" : "", s->s);
        navigate_to(u);
        return 1;
    }
    return 0;
}
static const struct js_class loc_class = { "Location", loc_get, loc_set, NULL };

NATIVE(Loc_assign) { UNUSED; jsstr *s; TRY(js_to_string(ARG(0), &s)); navigate_to(s->s); *ret = js_undef(); return 0; }
NATIVE(Loc_reload) { UNUSED; navigate_to(d_url); *ret = js_undef(); return 0; }
NATIVE(Loc_toString) { UNUSED; *ret = js_cstr(d_url); return 0; }

NATIVE(W_location_get) { UNUSED; *ret = js_objv(loc_obj); return 0; }
NATIVE(W_location_set) { UNUSED; jsstr *s; TRY(js_to_string(ARG(0), &s)); navigate_to(s->s); *ret = js_undef(); return 0; }
NATIVE(W_document_get) { UNUSED; *ret = dn_wrap(d_document); return 0; }

NATIVE(W_alert) {
    UNUSED;
    jsstr *s = js_intern("", 0);
    if (argc) TRY(js_to_string(argv[0], &s));
    char msg[200];
    snprintf(msg, sizeof(msg), "Alert: %.180s", s->s);
    if (d_host->status) d_host->status(msg);
    if (d_host->console) { d_host->console(msg, strlen(msg)); d_host->console("\n", 1); }
    *ret = js_undef();
    return 0;
}
NATIVE(W_confirm) { UNUSED; TRY(W_alert(thisv, argc, argv, ret, self)); *ret = js_bool(1); return 0; }
NATIVE(W_prompt) { UNUSED; TRY(W_alert(thisv, argc, argv, ret, self)); *ret = argc > 1 ? argv[1] : js_null(); return 0; }

static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
NATIVE(W_btoa) {
    UNUSED;
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    jsbuf b = { 0 };
    for (int i = 0; i < s->len; i += 3) {
        unsigned v = (ju8)s->s[i] << 16 | (i + 1 < s->len ? (ju8)s->s[i + 1] << 8 : 0) | (i + 2 < s->len ? (ju8)s->s[i + 2] : 0);
        char t[4] = { b64[v >> 18], b64[(v >> 12) & 63], i + 1 < s->len ? b64[(v >> 6) & 63] : '=', i + 2 < s->len ? b64[v & 63] : '=' };
        jb_add(&b, t, 4);
    }
    *ret = js_strv(jb_str(&b));
    return 0;
}
NATIVE(W_atob) {
    UNUSED;
    jsstr *s;
    TRY(js_to_string(ARG(0), &s));
    jsbuf b = { 0 };
    unsigned v = 0;
    int bits = 0;
    for (int i = 0; i < s->len; i++) {
        const char *p = strchr(b64, s->s[i]);
        if (!p || !s->s[i]) continue;
        v = (v << 6) | (p - b64);
        bits += 6;
        if (bits >= 8) { bits -= 8; jb_addc(&b, (v >> bits) & 255); }
    }
    *ret = js_strv(jb_str(&b));
    return 0;
}

/* --- __myos: помощники для прелюдии */

NATIVE(My_fetch) {
    UNUSED;
    jsstr *u, *m = js_intern("GET", 3), *body = NULL;
    TRY(js_to_string(ARG(0), &u));
    if (argc > 1 && !IS_NULLISH(argv[1])) TRY(js_to_string(argv[1], &m));
    if (argc > 2 && !IS_NULLISH(argv[2])) TRY(js_to_string(argv[2], &body));
    char url[1024];
    dn_resolve(d_url, u->s, url, sizeof(url));
    char *data = NULL, ctype[96] = "";
    int len = 0, status = 0;
    char msg[160];
    snprintf(msg, sizeof(msg), "JS fetch %.130s", url);
    if (d_host->status) d_host->status(msg);
    if (d_host->fetch(url, m->s, body ? body->s : NULL, body ? body->len : 0, &data, &len, &status, ctype, sizeof(ctype)) < 0) {
        free(data);
        *ret = js_null();
        return 0;
    }
    jsobj *r = js_new_object();
    js_define(r, "status", js_num(status), P_ENUM | P_WRITE);
    js_define(r, "text", js_strv(js_strn(data ? data : "", len)), P_ENUM | P_WRITE);
    js_define(r, "type", js_cstr(ctype), P_ENUM | P_WRITE);
    free(data);
    *ret = js_objv(r);
    return 0;
}

NATIVE(My_resolve) {
    UNUSED;
    jsstr *u;
    TRY(js_to_string(ARG(0), &u));
    char base[1024], out[2048];
    if (argc > 1 && argv[1].tag != JS_UNDEF) {
        jsstr *b;
        TRY(js_to_string(argv[1], &b));
        dn_resolve(d_url, b->s, base, sizeof(base));
    } else snprintf(base, sizeof(base), "%s", d_url);
    if (!strstr(base, ":") && !strstr(u->s, ":")) { *ret = js_undef(); return 0; }
    dn_resolve(base, u->s, out, sizeof(out));
    *ret = js_cstr(out);
    return 0;
}

static int url_changed;
NATIVE(My_setUrl) { UNUSED; jsstr *u; TRY(js_to_string(ARG(0), &u)); snprintf(d_url, sizeof(d_url), "%s", u->s); url_changed = 1; *ret = js_undef(); return 0; }
NATIVE(My_go) {
    UNUSED;
    int d = (int)js_num_nothrow(ARG(0));
    snprintf(pending_nav, sizeof(pending_nav), "%s", d < 0 ? "myos:back" : "myos:forward");
    nav_requested = 1;
    *ret = js_undef();
    return 0;
}

static int media_ok_q(const char *q) {
    char l[256];
    int n = 0;
    for (; q[n] && n < 255; n++) l[n] = tolower((ju8)q[n]);
    l[n] = 0;
    if (strstr(l, "print") || strstr(l, "prefers-color-scheme: dark") || strstr(l, "prefers-reduced-motion: reduce") || strstr(l, "hover: none") || strstr(l, "pointer: coarse")) return 0;
    for (char *p = l; (p = strstr(p, "width")); p += 5) {
        char *v = p + 5;
        while (*v == ' ' || *v == ':') v++;
        int num = atoi(v);
        if (p >= l + 4 && !strncmp(p - 4, "max-", 4) && 790 > num) return 0;
        if (p >= l + 4 && !strncmp(p - 4, "min-", 4) && 790 < num) return 0;
    }
    return 1;
}
NATIVE(My_media) { UNUSED; jsstr *q; TRY(js_to_string(ARG(0), &q)); *ret = js_bool(media_ok_q(q->s)); return 0; }

NATIVE(My_computed) {
    UNUSED;
    dnode *n = dn_of(ARG(0));
    jsstr *p;
    TRY(js_to_string(ARG(1), &p));
    if (!n || n->type != DN_ELEMENT) { *ret = js_cstr(""); return 0; }
    const char *st = dn_get_attr(n, "style");
    char kebab[64];
    camel_to_kebab(p->s, kebab);
    int vs, ve, ds, de;
    if (st && style_find(st, kebab, &vs, &ve, &ds, &de)) { *ret = js_strv(js_strn(st + vs, ve - vs)); return 0; }
    if (!strcmp(kebab, "display")) {
        if (dn_get_attr(n, "hidden") || css_hidden(n) || !strcmp(n->tag, "script") || !strcmp(n->tag, "style")) *ret = js_cstr("none");
        else {
            static const char *inl = " a span b i em strong img input button select textarea label code small sub sup abbr ";
            char t[40];
            snprintf(t, sizeof(t), " %s ", n->tag);
            *ret = js_cstr(strstr(inl, t) ? "inline" : "block");
        }
        return 0;
    }
    *ret = js_cstr("");
    return 0;
}

NATIVE(My_store) {
    UNUSED;
    jsstr *tag, *v;
    TRY(js_to_string(ARG(0), &tag));
    TRY(js_to_string(ARG(1), &v));
    snprintf(storage[tag->s[0] == 's'], sizeof(storage[0]), "%s", v->s);
    *ret = js_undef();
    return 0;
}
NATIVE(My_load) { UNUSED; jsstr *tag; TRY(js_to_string(ARG(0), &tag)); *ret = js_cstr(storage[tag->s[0] == 's']); return 0; }

static void console_print(const char *s, int n) { if (d_host && d_host->console) d_host->console(s, n); }

/* ------------------------------------------------------------------ сборка мира страницы */

static jsobj *alias_ctor(const char *name, jsobj *proto) {
    jsobj *c = js_make_ctor(name, El_noop, 0, proto);
    return c;
}

static void def_methods(jsobj *p, const char **names, jsnative *fns, int n) {
    for (int i = 0; i < n; i++) js_defn(p, names[i], fns[i], 1);
}

void domjs_init_realm(void) {
    js_set_print(console_print);
    js_set_mark_hook(domjs_mark);
    S_type_ = js_perm("type"); S_target = js_perm("target"); S_currentTarget = js_perm("currentTarget");
    S_defaultPrevented = js_perm("defaultPrevented"); S_cancelBubble = js_perm("cancelBubble"); S_stopNow = js_perm("__stopNow");
    S_handleEvent = js_perm("handleEvent"); S_srcElement = js_perm("srcElement"); S_eventPhase = js_perm("eventPhase");
    jsobj *g = js_global;

    et_proto = js_new_object();
    node_proto = js_new_object_proto(et_proto);
    element_proto = js_new_object_proto(node_proto);
    html_proto = js_new_object_proto(element_proto);
    jsobj *chardata = js_new_object_proto(node_proto);
    text_proto = js_new_object_proto(chardata);
    comment_proto = js_new_object_proto(chardata);
    doc_proto = js_new_object_proto(node_proto);
    frag_proto = js_new_object_proto(node_proto);
    style_proto = js_new_object();
    classlist_proto = js_new_object();
    nodelist_proto = js_new_object_proto(js_array_proto);
    loc_proto = js_new_object();

    alias_ctor("EventTarget", et_proto);
    jsobj *Node = alias_ctor("Node", node_proto);
    alias_ctor("Element", element_proto);
    alias_ctor("HTMLElement", html_proto);
    alias_ctor("CharacterData", chardata);
    alias_ctor("Text", text_proto);
    alias_ctor("Comment", comment_proto);
    alias_ctor("Document", doc_proto);
    alias_ctor("HTMLDocument", doc_proto);
    alias_ctor("DocumentFragment", frag_proto);
    alias_ctor("CSSStyleDeclaration", style_proto);
    alias_ctor("DOMTokenList", classlist_proto);
    alias_ctor("NodeList", nodelist_proto);
    alias_ctor("HTMLCollection", nodelist_proto);
    alias_ctor("Location", loc_proto);
    for (unsigned i = 0; i < N_KINDS; i++) {
        if (elem_kinds[i].name[0] == '%') { kind_proto[i] = html_proto; continue; }
        kind_proto[i] = js_new_object_proto(html_proto);
        alias_ctor(elem_kinds[i].name, kind_proto[i]);
    }
    alias_ctor("HTMLUnknownElement", js_new_object_proto(html_proto));
    svg_proto = js_new_object_proto(element_proto);
    alias_ctor("SVGElement", svg_proto);
    alias_ctor("SVGSVGElement", js_new_object_proto(svg_proto));
    alias_ctor("ShadowRoot", js_new_object_proto(frag_proto));
    const char *consts[] = { "ELEMENT_NODE", "ATTRIBUTE_NODE", "TEXT_NODE", "CDATA_SECTION_NODE", "", "", "PROCESSING_INSTRUCTION_NODE",
                             "COMMENT_NODE", "DOCUMENT_NODE", "DOCUMENT_TYPE_NODE", "DOCUMENT_FRAGMENT_NODE" };
    for (int i = 0; i < 11; i++) if (consts[i][0]) { js_define(Node, consts[i], js_num(i + 1), 0); js_define(node_proto, consts[i], js_num(i + 1), 0); }

    js_defn(et_proto, "addEventListener", ET_addEventListener, 2);
    js_defn(et_proto, "removeEventListener", ET_removeEventListener, 2);
    js_defn(et_proto, "dispatchEvent", ET_dispatchEvent, 1);
    js_defn(g, "addEventListener", ET_addEventListener, 2);
    js_defn(g, "removeEventListener", ET_removeEventListener, 2);
    js_defn(g, "dispatchEvent", ET_dispatchEvent, 1);

    const char *nn[] = { "appendChild", "insertBefore", "removeChild", "replaceChild", "cloneNode", "contains", "hasChildNodes",
                         "getRootNode", "isSameNode", "isEqualNode", "normalize", "compareDocumentPosition",
                         "querySelector", "querySelectorAll", "getElementsByTagName", "getElementsByClassName", "getElementById",
                         "append", "prepend" };
    jsnative nf[] = { Node_appendChild, Node_insertBefore, Node_removeChild, Node_replaceChild, Node_cloneNode, Node_contains,
                      Node_hasChildNodes, Node_getRootNode, Node_isSameNode, Node_isSameNode, Node_normalize, Node_compareDocumentPosition,
                      El_querySelector, El_querySelectorAll, El_getElementsByTagName, El_getElementsByClassName, Doc_getElementById,
                      El_append, El_prepend };
    def_methods(node_proto, nn, nf, sizeof(nn) / sizeof(nn[0]));
    const char *en[] = { "getAttribute", "setAttribute", "removeAttribute", "hasAttribute", "toggleAttribute", "hasAttributes",
                         "getAttributeNames", "getAttributeNS", "setAttributeNS", "removeAttributeNS", "closest", "matches",
                         "webkitMatchesSelector", "msMatchesSelector", "remove", "before", "after", "replaceWith",
                         "insertAdjacentHTML", "insertAdjacentElement", "click", "focus", "blur", "select", "scrollIntoView",
                         "scrollTo", "scrollBy", "scroll", "getBoundingClientRect", "getClientRects", "submit", "reset",
                         "requestSubmit", "setSelectionRange", "animate", "attachShadow", "play", "pause", "load",
                         "checkValidity", "reportValidity", "setCustomValidity", "showModal", "show", "close", "requestFullscreen",
                         "releasePointerCapture", "setPointerCapture" };
    jsnative ef[] = { El_getAttribute, El_setAttribute, El_removeAttribute, El_hasAttribute, El_toggleAttribute, El_hasAttributes,
                      El_getAttributeNames, El_getAttribute, El_setAttribute, El_removeAttribute, El_closest, El_matches,
                      El_matches, El_matches, El_remove, El_before, El_after, El_replaceWith,
                      El_insertAdjacentHTML, El_insertAdjacentElement, El_click, El_noop, El_noop, El_noop, El_noop,
                      El_noop, El_noop, El_noop, El_getBoundingClientRect, El_getClientRects, Form_submit, Form_reset,
                      Form_submit, El_noop, El_noop, El_noop, El_noop, El_noop, El_noop,
                      El_noop, El_noop, El_noop, El_noop, El_noop, El_noop, El_noop,
                      El_noop, El_noop };
    def_methods(element_proto, en, ef, sizeof(en) / sizeof(en[0]));
    /* getAttributeNS(ns, name) — имя во втором аргументе */
    js_defn(element_proto, "getAttributeNS", El_noop, 2);
    js_defn(chardata, "remove", El_remove, 0);
    js_defn(chardata, "before", El_before, 0);
    js_defn(chardata, "after", El_after, 0);
    js_defn(chardata, "replaceWith", El_replaceWith, 0);

    const char *dn[] = { "createElement", "createTextNode", "createComment", "createDocumentFragment", "write", "hasFocus",
                         "execCommand", "importNode", "adoptNode", "elementFromPoint", "getElementsByName", "open", "close" };
    jsnative df[] = { Doc_createElement, Doc_createTextNode, Doc_createComment, Doc_createDocumentFragment, Doc_write, Doc_hasFocus,
                      Doc_execCommand, Doc_importNode, Doc_adoptNode, Doc_elementFromPoint, Doc_getElementsByName, El_noop, El_noop };
    def_methods(doc_proto, dn, df, sizeof(dn) / sizeof(dn[0]));
    jsobj *writeln = js_new_native(Doc_write, "writeln", 1);
    writeln->is_arrow = 1;
    js_define(doc_proto, "writeln", js_objv(writeln), P_WRITE | P_CONF);
    jsobj *cens = js_new_native(Doc_createElement, "createElementNS", 2);
    cens->is_arrow = 1;
    js_define(doc_proto, "createElementNS", js_objv(cens), P_WRITE | P_CONF);
    jsobj *impl = js_new_object();
    js_defn(impl, "hasFeature", Doc_hasFocus, 0);
    js_defn(impl, "createHTMLDocument", Doc_createHTMLDocument, 1);
    js_defn(impl, "createDocument", Doc_createHTMLDocument, 1);
    js_define(doc_proto, "implementation", js_objv(impl), P_WRITE | P_CONF);

    js_defn(style_proto, "getPropertyValue", Style_getPropertyValue, 1);
    js_defn(style_proto, "setProperty", Style_setProperty, 2);
    js_defn(style_proto, "removeProperty", Style_removeProperty, 1);
    const char *cn[] = { "add", "remove", "contains", "toggle", "replace", "item", "toString" };
    jsnative cf[] = { ClassList_add, ClassList_remove, ClassList_contains, ClassList_toggle, ClassList_replace, ClassList_item, ClassList_toString };
    def_methods(classlist_proto, cn, cf, 7);
    jsval item;
    js_get_c(js_objv(js_array_proto), "at", &item);
    js_define(nodelist_proto, "item", item, P_WRITE | P_CONF);

    loc_obj = js_new_host(&loc_class, NULL, loc_proto);
    js_defn(loc_proto, "assign", Loc_assign, 1);
    js_defn(loc_proto, "replace", Loc_assign, 1);
    js_defn(loc_proto, "reload", Loc_reload, 0);
    js_defn(loc_proto, "toString", Loc_toString, 0);

    /* window */
    js_define(g, "window", js_objv(g), P_ENUM);
    js_define(g, "self", js_objv(g), P_ENUM | P_WRITE);
    js_define(g, "top", js_objv(g), P_ENUM);
    js_define(g, "parent", js_objv(g), P_ENUM);
    js_define(g, "frames", js_objv(g), P_ENUM);
    js_define(g, "opener", js_null(), P_ENUM | P_WRITE);
    js_define(g, "frameElement", js_null(), P_ENUM | P_WRITE);
    js_define(g, "name", js_cstr(""), P_ENUM | P_WRITE);
    js_define(g, "closed", js_bool(0), P_ENUM | P_WRITE);
    js_define(g, "length", js_num(0), P_ENUM | P_WRITE);
    js_define(g, "isSecureContext", js_bool(1), P_ENUM | P_WRITE);
    js_define(g, "origin", js_cstr(""), P_ENUM | P_WRITE);
    js_def_accessor(g, "document", W_document_get, NULL);
    js_def_accessor(g, "location", W_location_get, W_location_set);
    const char *dims[] = { "innerWidth", "innerHeight", "outerWidth", "outerHeight", "devicePixelRatio", "scrollX", "scrollY",
                           "pageXOffset", "pageYOffset", "screenX", "screenY", "screenLeft", "screenTop" };
    double dv[] = { 788, 520, 800, 600, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 13; i++) js_define(g, dims[i], js_num(dv[i]), P_ENUM | P_WRITE);
    jsobj *scr = js_new_object();
    js_define(scr, "width", js_num(800), P_ENUM | P_WRITE);
    js_define(scr, "height", js_num(600), P_ENUM | P_WRITE);
    js_define(scr, "availWidth", js_num(800), P_ENUM | P_WRITE);
    js_define(scr, "availHeight", js_num(576), P_ENUM | P_WRITE);
    js_define(scr, "colorDepth", js_num(32), P_ENUM | P_WRITE);
    js_define(scr, "pixelDepth", js_num(32), P_ENUM | P_WRITE);
    jsobj *orient = js_new_object();
    js_define(orient, "type", js_cstr("landscape-primary"), P_ENUM);
    js_define(orient, "angle", js_num(0), P_ENUM);
    js_define(scr, "orientation", js_objv(orient), P_ENUM | P_WRITE);
    js_define(g, "screen", js_objv(scr), P_ENUM | P_WRITE);
    jsobj *nav = js_new_object();
    js_define(nav, "userAgent", js_cstr("Mozilla/5.0 (MyOS; i386) MyOS-Browser/0.14"), P_ENUM);
    js_define(nav, "appName", js_cstr("Netscape"), P_ENUM);
    js_define(nav, "appVersion", js_cstr("5.0 (MyOS)"), P_ENUM);
    js_define(nav, "appCodeName", js_cstr("Mozilla"), P_ENUM);
    js_define(nav, "product", js_cstr("Gecko"), P_ENUM);
    js_define(nav, "platform", js_cstr("MyOS i386"), P_ENUM);
    js_define(nav, "vendor", js_cstr(""), P_ENUM);
    js_define(nav, "language", js_cstr("ru"), P_ENUM);
    jsobj *langs = js_new_array(0);
    js_array_push(langs, js_cstr("ru"));
    js_array_push(langs, js_cstr("en"));
    js_define(nav, "languages", js_objv(langs), P_ENUM);
    js_define(nav, "cookieEnabled", js_bool(1), P_ENUM);
    js_define(nav, "onLine", js_bool(1), P_ENUM);
    js_define(nav, "doNotTrack", js_cstr("1"), P_ENUM);
    js_define(nav, "hardwareConcurrency", js_num(1), P_ENUM);
    js_define(nav, "maxTouchPoints", js_num(0), P_ENUM);
    js_define(nav, "webdriver", js_bool(0), P_ENUM);
    js_define(nav, "plugins", js_objv(js_new_array(0)), P_ENUM);
    js_define(nav, "mimeTypes", js_objv(js_new_array(0)), P_ENUM);
    js_defn(nav, "javaEnabled", Doc_execCommand, 0);
    js_define(g, "navigator", js_objv(nav), P_ENUM | P_WRITE);
    js_define(g, "clientInformation", js_objv(nav), P_WRITE);

    js_defn(g, "setTimeout", W_setTimeout, 2);
    jsobj *si = js_new_native(W_setTimeout, "setInterval", 2);
    si->is_arrow = 1;                                          /* признак «повторять» */
    js_define(g, "setInterval", js_objv(si), P_WRITE | P_CONF);
    js_defn(g, "clearTimeout", W_clearTimeout, 1);
    js_defn(g, "clearInterval", W_clearTimeout, 1);
    js_defn(g, "alert", W_alert, 1);
    js_defn(g, "confirm", W_confirm, 1);
    js_defn(g, "prompt", W_prompt, 2);
    js_defn(g, "btoa", W_btoa, 1);
    js_defn(g, "atob", W_atob, 1);
    run_later_fn = js_new_native(W_runScriptLater, "", 1);

    jsobj *my = js_new_object();
    js_define(g, "__myos", js_objv(my), 0);
    js_define(my, "%runLater", js_objv(run_later_fn), 0);
    js_defn(my, "fetch", My_fetch, 3);
    js_defn(my, "resolve", My_resolve, 2);
    js_defn(my, "setUrl", My_setUrl, 1);
    js_defn(my, "go", My_go, 1);
    js_defn(my, "media", My_media, 1);
    js_defn(my, "computed", My_computed, 2);
    js_defn(my, "store", My_store, 2);
    js_defn(my, "load", My_load, 1);

    jsval r;
    if (js_eval(prelude_src, sizeof(prelude_src) - 1, "prelude", &r) < 0) report_error("prelude");
}

void domjs_reset(void) {
    n_timers = 0;
    n_scripts = n_errors = 0;
    n_deferred = 0;
    last_error[0] = 0;
    nav_requested = 0;
    url_changed = 0;
    current_script = NULL;
    total_js_bytes = 0;
}

/* ------------------------------------------------------------------ вход из браузера */

void dom_init(struct dom_host *h) {
    d_host = h;
    js_max_depth = 500;
    js_heap_limit = 24u << 20;
}

struct load_args { const char *html; int len; };

static void load_body(void *arg) {
    struct load_args *a = arg;
    d_document = dn_new(DN_DOCUMENT, NULL);
    domjs_init_realm();
    dn_wrap(d_document);
    dom_parse_document(a->html, a->len, dom_scripting);
    int prevented;
    for (int i = 0; i < n_deferred; i++) run_external(deferred[i]);
    n_deferred = 0;
    if (dom_scripting) {
        domjs_fire(d_document, "readystatechange", 0, 0, &prevented);
        domjs_fire(d_document, "DOMContentLoaded", 1, 0, &prevented);
        domjs_fire(d_window, "load", 0, 0, &prevented);
        js_run_jobs();
    }
}

void dom_load(const char *html, int len, const char *url) {
    js_reset();
    dom_free_all();
    domjs_reset();
    snprintf(d_url, sizeof(d_url), "%s", url);
    d_window = dn_new(DN_WINDOW, NULL);
    struct load_args a = { html, len };
    big_call(load_body, &a);
    js_gc();
    dom_dirty = 1;
}

struct ev_args { int node, kind, key; const char *value; int result; };

static void event_body(void *arg) {
    struct ev_args *a = arg;
    if (a->node < 0 || a->node >= d_count) return;
    dnode *n = d_nodes[a->node];
    js_deadline_ms = js_now_ms() + 5000;
    int prevented = 0;
    switch (a->kind) {
    case 0:                                                    /* щелчок */
        domjs_fire(n, "mousedown", 1, 1, &prevented);
        domjs_fire(n, "mouseup", 1, 1, &prevented);
        domjs_fire(n, "click", 1, 1, &prevented);
        if (!prevented && n->type == DN_ELEMENT && !strcmp(n->tag, "a")) {
            const char *h = dn_get_attr(n, "href");
            if (h && !strncmp(h, "javascript:", 11)) {
                jsval r;
                if (js_eval(h + 11, strlen(h + 11), "javascript: link", &r) < 0) report_error("link");
                prevented = 1;
            }
        }
        if (!prevented && n->type == DN_ELEMENT && (!strcmp(n->tag, "button") || !strcmp(n->tag, "input"))) {
            const char *t = dn_get_attr(n, "type");
            int submit = !strcmp(n->tag, "button") ? (!t || !strcmp(t, "submit")) : (t && (!strcmp(t, "submit") || !strcmp(t, "image")));
            dnode *f = n->parent;
            while (f && !(f->type == DN_ELEMENT && !strcmp(f->tag, "form"))) f = f->parent;
            if (!submit || !f) prevented = 1;                  /* обычная кнопка: только событие */
            else {
                domjs_fire(f, "submit", 1, 1, &prevented);
                if (!prevented) {
                    jsval r;
                    if (Form_submit(dn_wrap(f), 0, NULL, &r, NULL) < 0) report_error("submit");
                    prevented = 1;
                }
            }
        }
        break;
    case 1: domjs_fire(n, "submit", 1, 1, &prevented); break;
    case 2: {                                                  /* набрали текст */
        jsval kv;
        jsval ev = make_event("keydown", 1, 1, "KeyboardEvent");
        char k[8] = { (char)a->key, 0 };
        const char *kn = a->key == '\r' || a->key == '\n' ? "Enter" : a->key == 8 ? "Backspace" : a->key == 27 ? "Escape" : k;
        js_set_c(ev, "key", js_cstr(kn));
        js_set_c(ev, "keyCode", js_num(a->key == '\n' ? 13 : toupper(a->key)));
        js_set_c(ev, "which", js_num(a->key == '\n' ? 13 : toupper(a->key)));
        prevented = dispatch(n, ev);
        if (a->value) {
            int save = dom_dirty;                              /* сам набор текста раскладку не меняет */
            dn_set_attr(n, "value", a->value);
            dom_dirty = save;
            domjs_fire(n, "input", 1, 0, &prevented);
            prevented = 0;
        }
        ev = make_event("keyup", 1, 1, "KeyboardEvent");
        js_set_c(ev, "key", js_cstr(kn));
        js_set_c(ev, "keyCode", js_num(a->key == '\n' ? 13 : toupper(a->key)));
        dispatch(n, ev);
        (void)kv;
        break;
    }
    }
    js_run_jobs();
    js_deadline_ms = 0;
    a->result = prevented;
}

static int run_event(int node, int kind, int key, const char *value) {
    if (!dom_scripting) return 0;
    struct ev_args a = { node, kind, key, value, 0 };
    big_call(event_body, &a);
    if (js_alloc_since_gc > (4u << 20)) js_gc();
    return a.result;
}

int dom_click(int node) { return run_event(node, 0, 0, NULL); }
int dom_submit(int node) { return run_event(node, 1, 0, NULL); }

void dom_key(int node, int key, const char *value) {
    if (node < 0 || node >= d_count) return;
    if (!dom_scripting) {                                      /* без JS — просто запомнить значение */
        if (value) { int save = dom_dirty; dn_set_attr(d_nodes[node], "value", value); dom_dirty = save; }
        return;
    }
    run_event(node, 2, key, value);
}

int dom_pending_nav(char *url, int max) {
    if (!nav_requested) return 0;
    nav_requested = 0;
    snprintf(url, max, "%s", pending_nav);
    return 1;
}

void dom_set_url(const char *url) { snprintf(d_url, sizeof(d_url), "%s", url); }
void dom_get_url(char *out, int max) { snprintf(out, max, "%s", d_url); url_changed = 0; }
int dom_url_changed(void) { return url_changed; }

void dom_stats(char *out, int max) {
    if (!dom_scripting) { snprintf(out, max, "JS off"); return; }
    if (n_errors) snprintf(out, max, "JS: %d script%s, %d error%s", n_scripts, n_scripts == 1 ? "" : "s", n_errors, n_errors == 1 ? "" : "s");
    else snprintf(out, max, "JS: %d script%s", n_scripts, n_scripts == 1 ? "" : "s");
}

const char *dom_last_error(void) { return last_error; }
