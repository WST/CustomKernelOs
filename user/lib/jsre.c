/* jsre.c — регулярные выражения JS: компиляция шаблона в байт-код и
 * поиск с возвратами (backtracking) на явном стеке, без рекурсии.
 *
 * Поддерживается: . [] [^] \d\w\s\D\W\S\b\B ^ $ | () (?:) (?<имя>) (?=) (?!) (?<=) (?<!)
 * * + ? {n,m} и ленивые *? +? ??, \1..\99, флаги g i m s y u.
 * Строки — байты UTF-8; «.» и классы берут целый символ UTF-8. */
#include "jsint.h"

enum {
    R_CHAR = 1, R_ANY, R_ANYNL, R_CLASS, R_SPLIT, R_JMP, R_SAVE, R_BOL, R_EOL, R_WORDB, R_NWORDB,
    R_BACKREF, R_LOOK, R_MARK, R_CHECK, R_MATCH
};

typedef struct { int *p; int n, cap, nullable; } frag;

static void f_emit(frag *f, int v) {
    if (f->n == f->cap) {
        int cap = f->cap ? f->cap * 2 : 16;
        int *p = malloc(sizeof(int) * cap);
        if (f->n) memcpy(p, f->p, sizeof(int) * f->n);
        free(f->p);
        f->p = p;
        f->cap = cap;
    }
    f->p[f->n++] = v;
}
static void f_append(frag *f, frag *g) { for (int i = 0; i < g->n; i++) f_emit(f, g->p[i]); }
static void f_free(frag *f) { free(f->p); f->p = NULL; f->n = f->cap = 0; }

/* --- разбор шаблона */
static const char *rp;
static int rlen, rpos, ngroups, nregs, rerr, icase, multiline, dotall;
static char rerrmsg[80];

static void re_fail(const char *m) { if (!rerr) { rerr = 1; snprintf(rerrmsg, sizeof(rerrmsg), "%s", m); } }

static frag parse_alt(void);

static unsigned utf8_at(const char *s, int n, int i, int *len) {
    ju8 c = s[i];
    *len = 1;
    if (c < 0xC0) return c;
    int l = c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
    if (i + l > n) return c;
    unsigned cp = c & (l == 2 ? 0x1F : l == 3 ? 0x0F : 0x07);
    for (int k = 1; k < l; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    *len = l;
    return cp;
}

/* диапазоны для \d \w \s и их отрицаний */
static void add_range(frag *f, unsigned lo, unsigned hi) { f_emit(f, lo); f_emit(f, hi); }
static int class_escape(frag *ranges, char c) {
    switch (c) {
    case 'd': add_range(ranges, '0', '9'); return 1;
    case 'D': add_range(ranges, 0, '0' - 1); add_range(ranges, '9' + 1, 0x10FFFF); return 1;
    case 'w': add_range(ranges, '0', '9'); add_range(ranges, 'A', 'Z'); add_range(ranges, 'a', 'z'); add_range(ranges, '_', '_'); return 1;
    case 'W': add_range(ranges, 0, '0' - 1); add_range(ranges, '9' + 1, 'A' - 1); add_range(ranges, 'Z' + 1, '_' - 1);
              add_range(ranges, '_' + 1, 'a' - 1); add_range(ranges, 'z' + 1, 0x10FFFF); return 1;
    case 's': add_range(ranges, 9, 13); add_range(ranges, ' ', ' '); add_range(ranges, 0xA0, 0xA0);
              add_range(ranges, 0x2028, 0x2029); add_range(ranges, 0xFEFF, 0xFEFF); add_range(ranges, 0x2000, 0x200A); return 1;
    case 'S': add_range(ranges, 0, 8); add_range(ranges, 14, 31); add_range(ranges, 33, 0x9F); add_range(ranges, 0xA1, 0x1FFF);
              add_range(ranges, 0x200B, 0x2027); add_range(ranges, 0x202A, 0xFEFE); add_range(ranges, 0xFF00, 0x10FFFF); return 1;
    }
    return 0;
}

static int hexv(int c) { c = tolower(c); return isdigit(c) ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : -1; }

/* символ после '\' -> код (для простых escape) */
static unsigned escape_char(void) {
    char c = rp[rpos++];
    switch (c) {
    case 'n': return '\n';
    case 'r': return '\r';
    case 't': return '\t';
    case 'v': return '\v';
    case 'f': return '\f';
    case '0': return 0;
    case 'c': if (rpos < rlen) return rp[rpos++] & 31; return 'c';
    case 'x':
        if (rpos + 1 < rlen && hexv(rp[rpos]) >= 0 && hexv(rp[rpos + 1]) >= 0) { unsigned v = hexv(rp[rpos]) * 16 + hexv(rp[rpos + 1]); rpos += 2; return v; }
        return 'x';
    case 'u': {
        unsigned v = 0;
        if (rpos < rlen && rp[rpos] == '{') {
            rpos++;
            while (rpos < rlen && rp[rpos] != '}') { int h = hexv(rp[rpos++]); if (h < 0) break; v = v * 16 + h; }
            rpos++;
            return v;
        }
        for (int k = 0; k < 4; k++) {
            if (rpos >= rlen || hexv(rp[rpos]) < 0) return 'u';
            v = v * 16 + hexv(rp[rpos++]);
        }
        if (v >= 0xD800 && v <= 0xDBFF && rpos + 5 < rlen && rp[rpos] == '\\' && rp[rpos + 1] == 'u') {
            unsigned lo = 0;
            int ok = 1;
            for (int k = 0; k < 4; k++) { int h = hexv(rp[rpos + 2 + k]); if (h < 0) ok = 0; lo = lo * 16 + (h < 0 ? 0 : h); }
            if (ok && lo >= 0xDC00 && lo <= 0xDFFF) { rpos += 6; v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00); }
        }
        return v;
    }
    default: {
        int l;
        rpos--;
        unsigned cp = utf8_at(rp, rlen, rpos, &l);
        rpos += l;
        return cp;
    }
    }
}

