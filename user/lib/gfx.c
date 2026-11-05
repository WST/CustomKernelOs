/* Графическая библиотека программ MyOS. Рисуем в своём буфере в памяти
 * процесса, а на экран его переносит ядро (системный вызов gfx_blit). */
#include "gfx.h"

int gfx_w, gfx_h;
color_t *gfx_buf;
static uint8_t font[256 * 16];

struct gfx_info { unsigned width, height, bpp, pitch; };
struct gfx_rect { int x, y, w, h; };

#ifdef __MYOS_CC__
static int sc(int n, int a, int b) { return __syscall(n, a, b, 0); }
#else
static inline int sc(int n, int a, int b) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(0) : "memory");
    return r;
}
#endif

int gfx_load_font(void) { return sc(SYS_GFX_FONT, (int)font, 0); }

int gfx_open(void) {
    struct gfx_info gi;
    int r = sc(SYS_GFX_INFO, (int)&gi, 0);
    if (r < 0) return r;
    gfx_w = gi.width;
    gfx_h = gi.height;
    if (!gfx_buf) gfx_buf = malloc(gfx_w * gfx_h * 4);
    if (!gfx_buf) return -ENOMEM;
    gfx_load_font();
    if ((r = sc(SYS_GFX_ACQUIRE, 1, 0)) < 0) return r;
    ttymode(TTY_RAW | TTY_NONBLOCK);
    return 0;
}

void gfx_close(void) {
    sc(SYS_GFX_ACQUIRE, 0, 0);
    ttymode(0);
}

void gfx_flip_rect(int x, int y, int w, int h) {
    struct gfx_rect r = { x, y, w, h };
    sc(SYS_GFX_BLIT, (int)gfx_buf, (int)&r);
}

void gfx_flip(void) { gfx_flip_rect(0, 0, gfx_w, gfx_h); }

int mouse_get(struct mouse *m) { return sc(SYS_MOUSE, (int)m, 0); }

/* ---------------------------------------------------------------- видеокарта */

int gfx_info2(struct gfx_info2 *gi) { return sc(SYS_GFX_INFO2, (int)gi, 0); }
int gfx_set_mode(int w, int h) { return sc(SYS_GFX_MODE, w, h); }

int gfx_check_mode(void) {
    struct gfx_info2 gi;
    if (gfx_info2(&gi) < 0) return 0;
    if ((int)gi.width == gfx_w && (int)gi.height == gfx_h) return 0;
    color_t *nb = malloc(gi.width * gi.height * 4);
    if (!nb) return 0;
    free(gfx_buf);
    gfx_buf = nb;
    gfx_w = gi.width;
    gfx_h = gi.height;
    gfx_clear(C_BLACK);
    return 1;
}

int gfx_hw_copy(int sx, int sy, int dx, int dy, int w, int h) {
    int a[6];
    a[0] = sx; a[1] = sy; a[2] = dx; a[3] = dy; a[4] = w; a[5] = h;
    return sc(SYS_GFX_ACCEL, 1, (int)a);
}

int gfx_hw_fill(int x, int y, int w, int h, color_t c) {
    int a[6];
    a[0] = x; a[1] = y; a[2] = w; a[3] = h; a[4] = (int)c; a[5] = 0;
    return sc(SYS_GFX_ACCEL, 2, (int)a);
}

static const int std_modes[][2] = {
    { 640, 480 }, { 800, 600 }, { 1024, 768 }, { 1152, 864 }, { 1280, 720 }, { 1280, 800 },
    { 1280, 1024 }, { 1360, 768 }, { 1440, 900 }, { 1600, 900 }, { 1600, 1200 },
    { 1680, 1050 }, { 1920, 1080 }, { 1920, 1200 }, { 2560, 1440 }, { 2560, 1600 },
};

int gfx_modes(struct gfx_mode *out, int max) {
    struct gfx_info2 gi;
    int n = 0;
    if (gfx_info2(&gi) < 0) return 0;
    for (int i = 0; i < (int)(sizeof(std_modes) / sizeof(std_modes[0])) && n < max; i++) {
        int w = std_modes[i][0], h = std_modes[i][1];
        out[n].w = w; out[n].h = h;
        out[n].ok = w <= (int)gi.max_w && h <= (int)gi.max_h && (unsigned)(w * h / 256) <= gi.vram_kb;
        n++;
    }
    return n;
}

int gfx_save_config(int w, int h, int hw_cursor) {
    mkdir("/etc");
    int fd = open("/etc/display", O_WRITE);
    if (fd < 0) return fd;
    dprintf(fd, "%dx%d\ncursor=%s\n", w, h, hw_cursor ? "hw" : "sw");
    close(fd);
    return 0;
}

int gfx_load_config(int *w, int *h, int *hw_cursor) {
    char buf[64];
    int fd = open("/etc/display", O_READ);
    if (fd < 0) return fd;
    int n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -EINVAL;
    buf[n] = 0;
    *w = atoi(buf);
    char *x = strchr(buf, 'x');
    *h = x ? atoi(x + 1) : 0;
    *hw_cursor = strstr(buf, "cursor=hw") != NULL;
    return 0;
}

