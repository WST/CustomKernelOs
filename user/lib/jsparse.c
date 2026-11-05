/* jsparse.c — разбор JavaScript: лексер (весь исходник сразу в массив токенов),
 * парсер (рекурсивный спуск -> дерево узлов node) и «разрешение имён»: каждой
 * локальной переменной заранее назначается место (слот) в окружении, чтобы при
 * выполнении не искать её по имени. */
#include "jsint.h"

/* ------------------------------------------------------------------ токены */

enum { T_EOF, T_NUM, T_STR, T_TPL, T_REGEX, T_NAME, T_PUNCT };

/* многосимвольные знаки; одиночные кодируются своим символом */
enum {
    P_USHR_EQ = 200, P_ELLIPSIS, P_SEQ, P_SNE, P_EXP_EQ, P_SHL_EQ, P_SHR_EQ, P_USHR, P_AND_EQ, P_OR_EQ,
    P_NULLISH_EQ, P_ARROW, P_EQ, P_NE, P_LE, P_GE, P_AND, P_OR, P_NULLISH, P_OPTCHAIN, P_INC, P_DEC,
    P_ADD_EQ, P_SUB_EQ, P_MUL_EQ, P_DIV_EQ, P_MOD_EQ, P_BAND_EQ, P_BOR_EQ, P_BXOR_EQ, P_SHL, P_SHR, P_EXP
};
static const char *puncts[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--",
    "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<", ">>", "**",
};

/* ключевые слова: до KW_LAST_RESERVED — зарезервированные (не могут быть именами) */
enum {
    KW_NONE, KW_BREAK, KW_CASE, KW_CATCH, KW_CLASS, KW_CONST, KW_CONTINUE, KW_DEBUGGER, KW_DEFAULT,
    KW_DELETE, KW_DO, KW_ELSE, KW_EXPORT, KW_EXTENDS, KW_FINALLY, KW_FOR, KW_FUNCTION, KW_IF,
    KW_IMPORT, KW_IN, KW_INSTANCEOF, KW_NEW, KW_RETURN, KW_SUPER, KW_SWITCH, KW_THIS, KW_THROW,
    KW_TRY, KW_TYPEOF, KW_VAR, KW_VOID, KW_WHILE, KW_WITH, KW_NULL, KW_TRUE, KW_FALSE, KW_ENUM,
    KW_LAST_RESERVED = KW_ENUM,
    KW_LET, KW_STATIC, KW_GET, KW_SET, KW_OF, KW_ASYNC, KW_AWAIT, KW_YIELD, KW_COUNT
};
static const char *kw_text[] = {
    "", "break", "case", "catch", "class", "const", "continue", "debugger", "default", "delete", "do",
    "else", "export", "extends", "finally", "for", "function", "if", "import", "in", "instanceof",
    "new", "return", "super", "switch", "this", "throw", "try", "typeof", "var", "void", "while",
    "with", "null", "true", "false", "enum", "let", "static", "get", "set", "of", "async", "await", "yield",
};

struct tok {
    ju8 type, nl, tail, kw;
    int op;
    int line;
    jsstr *s, *s2;
    double num;
};

static jsstr *S_this, *S_arguments, *S_callee_slot, *S_empty;

void js_parse_init(void) {
    for (int i = 1; i < KW_COUNT; i++) js_kw_names[i] = js_perm(kw_text[i]);
    S_this = js_perm("this");
    S_arguments = js_perm("arguments");
    S_callee_slot = js_perm("%callee");
    S_empty = js_perm("");
}

static int kw_of(jsstr *s) {
    for (int i = 1; i < KW_COUNT; i++) if (js_kw_names[i] == s) return i;
    return 0;
}

/* --- состояние лексера */
static const char *src;
static int slen, spos, sline;
static struct tok *toks;
static int ntok, tcap;
static char *perr;                  /* текст ошибки (буфер вызывающего) */
static int perrmax, failed;

static void lex_error(const char *msg) {
    if (failed) return;
    failed = 1;
    snprintf(perr, perrmax, "SyntaxError: %s (line %d)", msg, sline);
}

static struct tok *new_tok(int type) {
    if (ntok == tcap) {
        int cap = tcap ? tcap * 2 : 1024;
        struct tok *t = malloc(sizeof(struct tok) * cap);
        if (!t) { lex_error("out of memory"); return &toks[0]; }
        if (ntok) memcpy(t, toks, sizeof(struct tok) * ntok);
        free(toks);
        toks = t;
        tcap = cap;
    }
    struct tok *t = &toks[ntok++];
    memset(t, 0, sizeof(*t));
    t->type = type;
    t->line = sline;
    return t;
}

static int is_id_start(int c) { return isalpha(c) || c == '_' || c == '$' || c >= 0x80; }
static int is_id_char(int c) { return isalnum(c) || c == '_' || c == '$' || c >= 0x80; }

static void put_utf8(jsbuf *b, unsigned cp) {
    char t[4];
    if (cp < 0x80) { t[0] = cp; jb_add(b, t, 1); }
    else if (cp < 0x800) { t[0] = 0xC0 | (cp >> 6); t[1] = 0x80 | (cp & 63); jb_add(b, t, 2); }
    else if (cp < 0x10000) { t[0] = 0xE0 | (cp >> 12); t[1] = 0x80 | ((cp >> 6) & 63); t[2] = 0x80 | (cp & 63); jb_add(b, t, 3); }
    else { t[0] = 0xF0 | (cp >> 18); t[1] = 0x80 | ((cp >> 12) & 63); t[2] = 0x80 | ((cp >> 6) & 63); t[3] = 0x80 | (cp & 63); jb_add(b, t, 4); }
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 32;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

/* escape-последовательность после '\' (spos указывает на символ после '\') */
static void lex_escape(jsbuf *b) {
    if (spos >= slen) return;
    char c = src[spos++];
    switch (c) {
    case 'n': jb_addc(b, '\n'); return;
    case 't': jb_addc(b, '\t'); return;
    case 'r': jb_addc(b, '\r'); return;
    case 'b': jb_addc(b, '\b'); return;
    case 'f': jb_addc(b, '\f'); return;
    case 'v': jb_addc(b, '\v'); return;
    case '\r': if (spos < slen && src[spos] == '\n') spos++; sline++; return;
    case '\n': sline++; return;
    case 'x': {
        int h1 = spos < slen ? hexval(src[spos]) : -1, h2 = spos + 1 < slen ? hexval(src[spos + 1]) : -1;
        if (h1 < 0 || h2 < 0) { jb_addc(b, 'x'); return; }
        spos += 2;
        put_utf8(b, h1 * 16 + h2);
        return;
    }
    case 'u': {
        unsigned cp = 0;
        if (spos < slen && src[spos] == '{') {
            spos++;
            while (spos < slen && src[spos] != '}') { int h = hexval(src[spos++]); if (h < 0) break; cp = cp * 16 + h; }
            spos++;
        } else {
            for (int k = 0; k < 4; k++) {
                int h = spos < slen ? hexval(src[spos]) : -1;
                if (h < 0) { lex_error("bad \\u escape"); return; }
                cp = cp * 16 + h;
                spos++;
            }
            if (cp >= 0xD800 && cp <= 0xDBFF && spos + 5 < slen && src[spos] == '\\' && src[spos + 1] == 'u') {
                unsigned lo = 0;
                int ok = 1;
                for (int k = 0; k < 4; k++) { int h = hexval(src[spos + 2 + k]); if (h < 0) ok = 0; lo = lo * 16 + (h < 0 ? 0 : h); }
                if (ok && lo >= 0xDC00 && lo <= 0xDFFF) { spos += 6; cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); }
            }
        }
        put_utf8(b, cp);
        return;
    }
    default:
        if (c >= '0' && c <= '7') {                           /* \0 и старые восьмеричные */
            int v = c - '0';
            if (spos < slen && src[spos] >= '0' && src[spos] <= '7') { v = v * 8 + src[spos++] - '0';
                if (v < 32 && spos < slen && src[spos] >= '0' && src[spos] <= '7') v = v * 8 + src[spos++] - '0'; }
            put_utf8(b, v);
            return;
        }
        jb_addc(b, c);
    }
}

/* ')' закрыл заголовок if/for/while — тогда дальше может быть /регулярка/ */
static ju8 paren_ctl[256];
static int paren_sp, ctl_paren_closed;

static int regex_allowed(void) {
    if (!ntok) return 1;
    struct tok *p = &toks[ntok - 1];
    if (p->type == T_PUNCT && p->op == ')' && ctl_paren_closed) return 1;
    switch (p->type) {
    case T_NUM: case T_STR: case T_REGEX: return 0;
    case T_TPL: return !p->tail;
    case T_NAME:
        if (p->kw && p->kw <= KW_LAST_RESERVED && p->kw != KW_THIS && p->kw != KW_SUPER &&
            p->kw != KW_NULL && p->kw != KW_TRUE && p->kw != KW_FALSE) return 1;
        return 0;
    case T_PUNCT:
        return !(p->op == ')' || p->op == ']' || p->op == '}' || p->op == P_INC || p->op == P_DEC);
    }
    return 1;
}

