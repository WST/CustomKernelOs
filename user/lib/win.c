/* Окна для программ: разговор с рабочим столом (desktop) сообщениями + общая память. */
#include "win.h"

#define MAX_WINDOWS 8
static int desktop;                             /* pid рабочего стола */
static window *wins[MAX_WINDOWS];
static struct msg pending[16];                  /* пришли, пока ждали ответа на WM_CREATE */
static int n_pending;
static bool font_loaded;

/* Pixel copies precede publication/acknowledgement on all guest CPUs. */
static void frame_barrier(void) {
#ifndef __MYOS_CC__
    __asm__ volatile ("mfence" : : : "memory");
#endif
}

static bool wait_consumed(window *w) {
    unsigned start = ticks();
    while (w->frame->submitted != w->frame->consumed) {
        frame_barrier();
        if (ticks() - start >= 2000 || service_find("desktop") != desktop) return false;
        sleep_ms(1);
    }
    frame_barrier();
    return true;
}

static void release_buffer(window *w) {
    if (w->frame) { shm_unmap(w->frame); free(w->pixels); }
    else shm_unmap(w->pixels);
}

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

    int id = shm_alloc(WM_FRAME_BYTES + w * h * 4);
    if (id < 0) return NULL;
    color_t *px = shm_map(id);
    if (!px) return NULL;

    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_CREATE;
    m.a = id; m.b = w; m.c = h; m.d = WM_BUFFERED;
    copy_title(m.data, title);
    if (msg_send(desktop, &m) < 0) { shm_unmap(px); return NULL; }

    for (;;) {                                  /* ждём номер окна */
        struct msg r;
        if (msg_recv(&r, 2000) < 0) { shm_unmap(px); return NULL; }
        if (r.from == desktop && r.type == WM_CREATED) {
            if (r.a < 0) { shm_unmap(px); return NULL; }
            window *win = calloc(1, sizeof(window));
            color_t *draw = (unsigned)r.b == WM_BUFFERED ? calloc(w * h, 4) : px;
            if (!win || !draw) {
                struct msg close = {0}; close.type = WM_DESTROY; close.a = r.a;
                msg_send(desktop, &close);
                if ((unsigned)r.b == WM_BUFFERED) free(draw);
                free(win); shm_unmap(px); return NULL;
            }
            win->id = r.a;
            win->w = w; win->h = h;
            win->pixels = draw;
            win->shm = id;
            if ((unsigned)r.b == WM_BUFFERED) {
                win->frame = (struct win_frame *)px;
                win->shared = (color_t *)((char *)px + WM_FRAME_BYTES);
            }
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
    release_buffer(w);
    free(w);
}

void win_select(window *w) {
    gfx_buf = w->pixels;
    gfx_w = w->w;
    gfx_h = w->h;
}

void win_update_rect(window *w, int x, int y, int rw, int rh) {
    if (!w || rw <= 0 || rh <= 0) return;
    if (w->full_update) { x = y = 0; rw = w->w; rh = w->h; }
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x >= w->w || y >= w->h || rw <= 0 || rh <= 0) return;
    if (rw > w->w - x) rw = w->w - x;
    if (rh > w->h - y) rh = w->h - y;
    if (w->frame) {
        if (!wait_consumed(w)) { w->full_update = true; return; }
        for (int row = y; row < y + rh; row++)
            memcpy(w->shared + row * w->w + x, w->pixels + row * w->w + x, rw * 4);
        frame_barrier();
        w->frame->submitted++;
        frame_barrier();
    }
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_UPDATE;
    m.a = w->id;
    m.b = x; m.c = y;
    m.d = (rw & 0xFFFF) | (rh << 16);
    int result;
    unsigned start = ticks();
    while ((result = msg_send(desktop, &m)) == -EAGAIN && ticks() - start < 2000)
        sleep_ms(1);
    /* No message references this publication after a failed send. The next
     * update publishes the entire buffer so its earlier damage is not lost. */
    if (result < 0 && w->frame) {
        w->frame->consumed = w->frame->submitted;
        w->full_update = true;
    } else if (result == 0) w->full_update = false;
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

void win_set_resizable(window *w, int min_w, int min_h) {
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_RESIZABLE;
    m.a = w->id; m.b = min_w; m.c = min_h;
    msg_send(desktop, &m);
}

/* Новый буфер под новый размер: старое содержимое — в левый верхний угол, остальное
 * цветом правого нижнего пикселя (обычно это фон окна), чтобы до перерисовки не мигало. */
static bool resize_buffer(window *w, int nw, int nh) {
    if (nw <= 0 || nh <= 0 || nw > 2560 || nh > 1600) return false;
    if (nw == w->w && nh == w->h) return false;
    int id = shm_alloc((w->frame ? WM_FRAME_BYTES : 0) + nw * nh * 4);
    if (id < 0) return false;
    void *mapping = shm_map(id);
    if (!mapping) return false;
    color_t *px = w->frame ? malloc(nw * nh * 4) : mapping;
    if (!px) { shm_unmap(mapping); return false; }
    color_t fill = w->pixels[w->w * w->h - 1];
    int cw = nw < w->w ? nw : w->w, ch = nh < w->h ? nh : w->h;
    for (int y = 0; y < nh; y++) {
        color_t *d = px + y * nw;
        int x = 0;
        if (y < ch) { memcpy(d, w->pixels + y * w->w, cw * 4); x = cw; }
        for (; x < nw; x++) d[x] = fill;
    }
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = WM_SETBUF;
    m.a = w->id; m.b = id; m.c = nw; m.d = nh;
    if (w->frame) {
        unsigned marker = WM_BUFFERED; memcpy(m.data, &marker, sizeof(marker));
        memcpy((char *)mapping + WM_FRAME_BYTES, px, nw * nh * 4);
        ((struct win_frame *)mapping)->submitted = 1;
        frame_barrier();
    }
    if (msg_send(desktop, &m) < 0) {
        if (w->frame) free(px);
        shm_unmap(mapping); return false;
    }
    bool selected = gfx_buf == w->pixels;
    bool buffered = w->frame != NULL;
    release_buffer(w);
    w->pixels = px;
    w->shm = id;
    w->w = nw; w->h = nh;
    if (buffered) {
        w->frame = mapping;
        w->shared = (color_t *)((char *)mapping + WM_FRAME_BYTES);
    }
    if (selected) win_select(w);
    return true;
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
        case WM_MOUSE_WHEEL:
            e->type = EV_MOUSE_WHEEL; e->x = m.b; e->y = m.c; e->wheel = m.d; return 1;
        case WM_MOUSE:
            e->x = m.b; e->y = m.c;
            e->buttons = m.d & 0xFF;
            e->type = (m.d >> 8) == 1 ? EV_MOUSE_DOWN : (m.d >> 8) == 2 ? EV_MOUSE_UP : EV_MOUSE_MOVE;
            return 1;
        case WM_CLOSE: e->type = EV_CLOSE; return 1;
        case WM_FOCUS: e->type = EV_FOCUS; e->key = m.b; return 1;
        case WM_QUIT: e->type = EV_QUIT; return 0;
        case WM_RESIZE:
            if (!e->win || !resize_buffer(e->win, m.b, m.c)) continue;
            e->type = EV_RESIZE; e->x = e->win->w; e->y = e->win->h;
            return 1;
        default: continue;
        }
    }
}

