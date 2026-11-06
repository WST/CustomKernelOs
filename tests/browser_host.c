/* tests/browser_host.c — весь браузер (user/browser.c: загрузка, DOM, скрипты, CSS,
 * раскладка, рисование) на Linux без QEMU, с AddressSanitizer (make browsertest).
 *
 *   build/browser-host страница.html [команды...]
 *     w N          ширина окна N (заново разложить и нарисовать)
 *     scroll       прокрутить всю страницу экранами, рисуя каждый
 *     click        щёлкнуть по каждой N-й ссылке/кнопке (по очереди)
 *     keys         набрать текст в первое поле ввода
 *     timers MS    крутить таймеры MS миллисекунд
 *     shot F.ppm   снимок окна
 *
 * Рабочий стол и сеть заменены заглушками: окно — просто буфер в памяти, http(s)
 * отвечает «нет такого хоста», file:/путь читает файлы Linux. */
#define BROWSER_HOST
#define main browser_main
#include "../user/browser.c"
#undef main

extern void *fopen(const char *, const char *);
extern unsigned long fread(void *, unsigned long, unsigned long, void *);
extern unsigned long fwrite(const void *, unsigned long, unsigned long, void *);
extern int fclose(void *);
extern int fseek(void *, long, int);
extern long ftell(void *);
extern int clock_gettime(int, void *);
extern int usleep(unsigned);
extern void *popen(const char *, const char *);
extern int pclose(void *);
static int host_net;
int browser_host_fetch(const char *url, char *location, int loc_max, bool *is_html) {
    if (!host_net || (strncmp(url,"https://",8) && strncmp(url,"http://",7))) return -999;
    (void)loc_max;
    char command[8192]; int used=snprintf(command,sizeof(command),"curl -sL --max-time 15 --compressed -w '\\n%%{http_code}\\n%%{content_type}' '");
    for(const char *p=url;*p && used<(int)sizeof(command)-12;p++) {
        if(*p=='\'') { memcpy(command+used,"'\\''",4);used+=4; } else command[used++]=*p;
    }
    command[used++]='\'';command[used]=0;
    void *pipe=popen(command,"r");if(!pipe)return -1;
    int total=fread(body,1,MAX_PAGE,pipe);
    if(total==MAX_PAGE){char discard[8192];while(fread(discard,1,sizeof(discard),pipe));}
    int result=pclose(pipe);
    if(result || total>=MAX_PAGE)return -1;
    body[total]=0;location[0]=0;
    char *type=body+total;while(type>body && type[-1]!='\n')type--;
    snprintf(resp_ctype,sizeof(resp_ctype),"%s",type);
    char *code=type-1;while(code>body && code[-1]!='\n')code--;
    int status_code=atoi(code);
    body_len=code>body?code-body-1:0;body[body_len]=0;
    *is_html=strstr(resp_ctype,"html")!=NULL;
    return status_code;
}

/* -------- «система» */
unsigned ticks(void) { long ts[2]; clock_gettime(1, ts); return ts[0] * 1000 + ts[1] / 1000000; }
void sleep_ms(unsigned ms) { usleep(ms * 1000); }
int get_time(struct datetime *t) { memset(t, 0, sizeof(*t)); t->year = 2026; t->month = 11; t->day = 6; t->hour = 12; return 0; }
int __syscall(int n, int a, int b, int c) { (void)n; (void)a; (void)b; (void)c; return -1; }
int getpid(void) { return 7; }
int ttymode(int m) { return m; }
int utf8_put(char *o, int cp) {
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | (cp >> 6); o[1] = 0x80 | (cp & 63); return 2; }
    o[0] = 0xE0 | (cp >> 12); o[1] = 0x80 | ((cp >> 6) & 63); o[2] = 0x80 | (cp & 63); return 3;
}

/* сеть: ничего нет */
int tcp_connect_timeout(const char *host, int port, unsigned ms) { (void)host; (void)port; (void)ms; return -ENOENT; }
int sockctl(int fd, int op, unsigned arg) { (void)fd; (void)op; (void)arg; return 0; }
const char *strerror(int e) { (void)e; return "error"; }

