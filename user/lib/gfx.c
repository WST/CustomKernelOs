/* Графическая библиотека программ MyOS. Рисуем в своём буфере в памяти
 * процесса, а на экран его переносит ядро (системный вызов gfx_blit). */
#include "gfx.h"

int gfx_w, gfx_h;
color_t *gfx_buf;
uint8_t gfx_font[256 * 16];
static int clip_on, clip_x0, clip_y0, clip_x1, clip_y1;

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

int gfx_load_font(void) { return sc(SYS_GFX_FONT, (int)gfx_font, 0); }

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

/* ---------------------------------------------------------------- обрезка */

void gfx_set_clip(int x, int y, int w, int h) {
    clip_on = 1;
    clip_x0 = x; clip_y0 = y; clip_x1 = x + w; clip_y1 = y + h;
}

void gfx_no_clip(void) { clip_on = 0; }

int gfx_clip_get(int *x, int *y, int *w, int *h) {
    *x = clip_x0; *y = clip_y0; *w = clip_x1 - clip_x0; *h = clip_y1 - clip_y0;
    return clip_on;
}

void gfx_bounds(int *x0, int *y0, int *x1, int *y1) {
    *x0 = 0; *y0 = 0; *x1 = gfx_w; *y1 = gfx_h;
    if (!clip_on) return;
    if (clip_x0 > *x0) *x0 = clip_x0;
    if (clip_y0 > *y0) *y0 = clip_y0;
    if (clip_x1 < *x1) *x1 = clip_x1;
    if (clip_y1 < *y1) *y1 = clip_y1;
}

