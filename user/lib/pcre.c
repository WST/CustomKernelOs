/* pcre.c — небольшой движок классического PCRE.
 * Жадные квантификаторы и обратный ход. Захват групп пишется в ovector.
 * Пустое совпадение атома не крутится бесконечно. */
#include "myos.h"
#include "stdlib.h"
#include "string.h"
#include "pcre.h"

void *(*pcre_malloc)(size_t) = malloc;
void (*pcre_free)(void *) = free;

enum { N_END, N_CHAR, N_ANY, N_CLASS, N_BOL, N_EOL, N_ALT, N_REP, N_CAP };

struct node {
    int kind, ch, min, max, cap, neg;
    unsigned char bits[32];
    struct node *a, *b, *next;
};

struct pcre {
    struct node *root;
    int ncap, caseless, dotall, multiline;
    struct node *nodes;
    int nnodes, cap_nodes;
};

struct parse {
    const char *s, *begin;
    struct pcre *re;
    int cap;
    const char *error;
    int erroff;
};

/* Узлы живут в одном блоке с pcre: realloc сдвинул бы уже записанные указатели. */
static struct node *node_new(struct parse *p, int kind) {
    struct pcre *re = p->re;
    struct node *n;
    if (re->nnodes >= re->cap_nodes) { p->error = "pattern too large"; return NULL; }
    n = &re->nodes[re->nnodes++];
    memset(n, 0, sizeof *n);
    n->kind = kind;
    n->max = -1;
    return n;
}

static void bit_set(struct node *n, int c) { n->bits[c >> 3] |= (unsigned char)(1u << (c & 7)); }
static int bit_get(const struct node *n, int c) { return (n->bits[c >> 3] >> (c & 7)) & 1; }

static int fold(int caseless, int c) { return caseless ? tolower((unsigned char)c) : (unsigned char)c; }

static struct node *parse_alt(struct parse *p);

static int parse_class(struct parse *p, struct node *n) {
    int neg = 0;
    if (*p->s == '^') { neg = 1; p->s++; }
    if (*p->s == ']') { bit_set(n, ']'); p->s++; }
    while (*p->s && *p->s != ']') {
        int a, b;
        if (*p->s == '\\' && p->s[1]) {
            p->s++;
            if (*p->s == 'd') { int c; for (c = '0'; c <= '9'; c++) bit_set(n, c); p->s++; continue; }
            if (*p->s == 'w') {
                int c;
                for (c = '0'; c <= '9'; c++) bit_set(n, c);
                for (c = 'A'; c <= 'Z'; c++) bit_set(n, c);
                for (c = 'a'; c <= 'z'; c++) bit_set(n, c);
                bit_set(n, '_');
                p->s++; continue;
            }
            if (*p->s == 's') { bit_set(n, ' '); bit_set(n, '\t'); bit_set(n, '\n'); bit_set(n, '\r'); bit_set(n, '\f'); p->s++; continue; }
            if (*p->s == 'n') a = '\n';
            else if (*p->s == 't') a = '\t';
            else if (*p->s == 'r') a = '\r';
            else a = (unsigned char)*p->s;
            p->s++;
        } else a = (unsigned char)*p->s++;
        if (*p->s == '-' && p->s[1] && p->s[1] != ']') {
            p->s++;
            b = (unsigned char)*p->s++;
            if (b < a) { int t = a; a = b; b = t; }
            while (a <= b) bit_set(n, a++);
        } else bit_set(n, a);
    }
    if (*p->s != ']') { p->error = "missing ]"; p->erroff = (int)(p->s - p->begin); return -1; }
    p->s++;
    n->neg = neg;
    if (p->re->caseless) {
        int c;
        for (c = 'A'; c <= 'Z'; c++) if (bit_get(n, c)) bit_set(n, c + 32);
        for (c = 'a'; c <= 'z'; c++) if (bit_get(n, c)) bit_set(n, c - 32);
    }
    return 0;
}