/* файлы MyOS -> файлы Linux (только для file:/) */
int stat(const char *path, struct dirent_info *out) {
    void *f = fopen(path, "rb");
    if (!f) return -ENOENT;
    fclose(f);
    memset(out, 0, sizeof(*out));
    out->type = 1;
    return 0;
}
int readdir(const char *path, int index, struct dirent_info *out) { (void)path; (void)index; (void)out; return 0; }

/* -------- рабочий стол: одно окно-буфер, сообщения — очередь */
static struct msg q[64];
static int nq;
static void *shm[16];
static int nshm;
static void push(int type, int a, int b, int c, int d) {
    if (nq == 64) return;
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.from = 1; m.type = type; m.a = a; m.b = b; m.c = c; m.d = d;
    q[nq++] = m;
}
int service_find(const char *n) { (void)n; return 1; }
int shm_alloc(unsigned size) { if (nshm == 16) return -1; shm[nshm] = calloc(1, size); return nshm++; }
void *shm_map(int id) { return id >= 0 && id < nshm ? shm[id] : NULL; }
int shm_unmap(void *p) { (void)p; return 0; }
int msg_send(int pid, struct msg *m) {
    (void)pid;
    if (m->type == WM_CREATE) push(WM_CREATED, 1, 0, 0, 0);
    return 0;
}
int msg_recv(struct msg *m, int timeout_ms) {
    (void)timeout_ms;
    if (!nq) return -1;
    *m = q[0];
    memmove(q, q + 1, sizeof(q[0]) * --nq);
    return 0;
}

static void shot(const char *file) {
    char hdr[64];
    int n = snprintf(hdr, sizeof(hdr), "P6\n%d %d\n255\n", W, H);
    unsigned char *buf = malloc(W * H * 3);
    for (int i = 0; i < W * H; i++) {
        color_t c = win->pixels[i];
        buf[i * 3] = c >> 16; buf[i * 3 + 1] = c >> 8; buf[i * 3 + 2] = c;
    }
    void *f = fopen(file, "wb");
    fwrite(hdr, 1, n, f);
    fwrite(buf, 1, W * H * 3, f);
    fclose(f);
    free(buf);
}

static void resize(int w, int h) {
    push(WM_RESIZE, win->id, w, h, 0);
    struct event e;
    if (win_event(&e, 0) && e.type == EV_RESIZE) {
        W = e.x; H = e.y;
        if (page_is_dom) relayout(true);
        else { arena_used = 0; layout_html(body, body_len, true); }
        redraw();
    }
}

static void pump(void) {                     /* обработать накопившиеся события, как главный цикл */
    struct event e;
    while (nq && win_event(&e, 0)) {
        if (e.type == EV_TIMEOUT) break;
        if (e.type == EV_MOUSE_DOWN) on_click(e.x, e.y);
        else if (e.type == EV_KEY) on_key(e.key);
        else if (e.type == EV_MOUSE_MOVE) on_move(e.x, e.y);
    }
}

#include "browser_compat.h"