void gfx_fill_rect(int x, int y, int w, int h, color_t c) {
    int x0, y0, x1, y1;
    gfx_bounds(&x0, &y0, &x1, &y1);
    if (x > x0) x0 = x;
    if (y > y0) y0 = y;
    if (x + w < x1) x1 = x + w;
    if (y + h < y1) y1 = y + h;
    for (int j = y0; j < y1; j++) {
        color_t *p = gfx_buf + j * gfx_w;
        for (int i = x0; i < x1; i++) p[i] = c;
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

/* растровый шрифт видеокарты 8x16 (с обрезкой) */
int gfx_text_bitmap(int x, int y, const char *s, color_t fg, color_t bg, int scale) {
    int x0 = x, bx0, by0, bx1, by1;
    gfx_bounds(&bx0, &by0, &bx1, &by1);
    for (; *s; s++, x += 8 * scale) {
        const uint8_t *g = gfx_font + (uint8_t)*s * 16;
        if (bg != TRANSPARENT) gfx_fill_rect(x, y, 8 * scale, 16 * scale, bg);
        for (int r = 0; r < 16; r++)
            for (int b = 0; b < 8; b++) {
                if (!(g[r] & (0x80 >> b))) continue;
                if (scale > 1) { gfx_fill_rect(x + b * scale, y + r * scale, scale, scale, fg); continue; }
                int px = x + b, py = y + r;
                if (px >= bx0 && py >= by0 && px < bx1 && py < by1) gfx_buf[py * gfx_w + px] = fg;
            }
    }
    return x - x0;
}

int gfx_text_scaled(int x, int y, const char *s, color_t fg, int scale) {
    int w = font_mono_draw(x, y, s, fg, TRANSPARENT, scale);
    return w >= 0 ? w : gfx_text_bitmap(x, y, s, fg, TRANSPARENT, scale);
}

int gfx_text(int x, int y, const char *s, color_t fg, color_t bg) {
    int w = font_mono_draw(x, y, s, fg, bg, 1);
    return w >= 0 ? w : gfx_text_bitmap(x, y, s, fg, bg, 1);
}

/* ---------------------------------------------------------------- курсор */

/* Стрелка: чёрное тело в белой обводке и мягкая тень — маски, нарисованные
 * сглаженными контурами (координаты в 1/16 пикселя). */
static const short arrow_out[] = { 14, 3, 14, 355, 93, 282, 135, 376, 215, 342, 176, 254, 283, 254 };
static const short arrow_in[]  = { 35, 51, 35, 307, 101, 246, 146, 349, 187, 331, 144, 234, 230, 234 };
static uint8_t cur_white[CURSOR_W * CURSOR_H], cur_black[CURSOR_W * CURSOR_H], cur_shadow[CURSOR_W * CURSOR_H];
static int cur_ready, cur_shape = -1, cur_want, cur_hx, cur_hy;
static color_t under[CURSOR_W * CURSOR_H];
static int ux = -1000, uy;

static void poly_mask(const short *pt, int n, int dx, int dy, uint8_t *m) {
    gfx_path p;
    path_begin(&p, 0, 0, 1, 16);
    for (int i = 0; i < n; i++) {
        if (i == 0) path_move(&p, pt[0] + dx, pt[1] + dy);
        else path_line(&p, pt[i * 2] + dx, pt[i * 2 + 1] + dy);
    }
    path_close(&p);
    path_mask(&p, m, 0, 0, CURSOR_W, CURSOR_H);
    path_free(&p);
}

/* двойная стрелка «тянуть» (в пикселях, центр 12,12), повёрнутая на угол: 0 — ↔, 1 — ↕, 2 — ↘↖, 3 — ↙↗ */
static const short dbl_arrow[] = { 3, 12, 8, 7, 8, 11, 16, 11, 16, 7, 21, 12, 16, 17, 16, 13, 8, 13, 8, 17 };

static void resize_mask(int kind, uint8_t *m) {
    short pt[20];
    for (int i = 0; i < 10; i++) {
        int x = dbl_arrow[i * 2] * 16 - 192 + 8, y = dbl_arrow[i * 2 + 1] * 16 - 192 + 8;   /* от центра, в 1/16 */
        int rx = x, ry = y;
        if (kind == 1) { rx = -y; ry = x; }
        else if (kind == 2) { rx = (x - y) * 181 / 256; ry = (x + y) * 181 / 256; }
        else if (kind == 3) { rx = (x + y) * 181 / 256; ry = (y - x) * 181 / 256; }
        pt[i * 2] = rx + 192; pt[i * 2 + 1] = ry + 192;
    }
    poly_mask(pt, 10, 0, 0, m);
}

/* белая обводка = чёрное тело, раздутое на ~1.5 пикселя */
static void dilate(const uint8_t *src, uint8_t *dst) {
    for (int y = 0; y < CURSOR_H; y++)
        for (int x = 0; x < CURSOR_W; x++) {
            int best = 0;
            for (int j = -2; j <= 2; j++)
                for (int i = -2; i <= 2; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx < 0 || yy < 0 || xx >= CURSOR_W || yy >= CURSOR_H) continue;
                    int v = src[yy * CURSOR_W + xx];
                    if (i * i + j * j > 2) v = v * (i * i + j * j > 4 ? 0 : 1) / 2;   /* дальние соседи слабее */
                    if (v > best) best = v;
                }
            dst[y * CURSOR_W + x] = (uint8_t)best;
        }
}

static void make_cursor(void) {
    static uint8_t tmp[CURSOR_W * CURSOR_H];
    cur_shape = cur_want;
    memset(cur_white, 0, sizeof(cur_white));
    memset(cur_black, 0, sizeof(cur_black));
    memset(tmp, 0, sizeof(tmp));
    if (cur_shape == GFX_CURSOR_ARROW) {
        poly_mask(arrow_out, 7, 0, 0, cur_white);
        poly_mask(arrow_in, 7, 0, 0, cur_black);
        poly_mask(arrow_out, 7, 12, 24, tmp);
        cur_hx = CURSOR_HOT_X; cur_hy = CURSOR_HOT_Y;
    } else {
        resize_mask(cur_shape - GFX_CURSOR_RESIZE_H, cur_black);
        dilate(cur_black, cur_white);
        for (int y = CURSOR_H - 1; y >= 0; y--)                  /* тень — обводка, сдвинутая на (1, 2) */
            for (int x = CURSOR_W - 1; x >= 0; x--)
                tmp[y * CURSOR_W + x] = (x >= 1 && y >= 2) ? cur_white[(y - 2) * CURSOR_W + x - 1] : 0;
        cur_hx = 12; cur_hy = 12;
    }
    for (int y = 0; y < CURSOR_H; y++)                  /* размываем тень 3x3 */
        for (int x = 0; x < CURSOR_W; x++) {
            int sum = 0;
            for (int j = -1; j <= 1; j++)
                for (int i = -1; i <= 1; i++) {
                    int xx = x + i, yy = y + j;
                    if (xx >= 0 && yy >= 0 && xx < CURSOR_W && yy < CURSOR_H) sum += tmp[yy * CURSOR_W + xx];
                }
            cur_shadow[y * CURSOR_W + x] = (uint8_t)(sum / 9 * 90 / 255);
        }
    cur_ready = 1;
}

static void cursor_ready(void) { if (!cur_ready || cur_shape != cur_want) make_cursor(); }

void gfx_cursor_shape(int shape) { cur_want = shape; }
int  gfx_cursor_get_shape(void) { return cur_want; }
void gfx_cursor_hot(int *hx, int *hy) { cursor_ready(); *hx = cur_hx; *hy = cur_hy; }

void gfx_cursor_draw(int x, int y) {
    cursor_ready();
    x -= cur_hx; y -= cur_hy;
    ux = x; uy = y;
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) under[j * CURSOR_W + i] = gfx_get(x + i, y + j);
    gfx_draw_mask(x, y, cur_shadow, CURSOR_W, CURSOR_H, C_BLACK, 255);
    gfx_draw_mask(x, y, cur_white, CURSOR_W, CURSOR_H, C_WHITE, 255);
    gfx_draw_mask(x, y, cur_black, CURSOR_W, CURSOR_H, RGB(20, 20, 20), 255);
}