static void emit_char(frag *f, unsigned cp) {          /* символ -> байты UTF-8 */
    char t[4];
    int n;
    if (cp < 0x80) { t[0] = cp; n = 1; }
    else if (cp < 0x800) { t[0] = 0xC0 | (cp >> 6); t[1] = 0x80 | (cp & 63); n = 2; }
    else if (cp < 0x10000) { t[0] = 0xE0 | (cp >> 12); t[1] = 0x80 | ((cp >> 6) & 63); t[2] = 0x80 | (cp & 63); n = 3; }
    else { t[0] = 0xF0 | (cp >> 18); t[1] = 0x80 | ((cp >> 12) & 63); t[2] = 0x80 | ((cp >> 6) & 63); t[3] = 0x80 | (cp & 63); n = 4; }
    for (int i = 0; i < n; i++) { f_emit(f, R_CHAR); f_emit(f, (ju8)t[i]); }
}

static frag parse_class(void) {
    frag ranges = { 0 }, f = { 0 };
    int neg = 0;
    if (rpos < rlen && rp[rpos] == '^') { neg = 1; rpos++; }
    int first = 1;
    while (rpos < rlen && (rp[rpos] != ']' || (first && 0))) {
        first = 0;
        unsigned lo;
        if (rp[rpos] == '\\') {
            rpos++;
            if (rpos >= rlen) break;
            if (class_escape(&ranges, rp[rpos])) { rpos++; continue; }
            if (rp[rpos] == 'b') { rpos++; lo = 8; }
            else lo = escape_char();
        } else {
            int l;
            lo = utf8_at(rp, rlen, rpos, &l);
            rpos += l;
        }
        unsigned hi = lo;
        if (rpos + 1 < rlen && rp[rpos] == '-' && rp[rpos + 1] != ']') {
            rpos++;
            if (rp[rpos] == '\\') {
                rpos++;
                if (class_escape(&ranges, rp[rpos])) { rpos++; add_range(&ranges, lo, lo); add_range(&ranges, '-', '-'); continue; }
                hi = escape_char();
            } else {
                int l;
                hi = utf8_at(rp, rlen, rpos, &l);
                rpos += l;
            }
        }
        add_range(&ranges, lo, hi);
        if (icase) {                                        /* добавить другой регистр */
            if (lo <= 'z' && hi >= 'a') add_range(&ranges, (lo < 'a' ? 'a' : lo) - 32, (hi > 'z' ? 'z' : hi) - 32);
            if (lo <= 'Z' && hi >= 'A') add_range(&ranges, (lo < 'A' ? 'A' : lo) + 32, (hi > 'Z' ? 'Z' : hi) + 32);
            if (lo <= 0x44F && hi >= 0x430) add_range(&ranges, (lo < 0x430 ? 0x430 : lo) - 32, (hi > 0x44F ? 0x44F : hi) - 32);
            if (lo <= 0x42F && hi >= 0x410) add_range(&ranges, (lo < 0x410 ? 0x410 : lo) + 32, (hi > 0x42F ? 0x42F : hi) + 32);
        }
    }
    if (rpos >= rlen) re_fail("missing ]");
    rpos++;
    f_emit(&f, R_CLASS);
    f_emit(&f, neg);
    f_emit(&f, ranges.n / 2);
    f_append(&f, &ranges);
    f_free(&ranges);
    return f;
}