/* часть шаблонной строки `...${ или }...` до следующего ${ или ` */
static void lex_template_part(void) {
    jsbuf b = { 0 }, raw = { 0 };
    struct tok *t = new_tok(T_TPL);
    for (;;) {
        if (spos >= slen) { lex_error("unterminated template"); break; }
        char c = src[spos];
        if (c == '`') { spos++; t->tail = 1; break; }
        if (c == '$' && spos + 1 < slen && src[spos + 1] == '{') { spos += 2; break; }
        if (c == '\\') {
            int st = spos;
            spos++;
            lex_escape(&b);
            jb_add(&raw, src + st, spos - st);
            continue;
        }
        if (c == '\n') sline++;
        if (c == '\r') { spos++; if (spos < slen && src[spos] == '\n') continue; jb_addc(&b, '\n'); jb_addc(&raw, '\n'); continue; }
        jb_addc(&b, c);
        jb_addc(&raw, c);
        spos++;
    }
    t = &toks[ntok - 1];
    jsstr *cooked = jb_str(&b);
    t->s = js_perm_str(cooked);
    t->s2 = js_perm_str(jb_str(&raw));
}

static int lex_all(void) {
    int tpl_stack[64], tpl_sp = 0, depth = 0;
    paren_sp = 0;
    ctl_paren_closed = 0;
    int nl = 1;
    while (!failed) {
        /* пробелы и комментарии */
        for (;;) {
            if (spos >= slen) break;
            ju8 c = src[spos];
            if (c == '\n') { sline++; nl = 1; spos++; continue; }
            if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') { spos++; continue; }
            if (c == 0xC2 && spos + 1 < slen && (ju8)src[spos + 1] == 0xA0) { spos += 2; continue; }   /* &nbsp; */
            if (c == 0xEF && spos + 2 < slen && (ju8)src[spos + 1] == 0xBB && (ju8)src[spos + 2] == 0xBF) { spos += 3; continue; }
            if (c == 0xE2 && spos + 2 < slen && (ju8)src[spos + 1] == 0x80 && ((ju8)src[spos + 2] == 0xA8 || (ju8)src[spos + 2] == 0xA9)) { spos += 3; nl = 1; continue; }
            if (c == '/' && spos + 1 < slen && src[spos + 1] == '/') { while (spos < slen && src[spos] != '\n') spos++; continue; }
            if (c == '/' && spos + 1 < slen && src[spos + 1] == '*') {
                spos += 2;
                while (spos + 1 < slen && !(src[spos] == '*' && src[spos + 1] == '/')) { if (src[spos] == '\n') { sline++; nl = 1; } spos++; }
                spos += 2;
                continue;
            }
            /* старый приём: <!-- в начале скрипта и --> в конце — как комментарии */
            if (c == '<' && spos + 3 < slen && !strncmp(src + spos, "<!--", 4)) { while (spos < slen && src[spos] != '\n') spos++; continue; }
            if (c == '-' && nl && spos + 2 < slen && !strncmp(src + spos, "-->", 3)) { while (spos < slen && src[spos] != '\n') spos++; continue; }
            break;
        }
        if (spos >= slen) { new_tok(T_EOF)->nl = 1; break; }
        int c = (ju8)src[spos];
        struct tok *t;
        if (is_id_start(c) || c == '\\' || (c == '#' && spos + 1 < slen && is_id_start((ju8)src[spos + 1]))) {
            jsbuf b = { 0 };
            if (c == '#') { jb_addc(&b, '#'); spos++; }
            int escaped = 0;
            while (spos < slen && (is_id_char((ju8)src[spos]) || src[spos] == '\\')) {
                if (src[spos] == '\\') { spos++; lex_escape(&b); escaped = 1; continue; }   /* A в имени */
                jb_addc(&b, src[spos++]);
            }
            t = new_tok(T_NAME);
            jsstr *tmp = jb_str(&b);
            t->s = js_perm_str(tmp);
            t->kw = escaped ? 0 : kw_of(t->s);
        } else if (isdigit(c) || (c == '.' && spos + 1 < slen && isdigit((ju8)src[spos + 1]))) {
            t = new_tok(T_NUM);
            double v = 0;
            if (c == '0' && spos + 1 < slen && strchr("xXoObB", src[spos + 1]) && src[spos + 1]) {
                int base = (src[spos + 1] | 32) == 'x' ? 16 : (src[spos + 1] | 32) == 'o' ? 8 : 2;
                spos += 2;
                while (spos < slen) {
                    if (src[spos] == '_') { spos++; continue; }
                    int d = hexval(src[spos]);
                    if (d < 0 || d >= base) break;
                    v = v * base + d;
                    spos++;
                }
            } else if (c == '0' && spos + 1 < slen && src[spos + 1] >= '0' && src[spos + 1] <= '7') {
                spos++;                                       /* старое восьмеричное 0777 */
                while (spos < slen && src[spos] >= '0' && src[spos] <= '7') v = v * 8 + (src[spos++] - '0');
            } else {
                char tmp[400];
                int n = 0, dot = 0, ex = 0;
                while (spos < slen && n < 396) {
                    char d = src[spos];
                    if (isdigit((ju8)d)) tmp[n++] = d;
                    else if (d == '.' && !dot && !ex) { tmp[n++] = d; dot = 1; }
                    else if (d == '_') {}
                    else if ((d == 'e' || d == 'E') && spos + 1 < slen && (isdigit((ju8)src[spos + 1]) ||
                             ((src[spos + 1] == '+' || src[spos + 1] == '-') && spos + 2 < slen && isdigit((ju8)src[spos + 2])))) {
                        tmp[n++] = d;
                        tmp[n++] = src[++spos];
                        ex = 1;
                    } else break;
                    spos++;
                }
                int used;
                v = js_parse_float(tmp, n, &used);
            }
            if (spos < slen && src[spos] == 'n') spos++;      /* BigInt — считаем обычным числом */
            t->num = v;
        } else if (c == '"' || c == '\'') {
            spos++;
            jsbuf b = { 0 };
            while (spos < slen && src[spos] != c) {
                if (src[spos] == '\\') { spos++; lex_escape(&b); continue; }
                if (src[spos] == '\n') { lex_error("unterminated string"); break; }
                jb_addc(&b, src[spos++]);
            }
            spos++;
            t = new_tok(T_STR);
            jsstr *tmp = jb_str(&b);
            t->s = js_perm_str(tmp);
        } else if (c == '`') {
            spos++;
            lex_template_part();
            t = &toks[ntok - 1];
            if (!t->tail) { if (tpl_sp < 64) tpl_stack[tpl_sp++] = depth; depth++; }
        } else if (c == '}' && tpl_sp > 0 && tpl_stack[tpl_sp - 1] == depth - 1) {
            spos++;
            depth--;
            tpl_sp--;
            lex_template_part();
            t = &toks[ntok - 1];
            t->kw = 1;                                       /* продолжение, не начало */
            if (!t->tail) { tpl_stack[tpl_sp++] = depth; depth++; }
        } else if (c == '/' && regex_allowed()) {
            spos++;
            int st = spos, in_class = 0;
            while (spos < slen && (in_class || src[spos] != '/')) {
                if (src[spos] == '\\') spos++;
                else if (src[spos] == '[') in_class = 1;
                else if (src[spos] == ']') in_class = 0;
                else if (src[spos] == '\n') { lex_error("unterminated regexp"); break; }
                spos++;
            }
            int en = spos;
            spos++;
            int fs = spos;
            while (spos < slen && is_id_char((ju8)src[spos])) spos++;
            t = new_tok(T_REGEX);
            t->s = js_perm_str(js_strn(src + st, en - st));
            t->s2 = js_perm_str(js_strn(src + fs, spos - fs));
        } else {
            t = new_tok(T_PUNCT);
            int op = 0;
            for (unsigned k = 0; k < sizeof(puncts) / sizeof(puncts[0]); k++) {
                int l = strlen(puncts[k]);
                if (spos + l <= slen && !strncmp(src + spos, puncts[k], l)) {
                    if (k + 200 == P_OPTCHAIN && spos + 2 < slen && isdigit((ju8)src[spos + 2])) continue;
                    op = 200 + k;
                    spos += l;
                    break;
                }
            }
            if (!op) {
                if (!strchr("{}()[];,<>+-*/%&|^!~?:=.@#", c)) { lex_error("unexpected character"); break; }
                op = c;
                spos++;
                if (c == '{') depth++;
                if (c == '}') depth--;
                if (c == '(') {
                    struct tok *pr = ntok >= 2 ? &toks[ntok - 2] : NULL;
                    int ctl = pr && pr->type == T_NAME && (pr->kw == KW_IF || pr->kw == KW_FOR || pr->kw == KW_WHILE || pr->kw == KW_WITH);
                    if (paren_sp < 256) paren_ctl[paren_sp] = ctl;
                    paren_sp++;
                }
            }
            t->op = op;
            if (op == ')') {
                ctl_paren_closed = paren_sp > 0 && paren_sp <= 256 && paren_ctl[paren_sp - 1];
                if (paren_sp > 0) paren_sp--;
            }
        }
        t->nl = nl;
        nl = 0;
    }
    return !failed;
}