/* Аппаратный курсор: картинка 0xAARRGGBB с настоящей прозрачностью (тень,
 * сглаженный край). Карта с альфа-курсором так её и покажет; остальные понимают
 * только «есть/нет» — ядро само отрежет по половине прозрачности. */
int gfx_hw_cursor(int on) {
    static struct { int w, h, hot_x, hot_y; unsigned px[32 * 32]; } c;
    if (!on) return sc(SYS_GFX_CURSOR, 0, 0);
    cursor_ready();
    c.w = CURSOR_W; c.h = CURSOR_H; c.hot_x = cur_hx; c.hot_y = cur_hy;
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) {
            int k = j * CURSOR_W + i;
            /* «над» друг другом: тень, белая обводка, чёрное тело (прозрачность без умножения) */
            int sa = cur_shadow[k], wa = cur_white[k], ba = cur_black[k];
            int a1 = wa + sa * (255 - wa) / 255;                       /* тень + белое */
            int r1 = a1 ? 255 * wa / a1 : 0;                           /* цвет: белое на чёрной тени */
            int a = ba + a1 * (255 - ba) / 255;
            int r = a ? (20 * ba + r1 * a1 * (255 - ba) / 255) / a : 0;
            if (r > 255) r = 255;
            c.px[k] = (unsigned)a << 24 | (unsigned)r << 16 | (unsigned)r << 8 | (unsigned)r;
        }
    return sc(SYS_GFX_CURSOR, 1, (int)&c);
}

void gfx_cursor_erase(void) {
    if (ux == -1000) return;
    for (int j = 0; j < CURSOR_H; j++)
        for (int i = 0; i < CURSOR_W; i++) gfx_pixel(ux + i, uy + j, under[j * CURSOR_W + i]);
    ux = -1000;
}
