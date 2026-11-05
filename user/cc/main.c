/* cc — компилятор Си для MyOS. Запуск:
 *
 *   cc hello.c                 -> программа hello в текущей папке
 *   cc -o /bin/hi a.c b.c      -> несколько файлов в одну программу
 *   cc -E hello.c              -> показать текст после препроцессора
 *
 * Заголовки (#include <myos.h>, <gfx.h>) берутся из /include, библиотеки —
 * исходники /lib/libc.c и /lib/gfx.c — компилируются вместе с программой. */
#include "cc.h"

char **include_paths;
int    n_include_paths;
bool   opt_linux;

static char *base_dir = "";         /* -B: где искать include/ и lib/ (по умолчанию корень MyOS) */

/* ------------------------------------------------------------ сообщения об ошибках */

static void vreport(const char *file, int line, char *loc, char *line_start, const char *kind, const char *fmt, va_list ap) {
    char msg[512];
    vsnprintf(msg, sizeof(msg), fmt, ap);
    if (file) dprintf(STDERR, "%s:%d: %s: %s\n", file, line, kind, msg);
    else dprintf(STDERR, "cc: %s: %s\n", kind, msg);
    if (loc && line_start) {
        char *end = line_start;
        while (*end && *end != '\n') end++;
        dprintf(STDERR, "    %.*s\n    ", (int)(end - line_start), line_start);
        for (char *p = line_start; p < loc; p++) dprintf(STDERR, "%c", *p == '\t' ? '\t' : ' ');
        dprintf(STDERR, "^\n");
    }
}

void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport(NULL, 0, NULL, NULL, "error", fmt, ap);
    va_end(ap);
    exit(1);
}

static char *line_start_of(Token *tok) {
    if (!tok->file || !tok->file->contents || !tok->loc) return NULL;
    char *base = tok->file->contents;
    if (tok->loc < base || tok->loc > base + strlen(base)) return NULL;    /* токен из макроса */
    char *p = tok->loc;
    while (p > base && p[-1] != '\n') p--;
    return p;
}

void error_tok(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport(tok->file ? tok->file->name : "?", tok->line, tok->loc, line_start_of(tok), "error", fmt, ap);
    va_end(ap);
    exit(1);
}

void warn_tok(Token *tok, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vreport(tok->file ? tok->file->name : "?", tok->line, tok->loc, line_start_of(tok), "warning", fmt, ap);
    va_end(ap);
}

/* ------------------------------------------------------------ мелочи */

void *xcalloc(int n) {
    void *p = calloc(1, n > 0 ? n : 1);
    if (!p) error("out of memory");
    return p;
}

char *strndup2(const char *s, int n) {
    char *p = xcalloc(n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

char *format(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strndup2(buf, strlen(buf));
}

char *read_file(const char *path, int *out_len) {
#ifdef CC_HOST
    int fd = open(path, O_RDONLY);
#else
    struct dirent_info st;
    if (stat(path, &st) < 0 || st.type != 1) return NULL;
    int fd = open(path, O_READ);
#endif
    if (fd < 0) return NULL;
    int cap = 4096, len = 0;
    char *buf = malloc(cap);
    for (;;) {
        if (len + 2048 > cap) {
            cap *= 2;
            char *nb = malloc(cap);
            if (!nb) error("out of memory");
            memcpy(nb, buf, len);
            free(buf);
            buf = nb;
        }
        int n = read(fd, buf + len, cap - len - 1);
        if (n < 0) { close(fd); free(buf); return NULL; }
        if (n == 0) break;
        len += n;
    }
    close(fd);
    if (len == 0 || buf[len - 1] != '\n') buf[len++] = '\n';  /* файл всегда кончается переводом строки */
    buf[len] = 0;
    *out_len = len;
    return buf;
}

static void add_include(char *path) {
    char **p = xcalloc(sizeof(char *) * (n_include_paths + 1));
    for (int i = 0; i < n_include_paths; i++) p[i] = include_paths[i];
    p[n_include_paths++] = path;
    include_paths = p;
}

static void usage(void) {
    dprintf(STDERR,
        "usage: cc [-o out] [-E] [-I dir] [-nostdlib] file.c...\n"
        "  -o out      name of the program (default: first file without .c)\n"
        "  -E          only preprocess, print the result\n"
        "  -I dir      also look for #include files in dir\n"
        "  -nostdlib   do not add /lib/libc.c, /lib/gfx.c, /lib/win.c\n");
    exit(2);
}

static void print_tokens(Token *tok) {
    int line = 1;
    for (; tok->kind != TK_EOF; tok = tok->next) {
        if (line > 1 && tok->at_bol) dprintf(STDOUT, "\n");
        if (tok->has_space && !tok->at_bol) dprintf(STDOUT, " ");
        dprintf(STDOUT, "%.*s", tok->len, tok->loc);
        line++;
    }
    dprintf(STDOUT, "\n");
}

/* последний компонент пути без .c */
static char *default_output(const char *src) {
    const char *b = strrchr(src, '/');
    b = b ? b + 1 : src;
    int n = strlen(b);
    if (n > 2 && b[n - 2] == '.' && b[n - 1] == 'c') n -= 2;
    if (n == 0) return "a.out";
    return strndup2(b, n);
}

int main(int argc, char **argv) {
    char *out = NULL;
    bool opt_E = false, nostdlib = false;
    char **inputs = xcalloc(sizeof(char *) * (argc + 1));
    int n_inputs = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-o") && i + 1 < argc) { out = argv[++i]; continue; }
        if (!strcmp(argv[i], "-E")) { opt_E = true; continue; }
        if (!strcmp(argv[i], "-linux")) { opt_linux = true; continue; }
        if (!strcmp(argv[i], "-nostdlib")) { nostdlib = true; continue; }
        if (!strcmp(argv[i], "-B") && i + 1 < argc) { base_dir = argv[++i]; continue; }
        if (!strcmp(argv[i], "-I") && i + 1 < argc) { add_include(argv[++i]); continue; }
        if (!strncmp(argv[i], "-I", 2) && argv[i][2]) { add_include(argv[i] + 2); continue; }
        if (argv[i][0] == '-') usage();
        inputs[n_inputs++] = argv[i];
    }
    if (!n_inputs) usage();
    add_include(format("%s/include", base_dir));

    if (!nostdlib && !opt_E) {
        inputs[n_inputs++] = format("%s/lib/libc.c", base_dir);
        static const char *extra[] = { "gfx.c", "win.c" };      /* есть на диске — добавляем */
        for (int k = 0; k < 2; k++) {
            char *g = format("%s/lib/%s", base_dir, extra[k]);
            int len;
            char *s = read_file(g, &len);
            if (s) { free(s); inputs[n_inputs++] = g; }
        }
    }
    if (!out) out = default_output(inputs[0]);

    Obj *prog = NULL, **tail = &prog;
    for (int i = 0; i < n_inputs; i++) {
        Token *tok = tokenize_file(inputs[i]);
        if (!tok) error("%s: no such file", inputs[i]);
        tok = preprocess(tok);
        if (opt_E) { print_tokens(tok); continue; }
        Obj *objs = parse(tok);
        *tail = objs;
        while (*tail) tail = &(*tail)->next;
    }
    if (opt_E) return 0;
    codegen(prog, out);
    return 0;
}