static struct node *parse_atom(struct parse *p) {
    struct node *n;
    if (!*p->s) { p->error = "empty atom"; return NULL; }
    if (*p->s == '(') {
        int cap;
        p->s++;
        if (*p->s == '?') {
            p->s++;
            if (*p->s != ':') { p->error = "unsupported group"; p->erroff = (int)(p->s - p->begin); return NULL; }
            p->s++;
            cap = 0;
        } else cap = ++p->cap;
        n = cap ? node_new(p, N_CAP) : NULL;
        if (cap && !n) return NULL;
        {
            struct node *inner = parse_alt(p);
            if (p->error) return NULL;
            if (*p->s != ')') { p->error = "missing )"; p->erroff = (int)(p->s - p->begin); return NULL; }
            p->s++;
            if (!cap) return inner;
            n->a = inner;
            n->cap = cap;
            return n;
        }
    }
    if (*p->s == '^') { p->s++; return node_new(p, N_BOL); }
    if (*p->s == '$') { p->s++; return node_new(p, N_EOL); }
    if (*p->s == '.') { p->s++; return node_new(p, N_ANY); }
    if (*p->s == '[') { p->s++; n = node_new(p, N_CLASS); if (!n || parse_class(p, n) < 0) return NULL; return n; }
    if (*p->s == '\\' && p->s[1]) {
        char e = p->s[1];
        p->s += 2;
        if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
            n = node_new(p, N_CLASS);
            if (!n) return NULL;
            if (e == 'd' || e == 'D') { int c; for (c = '0'; c <= '9'; c++) bit_set(n, c); }
            else if (e == 's' || e == 'S') { bit_set(n, ' '); bit_set(n, '\t'); bit_set(n, '\n'); bit_set(n, '\r'); bit_set(n, '\f'); }
            else {
                int c;
                for (c = '0'; c <= '9'; c++) bit_set(n, c);
                for (c = 'A'; c <= 'Z'; c++) bit_set(n, c);
                for (c = 'a'; c <= 'z'; c++) bit_set(n, c);
                bit_set(n, '_');
            }
            n->neg = e == 'D' || e == 'W' || e == 'S';
            return n;
        }
        n = node_new(p, N_CHAR);
        if (!n) return NULL;
        n->ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : (unsigned char)e;
        return n;
    }
    if (*p->s == '*' || *p->s == '+' || *p->s == '?' || *p->s == ')' || *p->s == '|') {
        p->error = "nothing to repeat";
        p->erroff = (int)(p->s - p->begin);
        return NULL;
    }
    n = node_new(p, N_CHAR);
    if (!n) return NULL;
    n->ch = (unsigned char)*p->s++;
    return n;
}

static int parse_num(struct parse *p, int *out) {
    int v = 0, any = 0;
    while (*p->s >= '0' && *p->s <= '9') { v = v * 10 + (*p->s - '0'); any = 1; p->s++; if (v > 1000) return -1; }
    if (!any) return -1;
    *out = v;
    return 0;
}

static struct node *apply_rep(struct parse *p, struct node *atom) {
    int min = 0, max = 0, inf = 0;
    if (*p->s == '*') { min = 0; inf = 1; p->s++; }
    else if (*p->s == '+') { min = 1; inf = 1; p->s++; }
    else if (*p->s == '?') { min = 0; max = 1; p->s++; }
    else if (*p->s == '{') {
        const char *save = p->s;
        p->s++;
        if (parse_num(p, &min) < 0) { p->s = save; return atom; }
        if (*p->s == '}') { max = min; p->s++; }
        else if (*p->s == ',') {
            p->s++;
            if (*p->s == '}') { inf = 1; p->s++; }
            else if (parse_num(p, &max) < 0 || *p->s != '}' || max < min) { p->s = save; return atom; }
            else p->s++;
        } else { p->s = save; return atom; }
    } else return atom;
    {
        struct node *r = node_new(p, N_REP);
        if (!r) return NULL;
        r->a = atom;
        r->min = min;
        r->max = inf ? -1 : max;
        return r;
    }
}

static struct node *parse_concat(struct parse *p) {
    struct node *head = NULL, *tail = NULL;
    while (*p->s && *p->s != ')' && *p->s != '|') {
        struct node *a = parse_atom(p);
        if (!a) return NULL;
        a = apply_rep(p, a);
        if (!a) return NULL;
        if (!head) head = tail = a;
        else { tail->next = a; tail = a; }
    }
    return head;
}

static struct node *parse_alt(struct parse *p) {
    struct node *left = parse_concat(p);
    if (p->error) return NULL;
    if (*p->s != '|') return left;
    p->s++;
    {
        struct node *alt = node_new(p, N_ALT);
        if (!alt) return NULL;
        alt->a = left;
        alt->b = parse_alt(p);
        return alt;
    }
}

static int class_ok(const struct node *n, int ch) {
    int on = bit_get(n, ch & 255);
    return n->neg ? !on : on;
}