/* ------------------------------------------------------------------ парсер */

static int tp;                       /* текущий токен */
static int no_in;                    /* внутри заголовка for: «in» — не оператор */
static int in_func, in_async, next_async, in_gen, next_gen;
static const char *cur_file;

#define CUR (&toks[tp])
#define PEEK(k) (&toks[tp + (k) < ntok ? tp + (k) : ntok - 1])

static void perror_at(const char *msg) {
    if (failed) return;
    failed = 1;
    struct tok *t = CUR;
    const char *what = t->type == T_EOF ? "end of script" : t->type == T_NAME ? t->s->s :
                       t->type == T_STR ? "string" : t->type == T_NUM ? "number" : t->type == T_TPL ? "template" : "";
    char pt[8] = "";
    if (t->type == T_PUNCT) {
        if (t->op >= 200) snprintf(pt, sizeof(pt), "%s", puncts[t->op - 200]);
        else { pt[0] = t->op; pt[1] = 0; }
        what = pt;
    }
    snprintf(perr, perrmax, "SyntaxError: %s near '%.30s' (line %d)", msg, what, t->line);
    tp = ntok - 1;                   /* дальше — сразу конец: все циклы разбора закончатся */
}

static void next(void) { if (tp < ntok - 1) tp++; }
static int is_op(int op) { return CUR->type == T_PUNCT && CUR->op == op; }
static int is_kw(int kw) { return CUR->type == T_NAME && CUR->kw == kw; }
static int at_end(void) { return CUR->type == T_EOF; }
static int p_accept(int op) { if (is_op(op)) { next(); return 1; } return 0; }
static void expect(int op) {
    if (p_accept(op)) return;
    char m[32];
    if (op >= 200) snprintf(m, sizeof(m), "expected '%s'", puncts[op - 200]);
    else snprintf(m, sizeof(m), "expected '%c'", op);
    perror_at(m);
}
static int is_ident_tok(struct tok *t) { return t->type == T_NAME && (!t->kw || t->kw > KW_LAST_RESERVED); }

static node *mk(int type) {
    node *n = js_alloc(sizeof(node));
    n->type = type;
    n->line = toks ? CUR->line : 0;
    n->depth = -1;
    n->file = cur_file;
    return n;
}

/* список узлов растёт во временном буфере, потом копируется в js_alloc */
typedef struct { node **p; int n, cap; } nlist;
static void nl_add(nlist *l, node *x) {
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 8;
        node **p = malloc(sizeof(node *) * cap);
        if (l->n) memcpy(p, l->p, sizeof(node *) * l->n);
        free(l->p);
        l->p = p;
        l->cap = cap;
    }
    l->p[l->n++] = x;
}
static void nl_store(nlist *l, node *dst) {
    dst->n = l->n;
    dst->list = js_alloc(sizeof(node *) * (l->n ? l->n : 1));
    if (l->n) memcpy(dst->list, l->p, sizeof(node *) * l->n);
    free(l->p);
}

static node *parse_assign(void);
static node *parse_expr(void);
static node *parse_stmt(void);
static node *parse_function(int flags, int is_decl);
static node *parse_class(int is_decl);
static node *parse_binding_target(void);
static node *parse_lhs(void);
static void consume_semi(void);
static jsstr *member_name(void);

static node *ident_node(jsstr *name) {
    node *n = mk(N_IDENT);
    n->str = name;
    return n;
}

static node *parse_template(node *tag) {
    node *n = mk(tag ? N_TAGGED : N_TPL);
    n->a = tag;
    nlist l = { 0 };
    for (;;) {
        struct tok *t = CUR;
        if (t->type != T_TPL) { perror_at("bad template"); break; }
        node *s = mk(N_STR);
        s->str = t->s;
        s->str2 = t->s2;
        nl_add(&l, s);
        next();
        if (t->tail) break;
        int save = no_in;
        no_in = 0;
        nl_add(&l, parse_expr());
        no_in = save;
        if (failed) break;
    }
    nl_store(&l, n);
    return n;
}

static jsstr *prop_key_name(struct tok *t) {
    if (t->type == T_NAME || t->type == T_STR) return t->s;
    if (t->type == T_NUM) { char b[40]; js_num_to_str(t->num, b); return js_perm(b); }
    return NULL;
}

/* ключ свойства в литерале объекта / классе: имя, строка, число или [выражение] */
static void parse_prop_key(node *p) {
    if (p_accept('[')) {
        p->flags |= F_COMPUTED;
        int save = no_in;
        no_in = 0;
        p->a = parse_assign();
        no_in = save;
        expect(']');
        return;
    }
    jsstr *k = prop_key_name(CUR);
    if (!k) { perror_at("bad property name"); return; }
    p->str = k;
    next();
}

static int method_follows(void) {   /* после get/set/async/static идёт имя метода? */
    struct tok *t = PEEK(1);
    if (t->type == T_PUNCT) return t->op == '[' || t->op == '*' || t->op == '#';
    return t->type == T_NAME || t->type == T_STR || t->type == T_NUM;
}

static node *parse_object_literal(void) {
    node *n = mk(N_OBJECT);
    nlist l = { 0 };
    expect('{');
    int save = no_in;
    no_in = 0;
    while (!is_op('}') && !at_end()) {
        node *p = mk(N_PROP);
        if (p_accept(P_ELLIPSIS)) {
            p->type = N_SPREAD;
            p->a = parse_assign();
        } else {
            int fl = 0;
            int async_m = 0, gen_m = 0;
            if (is_kw(KW_ASYNC) && method_follows() && !PEEK(1)->nl) { next(); async_m = 1; }
            if (p_accept('*')) gen_m = 1;
            if ((is_kw(KW_GET) || is_kw(KW_SET)) && method_follows()) {
                fl |= is_kw(KW_GET) ? F_GETTER : F_SETTER;
                next();
            }
            struct tok *keytok = CUR;
            parse_prop_key(p);
            p->flags |= fl;
            if (is_op('(')) {
                next_async = async_m;
                next_gen = gen_m;
                p->b = parse_function(F_METHOD | (fl & (F_GETTER | F_SETTER)), 0);
                if (p->str) p->b->str = p->str;
            } else if (p_accept(':')) {
                p->b = parse_assign();
            } else if (keytok->type == T_NAME && !(p->flags & F_COMPUTED)) {
                p->flags |= F_SHORTHAND;
                p->b = ident_node(p->str);
                if (p_accept('=')) {                         /* {a = 1} — только как шаблон */
                    node *d = mk(N_DEFAULT);
                    d->a = p->b;
                    d->b = parse_assign();
                    p->b = d;
                }
            } else perror_at("expected ':'");
        }
        nl_add(&l, p);
        if (!p_accept(',')) break;
    }
    no_in = save;
    expect('}');
    nl_store(&l, n);
    return n;
}

static node *parse_array_literal(void) {
    node *n = mk(N_ARRAY);
    nlist l = { 0 };
    expect('[');
    int save = no_in;
    no_in = 0;
    while (!is_op(']') && !at_end()) {
        if (is_op(',')) { next(); nl_add(&l, mk(N_HOLE)); continue; }
        node *e;
        if (p_accept(P_ELLIPSIS)) { e = mk(N_SPREAD); e->a = parse_assign(); }
        else e = parse_assign();
        nl_add(&l, e);
        if (!p_accept(',')) break;
    }
    no_in = save;
    expect(']');
    nl_store(&l, n);
    return n;
}

/* ( ... ) => — смотрим вперёд до парной скобки */
static int arrow_after_parens(void) {
    int depth = 0;
    for (int i = tp; i < ntok; i++) {
        struct tok *t = &toks[i];
        if (t->type == T_EOF) return 0;
        if (t->type != T_PUNCT) continue;
        if (t->op == '(' || t->op == '[' || t->op == '{') depth++;
        else if (t->op == ')' || t->op == ']' || t->op == '}') {
            if (--depth == 0) {
                struct tok *a = &toks[i + 1 < ntok ? i + 1 : i];
                return a->type == T_PUNCT && a->op == P_ARROW && !a->nl;
            }
        }
    }
    return 0;
}

static node *parse_params(void);
static node *parse_function_body(node *fn);

static node *parse_arrow(int async_fl) {
    node *fn = mk(N_FUNC);
    fn->flags = F_ARROW;
    fn->kind = async_fl;
    if (is_op('(')) {
        node *ps = parse_params();
        fn->list = ps->list;
        fn->n = ps->n;
    } else {
        fn->list = js_alloc(sizeof(node *));
        fn->list[0] = ident_node(CUR->s);
        fn->n = 1;
        next();
    }
    expect(P_ARROW);
    int save_f = in_func, save_a = in_async, save_in = no_in, save_g = in_gen;
    in_func = 1;
    in_async = async_fl;
    in_gen = 0;
    if (is_op('{')) {
        no_in = 0;
        parse_function_body(fn);
    } else {
        fn->flags |= F_EXPRBODY;
        fn->a = parse_assign();
    }
    in_func = save_f;
    in_async = save_a;
    in_gen = save_g;
    no_in = save_in;
    return fn;
}