static frag parse_atom(void) {
    frag f = { 0 };
    char c = rp[rpos];
    if (c == '(') {
        rpos++;
        int cap = -1, look = 0;
        if (rpos + 1 < rlen && rp[rpos] == '?') {
            char k = rp[rpos + 1];
            if (k == ':') rpos += 2;
            else if (k == '=' || k == '!') { look = k == '=' ? 1 : 2; rpos += 2; }
            else if (k == '<' && rpos + 2 < rlen && (rp[rpos + 2] == '=' || rp[rpos + 2] == '!')) { look = rp[rpos + 2] == '=' ? 3 : 4; rpos += 3; }
            else if (k == '<') {                             /* (?<имя>...) */
                rpos += 2;
                while (rpos < rlen && rp[rpos] != '>') rpos++;
                rpos++;
                cap = ++ngroups;
            } else { re_fail("invalid group"); return f; }
        } else cap = ++ngroups;
        frag inner = parse_alt();
        if (rpos >= rlen || rp[rpos] != ')') re_fail("missing )");
        rpos++;
        if (look) {
            f_emit(&f, R_LOOK);
            f_emit(&f, look);
            f_emit(&f, inner.n + 4);
            f_append(&f, &inner);
            f_emit(&f, R_MATCH);
            f.nullable = 1;
        } else if (cap >= 0) {
            f_emit(&f, R_SAVE); f_emit(&f, cap * 2);
            f_append(&f, &inner);
            f_emit(&f, R_SAVE); f_emit(&f, cap * 2 + 1);
            f.nullable = inner.nullable;
        } else { f_append(&f, &inner); f.nullable = inner.nullable; }
        f_free(&inner);
        return f;
    }
    rpos++;
    switch (c) {
    case '.': f_emit(&f, dotall ? R_ANYNL : R_ANY); return f;
    case '^': f_emit(&f, R_BOL); f.nullable = 1; return f;
    case '$': f_emit(&f, R_EOL); f.nullable = 1; return f;
    case '[': return parse_class();
    case '\\': {
        if (rpos >= rlen) { emit_char(&f, '\\'); return f; }
        char e = rp[rpos];
        if (e == 'b') { rpos++; f_emit(&f, R_WORDB); f.nullable = 1; return f; }
        if (e == 'B') { rpos++; f_emit(&f, R_NWORDB); f.nullable = 1; return f; }
        if (e >= '1' && e <= '9') {
            int n = 0;
            while (rpos < rlen && isdigit((ju8)rp[rpos]) && n * 10 + (rp[rpos] - '0') <= 99) n = n * 10 + (rp[rpos++] - '0');
            f_emit(&f, R_BACKREF);
            f_emit(&f, n);
            f.nullable = 1;
            return f;
        }
        if (e == 'k' && rpos + 1 < rlen && rp[rpos + 1] == '<') {   /* \k<имя> — не поддерживаем, пропускаем */
            while (rpos < rlen && rp[rpos] != '>') rpos++;
            rpos++;
            f.nullable = 1;
            return f;
        }
        frag r = { 0 };
        if (class_escape(&r, e)) {
            rpos++;
            f_emit(&f, R_CLASS);
            f_emit(&f, 0);
            f_emit(&f, r.n / 2);
            f_append(&f, &r);
            f_free(&r);
            return f;
        }
        unsigned cp = escape_char();
        emit_char(&f, cp);
        return f;
    }
    default: {
        rpos--;
        int l;
        unsigned cp = utf8_at(rp, rlen, rpos, &l);
        rpos += l;
        if (icase && cp < 128 && isalpha((int)cp)) {
            f_emit(&f, R_CLASS); f_emit(&f, 0); f_emit(&f, 2);
            add_range(&f, tolower(cp), tolower(cp));
            add_range(&f, toupper(cp), toupper(cp));
            return f;
        }
        if (icase && ((cp >= 0x410 && cp <= 0x44F))) {
            unsigned lo = cp >= 0x430 ? cp - 32 : cp;
            f_emit(&f, R_CLASS); f_emit(&f, 0); f_emit(&f, 2);
            add_range(&f, lo, lo);
            add_range(&f, lo + 32, lo + 32);
            return f;
        }
        emit_char(&f, cp);
        return f;
    }
    }
}

