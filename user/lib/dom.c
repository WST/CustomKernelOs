/* dom.c — дерево документа: узлы, разбор HTML (с выполнением скриптов по ходу
 * разбора и document.write), обратное превращение в HTML, селекторы CSS
 * (для querySelector). Стили — в css.c.
 * Объекты для JavaScript — в domjs.c. */
#include "domint.h"

dnode *d_document, *d_window, **d_nodes;
int d_count, d_cap;
struct dom_host *d_host;
char d_url[1024];
int d_parsing;
int dom_dirty;
int dom_mut;                       /* счётчик изменений дерева (для кэша стилей) */
int dom_scripting = 1;

/* ------------------------------------------------------------------ узлы */

static char *xstrdup(const char *s, int n) {
    if (n < 0) n = strlen(s);
    char *p = malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

dnode *dn_new(int type, const char *tag) {
    dnode *n = malloc(sizeof(dnode));
    memset(n, 0, sizeof(*n));
    n->type = type;
    if (tag) {
        n->tag = xstrdup(tag, -1);
        for (char *p = n->tag; *p; p++) *p = tolower((ju8)*p);
    }
    if (d_count == d_cap) {
        int cap = d_cap ? d_cap * 2 : 1024;
        dnode **p = malloc(sizeof(dnode *) * cap);
        if (d_count) memcpy(p, d_nodes, sizeof(dnode *) * d_count);
        free(d_nodes);
        d_nodes = p;
        d_cap = cap;
    }
    n->id = d_count;
    d_nodes[d_count++] = n;
    return n;
}

dnode *dn_text(const char *s, int n) {
    dnode *t = dn_new(DN_TEXT, NULL);
    t->text = xstrdup(s, n);
    t->tlen = n;
    return t;
}

void dn_remove(dnode *c) {
    dnode *p = c->parent;
    if (!p) return;
    if (c->prev) c->prev->next = c->next; else p->first = c->next;
    if (c->next) c->next->prev = c->prev; else p->last = c->prev;
    c->parent = c->prev = c->next = NULL;
    dom_dirty = 1; dom_mut++;
}

void dn_insert_before(dnode *p, dnode *c, dnode *ref) {
    if (c == p) return;
    for (dnode *a = p; a; a = a->parent) if (a == c) return;   /* нельзя вставить предка в потомка */
    if (c->type == DN_FRAGMENT) {
        while (c->first) { dnode *k = c->first; dn_remove(k); dn_insert_before(p, k, ref); }
        return;
    }
    dn_remove(c);
    if (ref && ref->parent != p) ref = NULL;
    c->parent = p;
    if (!ref) {
        c->prev = p->last;
        if (p->last) p->last->next = c; else p->first = c;
        p->last = c;
    } else {
        c->next = ref;
        c->prev = ref->prev;
        if (ref->prev) ref->prev->next = c; else p->first = c;
        ref->prev = c;
    }
    dom_dirty = 1; dom_mut++;
}

void dn_append(dnode *p, dnode *c) { dn_insert_before(p, c, NULL); }

int dn_connected(dnode *n) {
    while (n->parent) n = n->parent;
    return n == d_document;
}

const char *dn_get_attr(dnode *n, const char *name) {
    for (int i = 0; i < n->nattr; i++) if (!strcmp(n->attr[2 * i], name)) return n->attr[2 * i + 1];
    return NULL;
}

void dn_set_attr(dnode *n, const char *name, const char *value) {
    dom_dirty = 1; dom_mut++;
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->attr[2 * i], name)) {
            free(n->attr[2 * i + 1]);
            n->attr[2 * i + 1] = xstrdup(value, -1);
            return;
        }
    if (n->nattr == n->acap) {
        int cap = n->acap ? n->acap * 2 : 4;
        char **a = malloc(sizeof(char *) * 2 * cap);
        if (n->nattr) memcpy(a, n->attr, sizeof(char *) * 2 * n->nattr);
        free(n->attr);
        n->attr = a;
        n->acap = cap;
    }
    n->attr[2 * n->nattr] = xstrdup(name, -1);
    for (char *p = n->attr[2 * n->nattr]; *p; p++) *p = tolower((ju8)*p);
    n->attr[2 * n->nattr + 1] = xstrdup(value, -1);
    n->nattr++;
}

void dn_del_attr(dnode *n, const char *name) {
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->attr[2 * i], name)) {
            free(n->attr[2 * i]);
            free(n->attr[2 * i + 1]);
            memmove(&n->attr[2 * i], &n->attr[2 * i + 2], sizeof(char *) * 2 * (n->nattr - i - 1));
            n->nattr--;
            dom_dirty = 1; dom_mut++;
            return;
        }
}

dnode *walk_next(dnode *n, dnode *root) {      /* обход в глубину */
    if (n->first) return n->first;
    while (n && n != root) {
        if (n->next) return n->next;
        n = n->parent;
    }
    return NULL;
}

dnode *dn_by_id(dnode *root, const char *id) {
    for (dnode *n = root->first; n; n = walk_next(n, root))
        if (n->type == DN_ELEMENT) { const char *v = dn_get_attr(n, "id"); if (v && !strcmp(v, id)) return n; }
    return NULL;
}

dnode *dn_find_tag(dnode *root, const char *tag) {
    for (dnode *n = root->first; n; n = walk_next(n, root))
        if (n->type == DN_ELEMENT && !strcmp(n->tag, tag)) return n;
    return NULL;
}

dnode *dn_clone(dnode *n, int deep) {
    dnode *c = dn_new(n->type, n->tag);
    if (n->text) { c->text = xstrdup(n->text, n->tlen); c->tlen = n->tlen; }
    for (int i = 0; i < n->nattr; i++) dn_set_attr(c, n->attr[2 * i], n->attr[2 * i + 1]);
    c->script_done = n->script_done;
    if (deep) for (dnode *k = n->first; k; k = k->next) dn_append(c, dn_clone(k, 1));
    return c;
}