static node *parse_primary(void) {
    struct tok *t = CUR;
    node *n;
    switch (t->type) {
    case T_NUM: n = mk(N_NUM); n->num = t->num; next(); return n;
    case T_STR: n = mk(N_STR); n->str = t->s; next(); return n;
    case T_TPL: return parse_template(NULL);
    case T_REGEX: n = mk(N_REGEX); n->str = t->s; n->str2 = t->s2; next(); return n;
    case T_NAME:
        switch (t->kw) {
        case KW_THIS: next(); return mk(N_THIS);
        case KW_NULL: next(); return mk(N_NULL);
        case KW_TRUE: next(); return mk(N_TRUE);
        case KW_FALSE: next(); return mk(N_FALSE);
        case KW_FUNCTION: return parse_function(0, 0);
        case KW_CLASS: return parse_class(0);
        case KW_SUPER: next(); return mk(N_SUPER);
        case KW_IMPORT:                                      /* import(...) и import.meta: модулей нет */
            if (PEEK(1)->type == T_PUNCT && PEEK(1)->op == '(') { n = ident_node(js_perm("__import")); next(); return n; }
            if (PEEK(1)->type == T_PUNCT && PEEK(1)->op == '.') { next(); next(); member_name(); n = mk(N_OBJECT); n->list = js_alloc(sizeof(node *)); return n; }
            break;
        case KW_ASYNC:
            if (PEEK(1)->type == T_NAME && PEEK(1)->kw == KW_FUNCTION && !PEEK(1)->nl) {
                next();
                next_async = 1;
                return parse_function(0, 0);
            }
            break;
        default:
            if (t->kw && t->kw <= KW_LAST_RESERVED) { perror_at("unexpected keyword"); return mk(N_UNDEF); }
        }
        n = ident_node(t->s);
        next();
        return n;
    case T_PUNCT:
        if (t->op == '(') {
            next();
            int save = no_in;
            no_in = 0;
            n = parse_expr();
            no_in = save;
            expect(')');
            return n;
        }
        if (t->op == '[') return parse_array_literal();
        if (t->op == '{') return parse_object_literal();
        break;
    }
    perror_at("unexpected token");
    return mk(N_UNDEF);
}

static node *parse_args(node *call) {
    nlist l = { 0 };
    expect('(');
    int save = no_in;
    no_in = 0;
    while (!is_op(')') && !at_end()) {
        node *e;
        if (p_accept(P_ELLIPSIS)) { e = mk(N_SPREAD); e->a = parse_assign(); call->flags |= F_SPREAD_ARGS; }
        else e = parse_assign();
        nl_add(&l, e);
        if (!p_accept(',')) break;
    }
    no_in = save;
    expect(')');
    nl_store(&l, call);
    return call;
}

static jsstr *member_name(void) {
    struct tok *t = CUR;
    if (t->type != T_NAME) { perror_at("expected property name"); return S_empty; }
    next();
    return t->s;
}

/* вызовы и обращения к свойствам: a.b, a[b], a(b), a?.b, new A(b), a`tpl` */
static node *parse_lhs(void) {
    node *e;
    if (is_kw(KW_NEW)) {
        node *n = mk(N_NEW);
        next();
        if (is_op('.')) {
            next();
            member_name();                                  /* new.target */
            e = mk(N_NEWTARGET);
        } else {
            node *callee;
            if (is_kw(KW_NEW)) callee = parse_lhs();
            else callee = parse_primary();
            for (;;) {                                      /* только члены, без вызовов */
                if (p_accept('.')) { node *m = mk(N_MEMBER); m->a = callee; m->str = member_name(); callee = m; }
                else if (is_op('[')) {
                    next();
                    node *m = mk(N_INDEX);
                    m->a = callee;
                    int save = no_in; no_in = 0;
                    m->b = parse_expr();
                    no_in = save;
                    expect(']');
                    callee = m;
                } else if (CUR->type == T_TPL && !CUR->kw) callee = parse_template(callee);
                else break;
            }
            n->a = callee;
            if (is_op('(')) parse_args(n);
            else { n->list = js_alloc(sizeof(node *)); n->n = 0; }
            e = n;
        }
    } else e = parse_primary();
    int optional = 0;
    for (;;) {
        if (is_op('.')) {
            next();
            node *m = mk(N_MEMBER);
            m->a = e;
            m->str = member_name();
            e = m;
        } else if (is_op(P_OPTCHAIN)) {
            next();
            optional = 1;
            node *m;
            if (is_op('(')) { m = mk(N_CALL); m->a = e; parse_args(m); }
            else if (is_op('[')) {
                next();
                m = mk(N_INDEX);
                m->a = e;
                int save = no_in; no_in = 0;
                m->b = parse_expr();
                no_in = save;
                expect(']');
            } else { m = mk(N_MEMBER); m->a = e; m->str = member_name(); }
            m->flags |= F_OPTIONAL;
            e = m;
        } else if (is_op('[')) {
            next();
            node *m = mk(N_INDEX);
            m->a = e;
            int save = no_in; no_in = 0;
            m->b = parse_expr();
            no_in = save;
            expect(']');
            e = m;
        } else if (is_op('(')) {
            node *c = mk(N_CALL);
            c->a = e;
            parse_args(c);
            e = c;
        } else if (CUR->type == T_TPL && !CUR->kw) {
            e = parse_template(e);
        } else break;
    }
    if (optional) {
        node *o = mk(N_OPTCHAIN);
        o->a = e;
        e = o;
    }
    return e;
}

static node *parse_unary(void) {
    struct tok *t = CUR;
    int op = 0;
    if (t->type == T_PUNCT) {
        switch (t->op) {
        case '!': op = O_NOT; break;
        case '~': op = O_BNOT; break;
        case '-': op = O_NEG; break;
        case '+': op = O_PLUS; break;
        case P_INC: case P_DEC: {
            next();
            node *n = mk(N_UPDATE);
            n->op = t->op == P_INC ? O_INC : O_DEC;
            n->flags = F_PREFIX;
            n->a = parse_unary();
            return n;
        }
        }
    } else if (t->type == T_NAME) {
        if (t->kw == KW_TYPEOF) op = O_TYPEOF;
        else if (t->kw == KW_VOID) op = O_VOID;
        else if (t->kw == KW_DELETE) op = O_DELETE;
        else if (t->kw == KW_AWAIT && in_async) {
            next();
            node *n = mk(N_UNARY);
            n->op = O_NONE;                                  /* await */
            n->kind = 1;
            n->a = parse_unary();
            return n;
        }
    }
    if (op) {
        next();
        node *n = mk(N_UNARY);
        n->op = op;
        n->a = parse_unary();
        return n;
    }
    node *e = parse_lhs();
    if ((is_op(P_INC) || is_op(P_DEC)) && !CUR->nl) {
        node *n = mk(N_UPDATE);
        n->op = is_op(P_INC) ? O_INC : O_DEC;
        n->a = e;
        next();
        return n;
    }
    return e;
}

static int binop(int *op, int *logical) {
    struct tok *t = CUR;
    *logical = 0;
    if (t->type == T_NAME) {
        if (t->kw == KW_INSTANCEOF) { *op = O_INSTANCEOF; return 8; }
        if (t->kw == KW_IN && !no_in) { *op = O_IN; return 8; }
        return 0;
    }
    if (t->type != T_PUNCT) return 0;
    switch (t->op) {
    case P_NULLISH: *op = O_NULLISH; *logical = 1; return 1;
    case P_OR: *op = O_OR; *logical = 1; return 2;
    case P_AND: *op = O_AND; *logical = 1; return 3;
    case '|': *op = O_BOR; return 4;
    case '^': *op = O_BXOR; return 5;
    case '&': *op = O_BAND; return 6;
    case P_EQ: *op = O_EQ; return 7;
    case P_NE: *op = O_NE; return 7;
    case P_SEQ: *op = O_SEQ; return 7;
    case P_SNE: *op = O_SNE; return 7;
    case '<': *op = O_LT; return 8;
    case '>': *op = O_GT; return 8;
    case P_LE: *op = O_LE; return 8;
    case P_GE: *op = O_GE; return 8;
    case P_SHL: *op = O_SHL; return 9;
    case P_SHR: *op = O_SHR; return 9;
    case P_USHR: *op = O_USHR; return 9;
    case '+': *op = O_ADD; return 10;
    case '-': *op = O_SUB; return 10;
    case '*': *op = O_MUL; return 11;
    case '/': *op = O_DIV; return 11;
    case '%': *op = O_MOD; return 11;
    case P_EXP: *op = O_EXP; return 12;
    }
    return 0;
}

static node *parse_binary(int minprec) {
    node *left = parse_unary();
    for (;;) {
        int op, logical;
        int p = binop(&op, &logical);
        if (!p || p < minprec || failed) break;
        next();
        node *right = parse_binary(op == O_EXP ? p : p + 1);
        node *n = mk(logical ? N_LOGICAL : N_BINARY);
        n->op = op;
        n->a = left;
        n->b = right;
        left = n;
    }
    return left;
}