/* e* / e+ / e? / e{n,m} */
static frag repeat(frag *e, int min, int max, int lazy) {
    frag f = { 0 };
    if (min > 1000) min = 1000;
    if (max > 1000) max = -1;
    for (int i = 0; i < min; i++) f_append(&f, e);
    if (max == -1) {                                        /* ещё сколько угодно раз */
        int reg = e->nullable ? nregs++ : -1;
        int body = e->n + (reg >= 0 ? 4 : 0);
        f_emit(&f, R_SPLIT);
        f_emit(&f, lazy ? 3 + body + 2 : 3);
        f_emit(&f, lazy ? 3 : 3 + body + 2);
        if (reg >= 0) { f_emit(&f, R_MARK); f_emit(&f, reg); }
        f_append(&f, e);
        if (reg >= 0) { f_emit(&f, R_CHECK); f_emit(&f, reg); }
        f_emit(&f, R_JMP);
        f_emit(&f, -(3 + body));
    } else {
        for (int i = min; i < max; i++) {                   /* необязательные копии */
            f_emit(&f, R_SPLIT);
            f_emit(&f, lazy ? 3 + e->n : 3);
            f_emit(&f, lazy ? 3 : 3 + e->n);
            f_append(&f, e);
        }
    }
    f.nullable = min == 0 || e->nullable;
    return f;
}

static frag parse_concat(void) {
    frag f = { 0 };
    f.nullable = 1;
    while (rpos < rlen && rp[rpos] != '|' && rp[rpos] != ')' && !rerr) {
        frag a = parse_atom();
        if (rpos < rlen) {
            char q = rp[rpos];
            int min = -1, max = -1;
            if (q == '*') { min = 0; max = -1; rpos++; }
            else if (q == '+') { min = 1; max = -1; rpos++; }
            else if (q == '?') { min = 0; max = 1; rpos++; }
            else if (q == '{' && rpos + 1 < rlen && isdigit((ju8)rp[rpos + 1])) {
                int save = rpos++;
                int lo = 0, hi;
                while (rpos < rlen && isdigit((ju8)rp[rpos])) lo = lo * 10 + (rp[rpos++] - '0');
                hi = lo;
                if (rpos < rlen && rp[rpos] == ',') {
                    rpos++;
                    if (rpos < rlen && isdigit((ju8)rp[rpos])) { hi = 0; while (rpos < rlen && isdigit((ju8)rp[rpos])) hi = hi * 10 + (rp[rpos++] - '0'); }
                    else hi = -1;
                }
                if (rpos < rlen && rp[rpos] == '}') { rpos++; min = lo; max = hi; }
                else rpos = save + 0;
                if (min < 0) { rpos = save; }
            }
            if (min >= 0) {
                int lazy = rpos < rlen && rp[rpos] == '?';
                if (lazy) rpos++;
                if (a.n * (min > 0 ? min : 1) > 200000) { re_fail("regexp too big"); }
                frag r = repeat(&a, min, max, lazy);
                f_free(&a);
                a = r;
            } else if (q == '{' && min < 0 && rpos < rlen && rp[rpos] == '{') {
                rpos++;
                frag b = { 0 };
                emit_char(&b, '{');
                f_append(&a, &b);
                f_free(&b);
            }
        }
        f_append(&f, &a);
        if (!a.nullable) f.nullable = 0;
        f_free(&a);
    }
    return f;
}

