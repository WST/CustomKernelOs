/* Лексер: текст -> список токенов (слова, числа, строки, знаки). */
#include "cc.h"

static File *cur_file;
static bool  at_bol, has_space;

static Token *new_token(TokenKind kind, char *start, char *end, int line) {
    Token *t = xcalloc(sizeof(Token));
    t->kind = kind;
    t->loc = start;
    t->len = end - start;
    t->file = cur_file;
    t->line = line;
    t->at_bol = at_bol;
    t->has_space = has_space;
    at_bol = has_space = false;
    return t;
}

Token *copy_token(Token *t) {
    Token *c = xcalloc(sizeof(Token));
    *c = *t;
    c->next = NULL;
    return c;
}

Token *new_eof(Token *tmpl) {
    Token *t = copy_token(tmpl);
    t->kind = TK_EOF;
    t->len = 0;
    return t;
}

bool equal(Token *tok, const char *s) {
    int n = strlen(s);
    return tok->len == n && !strncmp(tok->loc, s, n) && tok->kind != TK_STR;
}

Token *skip(Token *tok, const char *s) {
    if (!equal(tok, s) && tok->at_bol && !strcmp(s, ";")) error_tok(tok, "expected ';' (missing at the end of the previous line?)");
    if (!equal(tok, s)) error_tok(tok, "expected '%s'", s);
    return tok->next;
}

bool consume(Token **rest, Token *tok, const char *s) {
    if (equal(tok, s)) { *rest = tok->next; return true; }
    *rest = tok;
    return false;
}

static bool is_ident1(int c) { return isalpha(c) || c == '_' || c == '$'; }
static bool is_ident2(int c) { return is_ident1(c) || isdigit(c); }

static int from_hex(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}
static bool is_hex(int c) { return isdigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

static int punct_len(char *p) {
    static const char *kw[] = {
        "<<=", ">>=", "...", "==", "!=", "<=", ">=", "->", "+=", "-=", "*=", "/=", "++", "--",
        "%=", "&=", "|=", "^=", "&&", "||", "<<", ">>", "##",
    };
    for (int i = 0; i < (int)(sizeof(kw) / sizeof(*kw)); i++) {
        int n = strlen(kw[i]);
        if (!strncmp(p, kw[i], n)) return n;
    }
    return strchr("+-*/%&|^~!=<>?:;,.()[]{}#", *p) && *p ? 1 : 0;
}

static bool is_keyword(Token *tok) {
    static const char *kw[] = {
        "return", "if", "else", "for", "while", "int", "sizeof", "char", "struct", "union",
        "short", "long", "void", "typedef", "_Bool", "enum", "static", "goto", "break",
        "continue", "switch", "case", "default", "extern", "_Alignof", "signed", "unsigned",
        "const", "volatile", "auto", "register", "restrict", "__restrict", "inline", "__inline",
        "float", "double", "do", "__attribute__", "__syscall", "_Static_assert", "_Noreturn",
    };
    for (int i = 0; i < (int)(sizeof(kw) / sizeof(*kw)); i++)
        if (equal(tok, kw[i])) return true;
    return false;
}

void convert_keywords(Token *tok) {
    for (Token *t = tok; t->kind != TK_EOF; t = t->next)
        if (t->kind == TK_IDENT && is_keyword(t)) t->kind = TK_KEYWORD;
}

static int read_escape(char **new_pos, char *p) {
    if (*p >= '0' && *p <= '7') {                  /* \0, \033 */
        int c = *p++ - '0';
        if (*p >= '0' && *p <= '7') { c = (c << 3) + (*p++ - '0'); if (*p >= '0' && *p <= '7') c = (c << 3) + (*p++ - '0'); }
        *new_pos = p;
        return c;
    }
    if (*p == 'x') {
        p++;
        if (!is_hex(*p)) error("invalid hex escape sequence");
        int c = 0;
        for (; is_hex(*p); p++) c = (c << 4) + from_hex(*p);
        *new_pos = p;
        return c;
    }
    *new_pos = p + 1;
    switch (*p) {
    case 'a': return 7;
    case 'b': return 8;
    case 't': return 9;
    case 'n': return 10;
    case 'v': return 11;
    case 'f': return 12;
    case 'r': return 13;
    case 'e': return 27;
    default:  return *p;
    }
}

static char *string_end(char *p, int line) {
    char *start = p;
    for (; *p != '"'; p++) {
        if (*p == '\n' || *p == 0) {
            cur_file = cur_file;
            error("%s:%d: unclosed string literal", cur_file->name, line);
        }
        if (*p == '\\') p++;
    }
    (void)start;
    return p;
}

static Token *read_string(char *start, int line) {
    char *end = string_end(start + 1, line);
    char *buf = xcalloc(end - start);
    int len = 0;
    for (char *p = start + 1; p < end;) {
        if (*p == '\\') buf[len++] = read_escape(&p, p + 1);
        else buf[len++] = *p++;
    }
    Token *t = new_token(TK_STR, start, end + 1, line);
    t->ty = array_of(ty_char, len + 1);
    t->str = buf;
    t->str_len = len + 1;
    return t;
}

static Token *read_char(char *start, int line) {
    char *p = start + 1;
    if (*p == 0) error("%s:%d: unclosed char literal", cur_file->name, line);
    int c;
    if (*p == '\\') c = read_escape(&p, p + 1);
    else c = (unsigned char)*p++;
    if (*p != '\'') error("%s:%d: unclosed char literal", cur_file->name, line);
    Token *t = new_token(TK_NUM, start, p + 1, line);
    t->val = (signed char)c;
    t->ival = (unsigned long long)(long long)t->val;
    t->ty = ty_int;
    return t;
}

/* Число: сначала берём «pp-число» целиком, разбираем его потом */
static bool convert_int(Token *tok) {
    char *p = tok->loc;
    unsigned base = 10;
    if ((p[0] == '0') && (p[1] == 'x' || p[1] == 'X') && is_hex(p[2])) { p += 2; base = 16; }
    else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B') && (p[2] == '0' || p[2] == '1')) { p += 2; base = 2; }
    else if (p[0] == '0') base = 8;
    unsigned long long v = 0;
    for (;; p++) {
        int d;
        if (isdigit(*p)) d = *p - '0';
        else if (base == 16 && is_hex(*p)) d = from_hex(*p);
        else break;
        if (d >= (int)base) return false;
        v = v * base + (unsigned)d;
    }
    bool uns = false;
    int nL = 0;
    for (;; p++) {
        if (*p == 'u' || *p == 'U') uns = true;
        else if (*p == 'l' || *p == 'L') nL++;
        else break;
    }
    if (p != tok->loc + tok->len) return false;
    Type *ty;
    if (nL >= 2) ty = uns || v > 0x7FFFFFFFFFFFFFFFull ? ty_ullong : ty_llong;
    else if (uns) ty = v > 0xFFFFFFFFu ? ty_ullong : ty_uint;
    else if (v > 0x7FFFFFFFFFFFFFFFull) ty = ty_ullong;
    else if (v > 0xFFFFFFFFu) ty = ty_llong;
    else if (v > 0x7FFFFFFFu) ty = base == 10 ? ty_llong : ty_uint;
    else ty = ty_int;
    tok->kind = TK_NUM;
    tok->val = (int)v;
    tok->ival = v;
    tok->ty = ty;
    return true;
}

static double parse_double(char *p, char **end) {
    double v = 0;
    while (isdigit(*p)) v = v * 10 + (*p++ - '0');
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (isdigit(*p)) { v += (*p++ - '0') * f; f /= 10; }
    }
    if (*p == 'e' || *p == 'E') {
        p++;
        int sign = 1, e = 0;
        if (*p == '+') p++;
        else if (*p == '-') { sign = -1; p++; }
        while (isdigit(*p)) e = e * 10 + (*p++ - '0');
        while (e--) v = sign > 0 ? v * 10 : v / 10;
    }
    *end = p;
    return v;
}

