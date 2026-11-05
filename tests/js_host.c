/* tests/js_host.c — движок JS на Linux: для тестов (make jstest) и отладки.
 *   build/js-host файл.js ...   — выполнить файлы
 *   build/js-host               — интерактивно (строка — выражение) */
#include <sys/time.h>
#include "js.h"

double js_host_now(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0;
}

int main(int argc, char **argv) {
    js_init();
    int fails = 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        static char buf[4 << 20];
        int n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        jsval r;
        if (js_eval(buf, n, argv[i], &r) < 0) {
            char e[300];
            printf("Uncaught %s\n", js_error_text(js_exc, e, sizeof(e)));
            fails++;
        }
        js_run_jobs();
        js_gc();
    }
    if (argc == 1) {
        char line[4096];
        while (printf("> "), fflush(stdout), fgets(line, sizeof(line), stdin)) {
            jsval r;
            if (js_eval(line, strlen(line), "repl", &r) < 0) {
                char e[300];
                printf("Uncaught %s\n", js_error_text(js_exc, e, sizeof(e)));
            } else {
                extern void js_inspect(jsbuf *b, jsval v, int top);
                jsbuf b = { 0 };
                js_inspect(&b, r, 0);
                printf("%.*s\n", b.n, b.p);
                jb_free(&b);
            }
            js_run_jobs();
            js_gc();
        }
    }
    return fails ? 1 : 0;
}