/* текст поддерева; без рекурсии — дерево может быть глубиной в тысячи узлов */
void dn_text_content(dnode *root, jsbuf *b, int inner_text) {
    dnode *n = root;
    for (;;) {
        int down = 0;
        if (n->type == DN_TEXT) jb_add(b, n->text, n->tlen);
        else if (n->type == DN_COMMENT) ;
        else if (inner_text && n->type == DN_ELEMENT && (!strcmp(n->tag, "script") || !strcmp(n->tag, "style"))) ;
        else if (inner_text && n->type == DN_ELEMENT && !strcmp(n->tag, "br")) jb_addc(b, '\n');
        else down = 1;
        if (down && n->first) { n = n->first; continue; }
        while (n != root && !n->next) n = n->parent;
        if (n == root) return;
        n = n->next;
    }
}

void dn_set_text(dnode *n, const char *s, int len) {
    if (n->type == DN_TEXT || n->type == DN_COMMENT) {
        free(n->text);
        n->text = xstrdup(s, len);
        n->tlen = len;
        dom_dirty = 1; dom_mut++;
        return;
    }
    while (n->first) dn_remove(n->first);
    if (len) dn_append(n, dn_text(s, len));
    dom_dirty = 1; dom_mut++;
}

int dn_has_class(dnode *n, const char *cls, int len) {
    const char *c = dn_get_attr(n, "class");
    if (!c) return 0;
    while (*c) {
        while (*c && isspace((ju8)*c)) c++;
        const char *st = c;
        while (*c && !isspace((ju8)*c)) c++;
        if (c - st == len && !strncmp(st, cls, len)) return 1;
    }
    return 0;
}

static const char *void_tags[] = { "area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta",
                                   "param", "source", "track", "wbr", "keygen", NULL };
int dn_is_void(const char *tag) {
    for (int i = 0; void_tags[i]; i++) if (!strcmp(tag, void_tags[i])) return 1;
    return 0;
}

static int in_list(const char *tag, const char *list) {   /* " a b c " */
    int l = strlen(tag);
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *st = p;
        while (*p && *p != ' ') p++;
        if (p - st == l && !strncmp(st, tag, l)) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ сущности */

static const struct { const char *n; unsigned cp; } ents[] = {
    { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' }, { "nbsp", 0xA0 },
    { "copy", 0xA9 }, { "reg", 0xAE }, { "laquo", 0xAB }, { "raquo", 0xBB }, { "mdash", 0x2014 },
    { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "bull", 0x2022 }, { "middot", 0xB7 }, { "lsquo", 0x2018 },
    { "rsquo", 0x2019 }, { "ldquo", 0x201C }, { "rdquo", 0x201D }, { "bdquo", 0x201E }, { "deg", 0xB0 },
    { "rarr", 0x2192 }, { "larr", 0x2190 }, { "uarr", 0x2191 }, { "darr", 0x2193 }, { "euro", 0x20AC },
    { "times", 0xD7 }, { "divide", 0xF7 }, { "eacute", 0xE9 }, { "egrave", 0xE8 }, { "agrave", 0xE0 },
    { "auml", 0xE4 }, { "ouml", 0xF6 }, { "uuml", 0xFC }, { "szlig", 0xDF }, { "ccedil", 0xE7 },
    { "ntilde", 0xF1 }, { "pound", 0xA3 }, { "yen", 0xA5 }, { "cent", 0xA2 }, { "sect", 0xA7 },
    { "para", 0xB6 }, { "plusmn", 0xB1 }, { "frac12", 0xBD }, { "trade", 0x2122 }, { "shy", 0xAD },
    { "zwnj", 0x200C }, { "zwj", 0x200D }, { "thinsp", 0x2009 }, { "ensp", 0x2002 }, { "emsp", 0x2003 },
    { "minus", 0x2212 }, { "infin", 0x221E }, { "ne", 0x2260 }, { "le", 0x2264 }, { "ge", 0x2265 },
    { "check", 0x2713 }, { "hearts", 0x2665 }, { "star", 0x2606 }, { "numero", 0x2116 }, { "iexcl", 0xA1 },
    { "iquest", 0xBF }, { "acute", 0xB4 }, { "uml", 0xA8 }, { "micro", 0xB5 }, { "sup2", 0xB2 },
};

static void put_utf8(jsbuf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = cp; jb_add(b, t, 1); }
    else if (cp < 0x800) { t[0] = 0xC0 | (cp >> 6); t[1] = 0x80 | (cp & 63); jb_add(b, t, 2); }
    else if (cp < 0x10000) { t[0] = 0xE0 | (cp >> 12); t[1] = 0x80 | ((cp >> 6) & 63); t[2] = 0x80 | (cp & 63); jb_add(b, t, 3); }
    else { t[0] = 0xF0 | (cp >> 18); t[1] = 0x80 | ((cp >> 12) & 63); t[2] = 0x80 | ((cp >> 6) & 63); t[3] = 0x80 | (cp & 63); jb_add(b, t, 4); }
}

void html_decode(const char *s, int n, jsbuf *out) {
    for (int i = 0; i < n;) {
        if (s[i] != '&') {
            int j = i;
            while (j < n && s[j] != '&') j++;
            jb_add(out, s + i, j - i);
            i = j;
            continue;
        }
        int j = i + 1;
        while (j < n && j - i < 12 && (isalnum((ju8)s[j]) || s[j] == '#')) j++;
        unsigned cp = 0;
        int ok = 0;
        if (j > i + 1 && s[i + 1] == '#') {
            if (j > i + 2 && (s[i + 2] == 'x' || s[i + 2] == 'X')) {
                for (int k = i + 3; k < j; k++) { int c = tolower((ju8)s[k]); cp = cp * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10); }
                ok = j > i + 3;
            } else {
                for (int k = i + 2; k < j; k++) cp = cp * 10 + (s[k] - '0');
                ok = j > i + 2;
            }
            if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
            if (cp >= 0x80 && cp <= 0x9F) cp = cp == 0x96 ? 0x2013 : cp == 0x97 ? 0x2014 : cp == 0x85 ? 0x2026 : cp;   /* cp1252 */
        } else {
            for (unsigned k = 0; k < sizeof(ents) / sizeof(ents[0]); k++)
                if ((int)strlen(ents[k].n) == j - i - 1 && !strncmp(ents[k].n, s + i + 1, j - i - 1)) { cp = ents[k].cp; ok = 1; break; }
        }
        if (!ok) { jb_addc(out, '&'); i++; continue; }
        put_utf8(out, cp);
        i = (j < n && s[j] == ';') ? j + 1 : j;
    }
}

/* ------------------------------------------------------------------ разбор HTML */