void convert_pp_number(Token *tok) {
    if (convert_int(tok)) return;
    char *end;
    double v = parse_double(tok->loc, &end);
    Type *ty = ty_double;
    if (*end == 'f' || *end == 'F') { ty = ty_float; end++; }
    else if (*end == 'l' || *end == 'L') end++;
    if (end != tok->loc + tok->len) error_tok(tok, "invalid number");
    tok->kind = TK_NUM;
    tok->fval = v;
    tok->ty = ty;
}

Token *tokenize(File *file) {
    cur_file = file;
    char *p = file->contents;
    Token head = {0}, *cur = &head;
    int line = 1;
    at_bol = true;
    has_space = false;
    while (*p) {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n') p++;
            has_space = true;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            char *q = p + 2;
            while (*q && !(q[0] == '*' && q[1] == '/')) { if (*q == '\n') line++; q++; }
            if (!*q) error("%s:%d: unclosed comment", file->name, line);
            p = q + 2;
            has_space = true;
            continue;
        }
        if (*p == '\n') { p++; line++; at_bol = true; has_space = false; continue; }
        if (isspace(*p)) { p++; has_space = true; continue; }
        if (isdigit(*p) || (*p == '.' && isdigit(p[1]))) {
            char *q = p++;
            for (;;) {
                if (p[0] && p[1] && strchr("eEpP", p[0]) && strchr("+-", p[1])) p += 2;
                else if (isalnum(*p) || *p == '.') p++;
                else break;
            }
            cur = cur->next = new_token(TK_PP_NUM, q, p, line);
            continue;
        }
        if (*p == 'L' && p[1] == '\'') {
            Token *t = read_char(p + 1, line);
            t->len += 1;
            t->loc = p;
            cur = cur->next = t;
            p += t->len;
            continue;
        }
        if (*p == '"') { cur = cur->next = read_string(p, line); p += cur->len; continue; }
        if (*p == '\'') { cur = cur->next = read_char(p, line); p += cur->len; continue; }
        if (is_ident1(*p)) {
            char *q = p++;
            while (is_ident2(*p)) p++;
            cur = cur->next = new_token(TK_IDENT, q, p, line);
            continue;
        }
        int n = punct_len(p);
        if (n) { cur = cur->next = new_token(TK_PUNCT, p, p + n, line); p += n; continue; }
        if ((unsigned char)*p >= 0x80 || *p == '\r' || *p == '\f') { p++; continue; }   /* мусор вне строк */
        error("%s:%d: unexpected character '%c'", file->name, line, *p);
    }
    cur = cur->next = new_token(TK_EOF, p, p, line);
    return head.next;
}

/* убрать «\ + перевод строки», сохранив количество строк для сообщений об ошибках */
static void remove_backslash_newline(char *p) {
    int i = 0, j = 0, n = 0;
    while (p[i]) {
        if (p[i] == '\\' && p[i + 1] == '\n') { i += 2; n++; }
        else if (p[i] == '\n') { p[j++] = p[i++]; for (; n > 0; n--) p[j++] = '\n'; }
        else p[j++] = p[i++];
    }
    for (; n > 0; n--) p[j++] = '\n';
    p[j] = 0;
}

Token *tokenize_file(const char *path) {
    int len;
    char *s = read_file(path, &len);
    if (!s) return NULL;
    remove_backslash_newline(s);
    File *f = xcalloc(sizeof(File));
    f->name = strndup2(path, strlen(path));
    f->contents = s;
    return tokenize(f);
}