static node *parse_cond(void) {
    node *c = parse_binary(1);
    if (!is_op('?')) return c;
    next();
    node *n = mk(N_COND);
    n->a = c;
    int save = no_in;
    no_in = 0;
    n->b = parse_assign();
    no_in = save;
    expect(':');
    n->c = parse_assign();
    return n;
}

/* выражение -> шаблон присваивания ([a, b] = ..., {x} = ...) */
static node *to_pattern(node *e) {
    if (!e) return e;
    switch (e->type) {
    case N_IDENT: case N_MEMBER: case N_INDEX: case N_HOLE: return e;
    case N_ASSIGN:
        if (e->op != O_NONE) break;
        e->type = N_DEFAULT;
        e->a = to_pattern(e->a);
        return e;
    case N_DEFAULT: e->a = to_pattern(e->a); return e;
    case N_ARRAY:
        e->type = N_ARRPAT;
        for (int i = 0; i < e->n; i++) {
            node *x = e->list[i];
            if (x->type == N_SPREAD) { x->type = N_REST; x->a = to_pattern(x->a); }
            else e->list[i] = to_pattern(x);
        }
        return e;
    case N_OBJECT:
        e->type = N_OBJPAT;
        for (int i = 0; i < e->n; i++) {
            node *x = e->list[i];
            if (x->type == N_SPREAD) { x->type = N_REST; x->a = to_pattern(x->a); }
            else x->b = to_pattern(x->b);
        }
        return e;
    }
    perror_at("invalid assignment target");
    return e;
}

static node *parse_assign(void) {
    struct tok *t = CUR;
    if (t->type == T_NAME && t->kw == KW_ASYNC && !PEEK(1)->nl) {
        struct tok *t1 = PEEK(1);
        if (is_ident_tok(t1) && PEEK(2)->type == T_PUNCT && PEEK(2)->op == P_ARROW) { next(); return parse_arrow(1); }
        if (t1->type == T_PUNCT && t1->op == '(') {
            next();
            if (arrow_after_parens()) return parse_arrow(1);
            tp--;
        }
    }
    if (is_ident_tok(t) && PEEK(1)->type == T_PUNCT && PEEK(1)->op == P_ARROW && !PEEK(1)->nl) return parse_arrow(0);
    if (t->type == T_PUNCT && t->op == '(' && arrow_after_parens()) return parse_arrow(0);
    if (t->type == T_NAME && t->kw == KW_YIELD && in_gen) {      /* yield [*] выражение */
        next();
        node *y = mk(N_YIELD);
        if (p_accept('*')) y->op = 1;
        struct tok *nt = CUR;
        if (y->op || !(nt->nl || nt->type == T_EOF || (nt->type == T_PUNCT && strchr(")]};,:", nt->op) && nt->op < 128) ||
                       (nt->type == T_NAME && nt->kw == KW_IN)))
            y->a = parse_assign();
        return y;
    }

    node *lhs = parse_cond();
    struct tok *o = CUR;
    if (o->type != T_PUNCT) return lhs;
    int op;
    switch (o->op) {
    case '=': op = O_NONE; break;
    case P_ADD_EQ: op = O_ADD; break;
    case P_SUB_EQ: op = O_SUB; break;
    case P_MUL_EQ: op = O_MUL; break;
    case P_DIV_EQ: op = O_DIV; break;
    case P_MOD_EQ: op = O_MOD; break;
    case P_EXP_EQ: op = O_EXP; break;
    case P_SHL_EQ: op = O_SHL; break;
    case P_SHR_EQ: op = O_SHR; break;
    case P_USHR_EQ: op = O_USHR; break;
    case P_BAND_EQ: op = O_BAND; break;
    case P_BOR_EQ: op = O_BOR; break;
    case P_BXOR_EQ: op = O_BXOR; break;
    case P_AND_EQ: op = O_AND; break;
    case P_OR_EQ: op = O_OR; break;
    case P_NULLISH_EQ: op = O_NULLISH; break;
    default: return lhs;
    }
    next();
    node *n = mk(N_ASSIGN);
    n->op = op;
    if (op == O_NONE && (lhs->type == N_ARRAY || lhs->type == N_OBJECT)) lhs = to_pattern(lhs);
    else if (lhs->type != N_IDENT && lhs->type != N_MEMBER && lhs->type != N_INDEX) perror_at("invalid assignment target");
    n->a = lhs;
    n->b = parse_assign();
    return n;
}

static node *parse_expr(void) {
    node *e = parse_assign();
    if (!is_op(',')) return e;
    node *n = mk(N_SEQ);
    nlist l = { 0 };
    nl_add(&l, e);
    while (p_accept(',') && !failed) nl_add(&l, parse_assign());
    nl_store(&l, n);
    return n;
}

/* --- шаблоны в объявлениях и параметрах */

static node *parse_binding_element(void) {
    node *t = parse_binding_target();
    if (is_op('=')) {
        next();
        node *d = mk(N_DEFAULT);
        d->a = t;
        int save = no_in;
        d->b = parse_assign();
        no_in = save;
        return d;
    }
    return t;
}

static node *parse_binding_target(void) {
    if (is_op('[')) {
        node *n = mk(N_ARRPAT);
        nlist l = { 0 };
        next();
        while (!is_op(']') && !at_end()) {
            if (is_op(',')) { next(); nl_add(&l, mk(N_HOLE)); continue; }
            if (p_accept(P_ELLIPSIS)) { node *r = mk(N_REST); r->a = parse_binding_target(); nl_add(&l, r); }
            else nl_add(&l, parse_binding_element());
            if (!p_accept(',')) break;
        }
        expect(']');
        nl_store(&l, n);
        return n;
    }
    if (is_op('{')) {
        node *n = mk(N_OBJPAT);
        nlist l = { 0 };
        next();
        while (!is_op('}') && !at_end()) {
            node *p = mk(N_PROP);
            if (p_accept(P_ELLIPSIS)) { p->type = N_REST; p->a = parse_binding_target(); nl_add(&l, p); if (!p_accept(',')) break; continue; }
            struct tok *kt = CUR;
            parse_prop_key(p);
            if (p_accept(':')) p->b = parse_binding_element();
            else if (kt->type == T_NAME) {
                p->b = ident_node(p->str);
                if (p_accept('=')) { node *d = mk(N_DEFAULT); d->a = p->b; d->b = parse_assign(); p->b = d; }
            } else perror_at("expected ':'");
            nl_add(&l, p);
            if (!p_accept(',')) break;
        }
        expect('}');
        nl_store(&l, n);
        return n;
    }
    if (is_ident_tok(CUR)) { node *n = ident_node(CUR->s); next(); return n; }
    perror_at("expected variable name");
    return mk(N_HOLE);
}

static node *parse_params(void) {
    node *n = mk(N_BLOCK);
    nlist l = { 0 };
    expect('(');
    int save = no_in;
    no_in = 0;
    while (!is_op(')') && !at_end()) {
        if (p_accept(P_ELLIPSIS)) { node *r = mk(N_REST); r->a = parse_binding_target(); nl_add(&l, r); }
        else nl_add(&l, parse_binding_element());
        if (!p_accept(',')) break;
    }
    no_in = save;
    expect(')');
    nl_store(&l, n);
    return n;
}

static node *parse_block_body(int end_op) {
    node *b = mk(N_BLOCK);
    nlist l = { 0 };
    while (!is_op(end_op) && !at_end()) nl_add(&l, parse_stmt());
    nl_store(&l, b);
    return b;
}

static node *parse_function_body(node *fn) {
    int save_f = in_func;
    in_func = 1;
    expect('{');
    fn->a = parse_block_body('}');
    expect('}');
    in_func = save_f;
    return fn;
}

static node *parse_function(int flags, int is_decl) {
    node *fn = mk(N_FUNC);
    fn->flags = flags;
    if (!(flags & F_METHOD)) {
        next();                                             /* function */
        if (p_accept('*')) next_gen = 1;
        if (is_ident_tok(CUR)) { fn->str = CUR->s; next(); }
        else if (is_decl) perror_at("expected function name");
    }
    int save_a = in_async, save_in = no_in, save_g = in_gen;
    fn->kind = next_async;
    in_async = next_async;
    in_gen = next_gen;
    if (next_gen) {
        fn->flags |= F_GENERATOR;
        if (next_async) perror_at("async generators are not supported");
    }
    next_async = next_gen = 0;
    no_in = 0;
    node *ps = parse_params();
    fn->list = ps->list;
    fn->n = ps->n;
    parse_function_body(fn);
    in_async = save_a;
    in_gen = save_g;
    no_in = save_in;
    return fn;
}

/* this.<поле> = <значение> — для полей класса */
static node *field_assign(node *key, node *value) {
    node *as = mk(N_ASSIGN);
    as->op = O_NONE;
    if (key->flags & F_COMPUTED) { as->a = mk(N_INDEX); as->a->a = mk(N_THIS); as->a->b = key->a; }
    else { as->a = mk(N_MEMBER); as->a->a = mk(N_THIS); as->a->str = key->str; }
    as->b = value ? value : mk(N_UNDEF);
    node *st = mk(N_EXPR);
    st->a = as;
    return st;
}

