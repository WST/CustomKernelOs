/* tests/dom_host.c — DOM и скрипты браузера на Linux, без окна (make jstest).
 *   build/dom-host страница.html [click:id] [key:id:текст] [wait:мс] ...
 * Печатает HTML, который получит раскладка браузера, и вывод console.log. */
#include <sys/time.h>
#include <unistd.h>
#include "domint.h"

double js_host_now(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }

static char base_dir[512];
static int net_mode;                 /* --net: качать по-настоящему (через curl) */

static int curl_fetch(const char *url, char **data, int *len) {
    char cmd[2048];
    snprintf(cmd, sizeof(cmd), "curl -sL -m 30 -A 'Mozilla/5.0 (MyOS; i386) MyOS-Browser/0.14' '%s'", url);
    FILE *p = popen(cmd, "r");
    if (!p) return -1;
    *data = malloc(8 << 20);
    *len = fread(*data, 1, 8 << 20, p);
    pclose(p);
    return *len > 0 ? 0 : -1;
}

static int h_fetch(const char *url, const char *method, const char *body, int blen, char **data, int *len, int *status, char *ctype, int cmax) {
    (void)body; (void)blen;
    if (net_mode) {
        int r = curl_fetch(url, data, len);
        printf("[fetch %s %s -> %s %d]\n", method, url, r ? "fail" : "ok", *len);
        *status = r ? 0 : 200;
        snprintf(ctype, cmax, "application/javascript"); /* --net is a script compatibility harness */
        return r;
    }
    const char *p = strrchr(url, '/');
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", base_dir, p ? p + 1 : url);
    char *q = strchr(path, '?'); if (q) *q = 0;
    FILE *f = fopen(path, "rb");
    printf("[fetch %s %s -> %s]\n", method, url, f ? "ok" : "404");
    if (!f) { *status = 404; *data = NULL; *len = 0; return 0; }
    *data = malloc(4 << 20);
    *len = fread(*data, 1, 4 << 20, f);
    fclose(f);
    *status = 200;
    snprintf(ctype, cmax, "%s", strstr(path, ".json") ? "application/json" : strstr(path, ".js") ? "application/javascript" : "text/plain");
    return 0;
}
static void h_status(const char *m) { (void)m; }
static void h_console(const char *s, int n) { printf("[console] %.*s", n, s); if (n && s[n - 1] != '\n') printf("\n"); }

int main(int argc, char **argv) {
    if (argc < 2) return 1;
    static char buf[8 << 20];
    int n;
    const char *page_url = "http://test.local/dir/page.html";
    if (!strncmp(argv[1], "http", 4)) {                       /* настоящий адрес */
        net_mode = 1;
        char *d;
        if (curl_fetch(argv[1], &d, &n) < 0) { printf("cannot fetch\n"); return 1; }
        memcpy(buf, d, n);
        free(d);
        page_url = argv[1];
    } else {
        FILE *f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
    }
    snprintf(base_dir, sizeof(base_dir), "%s", argv[1]);
    char *sl = strrchr(base_dir, '/'); if (sl) *sl = 0; else strcpy(base_dir, ".");
    struct dom_host h = { h_fetch, h_status, h_console };
    js_init();
    dom_init(&h);
    dom_load(buf, n, page_url);
    for (int i = 2; i < argc; i++) {
        if (!strncmp(argv[i], "click:", 6)) {
            dnode *e = dn_by_id(d_document, argv[i] + 6);
            printf("[click #%s] prevented=%d\n", argv[i] + 6, e ? dom_click(e->id) : -1);
        } else if (!strncmp(argv[i], "key:", 4)) {
            char id[64]; const char *t = strchr(argv[i] + 4, ':');
            snprintf(id, sizeof(id), "%.*s", (int)(t - argv[i] - 4), argv[i] + 4);
            dnode *e = dn_by_id(d_document, id);
            char val[256] = "";
            for (int k = 0; t[1 + k]; k++) { val[k] = t[1 + k]; val[k + 1] = 0; if (e) dom_key(e->id, val[k], val); }
        } else if (!strncmp(argv[i], "wait:", 5)) {
            double until = js_now_ms() + atoi(argv[i] + 5);
            while (js_now_ms() < until) { int t = dom_next_timer(); if (t < 0) break; usleep((t > 0 ? t : 1) * 1000); dom_run_timers(); js_gc(); }
        }
        else if (!strcmp(argv[i], "styles")) {                 /* вычисленные стили видимых элементов */
            int len0;
            free(dom_serialize(&len0));
            static const char *dn[] = { "inline", "block", "list-item", "inline-block", "flex", "grid", "table", "row", "cell", "contents", "none" };
            for (int k = 0; k < d_count; k++) {
                dnode *e = d_nodes[k];
                if (e->type != DN_ELEMENT || !dn_connected(e) || css_hidden(e)) continue;
                const struct dom_style *y = dom_style(k);
                const char *idv = dn_get_attr(e, "id");
                if (!idv) continue;                               /* только элементы с id */
                printf("[style #%s] %s color=%06x bg=%s%06x%s fs=%d%s%s%s%s align=%d m=%d,%d,%d,%d p=%d,%d,%d,%d b=%d/%06x w=%d%s mw=%d%s list=%d%s%s\n",
                       idv, dn[y->display], y->color, y->bg < 0 ? "(" : "", y->bg < 0 ? 0 : y->bg, y->bg < 0 ? ")" : "",
                       y->font_px, y->bold ? " bold" : "", y->italic ? " italic" : "", y->underline ? " underline" : "",
                       y->strike ? " strike" : "", y->align,
                       y->margin[0], y->margin[1], y->margin[2], y->margin[3], y->padding[0], y->padding[1], y->padding[2], y->padding[3],
                       y->border[0], y->border_color[0] < 0 ? 0 : y->border_color[0], y->width, y->width_pct ? "%" : "", y->max_width, y->max_width_pct ? "%" : "",
                       y->list, y->transform ? " upper" : "", y->notext ? " notext" : "");
            }
            continue;
        }
        char nav[1024];
        if (dom_pending_nav(nav, sizeof(nav))) printf("[navigate %s]\n", nav);
    }
    int len;
    char *html = dom_serialize(&len);
    char st[64];
    dom_stats(st, sizeof(st));
    printf("--- %s; last error: %s\n%s\n", st, dom_last_error(), html);
    free(html);
    return 0;
}