static frag parse_alt(void) {
    frag f = parse_concat();
    while (rpos < rlen && rp[rpos] == '|' && !rerr) {
        rpos++;
        frag g = parse_concat();
        frag r = { 0 };
        f_emit(&r, R_SPLIT);
        f_emit(&r, 3);
        f_emit(&r, 3 + f.n + 2);
        f_append(&r, &f);
        f_emit(&r, R_JMP);
        f_emit(&r, g.n + 2);
        f_append(&r, &g);
        r.nullable = f.nullable || g.nullable;
        f_free(&f);
        f_free(&g);
        f = r;
    }
    return f;
}

/* программа: [ngroups, nregs, icase, multiline, first_byte, anchored, код...] */
enum { H_NGROUPS, H_NREGS, H_ICASE, H_MULTI, H_FIRST, H_ANCHOR, H_SIZE };

static struct { jsstr *src, *flags, *prog; } cache[64];
static int cache_next;

void js_re_reset(void) { memset(cache, 0, sizeof(cache)); cache_next = 0; }

static jsstr *compile(jsstr *src, jsstr *flags) {
    for (int i = 0; i < 64; i++)
        if (cache[i].prog && cache[i].src->len == src->len && cache[i].flags->len == flags->len &&
            !memcmp(cache[i].src->s, src->s, src->len) && !memcmp(cache[i].flags->s, flags->s, flags->len)) return cache[i].prog;
    rp = src->s;
    rlen = src->len;
    rpos = 0;
    ngroups = 0;
    nregs = 0;
    rerr = 0;
    icase = strchr(flags->s, 'i') != NULL;
    multiline = strchr(flags->s, 'm') != NULL;
    dotall = strchr(flags->s, 's') != NULL;
    frag f = parse_alt();
    if (rpos < rlen && !rerr) re_fail(rp[rpos] == ')' ? "unmatched )" : "bad pattern");
    if (rerr) { f_free(&f); return NULL; }
    f_emit(&f, R_MATCH);
    int hdr[H_SIZE];
    hdr[H_NGROUPS] = ngroups;
    hdr[H_NREGS] = nregs;
    hdr[H_ICASE] = icase;
    hdr[H_MULTI] = multiline;
    hdr[H_FIRST] = f.p[0] == R_CHAR ? f.p[1] : -1;
    hdr[H_ANCHOR] = f.p[0] == R_BOL && !multiline;
    jsstr *prog;
    jsbuf b = { 0 };
    jb_add(&b, (char *)hdr, sizeof(hdr));
    jb_add(&b, (char *)f.p, sizeof(int) * f.n);
    f_free(&f);
    prog = jb_str(&b);
    /* в кэш: пока программа в кэше, сборщик её не трогает */
    if (cache[cache_next].prog) {
        if (cache[cache_next].prog->h.mark == 2) cache[cache_next].prog->h.mark = 0;
        if (cache[cache_next].src->h.mark == 2 && !cache[cache_next].src->interned) cache[cache_next].src->h.mark = 0;
    }
    cache[cache_next].src = js_perm_str(src);
    cache[cache_next].flags = js_perm_str(flags);
    cache[cache_next].prog = prog;
    prog->h.mark = 2;
    cache_next = (cache_next + 1) & 63;
    return prog;
}

int js_regexp_create(jsstr *src, jsstr *flags, jsobj **out) {
    for (int i = 0; i < flags->len; i++)
        if (!strchr("gimsuyd", flags->s[i])) return js_throw("SyntaxError", "Invalid regular expression flags '%.20s'", flags->s);
    jsstr *prog = compile(src, flags);
    if (!prog) return js_throw("SyntaxError", "Invalid regular expression: /%.60s/: %s", src->s, rerrmsg);
    jsobj *re = js_new_object_proto(js_regexp_proto);
    re->cls = C_REGEXP;
    re->iv[0] = js_strv(flags);
    re->iv[1] = js_strv(prog);
    js_define_key(re, S_lastIndex, js_num(0), P_WRITE);
    js_define(re, "source", js_strv(src->len ? src : js_intern("(?:)", 4)), 0);
    js_define(re, "flags", js_strv(flags), 0);
    js_define(re, "global", js_bool(strchr(flags->s, 'g') != NULL), 0);
    js_define(re, "ignoreCase", js_bool(strchr(flags->s, 'i') != NULL), 0);
    js_define(re, "multiline", js_bool(strchr(flags->s, 'm') != NULL), 0);
    js_define(re, "sticky", js_bool(strchr(flags->s, 'y') != NULL), 0);
    js_define(re, "unicode", js_bool(strchr(flags->s, 'u') != NULL), 0);
    js_define(re, "dotAll", js_bool(strchr(flags->s, 's') != NULL), 0);
    *out = re;
    return 0;
}