int main(int argc, char **argv) {
    if(argc>2 && !strcmp(argv[1],"--net")){host_net=1;argc--;argv++;}
    if (argc < 2) { printf("usage: browser-host page.html commands...\n"); return 1; }
    font_set_dir("fonts");
    char target[600];
    if (argv[1][0] == '/') snprintf(target, sizeof(target), "file:%s", argv[1]);
    else if (!strncmp(argv[1], "http", 4) || !strncmp(argv[1], "file:", 5) || !strncmp(argv[1], "about:", 6)) snprintf(target, sizeof(target), "%s", argv[1]);
    else {
        char cwd[400];
        getcwd(cwd, sizeof(cwd));
        snprintf(target, sizeof(target), "file:%s/%s", cwd, argv[1]);
    }
    /* как main() браузера, но без главного цикла */
    win = win_create("Browser", W, H);
    win_set_resizable(win, 480, 300);
    items = malloc(sizeof(struct item) * MAX_ITEMS);
    boxes = malloc(sizeof(struct box) * MAX_BOXES);
    init_styles();
    body = malloc(MAX_PAGE + 1);
    arena_cap = 1024 * 1024;
    arena = malloc(arena_cap);
    dom_init(&dom_host);
    js_poll_hook = js_busy_poll;
    if (!strcmp(argv[1], "--compat-test")) return compat_tests();
    unsigned t0 = ticks();
    navigate(target, true);
    printf("load: %u ms, items %d, links %d, doc_h %d; %s\n", ticks() - t0, n_items, n_links, doc_h, status);
    for (int i = 2; i < argc; i++) {
        const char *c = argv[i];
        if (!strcmp(c,"console")) { printf("%.*s\n",console_len,console_buf);continue; }
        if (!strcmp(c, "assert-no-errors")) {
            if (dom_last_error()[0]) { printf("FAIL: %s\n", dom_last_error()); return 1; }
            continue;
        }
        if (!strcmp(c, "w") && i + 1 < argc) resize(atoi(argv[++i]), H);
        else if (!strcmp(c, "h") && i + 1 < argc) resize(W, atoi(argv[++i]));
        else if (!strcmp(c, "at") && i + 1 < argc) { scroll = 0; scroll_by(atoi(argv[++i])); redraw(); }
        else if (!strcmp(c, "scroll")) {
            int pages = 0;
            for (scroll = 0; pages < 400; pages++) {
                redraw();
                if (scroll >= doc_h - VIEW_H) break;
                scroll_by(VIEW_H - 32);
            }
            printf("scrolled %d screens\n", pages);
        } else if (!strcmp(c, "click")) {
            int clicks = 0;
            for (int k = 0; k < n_items && clicks < 40; k += 1 + n_items / 40) {
                struct item *it = &items[k];
                if (it->link < 0 && it->kind != IT_BUTTON && it->kind != IT_INPUT) continue;
                if (it->link >= 0 && strncmp(links[it->link], "#", 1) && !strstr(links[it->link], "javascript")) continue;
                scroll = it->y > VIEW_H / 2 ? it->y - VIEW_H / 2 : 0;
                int y = VIEW_Y + it->y - scroll + 2;
                if (y < VIEW_Y || y >= VIEW_Y + VIEW_H) continue;
                push(WM_MOUSE, win->id, it->x + 2, y, MOUSE_LEFT | (1 << 8));
                pump();
                clicks++;
            }
            printf("clicked %d\n", clicks);
        } else if (!strcmp(c, "keys")) {
            for (int k = 0; k < n_items; k++) if (items[k].kind == IT_INPUT) {
                struct item *it = &items[k];
                focus_input = it->input;
                const char *t = "hello <b>&amp;\x08\x08 мир";
                for (int j = 0; t[j]; j++) push(WM_KEY, win->id, (unsigned char)t[j], 0, 0);
                pump();
                break;
            }
        } else if (!strcmp(c, "timers") && i + 1 < argc) {
            unsigned until = ticks() + atoi(argv[++i]);
            while ((int)(until - ticks()) > 0) {
                int t = dom_next_timer();
                if (t < 0) break;
                if (t > 0) sleep_ms(t > 20 ? 20 : t);
                dom_run_timers();
                after_js_ex(true);
            }
            after_js();
        } else if (!strcmp(c, "reload") && i + 1 < argc) {      /* утечки: та же страница N раз */
            struct mi { int arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost; };
            extern struct mi mallinfo(void);
            int nrel = atoi(argv[++i]);
            for (int k = 0; k < nrel; k++) {
                navigate(url, false);
                if (k == 0 || k == nrel - 1 || k % 5 == 4) {
                    struct mi m = mallinfo();
                    printf("reload %d: heap in use %d KB (mmap %d KB)\n", k + 1, m.uordblks / 1024, m.hblkhd / 1024);
                }
            }
        } else if (!strcmp(c, "images")) {                     /* скачать все картинки, как главный цикл */
            int n = 0;
            while (img_pump()) n++;
            printf("images: %d loaded, doc_h %d\n", n, doc_h);
        } else if (!strcmp(c, "shot") && i + 1 < argc) shot(argv[++i]);
        else { printf("bad command %s\n", c); return 1; }
    }
    printf("done: items %d, doc_h %d; %s\n", n_items, doc_h, status);
    return 0;
}