/* Совпадение узла и его next. Возвращает указатель после совпадения или NULL. */
static const char *eat(const struct node *n, const char *s, const char *end, const char *p, int *ov, int nov, int caseless, int dotall, int multi) {
    const char *q;
    if (!n) return p;
    if (n->kind == N_CHAR) {
        if (p >= end || fold(caseless, (unsigned char)*p) != fold(caseless, n->ch)) return NULL;
        return eat(n->next, s, end, p + 1, ov, nov, caseless, dotall, multi);
    }
    if (n->kind == N_ANY) {
        if (p >= end || (*p == '\n' && !dotall)) return NULL;
        return eat(n->next, s, end, p + 1, ov, nov, caseless, dotall, multi);
    }
    if (n->kind == N_CLASS) {
        if (p >= end || !class_ok(n, (unsigned char)*p)) return NULL;
        return eat(n->next, s, end, p + 1, ov, nov, caseless, dotall, multi);
    }
    if (n->kind == N_BOL) {
        if (p != s && !(multi && p > s && p[-1] == '\n')) return NULL;
        return eat(n->next, s, end, p, ov, nov, caseless, dotall, multi);
    }
    if (n->kind == N_EOL) {
        if (p != end && !(multi && *p == '\n')) return NULL;
        return eat(n->next, s, end, p, ov, nov, caseless, dotall, multi);
    }
    if (n->kind == N_ALT) {
        /* Левая ветка не окончательна: если хвост не сошёлся, пробуем правую. */
        q = eat(n->a, s, end, p, ov, nov, caseless, dotall, multi);
        if (q) {
            const char *r = eat(n->next, s, end, q, ov, nov, caseless, dotall, multi);
            if (r) return r;
        }
        q = eat(n->b, s, end, p, ov, nov, caseless, dotall, multi);
        if (!q) return NULL;
        return eat(n->next, s, end, q, ov, nov, caseless, dotall, multi);
    }
    if (n->kind == N_CAP) {
        int slot = n->cap * 2;
        int old0 = slot + 1 < nov ? ov[slot] : 0, old1 = slot + 1 < nov ? ov[slot + 1] : 0;
        q = eat(n->a, s, end, p, ov, nov, caseless, dotall, multi);
        if (!q) return NULL;
        if (slot + 1 < nov) { ov[slot] = (int)(p - s); ov[slot + 1] = (int)(q - s); }
        {
            const char *r = eat(n->next, s, end, q, ov, nov, caseless, dotall, multi);
            if (!r && slot + 1 < nov) { ov[slot] = old0; ov[slot + 1] = old1; }
            return r;
        }
    }
    if (n->kind == N_REP) {
        const char *at[64];
        int count = 0, i, limit;
        at[0] = p;
        limit = n->max < 0 ? 63 : n->max;
        if (limit > 63) limit = 63;
        while (count < limit) {
            q = eat(n->a, s, end, at[count], ov, nov, caseless, dotall, multi);
            if (!q || q == at[count]) break;
            at[++count] = q;
        }
        i = count;
        if (i < n->min) return NULL;
        for (; i >= n->min; i--) {
            q = eat(n->next, s, end, at[i], ov, nov, caseless, dotall, multi);
            if (q) return q;
        }
        return NULL;
    }
    return NULL;
}

pcre *pcre_compile(const char *pattern, int options, const char **errptr, int *erroffset, const unsigned char *tableptr) {
    struct parse ps;
    struct pcre *re;
    (void)tableptr;
    if (errptr) *errptr = NULL;
    if (erroffset) *erroffset = 0;
    if (!pattern) { if (errptr) *errptr = "NULL"; return NULL; }
    re = calloc(1, sizeof *re + 256 * sizeof(struct node));
    if (!re) { if (errptr) *errptr = "memory"; return NULL; }
    re->nodes = (struct node *)(re + 1);
    re->cap_nodes = 256;
    re->caseless = (options & PCRE_CASELESS) != 0;
    re->dotall = (options & PCRE_DOTALL) != 0;
    re->multiline = (options & PCRE_MULTILINE) != 0;
    memset(&ps, 0, sizeof ps);
    ps.s = pattern;
    ps.begin = pattern;
    ps.re = re;
    re->root = parse_alt(&ps);
    if (!ps.error && *ps.s) { ps.error = "trailing junk"; ps.erroff = (int)(ps.s - pattern); }
    if (ps.error) {
        if (errptr) *errptr = ps.error;
        if (erroffset) *erroffset = ps.erroff;
        free(re);
        return NULL;
    }
    re->ncap = ps.cap;
    return re;
}

int pcre_exec(const pcre *code, const pcre_extra *extra, const char *subject, int length, int startoffset,
              int options, int *ovector, int ovecsize) {
    int i;
    (void)extra; (void)options;
    if (!code || !subject || length < 0 || startoffset < 0 || startoffset > length) return PCRE_ERROR_NULL;
    for (i = startoffset; i <= length; i++) {
        int ov[32];
        const char *end;
        int k;
        for (k = 0; k < 32; k++) ov[k] = -1;
        end = eat(code->root, subject, subject + length, subject + i, ov, 32, code->caseless, code->dotall, code->multiline);
        if (end) {
            ov[0] = i;
            ov[1] = (int)(end - subject);
            if (ovector && ovecsize > 0) {
                int n = ovecsize < 32 ? ovecsize : 32;
                for (k = 0; k < n; k++) ovector[k] = ov[k];
            }
            return 1 + code->ncap;
        }
        if (code->root && code->root->kind == N_BOL && !code->multiline) break;
    }
    return PCRE_ERROR_NOMATCH;
}