int js_regexp_ngroups(jsobj *re) { return ((int *)re->iv[1].u.s->s)[H_NGROUPS]; }

/* --- выполнение */

struct bt { int kind, a, b; };              /* kind 0: ветка (pc=a, sp=b); 1: вернуть слот a = b */
static struct bt *bts;
static int btn, btcap;
static unsigned long budget;                /* защита от «взрывных» шаблонов */

static int bt_push(int kind, int a, int b) {
    if (btn == btcap) {
        int cap = btcap ? btcap * 2 : 256;
        if (cap > 4000000) return 0;
        struct bt *p = malloc(sizeof(struct bt) * cap);
        if (!p) return 0;
        if (btn) memcpy(p, bts, sizeof(struct bt) * btn);
        free(bts);
        bts = p;
        btcap = cap;
    }
    bts[btn].kind = kind;
    bts[btn].a = a;
    bts[btn].b = b;
    btn++;
    return 1;
}

static int is_word(const char *s, int n, int i) {
    if (i < 0 || i >= n) return 0;
    ju8 c = s[i];
    return isalnum(c) || c == '_';
}

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

/* выполнить код с позиции pc от sp; слоты: caps[0..2*(ng+1)), затем регистры */
static int run(const int *code, const int *hdr, const char *s, int n, int pc, int sp, int *slots, int *end) {
    int base = btn;
    int icase_ = hdr[H_ICASE], multi = hdr[H_MULTI];
    for (;;) {
        if (++budget > 30000000ul) { btn = base; return -1; }
        int ok = 1;
        switch (code[pc]) {
        case R_CHAR:
            if (sp < n && ((ju8)s[sp] == code[pc + 1] || (icase_ && fold((ju8)s[sp]) == fold(code[pc + 1])))) { sp++; pc += 2; }
            else ok = 0;
            break;
        case R_ANY: case R_ANYNL: {
            if (sp >= n) { ok = 0; break; }
            int l;
            unsigned cp = utf8_at(s, n, sp, &l);
            if (code[pc] == R_ANY && (cp == '\n' || cp == '\r' || cp == 0x2028 || cp == 0x2029)) { ok = 0; break; }
            sp += l;
            pc += 1;
            break;
        }
        case R_CLASS: {
            int neg = code[pc + 1], nr = code[pc + 2];
            if (sp >= n) { ok = 0; break; }
            int l;
            unsigned cp = utf8_at(s, n, sp, &l);
            int in = 0;
            for (int k = 0; k < nr; k++) if (cp >= (unsigned)code[pc + 3 + 2 * k] && cp <= (unsigned)code[pc + 4 + 2 * k]) { in = 1; break; }
            if (in == neg) { ok = 0; break; }
            sp += l;
            pc += 3 + 2 * nr;
            break;
        }
        case R_SPLIT:
            if (!bt_push(0, pc + code[pc + 2], sp)) { btn = base; return -1; }
            pc += code[pc + 1];
            break;
        case R_JMP: pc += code[pc + 1]; break;
        case R_SAVE: case R_MARK: {
            int slot = code[pc + 1];
            if (!bt_push(1, slot, slots[slot])) { btn = base; return -1; }
            slots[slot] = sp;
            pc += 2;
            break;
        }
        case R_CHECK:
            if (slots[code[pc + 1]] == sp) ok = 0;             /* пустая итерация — дальше не крутим */
            else pc += 2;
            break;
        case R_BOL:
            if (sp == 0 || (multi && (s[sp - 1] == '\n' || s[sp - 1] == '\r'))) pc++;
            else ok = 0;
            break;
        case R_EOL:
            if (sp == n || (multi && (s[sp] == '\n' || s[sp] == '\r'))) pc++;
            else ok = 0;
            break;
        case R_WORDB: case R_NWORDB: {
            int b = is_word(s, n, sp - 1) != is_word(s, n, sp);
            if (b == (code[pc] == R_WORDB)) pc++;
            else ok = 0;
            break;
        }
        case R_BACKREF: {
            int g = code[pc + 1], st = slots[2 * g], en = slots[2 * g + 1];
            if (g > hdr[H_NGROUPS] || st < 0 || en < 0) { pc += 2; break; }
            int l = en - st;
            if (sp + l > n) { ok = 0; break; }
            for (int k = 0; k < l && ok; k++) {
                int a = (ju8)s[st + k], b = (ju8)s[sp + k];
                if (a != b && !(icase_ && fold(a) == fold(b))) ok = 0;
            }
            if (ok) { sp += l; pc += 2; }
            break;
        }
        case R_LOOK: {
            int kind = code[pc + 1], skip = code[pc + 2];
            int e, r = 0;
            if (kind <= 2) {
                r = run(code, hdr, s, n, pc + 3, sp, slots, &e);
                if (r < 0) { btn = base; return -1; }
                if (kind == 2) r = !r;
            } else {                                         /* назад: ищем начало, чтобы кончилось ровно в sp */
                for (int st = sp; st >= 0 && st >= sp - 255 && !r; st--) {
                    int rr = run(code, hdr, s, n, pc + 3, st, slots, &e);
                    if (rr < 0) { btn = base; return -1; }
                    if (rr && e == sp) r = 1;
                }
                if (kind == 4) r = !r;
            }
            if (r) pc += skip;
            else ok = 0;
            break;
        }
        case R_MATCH:
            *end = sp;
            /* откатывать нечего: оставляем сохранённые группы, но стек веток выбрасываем */
            while (btn > base) btn--;
            return 1;
        default:
            ok = 0;
        }
        if (ok) continue;
        /* неудача: откат */
        for (;;) {
            if (btn == base) return 0;
            struct bt *t = &bts[--btn];
            if (t->kind == 1) { slots[t->a] = t->b; continue; }
            pc = t->a;
            sp = t->b;
            break;
        }
    }
}