/* Состояние разбора: вход — изменяемый буфер (document.write вставляет текст
 * прямо в место, где сейчас читаем). Стек открытых элементов. */
struct hparser {
    char *s;
    int n, cap, pos;
    dnode *stack[256];
    int sp;
    int in_body, run_scripts;
    dnode *head, *body, *html;
};
static struct hparser *hp;

void html_write(const char *s, int n) {
    if (!hp || n <= 0) return;
    if (hp->n + n + 1 > hp->cap) {
        int cap = (hp->n + n + 1) * 2;
        char *p = malloc(cap);
        memcpy(p, hp->s, hp->n);
        free(hp->s);
        hp->s = p;
        hp->cap = cap;
    }
    memmove(hp->s + hp->pos + n, hp->s + hp->pos, hp->n - hp->pos);
    memcpy(hp->s + hp->pos, s, n);
    hp->n += n;
    hp->s[hp->n] = 0;
}

static dnode *top(void) { return hp->stack[hp->sp - 1]; }
static void push(dnode *n) { if (hp->sp < 256) hp->stack[hp->sp++] = n; }

static void add_text(const char *s, int n) {
    if (n <= 0) return;
    jsbuf b = { 0 };
    html_decode(s, n, &b);
    dnode *t = top();
    if (t->last && t->last->type == DN_TEXT) {                /* склеить с предыдущим текстом */
        dnode *x = t->last;
        char *p = malloc(x->tlen + b.n + 1);
        memcpy(p, x->text, x->tlen);
        memcpy(p + x->tlen, b.p, b.n);
        p[x->tlen + b.n] = 0;
        free(x->text);
        x->text = p;
        x->tlen += b.n;
    } else dn_append(t, dn_text(b.p ? b.p : "", b.n));
    jb_free(&b);
}

static void to_body(void) {
    if (hp->in_body) return;
    hp->in_body = 1;
    if (hp->body) { hp->sp = 0; push(hp->html); push(hp->body); }
}

/* закрыть открытые элементы до тега name (если он есть до «границы») */
static int pop_to(const char *name, const char *stop) {
    for (int i = hp->sp - 1; i > 0; i--) {
        dnode *e = hp->stack[i];
        if (e->type == DN_ELEMENT && !strcmp(e->tag, name)) { hp->sp = i; return 1; }
        if (stop && e->type == DN_ELEMENT && in_list(e->tag, stop)) return 0;
        if (e == hp->body || e == hp->head) return 0;
    }
    return 0;
}

static const char *closes_p = "address article aside blockquote center details dialog dir div dl fieldset figcaption figure "
                              "footer form h1 h2 h3 h4 h5 h6 header hgroup hr main menu nav ol p pre section table ul li dd dt";
static const char *head_tags = "title meta link style script base noscript template";
static const char *raw_tags = "script style textarea title xmp noscript iframe noembed noframes plaintext";

static void run_parser_script(dnode *e) {
    if (!hp->run_scripts) { e->script_done = 1; return; }     /* innerHTML: скрипты не выполняются */
    if (e->script_done) return;
    e->script_done = 1;
    e->parser_inserted = 1;
    domjs_script_inserted(e);
}

static void start_tag(const char *t, int n) {
    char name[32];
    int k = 0;
    while (k < n && k < 31 && (isalnum((ju8)t[k]) || t[k] == '-' || t[k] == ':' || t[k] == '_')) { name[k] = tolower((ju8)t[k]); k++; }
    name[k] = 0;
    if (!name[0]) return;
    int self_close = n > 0 && t[n - 1] == '/';
    dnode *e;
    if (!strcmp(name, "html") || !strcmp(name, "head") || !strcmp(name, "body")) {
        e = !strcmp(name, "html") ? hp->html : !strcmp(name, "head") ? hp->head : hp->body;
        if (!e) return;
        if (!strcmp(name, "body")) to_body();
    } else {
        if (!hp->in_body && !in_list(name, head_tags)) to_body();
        if (in_list(name, closes_p)) pop_to("p", "table td th button");
        if (!strcmp(name, "li")) pop_to("li", "ul ol");
        if (!strcmp(name, "dt") || !strcmp(name, "dd")) { if (!pop_to("dt", "dl")) pop_to("dd", "dl"); }
        if (!strcmp(name, "option") && top()->tag && !strcmp(top()->tag, "option")) hp->sp--;
        if (!strcmp(name, "optgroup")) pop_to("optgroup", "select");
        if (!strcmp(name, "tr")) pop_to("tr", "table");
        if (!strcmp(name, "td") || !strcmp(name, "th")) { if (!pop_to("td", "tr table")) pop_to("th", "tr table"); }
        if (!strcmp(name, "thead") || !strcmp(name, "tbody") || !strcmp(name, "tfoot")) {
            if (!pop_to("tbody", "table")) if (!pop_to("thead", "table")) pop_to("tfoot", "table");
        }
        if (!strcmp(name, "a")) pop_to("a", NULL);
        e = dn_new(DN_ELEMENT, name);
        dn_append(top(), e);
    }
    /* атрибуты */
    int i = k;
    while (i < n) {
        while (i < n && (isspace((ju8)t[i]) || t[i] == '/')) i++;
        int as = i;
        while (i < n && !isspace((ju8)t[i]) && t[i] != '=' && t[i] != '>' && !(t[i] == '/' && i == n - 1)) i++;
        int ae = i;
        if (ae == as) { i++; continue; }
        while (i < n && isspace((ju8)t[i])) i++;
        jsbuf v = { 0 };
        if (i < n && t[i] == '=') {
            i++;
            while (i < n && isspace((ju8)t[i])) i++;
            if (i < n && (t[i] == '"' || t[i] == '\'')) {
                char q = t[i++];
                int vs = i;
                while (i < n && t[i] != q) i++;
                html_decode(t + vs, i - vs, &v);
                i++;
            } else {
                int vs = i;
                while (i < n && !isspace((ju8)t[i])) i++;
                if (i == n && i > vs && t[i - 1] == '/' && self_close) i--;
                html_decode(t + vs, i - vs, &v);
            }
        }
        char an[64];
        int al = ae - as < 63 ? ae - as : 63;
        memcpy(an, t + as, al);
        an[al] = 0;
        for (char *p = an; *p; p++) *p = tolower((ju8)*p);
        if (!dn_get_attr(e, an)) {
            jb_addc(&v, 0);
            dn_set_attr(e, an, v.p);
        }
        jb_free(&v);
    }
    if (e == hp->html || e == hp->head || e == hp->body) return;
    if (dn_is_void(name)) return;
    if (self_close && strcmp(name, "script")) {
        if (!in_list(name, "div span p a li td tr ul ol table section")) return;   /* <svg .../> и т. п. */
    }
    if (in_list(name, raw_tags) && !(!strcmp(name, "noscript") && !dom_scripting)) {
        /* содержимое до </name> — как есть */
        int l = strlen(name), j = hp->pos;
        while (j < hp->n) {
            if (hp->s[j] == '<' && hp->s[j + 1] == '/' && j + 2 + l <= hp->n) {
                int m = 1;
                for (int q = 0; q < l; q++) if (tolower((ju8)hp->s[j + 2 + q]) != name[q]) { m = 0; break; }
                if (m) break;
            }
            j++;
        }
        int cs = hp->pos, ce = j;
        while (j < hp->n && hp->s[j] != '>') j++;
        hp->pos = j < hp->n ? j + 1 : j;
        if (ce > cs) {
            if (!strcmp(name, "textarea") || !strcmp(name, "title")) {
                jsbuf b = { 0 };
                html_decode(hp->s + cs, ce - cs, &b);
                dn_append(e, dn_text(b.p ? b.p : "", b.n));
                jb_free(&b);
            } else dn_append(e, dn_text(hp->s + cs, ce - cs));
        }
        if (!strcmp(name, "script")) run_parser_script(e);
        return;
    }
    if (!strcmp(name, "script")) { run_parser_script(e); return; }
    push(e);
}

