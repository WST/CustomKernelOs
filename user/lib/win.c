/* Окна для программ: разговор с рабочим столом (desktop) сообщениями + общая память. */
#include "win.h"

#define MAX_WINDOWS 8
static int desktop;                             /* pid рабочего стола */
static window *wins[MAX_WINDOWS];
static struct msg pending[16];                  /* пришли, пока ждали ответа на WM_CREATE */
static int n_pending;
static bool font_loaded;

int win_desktop_pid(void) {
    if (!desktop) desktop = service_find("desktop");
    return desktop;
}

static void copy_title(char *dst, const char *src) {
    int i = 0;
    for (; src[i] && i < 39; i++) dst[i] = src[i];
    dst[i] = 0;
}

window *win_create(const char *title, int w, int h) {
    if (!win_desktop_pid() || w <= 0 || h <= 0 || w > 2560 || h > 1600) return NULL;
    if (!font_loaded) { gfx_load_font(); font_loaded = true; }
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) if (!wins[i]) { slot = i; break; }
    if (slot < 0) return NULL;

    int id = shm_alloc(w * h * 4);
    if (id < 0) return NULL;
    color_t *px = shm_map(id);
    if (!px) return NULL;

    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_CREATE;
    m.a = id; m.b = w; m.c = h;
    copy_title(m.data, title);
    if (msg_send(desktop, &m) < 0) { shm_unmap(px); return NULL; }

    for (;;) {                                  /* ждём номер окна */
        struct msg r;
        if (msg_recv(&r, 2000) < 0) { shm_unmap(px); return NULL; }
        if (r.from == desktop && r.type == WM_CREATED) {
            if (r.a < 0) { shm_unmap(px); return NULL; }
            window *win = calloc(1, sizeof(window));
            win->id = r.a;
            win->w = w; win->h = h;
            win->pixels = px;
            win->shm = id;
            wins[slot] = win;
            win_select(win);
            return win;
        }
        if (n_pending < 16) pending[n_pending++] = r;
    }
}

void win_destroy(window *w) {
    if (!w) return;
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_DESTROY;
    m.a = w->id;
    msg_send(desktop, &m);
    for (int i = 0; i < MAX_WINDOWS; i++) if (wins[i] == w) wins[i] = NULL;
    shm_unmap(w->pixels);
    free(w);
}

void win_select(window *w) {
    gfx_buf = w->pixels;
    gfx_w = w->w;
    gfx_h = w->h;
}

void win_update_rect(window *w, int x, int y, int rw, int rh) {
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_UPDATE;
    m.a = w->id;
    m.b = x; m.c = y;
    m.d = (rw & 0xFFFF) | (rh << 16);
    msg_send(desktop, &m);
}

void win_update(window *w) { win_update_rect(w, 0, 0, w->w, w->h); }

void win_set_title(window *w, const char *title) {
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_TITLE;
    m.a = w->id;
    copy_title(m.data, title);
    msg_send(desktop, &m);
}

static window *find_win(int id) {
    for (int i = 0; i < MAX_WINDOWS; i++) if (wins[i] && wins[i]->id == id) return wins[i];
    return NULL;
}

/* -> 1 — событие в e, 0 — рабочего стола больше нет (пора выходить) */
int win_event(struct event *e, int timeout_ms) {
    memset(e, 0, sizeof(*e));
    unsigned start = ticks();
    for (;;) {
        struct msg m;
        if (n_pending) {
            m = pending[0];
            for (int i = 1; i < n_pending; i++) pending[i - 1] = pending[i];
            n_pending--;
        } else {
            int wait = timeout_ms;
            if (wait < 0 || wait > 1000) wait = 1000;   /* раз в секунду проверяем, жив ли стол */
            int r = msg_recv(&m, wait);
            if (r < 0) {
                if (r == -EINTR) { e->type = EV_QUIT; return 0; }
                desktop = service_find("desktop");
                if (!desktop) { e->type = EV_QUIT; return 0; }
                if (timeout_ms >= 0 && (int)(ticks() - start) >= timeout_ms) { e->type = EV_TIMEOUT; return 1; }
                continue;
            }
        }
        if (m.from != desktop) { e->type = EV_MESSAGE; e->msg = m; return 1; }
        e->win = find_win(m.a);
        switch (m.type) {
        case WM_KEY: e->type = EV_KEY; e->key = m.b; return 1;
        case WM_MOUSE:
            e->x = m.b; e->y = m.c;
            e->buttons = m.d & 0xFF;
            e->type = (m.d >> 8) == 1 ? EV_MOUSE_DOWN : (m.d >> 8) == 2 ? EV_MOUSE_UP : EV_MOUSE_MOVE;
            return 1;
        case WM_CLOSE: e->type = EV_CLOSE; return 1;
        case WM_FOCUS: e->type = EV_FOCUS; e->key = m.b; return 1;
        case WM_QUIT: e->type = EV_QUIT; return 0;
        default: continue;
        }
    }
}

/* ------------------------------------------------------------ элементы интерфейса */

bool ui_inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

void ui_text_center(int x, int y, int w, int h, const char *s, color_t c) {
    int tw = strlen(s) * 8;
    gfx_text(x + (w - tw) / 2, y + (h - 16) / 2, s, c, TRANSPARENT);
}

void ui_button(int x, int y, int w, int h, const char *label, bool pressed) {
    gfx_fill_rect(x, y, w, h, pressed ? RGB(204, 228, 247) : RGB(225, 225, 225));
    gfx_rect(x, y, w, h, pressed ? UI_ACCENT : RGB(173, 173, 173));
    ui_text_center(x + (pressed ? 1 : 0), y + (pressed ? 1 : 0), w, h, label, UI_TEXT);
}

void ui_sunken(int x, int y, int w, int h, color_t bg) {
    gfx_fill_rect(x, y, w, h, bg);
    gfx_rect(x, y, w, h, RGB(122, 122, 122));
}