static node *make_init_func(nlist *l) {
    node *fn = mk(N_FUNC);
    fn->flags = F_METHOD;
    fn->list = js_alloc(sizeof(node *));
    fn->n = 0;
    fn->a = mk(N_BLOCK);
    nl_store(l, fn->a);
    return fn;
}

static node *parse_class(int is_decl) {
    node *c = mk(N_CLASS);
    next();                                                 /* class */
    if (is_ident_tok(CUR)) { c->str = CUR->s; next(); }
    else if (is_decl) perror_at("expected class name");
    if (is_kw(KW_EXTENDS)) { next(); c->a = parse_lhs(); }
    expect('{');
    nlist methods = { 0 }, fields = { 0 }, sfields = { 0 };
    while (!is_op('}') && !at_end()) {
        if (p_accept(';')) continue;
        node *p = mk(N_PROP);
        int fl = 0;
        if (is_kw(KW_STATIC) && !(PEEK(1)->type == T_PUNCT && (PEEK(1)->op == '(' || PEEK(1)->op == '='))) {
            next();
            fl |= F_STATIC;
            if (is_op('{')) {                                 /* static { ... } */
                next();
                node *b = parse_block_body('}');
                expect('}');
                for (int i = 0; i < b->n; i++) nl_add(&sfields, b->list[i]);
                continue;
            }
        }
        int async_m = 0, gen_m = 0;
        if (is_kw(KW_ASYNC) && method_follows() && !PEEK(1)->nl) { next(); async_m = 1; }
        if (p_accept('*')) gen_m = 1;
        if ((is_kw(KW_GET) || is_kw(KW_SET)) && method_follows()) { fl |= is_kw(KW_GET) ? F_GETTER : F_SETTER; next(); }
        parse_prop_key(p);
        p->flags |= fl;
        if (is_op('(')) {
            int is_ctor = !(fl & (F_STATIC | F_GETTER | F_SETTER)) && p->str && !strcmp(p->str->s, "constructor");
            next_async = async_m;
            next_gen = gen_m;
            p->b = parse_function(F_METHOD | (is_ctor ? F_CTOR : 0) | (fl & (F_GETTER | F_SETTER)), 0);
            if (p->str) p->b->str = p->str;
            if (is_ctor) c->b = p->b;
            else nl_add(&methods, p);
        } else {                                              /* поле: x = 1; */
            node *val = NULL;
            if (p_accept('=')) val = parse_assign();
            consume_semi();
            nl_add(fl & F_STATIC ? &sfields : &fields, field_assign(p, val));
        }
    }
    expect('}');
    nl_store(&methods, c);
    if (fields.n) c->c = make_init_func(&fields); else free(fields.p);
    if (sfields.n) c->d = make_init_func(&sfields); else free(sfields.p);
    if (!c->b) {                                              /* конструктор по умолчанию */
        node *fn = mk(N_FUNC);
        fn->flags = F_METHOD | F_CTOR | F_DEFAULT_CTOR;
        fn->list = js_alloc(sizeof(node *));
        fn->a = mk(N_BLOCK);
        fn->a->list = js_alloc(sizeof(node *));
        c->b = fn;
    }
    c->b->str = c->str;
    if (c->a) c->b->flags |= F_DERIVED;
    return c;
}

/* --- операторы */

static void consume_semi(void) {
    if (p_accept(';')) return;
    if (is_op('}') || at_end() || CUR->nl) return;
    perror_at("expected ';'");
}

static node *parse_var(int kind, int need_semi) {
    node *n = mk(N_VAR);
    n->kind = kind;
    next();                                                 /* var / let / const */
    nlist l = { 0 };
    do {
        node *d = mk(N_DECL);
        d->a = parse_binding_target();
        if (p_accept('=')) d->b = parse_assign();
        nl_add(&l, d);
    } while (p_accept(',') && !failed);
    nl_store(&l, n);
    if (need_semi) consume_semi();
    return n;
}

static int let_is_decl(void) {
    if (!is_kw(KW_LET)) return 0;
    struct tok *t = PEEK(1);
    return (t->type == T_NAME && (!t->kw || t->kw > KW_LAST_RESERVED)) || (t->type == T_PUNCT && (t->op == '[' || t->op == '{'));
}

static node *parse_for(void) {
    int line = CUR->line;
    next();                                                 /* for */
    if (is_kw(KW_AWAIT)) perror_at("for await is not supported");
    expect('(');
    node *init = NULL;
    int kind = 0;
    if (is_op(';')) {
    } else {
        no_in = 1;
        if (is_kw(KW_VAR) || is_kw(KW_CONST) || let_is_decl()) {
            kind = is_kw(KW_VAR) ? K_VAR : is_kw(KW_CONST) ? K_CONST : K_LET;
            init = parse_var(kind, 0);
        } else init = parse_expr();
        no_in = 0;
        if (is_kw(KW_OF) || is_kw(KW_IN)) {
            node *n = mk(is_kw(KW_OF) ? N_FOROF : N_FORIN);
            n->line = line;
            next();
            n->kind = kind;
            if (init->type == N_VAR) {
                if (init->n != 1) perror_at("bad for-in/of declaration");
                n->a = init->list[0]->a;
            } else n->a = to_pattern(init);
            n->b = n->type == N_FOROF ? parse_assign() : parse_expr();
            expect(')');
            n->c = parse_stmt();
            return n;
        }
    }
    node *n = mk(N_FOR);
    n->line = line;
    n->kind = kind;
    n->a = init;
    expect(';');
    if (!is_op(';')) n->b = parse_expr();
    expect(';');
    if (!is_op(')')) n->c = parse_expr();
    expect(')');
    n->d = parse_stmt();
    return n;
}

static node *parse_stmt(void) {
    struct tok *t = CUR;
    node *n;
    if (failed) return mk(N_EMPTY);
    if (t->type == T_PUNCT) {
        if (t->op == '{') {
            next();
            n = parse_block_body('}');
            expect('}');
            return n;
        }
        if (t->op == ';') { next(); return mk(N_EMPTY); }
    }
    if (t->type == T_NAME) {
        switch (t->kw) {
        case KW_VAR: return parse_var(K_VAR, 1);
        case KW_CONST: return parse_var(K_CONST, 1);
        case KW_LET: if (let_is_decl()) return parse_var(K_LET, 1); break;
        case KW_FUNCTION: n = mk(N_FUNCDECL); n->a = parse_function(0, 1); n->str = n->a->str; return n;
        case KW_ASYNC:
            if (PEEK(1)->type == T_NAME && PEEK(1)->kw == KW_FUNCTION && !PEEK(1)->nl) {
                next();
                n = mk(N_FUNCDECL);
                next_async = 1;
                n->a = parse_function(0, 1);
                n->str = n->a->str;
                return n;
            }
            break;
        case KW_CLASS: n = mk(N_CLASSDECL); n->a = parse_class(1); n->str = n->a->str; return n;
        case KW_IF:
            n = mk(N_IF);
            next();
            expect('(');
            n->a = parse_expr();
            expect(')');
            n->b = parse_stmt();
            if (is_kw(KW_ELSE)) { next(); n->c = parse_stmt(); }
            return n;
        case KW_FOR: return parse_for();
        case KW_WHILE:
            n = mk(N_WHILE);
            next();
            expect('(');
            n->a = parse_expr();
            expect(')');
            n->b = parse_stmt();
            return n;
        case KW_DO:
            n = mk(N_DOWHILE);
            next();
            n->b = parse_stmt();
            if (!is_kw(KW_WHILE)) { perror_at("expected while"); return n; }
            next();
            expect('(');
            n->a = parse_expr();
            expect(')');
            p_accept(';');
            return n;
        case KW_RETURN:
            n = mk(N_RETURN);
            next();
            if (!is_op(';') && !is_op('}') && !at_end() && !CUR->nl) n->a = parse_expr();
            consume_semi();
            return n;
        case KW_BREAK: case KW_CONTINUE:
            n = mk(t->kw == KW_BREAK ? N_BREAK : N_CONTINUE);
            next();
            if (is_ident_tok(CUR) && !CUR->nl) { n->str = CUR->s; next(); }
            consume_semi();
            return n;
        case KW_THROW:
            n = mk(N_THROW);
            next();
            n->a = parse_expr();
            consume_semi();
            return n;
        case KW_TRY:
            n = mk(N_TRY);
            next();
            expect('{');
            n->a = parse_block_body('}');
            expect('}');
            if (is_kw(KW_CATCH)) {
                next();
                if (p_accept('(')) { n->b = parse_binding_target(); expect(')'); }
                expect('{');
                n->c = parse_block_body('}');
                expect('}');
            }
            if (is_kw(KW_FINALLY)) {
                next();
                expect('{');
                n->d = parse_block_body('}');
                expect('}');
            }
            if (!n->c && !n->d) perror_at("expected catch or finally");
            return n;
        case KW_SWITCH: {
            n = mk(N_SWITCH);
            next();
            expect('(');
            n->a = parse_expr();
            expect(')');
            expect('{');
            nlist cases = { 0 };
            while (!is_op('}') && !at_end()) {
                node *c = mk(N_CASE);
                if (is_kw(KW_CASE)) { next(); c->a = parse_expr(); }
                else if (is_kw(KW_DEFAULT)) next();
                else { perror_at("expected case"); break; }
                expect(':');
                nlist body = { 0 };
                while (!is_kw(KW_CASE) && !is_kw(KW_DEFAULT) && !is_op('}') && !at_end()) nl_add(&body, parse_stmt());
                nl_store(&body, c);
                nl_add(&cases, c);
            }
            expect('}');
            nl_store(&cases, n);
            return n;
        }
        case KW_DEBUGGER: next(); consume_semi(); return mk(N_EMPTY);
        case KW_WITH:                                        /* with (obj) оператор */
            n = mk(N_WITH);
            next();
            expect('(');
            n->a = parse_expr();
            expect(')');
            n->b = parse_stmt();
            return n;
        case KW_IMPORT: case KW_EXPORT:
            if (!(PEEK(1)->type == T_PUNCT && (PEEK(1)->op == '(' || PEEK(1)->op == '.'))) { perror_at("modules are not supported"); return mk(N_EMPTY); }
            break;
        }
        if (is_ident_tok(t) && PEEK(1)->type == T_PUNCT && PEEK(1)->op == ':') {
            n = mk(N_LABEL);
            n->str = t->s;
            next();
            next();
            n->a = parse_stmt();
            return n;
        }
    }
    n = mk(N_EXPR);
    n->a = parse_expr();
    consume_semi();
    return n;
}