static void end_tag(const char *t, int n) {
    char name[32];
    int k = 0;
    while (k < n && k < 31 && (isalnum((ju8)t[k]) || t[k] == '-' || t[k] == ':' || t[k] == '_')) { name[k] = tolower((ju8)t[k]); k++; }
    name[k] = 0;
    if (!strcmp(name, "html") || !strcmp(name, "body")) return;
    if (!strcmp(name, "head")) { to_body(); return; }
    if (!strcmp(name, "p") && !pop_to("p", "table td th button")) { if (hp->in_body) dn_append(top(), dn_new(DN_ELEMENT, "p")); return; }
    pop_to(name, !strcmp(name, "li") ? "ul ol" : (!strcmp(name, "td") || !strcmp(name, "th") || !strcmp(name, "tr")) ? "table" : NULL);
}

static void parse_loop(void) {
    while (hp->pos < hp->n) {
        char *s = hp->s;
        int i = hp->pos;
        if (s[i] == '<') {
            if (i + 3 < hp->n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {
                int j = i + 4;
                while (j + 2 < hp->n && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
                hp->pos = j + 3 > hp->n ? hp->n : j + 3;
                continue;
            }
            if (i + 1 < hp->n && (s[i + 1] == '!' || s[i + 1] == '?')) {
                int j = i + 1;
                while (j < hp->n && s[j] != '>') j++;
                hp->pos = j + 1;
                continue;
            }
            if (i + 1 < hp->n && (isalpha((ju8)s[i + 1]) || (s[i + 1] == '/' && i + 2 < hp->n && isalpha((ju8)s[i + 2])))) {
                int j = i + 1;
                char q = 0;
                while (j < hp->n && (q || s[j] != '>')) {
                    if (!q && (s[j] == '"' || s[j] == '\'') && j > i + 1 && (s[j - 1] == '=' || isspace((ju8)s[j - 1]))) q = s[j];
                    else if (q && s[j] == q) q = 0;
                    j++;
                }
                hp->pos = j + 1 > hp->n ? hp->n : j + 1;
                if (s[i + 1] == '/') end_tag(s + i + 2, j - i - 2);
                else start_tag(s + i + 1, j - i - 1);
                continue;
            }
        }
        int j = i + 1;
        while (j < hp->n && s[j] != '<') j++;
        if (!hp->in_body) {                                   /* текст до <body>: пробелы — в head, остальное — в body */
            int ws = 1;
            for (int k = i; k < j; k++) if (!isspace((ju8)s[k])) { ws = 0; break; }
            if (ws) { hp->pos = j; continue; }
            to_body();
        }
        hp->pos = j;
        add_text(s + i, j - i);
    }
}

void dom_parse_document(const char *s, int n, int run_scripts) {
    struct hparser p;
    memset(&p, 0, sizeof(p));
    p.cap = n + 1024;
    p.s = malloc(p.cap);
    memcpy(p.s, s, n);
    p.n = n;
    p.s[n] = 0;
    p.run_scripts = run_scripts;
    p.html = dn_new(DN_ELEMENT, "html");
    p.head = dn_new(DN_ELEMENT, "head");
    p.body = dn_new(DN_ELEMENT, "body");
    dn_append(d_document, p.html);
    dn_append(p.html, p.head);
    dn_append(p.html, p.body);
    struct hparser *save = hp;
    hp = &p;
    push(p.html);
    push(p.head);
    d_parsing = 1;
    parse_loop();
    d_parsing = 0;
    hp = save;
    free(p.s);
}

void html_parse_into(dnode *parent, const char *s, int n) {
    struct hparser p;
    memset(&p, 0, sizeof(p));
    p.cap = n + 64;
    p.s = malloc(p.cap);
    memcpy(p.s, s, n);
    p.n = n;
    p.s[n] = 0;
    p.in_body = 1;
    struct hparser *save = hp;
    hp = &p;
    push(parent);
    parse_loop();
    hp = save;
    free(p.s);
    dom_dirty = 1; dom_mut++;
}

/* ------------------------------------------------------------------ обратно в HTML */

static void esc(jsbuf *b, const char *s, int n, int attr) {
    int st = 0;
    for (int i = 0; i < n; i++) {
        const char *r = NULL;
        switch (s[i]) {
        case '&': r = "&amp;"; break;
        case '<': r = "&lt;"; break;
        case '>': r = "&gt;"; break;
        case '"': if (attr) r = "&quot;"; break;
        case 0: r = ""; break;
        }
        if (r) { jb_add(b, s + st, i - st); jb_adds(b, r); st = i + 1; }
    }
    jb_add(b, s + st, n - st);
}

void html_serialize(jsbuf *b, dnode *n, int outer) {
    if (n->type == DN_TEXT) {
        dnode *p = n->parent;
        if (p && p->type == DN_ELEMENT && (!strcmp(p->tag, "script") || !strcmp(p->tag, "style"))) jb_add(b, n->text, n->tlen);
        else esc(b, n->text, n->tlen, 0);
        return;
    }
    if (n->type == DN_COMMENT) { jb_adds(b, "<!--"); jb_add(b, n->text, n->tlen); jb_adds(b, "-->"); return; }
    if (outer && n->type == DN_ELEMENT) {
        jb_addc(b, '<');
        jb_adds(b, n->tag);
        for (int i = 0; i < n->nattr; i++) {
            jb_addc(b, ' ');
            jb_adds(b, n->attr[2 * i]);
            jb_adds(b, "=\"");
            esc(b, n->attr[2 * i + 1], strlen(n->attr[2 * i + 1]), 1);
            jb_addc(b, '"');
        }
        jb_addc(b, '>');
        if (dn_is_void(n->tag)) return;
    }
    static int depth;
    if (depth > 1500) {                                        /* очень глубокое дерево: дальше только текст */
        jsbuf tb = { 0 };
        dn_text_content(n, &tb, 0);
        esc(b, tb.p ? tb.p : "", tb.n, 0);
        jb_free(&tb);
    } else {
        depth++;
        for (dnode *k = n->first; k; k = k->next) html_serialize(b, k, 1);
        depth--;
    }
    if (outer && n->type == DN_ELEMENT) { jb_adds(b, "</"); jb_adds(b, n->tag); jb_addc(b, '>'); }
}

/* --- для раскладки браузера: без скриптов и скрытого, с номерами узлов */

extern int domjs_has_click(dnode *n);

static const char *keep_attrs[] = { "href", "type", "name", "value", "size", "alt", "action", "method", "src", "checked",
                                     "selected", "charset", "content", "http-equiv", "colspan", "start",
                                     "width", "height", "data-src", "data-original", "data-lazy-src", "srcset", NULL };

static void layout_ser(jsbuf *b, dnode *n, int depth) {
    if (n->type == DN_TEXT) { esc(b, n->text, n->tlen, 0); return; }
    if (depth > 600) {                                         /* слишком глубоко: дальше только текст */
        jsbuf tb = { 0 };
        dn_text_content(n, &tb, 1);
        esc(b, tb.p ? tb.p : "", tb.n, 0);
        jb_free(&tb);
        return;
    }
    if (n->type != DN_ELEMENT) { for (dnode *k = n->first; k; k = k->next) layout_ser(b, k, depth + 1); return; }
    const char *t = n->tag;
    if (!strcmp(t, "script") || !strcmp(t, "style") || !strcmp(t, "template") || !strcmp(t, "head")) return;
    if (!strcmp(t, "noscript") && dom_scripting) return;
    if (css_hidden(n)) return;                                 /* display:none, [hidden], спрятан стилями */
    if (!strcmp(t,"svg")) {
        jsbuf xml={0};html_serialize(&xml,n,1);
        if(xml.n>=4) {
            const struct dom_style *st=css_style(n);
            char color[40];snprintf(color,sizeof(color),"<svg color=\"#%06x\"",st->color);
            jsbuf colored={0};jb_adds(&colored,color);jb_add(&colored,xml.p+4,xml.n-4);
            static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
            jb_adds(b,"<img src=\"data:image/svg+xml;base64,");
            for(int i=0;i<colored.n;i+=3) {
                unsigned v=(ju8)colored.p[i]<<16;if(i+1<colored.n)v|=(ju8)colored.p[i+1]<<8;if(i+2<colored.n)v|=(ju8)colored.p[i+2];
                jb_addc(b,alphabet[v>>18]);jb_addc(b,alphabet[(v>>12)&63]);
                jb_addc(b,i+1<colored.n?alphabet[(v>>6)&63]:'=');jb_addc(b,i+2<colored.n?alphabet[v&63]:'=');
            }
            char attr[50];snprintf(attr,sizeof(attr),"\" data-n=\"%d\">",n->id);jb_adds(b,attr);jb_free(&colored);
        }
        jb_free(&xml);return;
    }
    char num[40];
    jb_addc(b, '<');
    jb_adds(b, t);
    if ((!strcmp(t, "button") || !strcmp(t, "textarea")) && !dn_get_attr(n, "value")) {   /* надпись / текст — в value */
        jsbuf tb = { 0 };
        dn_text_content(n, &tb, 1);
        int st = 0, en = tb.n;
        while (st < en && isspace((ju8)tb.p[st])) st++;
        while (en > st && isspace((ju8)tb.p[en - 1])) en--;
        jb_adds(b, " value=\"");
        esc(b, tb.p ? tb.p + st : "", en - st, 1);
        jb_addc(b, '"');
        jb_free(&tb);
    }
    for (int i = 0; keep_attrs[i]; i++) {
        const char *v = dn_get_attr(n, keep_attrs[i]);
        if (!v) continue;
        if (!strcmp(keep_attrs[i], "href") && !strncmp(v, "javascript:", 11)) {
            snprintf(num, sizeof(num), "myosjs:%d", n->id);
            v = num;
        }
        jb_addc(b, ' ');
        jb_adds(b, keep_attrs[i]);
        jb_adds(b, "=\"");
        esc(b, v, strlen(v), 1);
        jb_addc(b, '"');
    }
    /* <a> без адреса, но с обработчиком щелчка — тоже ссылка */
    if (!strcmp(t, "a") && !dn_get_attr(n, "href") && domjs_has_click(n)) { snprintf(num, sizeof(num), " href=\"myosjs:%d\"", n->id); jb_adds(b, num); }
    snprintf(num, sizeof(num), " data-n=\"%d\"", n->id);       /* номер узла: щелчок по тексту -> событие этому элементу */
    jb_adds(b, num);
    jb_addc(b, '>');
    if (dn_is_void(t)) return;
    const struct dom_style *button_style=!strcmp(t,"button")?css_style(n):NULL;
    int styled_button=button_style && dom_css && (button_style->flex_row || button_style->bg>=0 || button_style->padding[0] || button_style->padding[1] || button_style->padding[2] || button_style->padding[3]);
    if (strcmp(t, "textarea") && (strcmp(t, "button") || styled_button)) for (dnode *k = n->first; k; k = k->next) layout_ser(b, k, depth + 1);
    jb_adds(b, "</");
    jb_adds(b, t);
    jb_addc(b, '>');
}

struct ser_args { char *p; int len; };
static void serialize_body(void *arg);

/* на большом стеке: обход дерева рекурсивный, а у программы стек всего 64 КБ */
char *dom_serialize(int *len) {
    struct ser_args a = { NULL, 0 };
    js_big_call(serialize_body, &a);
    *len = a.len;
    return a.p;
}

static void serialize_body(void *arg) {
    struct ser_args *a = arg;
    int *len = &a->len;
    jsbuf b = { 0 };
    char title[256];
    dom_title(title, sizeof(title));
    jb_adds(&b, "<meta charset=\"utf-8\"><title>");
    esc(&b, title, strlen(title), 0);
    jb_adds(&b, "</title>");
    dnode *root = dn_find_tag(d_document, "html");          /* <html> и <body> тоже: у них бывает фон */
    if (!root) root = dn_find_tag(d_document, "body");
    layout_ser(&b, root ? root : d_document, 0);
    jb_addc(&b, 0);
    *len = b.n - 1;
    dom_dirty = 0;
    a->p = b.p;
}

void dom_title(char *out, int max) {
    out[0] = 0;
    dnode *t = dn_find_tag(d_document, "title");
    if (!t) return;
    jsbuf b = { 0 };
    dn_text_content(t, &b, 0);
    int o = 0, sp = 1;
    for (int i = 0; i < b.n && o < max - 1; i++) {
        char c = b.p[i];
        if (isspace((ju8)c)) { if (!sp) out[o++] = ' '; sp = 1; continue; }
        out[o++] = c;
        sp = 0;
    }
    while (o > 0 && out[o - 1] == ' ') o--;
    out[o] = 0;
    jb_free(&b);
}

/* ------------------------------------------------------------------ селекторы */

static const char *sp_;

static char *sel_ident(void) {
    const char *st = sp_;
    jsbuf b = { 0 };
    while (*sp_ && (isalnum((ju8)*sp_) || *sp_ == '-' || *sp_ == '_' || (ju8)*sp_ >= 0x80 || *sp_ == '\\')) {
        if (*sp_ == '\\' && sp_[1]) { sp_++; }
        jb_addc(&b, *sp_++);
    }
    (void)st;
    jb_addc(&b, 0);
    return b.p;
}

static int known_pseudo(const char *p) {
    static const char *list = " link visited hover focus active focus-within focus-visible target first-of-type last-of-type "
                              "only-of-type nth-last-child nth-last-of-type required optional read-only read-write "
                              "placeholder-shown default indeterminate valid invalid lang is where has any-link "
                              "fullscreen defined host autofill scope ";
    char t[48];
    if (!p[0] || strlen(p) > 40) return 0;
    snprintf(t, sizeof(t), " %s ", p);
    return strstr(list, t) != NULL;
}

static int parse_simple(struct simple *s) {
    memset(s, 0, sizeof(*s));
    int any = 0;
    if (*sp_ == '*') { sp_++; any = 1; }
    else if (isalpha((ju8)*sp_)) { s->tag = sel_ident(); for (char *p = s->tag; *p; p++) *p = tolower((ju8)*p); any = 1; }
    for (;;) {
        if (*sp_ == '#') { sp_++; free(s->id); s->id = sel_ident(); any = 1; }
        else if (*sp_ == '.') {
            sp_++;
            char **c = malloc(sizeof(char *) * (s->ncls + 1));
            if (s->ncls) memcpy(c, s->cls, sizeof(char *) * s->ncls);
            free(s->cls);
            s->cls = c;
            s->cls[s->ncls++] = sel_ident();
            any = 1;
        } else if (*sp_ == '[') {
            sp_++;
            while (*sp_ == ' ') sp_++;
            struct attrsel a = { 0 };
            a.name = sel_ident();
            for (char *p = a.name; *p; p++) *p = tolower((ju8)*p);
            while (*sp_ == ' ') sp_++;
            if (*sp_ && *sp_ != ']') {
                if (*sp_ != '=') a.op = *sp_++;
                else a.op = '=';
                if (*sp_ == '=') sp_++;
                while (*sp_ == ' ') sp_++;
                jsbuf v = { 0 };
                if (*sp_ == '"' || *sp_ == '\'') {
                    char q = *sp_++;
                    while (*sp_ && *sp_ != q) jb_addc(&v, *sp_++);
                    if (*sp_) sp_++;
                } else while (*sp_ && *sp_ != ']' && *sp_ != ' ') jb_addc(&v, *sp_++);
                jb_addc(&v, 0);
                a.val = v.p;
                while (*sp_ == ' ' || *sp_ == 'i' || *sp_ == 's') sp_++;
            }
            if (*sp_ != ']') return 0;
            sp_++;
            struct attrsel *n = malloc(sizeof(struct attrsel) * (s->nat + 1));
            if (s->nat) memcpy(n, s->at, sizeof(struct attrsel) * s->nat);
            free(s->at);
            s->at = n;
            s->at[s->nat++] = a;
            any = 1;
        } else if (*sp_ == ':') {
            sp_++;
            if (*sp_ == ':') { sp_++; free(sel_ident()); s->pseudo = 9; any = 1; continue; }   /* ::before — никогда */
            char *p = sel_ident();
            if (!strcmp(p, "first-child")) s->pseudo = 1;
            else if (!strcmp(p, "last-child")) s->pseudo = 2;
            else if (!strcmp(p, "only-child")) s->pseudo = 3;
            else if (!strcmp(p, "checked")) s->pseudo = 5;
            else if (!strcmp(p, "disabled")) s->pseudo = 6;
            else if (!strcmp(p, "empty")) s->pseudo = 7;
            else if (!strcmp(p, "root")) s->pseudo = 8;
            else if (!strcmp(p, "nth-child") || !strcmp(p, "nth-of-type")) {
                s->pseudo = 4;
                if (*sp_ == '(') {
                    sp_++;
                    while (*sp_ == ' ') sp_++;
                    if (!strncmp(sp_, "odd", 3)) { s->nth_a = 2; s->nth_b = 1; sp_ += 3; }
                    else if (!strncmp(sp_, "even", 4)) { s->nth_a = 2; s->nth_b = 0; sp_ += 4; }
                    else {
                        int sign = 1, v = 0, has = 0;
                        if (*sp_ == '-') { sign = -1; sp_++; } else if (*sp_ == '+') sp_++;
                        while (isdigit((ju8)*sp_)) { v = v * 10 + (*sp_++ - '0'); has = 1; }
                        if (*sp_ == 'n') {
                            sp_++;
                            s->nth_a = has ? sign * v : sign;
                            while (*sp_ == ' ') sp_++;
                            int bs = 1;
                            if (*sp_ == '+' || *sp_ == '-') { bs = *sp_ == '-' ? -1 : 1; sp_++; }
                            while (*sp_ == ' ') sp_++;
                            v = 0;
                            while (isdigit((ju8)*sp_)) v = v * 10 + (*sp_++ - '0');
                            s->nth_b = bs * v;
                        } else s->nth_b = sign * v;
                    }
                    while (*sp_ && *sp_ != ')') sp_++;
                    if (*sp_) sp_++;
                }
            } else if (!strcmp(p, "not")) {
                if (*sp_ == '(') {
                    sp_++;
                    while (*sp_ == ' ') sp_++;
                    s->not_ = malloc(sizeof(struct simple));
                    if (!parse_simple(s->not_)) { free(p); return 0; }
                    while (*sp_ == ' ') sp_++;
                    if (*sp_ != ')') { free(p); return 0; }
                    sp_++;
                }
            } else if (!strcmp(p, "enabled")) {
                s->pseudo = 10;
            } else if (!strcmp(p, "link") || !strcmp(p, "any-link")) {
                s->pseudo = 11;
            } else if (known_pseudo(p)) {                      /* известный, но не поддерживаем — не совпадает */
                if (*sp_ == '(') { int d = 0; while (*sp_) { if (*sp_ == '(') d++; if (*sp_ == ')' && --d == 0) { sp_++; break; } sp_++; } }
                s->pseudo = 9;
            } else { free(p); return 0; }                      /* неизвестный (:hidden у jQuery) — ошибка, как в браузерах */
            free(p);
            any = 1;
        } else break;
    }
    return any;
}

static void simple_free(struct simple *s) {
    free(s->tag);
    free(s->id);
    for (int i = 0; i < s->ncls; i++) free(s->cls[i]);
    free(s->cls);
    for (int i = 0; i < s->nat; i++) { free(s->at[i].name); free(s->at[i].val); }
    free(s->at);
    if (s->not_) { simple_free(s->not_); free(s->not_); }
}

void sel_free(struct selector *s) {
    if (!s) return;
    for (int a = 0; a < s->nalt; a++) {
        for (int i = 0; i < s->nparts[a]; i++) simple_free(&s->parts[a][i]);
        free(s->parts[a]);
    }
    free(s->parts);
    free(s->nparts);
    free(s);
}

struct selector *sel_parse(const char *str) {
    struct selector *s = malloc(sizeof(struct selector));
    memset(s, 0, sizeof(*s));
    sp_ = str;
    for (;;) {
        while (*sp_ == ' ' || *sp_ == '\n' || *sp_ == '\t') sp_++;
        struct simple *chain = NULL;
        int n = 0;
        char comb = 0;
        for (;;) {
            struct simple tmp;
            if (!parse_simple(&tmp)) { simple_free(&tmp); free(chain); sel_free(s); return NULL; }
            tmp.comb = comb;
            struct simple *c = malloc(sizeof(struct simple) * (n + 1));
            if (n) memcpy(c, chain, sizeof(struct simple) * n);
            free(chain);
            chain = c;
            chain[n++] = tmp;
            int ws = 0;
            while (*sp_ == ' ' || *sp_ == '\n' || *sp_ == '\t') { sp_++; ws = 1; }
            if (*sp_ == '>' || *sp_ == '+' || *sp_ == '~') { comb = *sp_++; while (*sp_ == ' ') sp_++; continue; }
            if (*sp_ == ',' || !*sp_) break;
            if (ws) { comb = ' '; continue; }
            for (int i = 0; i < n; i++) simple_free(&chain[i]);   /* неожиданный символ */
            free(chain);
            sel_free(s);
            return NULL;
        }
        struct simple **pp = malloc(sizeof(void *) * (s->nalt + 1));
        int *nn = malloc(sizeof(int) * (s->nalt + 1));
        if (s->nalt) { memcpy(pp, s->parts, sizeof(void *) * s->nalt); memcpy(nn, s->nparts, sizeof(int) * s->nalt); }
        free(s->parts);
        free(s->nparts);
        s->parts = pp;
        s->nparts = nn;
        s->parts[s->nalt] = chain;
        s->nparts[s->nalt++] = n;
        if (*sp_ != ',') break;
        sp_++;
    }
    return s;
}

static int elem_index(dnode *n, int from_end) {
    int i = 1;
    for (dnode *k = from_end ? n->next : n->prev; k; k = from_end ? k->next : k->prev) if (k->type == DN_ELEMENT) i++;
    return i;
}

static int match_simple(struct simple *s, dnode *n) {
    if (n->type != DN_ELEMENT) return 0;
    if (s->tag && strcmp(s->tag, n->tag)) return 0;
    if (s->id) { const char *v = dn_get_attr(n, "id"); if (!v || strcmp(v, s->id)) return 0; }
    for (int i = 0; i < s->ncls; i++) if (!dn_has_class(n, s->cls[i], strlen(s->cls[i]))) return 0;
    for (int i = 0; i < s->nat; i++) {
        struct attrsel *a = &s->at[i];
        const char *v = dn_get_attr(n, a->name);
        if (!v) return 0;
        if (!a->op) continue;
        int vl = strlen(v), al = strlen(a->val);
        switch (a->op) {
        case '=': if (strcmp(v, a->val)) return 0; break;
        case '^': if (strncmp(v, a->val, al)) return 0; break;
        case '$': if (vl < al || strcmp(v + vl - al, a->val)) return 0; break;
        case '*': if (!strstr(v, a->val)) return 0; break;
        case '~': {
            int ok = 0;
            for (const char *p = v; *p;) {
                while (*p == ' ') p++;
                const char *st = p;
                while (*p && *p != ' ') p++;
                if (p - st == al && !strncmp(st, a->val, al)) ok = 1;
            }
            if (!ok) return 0;
            break;
        }
        case '|': if (strcmp(v, a->val) && !(strncmp(v, a->val, al) == 0 && v[al] == '-')) return 0; break;
        default: return 0;
        }
    }
    switch (s->pseudo) {
    case 1: if (elem_index(n, 0) != 1) return 0; break;
    case 2: if (elem_index(n, 1) != 1) return 0; break;
    case 3: if (elem_index(n, 0) != 1 || elem_index(n, 1) != 1) return 0; break;
    case 4: {
        int i = elem_index(n, 0);
        if (s->nth_a == 0) { if (i != s->nth_b) return 0; }
        else { int d = i - s->nth_b; if (d % s->nth_a != 0 || d / s->nth_a < 0) return 0; }
        break;
    }
    case 5: if (!dn_get_attr(n, "checked") && !dn_get_attr(n, "selected")) return 0; break;
    case 6: if (!dn_get_attr(n, "disabled")) return 0; break;
    case 7: if (n->first) return 0; break;
    case 8: if (!n->parent || n->parent->type != DN_DOCUMENT) return 0; break;
    case 9: return 0;
    case 10: if (dn_get_attr(n, "disabled")) return 0; break;
    case 11: if ((strcmp(n->tag, "a") && strcmp(n->tag, "area")) || !dn_get_attr(n, "href")) return 0; break;
    }
    if (s->not_ && match_simple(s->not_, n)) return 0;
    return 1;
}

static int match_chain(struct simple *c, int i, dnode *n) {
    if (!match_simple(&c[i], n)) return 0;
    if (i == 0) return 1;
    switch (c[i].comb) {
    case '>': return n->parent && match_chain(c, i - 1, n->parent);
    case '+': { dnode *p = n->prev; while (p && p->type != DN_ELEMENT) p = p->prev; return p && match_chain(c, i - 1, p); }
    case '~': for (dnode *p = n->prev; p; p = p->prev) if (p->type == DN_ELEMENT && match_chain(c, i - 1, p)) return 1; return 0;
    default: for (dnode *p = n->parent; p; p = p->parent) if (match_chain(c, i - 1, p)) return 1; return 0;
    }
}

int sel_match(struct selector *s, dnode *n) {
    for (int a = 0; a < s->nalt; a++) if (match_chain(s->parts[a], s->nparts[a] - 1, n)) return 1;
    return 0;
}

void sel_query(dnode *root, struct selector *s, int first_only, dnode ***out, int *count) {
    int cap = 0;
    *out = NULL;
    *count = 0;
    for (dnode *n = root->first; n; n = walk_next(n, root)) {
        if (n->type != DN_ELEMENT || !sel_match(s, n)) continue;
        if (*count == cap) {
            cap = cap ? cap * 2 : 16;
            dnode **p = malloc(sizeof(dnode *) * cap);
            if (*count) memcpy(p, *out, sizeof(dnode *) * *count);
            free(*out);
            *out = p;
        }
        (*out)[(*count)++] = n;
        if (first_only) return;
    }
}

/* ------------------------------------------------------------------ cookie */

struct cookie { char host[96]; char name[64]; char *value; };
static struct cookie jar[200];
static int n_cookies;

void cookie_set(const char *host, const char *h) {
    while (*h == ' ') h++;
    const char *eq = strchr(h, '=');
    if (!eq) return;
    const char *semi = strchr(h, ';');
    if (semi && semi < eq) return;
    char name[64];
    int nl = eq - h < 63 ? eq - h : 63;
    memcpy(name, h, nl);
    name[nl] = 0;
    while (nl > 0 && name[nl - 1] == ' ') name[--nl] = 0;
    const char *v = eq + 1, *ve = semi ? semi : v + strlen(v);
    char dom[96];
    snprintf(dom, sizeof(dom), "%s", host);
    int del = 0;
    for (const char *p = semi; p && *p; p = strchr(p + 1, ';')) {
        const char *a = p + 1;
        while (*a == ' ') a++;
        if (!strncmp(a, "domain=", 7) || !strncmp(a, "Domain=", 7)) {
            a += 7;
            if (*a == '.') a++;
            int l = 0;
            while (a[l] && a[l] != ';' && l < 95) { dom[l] = tolower((ju8)a[l]); l++; }
            dom[l] = 0;
        }
        if ((!strncmp(a, "max-age=", 8) || !strncmp(a, "Max-Age=", 8)) && atoi(a + 8) <= 0 && a[8] != ' ') del = a[8] == '0' || a[8] == '-';
        if ((!strncmp(a, "expires=", 8) || !strncmp(a, "Expires=", 8)) && strstr(a, "1970")) del = 1;
    }
    for (int i = 0; i < n_cookies; i++)
        if (!strcmp(jar[i].name, name) && !strcmp(jar[i].host, dom)) {
            free(jar[i].value);
            if (del) { jar[i] = jar[--n_cookies]; return; }
            jar[i].value = xstrdup(v, ve - v);
            return;
        }
    if (del || n_cookies >= 200) return;
    struct cookie *c = &jar[n_cookies++];
    snprintf(c->host, sizeof(c->host), "%s", dom);
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->value = xstrdup(v, ve - v);
}

void cookie_get(const char *host, char *out, int max) {
    int o = 0;
    out[0] = 0;
    int hl = strlen(host);
    for (int i = 0; i < n_cookies; i++) {
        int cl = strlen(jar[i].host);
        if (strcmp(jar[i].host, host) && !(hl > cl && !strcmp(host + hl - cl, jar[i].host) && host[hl - cl - 1] == '.')) continue;
        o += snprintf(out + o, max - o, "%s%s=%s", o ? "; " : "", jar[i].name, jar[i].value);
        if (o >= max - 1) { out[max - 1] = 0; return; }
    }
}

/* ------------------------------------------------------------------ страница целиком */

void dom_free_all(void) {
    for (int i = 0; i < d_count; i++) {
        dnode *n = d_nodes[i];
        free(n->tag);
        for (int k = 0; k < 2 * n->nattr; k++) free(n->attr[k]);
        free(n->attr);
        free(n->text);
        free(n->ls);
        free(n);
    }
    d_count = 0;
    d_document = d_window = NULL;
    css_reset();
}