/* ---------------------------------------------------------------- фигуры */

void gfx_fill_rect(int x, int y, int w, int h, color_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > gfx_w) w = gfx_w - x;
    if (y + h > gfx_h) h = gfx_h - y;
    for (int j = 0; j < h; j++) {
        color_t *p = gfx_buf + (y + j) * gfx_w + x;
        for (int i = 0; i < w; i++) p[i] = c;
    }
}

void gfx_clear(color_t c) { gfx_fill_rect(0, 0, gfx_w, gfx_h, c); }

void gfx_rect(int x, int y, int w, int h, color_t c) {
    gfx_fill_rect(x, y, w, 1, c);
    gfx_fill_rect(x, y + h - 1, w, 1, c);
    gfx_fill_rect(x, y, 1, h, c);
    gfx_fill_rect(x + w - 1, y, 1, h, c);
}

/* алгоритм Брезенхэма: только целые числа */
void gfx_line(int x0, int y0, int x1, int y1, color_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_pixel(x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

void gfx_fill_circle(int cx, int cy, int r, color_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) gfx_pixel(cx + x, cy + y, c);
}

/* толстая линия = кружочки вдоль линии */
void gfx_thick_line(int x0, int y0, int x1, int y1, int r, color_t c) {
    if (r <= 0) { gfx_line(x0, y0, x1, y1, c); return; }
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        gfx_fill_circle(x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

/* окружность по алгоритму средней точки */
void gfx_circle(int cx, int cy, int r, color_t c) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        gfx_pixel(cx + x, cy + y, c); gfx_pixel(cx - x, cy + y, c);
        gfx_pixel(cx + x, cy - y, c); gfx_pixel(cx - x, cy - y, c);
        gfx_pixel(cx + y, cy + x, c); gfx_pixel(cx - y, cy + x, c);
        gfx_pixel(cx + y, cy - x, c); gfx_pixel(cx - y, cy - x, c);
        y++;
        if (err < 0) err += 2 * y + 1;
        else { x--; err += 2 * (y - x) + 1; }
    }
}

/* ---------------------------------------------------------------- текст */

int gfx_text_scaled(int x, int y, const char *s, color_t fg, int scale) {
    int x0 = x;
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *g = font + (uint8_t)*s * 16;
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++)
                if (g[r] & (0x80 >> b)) gfx_fill_rect(x + b * scale, y + r * scale, scale, scale, fg);
    }
    return x - x0;
}

int gfx_text(int x, int y, const char *s, color_t fg, color_t bg) {
    int x0 = x;
    for (; *s; s++, x += 8) {
        const uint8_t *g = font + (uint8_t)*s * 16;
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++) {
                bool on = g[r] & (0x80 >> b);
                if (on) gfx_pixel(x + b, y + r, fg);
                else if (bg != TRANSPARENT) gfx_pixel(x + b, y + r, bg);
            }
    }
    return x - x0;
}

/* ---------------------------------------------------------------- курсор */

static const char *arrow[CURSOR_H] = {
    "X           ", "XX          ", "X.X         ", "X..X        ",
    "X...X       ", "X....X      ", "X.....X     ", "X......X    ",
    "X.......X   ", "X........X  ", "X.........X ", "X..........X",
    "X......XXXXX", "X...X..X    ", "X..XX..X    ", "X.X  X..X   ",
    "XX   X..X   ", "X     X..X  ", "      XXX   ",
};
static color_t under[CURSOR_W * CURSOR_H];
static int ux = -1, uy;

void gfx_cursor_draw(int x, int y) {
    ux = x; uy = y;
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) {
            under[j * CURSOR_W + i] = gfx_get(x + i, y + j);
            char c = arrow[j][i];
            if (c == 'X') gfx_pixel(x + i, y + j, C_BLACK);
            else if (c == '.') gfx_pixel(x + i, y + j, C_WHITE);
        }
}

/* аппаратный курсор: та же стрелка, картинкой для видеокарты */
int gfx_hw_cursor(int on) {
    static struct { int w, h, hot_x, hot_y; unsigned px[32 * 32]; } c;
    if (!on) return sc(SYS_GFX_CURSOR, 0, 0);
    c.w = CURSOR_W; c.h = CURSOR_H; c.hot_x = 0; c.hot_y = 0;
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) {
            char ch = arrow[j][i];
            c.px[j * CURSOR_W + i] = ch == 'X' ? 0xFF000000u : ch == '.' ? 0xFFFFFFFFu : 0;
        }
    return sc(SYS_GFX_CURSOR, 1, (int)&c);
}

void gfx_cursor_erase(void) {
    if (ux < 0) return;
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++)
            if (arrow[j][i] != ' ') gfx_pixel(ux + i, uy + j, under[j * CURSOR_W + i]);
    ux = -1;
}