/* ------------------------------------------------------------------ разрешение имён */

enum { B_VAR = 1, B_LET, B_CONST, B_PARAM, B_FUNC, B_HIDDEN };

struct rs {
    struct rs *parent;
    int is_func, is_global, mat, is_with;
    jsstr **names;
    ju8 *kinds;
    int n, cap;
    node *fn;
};

static int rs_find(struct rs *r, jsstr *name) {
    for (int i = 0; i < r->n; i++) if (r->names[i] == name) return i;
    return -1;
}

static int declare(struct rs *r, jsstr *name, int kind) {
    if (r->is_global) return -1;
    int i = rs_find(r, name);
    if (i >= 0) {
        if (kind == B_CONST || kind == B_LET) r->kinds[i] = kind;
        return i;
    }
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 8;
        jsstr **nn = malloc(sizeof(jsstr *) * cap);
        ju8 *kk = malloc(cap);
        if (r->n) { memcpy(nn, r->names, sizeof(jsstr *) * r->n); memcpy(kk, r->kinds, r->n); }
        free(r->names);
        free(r->kinds);
        r->names = nn;
        r->kinds = kk;
        r->cap = cap;
    }
    r->names[r->n] = name;
    r->kinds[r->n] = kind;
    r->mat = 1;
    return r->n++;
}

static void rs_free(struct rs *r) { free(r->names); free(r->kinds); }

static void lookup(struct rs *r, node *id, jsstr *name) {
    int hops = 0;
    for (; r; r = r->parent) {
        if (r->is_global) break;
        if (r->is_with && !(id->flags & F_WITH) && name->s[0] != '%') {   /* внутри with: сначала свойство объекта */
            id->flags |= F_WITH;
            id->num = hops;
        }
        int i = rs_find(r, name);
        if (i >= 0) {
            id->depth = hops;
            id->slot = i;
            if (r->kinds[i] == B_CONST) id->flags |= F_CONST;
            if (name == S_arguments && r->kinds[i] == B_HIDDEN && r->fn) r->fn->flags |= F_USES_ARGS;
            return;
        }
        if (r->mat) hops++;
    }
    id->depth = -1;
}

static void pattern_names(node *p, struct rs *r, int kind) {
    if (!p) return;
    switch (p->type) {
    case N_IDENT: declare(r, p->str, kind); break;
    case N_DEFAULT: case N_REST: pattern_names(p->a, r, kind); break;
    case N_ARRPAT: for (int i = 0; i < p->n; i++) pattern_names(p->list[i], r, kind); break;
    case N_OBJPAT:
        for (int i = 0; i < p->n; i++) pattern_names(p->list[i]->type == N_REST ? p->list[i]->a : p->list[i]->b, r, kind);
        break;
    }
}

/* var — поднимаются до начала функции (из всех вложенных блоков) */
static void hoist_vars(node *s, struct rs *r) {
    if (!s) return;
    switch (s->type) {
    case N_VAR:
        if (s->kind == K_VAR) for (int i = 0; i < s->n; i++) pattern_names(s->list[i]->a, r, B_VAR);
        break;
    case N_BLOCK: for (int i = 0; i < s->n; i++) hoist_vars(s->list[i], r); break;
    case N_IF: hoist_vars(s->b, r); hoist_vars(s->c, r); break;
    case N_FOR: hoist_vars(s->a, r); hoist_vars(s->d, r); break;
    case N_FORIN: case N_FOROF: if (s->kind == K_VAR) pattern_names(s->a, r, B_VAR); hoist_vars(s->c, r); break;
    case N_WHILE: case N_DOWHILE: hoist_vars(s->b, r); break;
    case N_TRY: hoist_vars(s->a, r); hoist_vars(s->c, r); hoist_vars(s->d, r); break;
    case N_SWITCH: for (int i = 0; i < s->n; i++) for (int k = 0; k < s->list[i]->n; k++) hoist_vars(s->list[i]->list[k], r); break;
    case N_LABEL: hoist_vars(s->a, r); break;
    case N_WITH: hoist_vars(s->b, r); break;
    }
}

/* let/const/class/function прямо в этом списке операторов */
static void lexical_decls(node **list, int n, struct rs *r, nlist *funcs) {
    for (int i = 0; i < n; i++) {
        node *s = list[i];
        while (s->type == N_LABEL) s = s->a;
        if (s->type == N_VAR && s->kind != K_VAR) for (int k = 0; k < s->n; k++) pattern_names(s->list[k]->a, r, s->kind == K_CONST ? B_CONST : B_LET);
        else if (s->type == N_CLASSDECL) declare(r, s->str, B_LET);
        else if (s->type == N_FUNCDECL) { s->slot = declare(r, s->str, B_FUNC); nl_add(funcs, s); }
    }
}

static struct scope *make_scope(struct rs *r, nlist *funcs) {
    struct scope *sc = js_alloc(sizeof(struct scope));
    sc->nslots = r->n;
    sc->self_slot = -1;
    sc->nfuncs = funcs->n;
    sc->funcs = js_alloc(sizeof(node *) * (funcs->n ? funcs->n : 1));
    if (funcs->n) memcpy(sc->funcs, funcs->p, sizeof(node *) * funcs->n);
    free(funcs->p);
    return sc;
}

static void res_expr(node *e, struct rs *r);
static void res_stmt(node *s, struct rs *r);
static void res_func(node *fn, struct rs *r, int is_decl);

static void res_list(node **l, int n, struct rs *r) { for (int i = 0; i < n; i++) res_stmt(l[i], r); }

static void res_pattern(node *p, struct rs *r) {
    if (!p) return;
    switch (p->type) {
    case N_IDENT: lookup(r, p, p->str); break;
    case N_DEFAULT: res_pattern(p->a, r); res_expr(p->b, r); break;
    case N_REST: res_pattern(p->a, r); break;
    case N_ARRPAT: for (int i = 0; i < p->n; i++) res_pattern(p->list[i], r); break;
    case N_OBJPAT:
        for (int i = 0; i < p->n; i++) {
            node *x = p->list[i];
            if (x->type == N_REST) res_pattern(x->a, r);
            else { if (x->flags & F_COMPUTED) res_expr(x->a, r); res_pattern(x->b, r); }
        }
        break;
    default: res_expr(p, r);
    }
}

static void res_expr(node *e, struct rs *r) {
    if (!e) return;
    switch (e->type) {
    case N_IDENT: lookup(r, e, e->str); break;
    case N_THIS: lookup(r, e, S_this); break;
    case N_SUPER: lookup(r, e, S_callee_slot); break;
    case N_FUNC: res_func(e, r, 0); break;
    case N_CLASS:
        res_expr(e->a, r);
        res_func(e->b, r, 0);
        for (int i = 0; i < e->n; i++) { if (e->list[i]->flags & F_COMPUTED) res_expr(e->list[i]->a, r); res_func(e->list[i]->b, r, 0); }
        if (e->c) res_func(e->c, r, 0);
        if (e->d) res_func(e->d, r, 0);
        break;
    case N_ARRAY: case N_SEQ: case N_TPL:
        for (int i = 0; i < e->n; i++) res_expr(e->list[i], r);
        break;
    case N_TAGGED: res_expr(e->a, r); for (int i = 0; i < e->n; i++) res_expr(e->list[i], r); break;
    case N_OBJECT:
        for (int i = 0; i < e->n; i++) {
            node *p = e->list[i];
            if (p->type == N_SPREAD) { res_expr(p->a, r); continue; }
            if (p->flags & F_COMPUTED) res_expr(p->a, r);
            res_expr(p->b, r);
        }
        break;
    case N_CALL: case N_NEW:
        res_expr(e->a, r);
        for (int i = 0; i < e->n; i++) res_expr(e->list[i], r);
        break;
    case N_ASSIGN:
        if (e->a->type == N_ARRPAT || e->a->type == N_OBJPAT) res_pattern(e->a, r);
        else res_expr(e->a, r);
        res_expr(e->b, r);
        break;
    case N_ARRPAT: case N_OBJPAT: case N_DEFAULT: case N_REST: res_pattern(e, r); break;
    default:
        res_expr(e->a, r);
        res_expr(e->b, r);
        res_expr(e->c, r);
        break;
    }
}