int js_regexp_exec(jsobj *re, jsstr *str, int start, int *caps, int ncap_max) {
    const int *hdr = (const int *)re->iv[1].u.s->s;
    const int *code = hdr + H_SIZE;
    int ng = hdr[H_NGROUPS], nslots = 2 * (ng + 1) + hdr[H_NREGS];
    int slots_small[128], *slots = nslots <= 128 ? slots_small : malloc(sizeof(int) * nslots);
    int sticky = strchr(re->iv[0].u.s->s, 'y') != NULL;
    const char *s = str->s;
    int n = str->len, first = hdr[H_FIRST];
    budget = 0;
    btn = 0;
    int result = 0;
    for (int p = start; p <= n; p++) {
        if (hdr[H_ANCHOR] && p > 0) break;
        if (first >= 0 && !hdr[H_ICASE] && !sticky) {          /* быстрый пропуск до первой буквы */
            while (p < n && (ju8)s[p] != first) p++;
            if (p >= n) break;
        }
        for (int i = 0; i < nslots; i++) slots[i] = -1;
        int end;
        int r = run(code, hdr, s, n, 0, p, slots, &end);
        if (r < 0) { result = js_throw("RangeError", "regular expression is too complex"); break; }
        if (r) {
            slots[0] = p;
            slots[1] = end;
            int cnt = ng + 1 < ncap_max ? ng + 1 : ncap_max;
            for (int i = 0; i < cnt * 2; i++) caps[i] = slots[i];
            for (int g = 1; g < cnt; g++) if (caps[2 * g] < 0 || caps[2 * g + 1] < 0) caps[2 * g] = caps[2 * g + 1] = -1;
            result = cnt;
            break;
        }
        if (sticky) break;
        if (p < n && ((ju8)s[p] & 0xC0) == 0xC0) {            /* не начинать с середины символа UTF-8 */
            while (p + 1 < n && ((ju8)s[p + 1] & 0xC0) == 0x80) p++;
        }
    }
    if (slots != slots_small) free(slots);
    return result;
}
