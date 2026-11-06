/* js [файл.js ...] — JavaScript в терминале MyOS.
 *
 *   js             интерактивно: строка — выражение, ответ печатается
 *   js a.js b.js   выполнить файлы по очереди
 *   js -e 'код'    выполнить строку
 *
 * Тот же движок, что в браузере (user/lib/js*.c), только без DOM. */
#include "myos.h"
#include "js.h"

void js_inspect(jsbuf *b, jsval v, int top);

static int failed;

struct run { const char *src; int len; const char *name; int show; };

static void run_body(void *arg) {
    struct run *r = arg;
    jsval v;
    if (js_eval(r->src, r->len, r->name, &v) < 0) {
        char e[300];
        printf(ANSI_RED "Uncaught %s" ANSI_RESET "\n", js_error_text(js_exc, e, sizeof(e)));
        js_exc = js_undef();
        failed = 1;
    } else if (r->show) {
        jsbuf b = { 0 };
        js_inspect(&b, v, 0);
        printf("%.*s\n", b.n, b.p);
        jb_free(&b);
    }
    js_run_jobs();
}

static void run(const char *src, int len, const char *name, int show) {
    struct run r = { src, len, name, show };
    js_big_call(run_body, &r);
    js_gc();
}

static int run_file(const char *path) {
    int fd = open(path, O_READ);
    if (fd < 0) { printf("js: cannot open %s\n", path); return 1; }
    int cap = 65536, n = 0, k;
    char *buf = malloc(cap);
    while (buf && (k = read(fd, buf + n, cap - n)) > 0) {
        n += k;
        if (n == cap) {
            char *p = malloc(cap * 2);
            if (!p) break;
            memcpy(p, buf, n);
            free(buf);
            buf = p;
            cap *= 2;
        }
    }
    close(fd);
    if (!buf) { printf("js: out of memory\n"); return 1; }
    run(buf, n, path, 0);
    free(buf);
    return 0;
}

int main(int argc, char **argv) {
    js_init();
    if (argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "-e") && i + 1 < argc) { run(argv[i + 1], strlen(argv[i + 1]), "-e", 1); i++; continue; }
            run_file(argv[i]);
        }
        return failed;
    }
    printf("MyOS JavaScript. Type an expression, empty line or Ctrl+D to quit.\n");
    char line[1024];
    for (;;) {
        printf(ANSI_CYAN "> " ANSI_RESET);
        if (getln(line, sizeof(line)) < 0 || !line[0]) break;
        run(line, strlen(line), "repl", 1);
    }
    return 0;
}