/* ------------------------------------------------------------ элементы интерфейса */

bool ui_inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

int ui_text(int x, int y, const char *s, color_t c) { return text_draw(FONT_UI, UI_FONT_SIZE, x, y, s, c); }
int ui_text_width(const char *s) { return text_width(FONT_UI, UI_FONT_SIZE, s); }

void ui_text_center(int x, int y, int w, int h, const char *s, color_t c) {
    int tw = ui_text_width(s), th = text_line_h(FONT_UI, UI_FONT_SIZE);
    ui_text(x + (w - tw) / 2, y + (h - th) / 2, s, c);
}

void ui_button(int x, int y, int w, int h, const char *label, bool pressed) {
    gfx_round_frame(x, y, w, h, 6, pressed ? RGB(190, 184, 178) : UI_SHADOW, pressed ? RGB(222, 221, 218) : RGB(246, 245, 244));
    ui_text_center(x, y, w, h, label, UI_TEXT);
}

void ui_button_accent(int x, int y, int w, int h, const char *label, bool pressed) {
    gfx_round_rect(x, y, w, h, 6, pressed ? RGB(200, 66, 20) : UI_ACCENT);
    int tw = text_width(FONT_UI_MEDIUM, UI_FONT_SIZE, label), th = text_line_h(FONT_UI_MEDIUM, UI_FONT_SIZE);
    text_draw(FONT_UI_MEDIUM, UI_FONT_SIZE, x + (w - tw) / 2, y + (h - th) / 2, label, C_WHITE);
}

void ui_sunken(int x, int y, int w, int h, color_t bg) {
    gfx_round_frame(x, y, w, h, 5, UI_SHADOW, bg);
}