/* блок со своими let/const/function: своё окружение, если есть что объявить */
static void res_block(node *b, struct rs *r) {
    struct rs br = { 0 };
    br.parent = r;
    nlist funcs = { 0 };
    lexical_decls(b->list, b->n, &br, &funcs);
    b->scope = br.n ? make_scope(&br, &funcs) : NULL;
    if (!br.n) free(funcs.p);
    res_list(b->list, b->n, br.n ? &br : r);
    rs_free(&br);
}

static void res_stmt(node *s, struct rs *r) {
    if (!s) return;
    switch (s->type) {
    case N_VAR:
        for (int i = 0; i < s->n; i++) {
            res_pattern(s->list[i]->a, r);
            res_expr(s->list[i]->b, r);
            if (s->kind == K_CONST) s->list[i]->a->flags &= ~F_CONST;
        }
        break;
    case N_EXPR: case N_THROW: case N_RETURN: res_expr(s->a, r); break;
    case N_BLOCK: res_block(s, r); break;
    case N_IF: res_expr(s->a, r); res_stmt(s->b, r); res_stmt(s->c, r); break;
    case N_WHILE: case N_DOWHILE: res_expr(s->a, r); res_stmt(s->b, r); break;
    case N_LABEL: res_stmt(s->a, r); break;
    case N_WITH: {
        res_expr(s->a, r);
        struct rs wr = { 0 };
        wr.parent = r;
        wr.is_with = 1;
        declare(&wr, js_perm("%with"), B_HIDDEN);
        nlist none = { 0 };
        s->scope = make_scope(&wr, &none);
        res_stmt(s->b, &wr);
        rs_free(&wr);
        break;
    }
    case N_FUNCDECL:
        res_func(s->a, r, 1);
        if (s->slot < 0 || r->is_global) { s->depth = -1; }
        break;
    case N_CLASSDECL: {
        res_expr(s->a, r);
        node id = { 0 };
        lookup(r, &id, s->str);
        s->depth = id.depth;
        s->slot = id.slot;
        break;
    }
    case N_FOR: {
        struct rs lr = { 0 };
        lr.parent = r;
        if (s->a && s->a->type == N_VAR && s->kind != K_VAR) {
            for (int i = 0; i < s->a->n; i++) pattern_names(s->a->list[i]->a, &lr, s->kind == K_CONST ? B_CONST : B_LET);
        }
        struct rs *use = lr.n ? &lr : r;
        nlist none = { 0 };
        s->scope = lr.n ? make_scope(&lr, &none) : NULL;
        if (s->a) { if (s->a->type == N_VAR) res_stmt(s->a, use); else res_expr(s->a, use); }
        res_expr(s->b, use);
        res_expr(s->c, use);
        res_stmt(s->d, use);
        rs_free(&lr);
        break;
    }
    case N_FORIN: case N_FOROF: {
        res_expr(s->b, r);
        struct rs lr = { 0 };
        lr.parent = r;
        if (s->kind == K_LET || s->kind == K_CONST) pattern_names(s->a, &lr, B_LET);
        struct rs *use = lr.n ? &lr : r;
        nlist none = { 0 };
        s->scope = lr.n ? make_scope(&lr, &none) : NULL;
        res_pattern(s->a, use);
        res_stmt(s->c, use);
        rs_free(&lr);
        break;
    }
    case N_TRY:
        res_block(s->a, r);
        if (s->c) {
            struct rs cr = { 0 };
            cr.parent = r;
            if (s->b) pattern_names(s->b, &cr, B_LET);
            nlist none = { 0 };
            s->scope = cr.n ? make_scope(&cr, &none) : NULL;
            struct rs *use = cr.n ? &cr : r;
            res_pattern(s->b, use);
            res_block(s->c, use);
            rs_free(&cr);
        }
        if (s->d) res_block(s->d, r);
        break;
    case N_SWITCH: {
        res_expr(s->a, r);
        struct rs sr = { 0 };
        sr.parent = r;
        nlist funcs = { 0 };
        for (int i = 0; i < s->n; i++) lexical_decls(s->list[i]->list, s->list[i]->n, &sr, &funcs);
        s->scope = sr.n ? make_scope(&sr, &funcs) : NULL;
        if (!sr.n) free(funcs.p);
        struct rs *use = sr.n ? &sr : r;
        for (int i = 0; i < s->n; i++) { res_expr(s->list[i]->a, use); res_list(s->list[i]->list, s->list[i]->n, use); }
        rs_free(&sr);
        break;
    }
    }
}

static void res_func(node *fn, struct rs *r, int is_decl) {
    struct rs fr = { 0 };
    fr.parent = r;
    fr.is_func = 1;
    fr.mat = 1;
    fr.fn = fn;
    if (!(fn->flags & F_ARROW)) {
        declare(&fr, S_this, B_HIDDEN);
        declare(&fr, S_arguments, B_HIDDEN);
        declare(&fr, S_callee_slot, B_HIDDEN);
    }
    int simple = 1;
    for (int i = 0; i < fn->n; i++) {
        pattern_names(fn->list[i], &fr, B_PARAM);
        if (fn->list[i]->type != N_IDENT) simple = 0;
    }
    if (simple) fn->flags |= F_SIMPLE_PARAMS;
    int self = -1;
    if (fn->str && !is_decl && !(fn->flags & F_METHOD) && rs_find(&fr, fn->str) < 0) self = declare(&fr, fn->str, B_FUNC);
    nlist funcs = { 0 };
    if (!(fn->flags & F_EXPRBODY)) {
        hoist_vars(fn->a, &fr);
        lexical_decls(fn->a->list, fn->a->n, &fr, &funcs);
    }
    for (int i = 0; i < fn->n; i++) res_pattern(fn->list[i], &fr);
    if (fn->flags & F_EXPRBODY) res_expr(fn->a, &fr);
    else res_list(fn->a->list, fn->a->n, &fr);
    /* имена, объявленные позже (при разборе тела), тоже должны попасть в слоты */
    fn->scope = make_scope(&fr, &funcs);
    fn->scope->self_slot = self;
    rs_free(&fr);
}

static void res_script(node *sc) {
    struct rs g = { 0 };
    g.is_global = 1;
    nlist funcs = { 0 };
    for (int i = 0; i < sc->n; i++) {
        node *s = sc->list[i];
        while (s->type == N_LABEL) s = s->a;
        if (s->type == N_FUNCDECL) { s->slot = -1; nl_add(&funcs, s); }
    }
    sc->scope = js_alloc(sizeof(struct scope));
    sc->scope->nfuncs = funcs.n;
    sc->scope->funcs = js_alloc(sizeof(node *) * (funcs.n ? funcs.n : 1));
    if (funcs.n) memcpy(sc->scope->funcs, funcs.p, sizeof(node *) * funcs.n);
    free(funcs.p);
    sc->scope->self_slot = -1;
    /* имена var — чтобы заранее завести их в глобальном объекте */
    struct rs tmp = { 0 };
    for (int i = 0; i < sc->n; i++) hoist_vars(sc->list[i], &tmp);
    nlist dummy = { 0 };
    lexical_decls(sc->list, sc->n, &tmp, &dummy);
    free(dummy.p);
    sc->n = sc->n;
    node *vars = mk(N_BLOCK);
    vars->n = tmp.n;
    vars->list = js_alloc(sizeof(node *) * (tmp.n ? tmp.n : 1));
    for (int i = 0; i < tmp.n; i++) vars->list[i] = ident_node(tmp.names[i]);
    sc->a = vars;
    rs_free(&tmp);
    res_list(sc->list, sc->n, &g);
}

/* ------------------------------------------------------------------ вход */

node *js_parse(const char *s, int len, const char *file, int as_function, char *err, int errmax) {
    src = s;
    slen = len;
    spos = 0;
    sline = 1;
    ntok = 0;
    failed = 0;
    perr = err;
    perrmax = errmax;
    err[0] = 0;
    int fl = strlen(file ? file : "?");
    char *fcopy = js_alloc(fl + 1);                           /* имя живёт столько же, сколько дерево */
    memcpy(fcopy, file ? file : "?", fl + 1);
    cur_file = fcopy;
    if (!lex_all()) { free(toks); toks = NULL; tcap = 0; return NULL; }
    tp = 0;
    no_in = 0;
    in_func = as_function;
    in_async = 0;
    in_gen = next_gen = next_async = 0;
    node *sc = mk(N_SCRIPT);
    nlist l = { 0 };
    while (!at_end() && !failed) nl_add(&l, parse_stmt());
    nl_store(&l, sc);
    free(toks);
    toks = NULL;
    tcap = 0;
    ntok = 0;
    if (failed) return NULL;
    res_script(sc);
    return sc;
}
