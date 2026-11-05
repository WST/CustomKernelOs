/* gfx.h — графика для программ MyOS.
 *
 *   gfx_open();                         забрать экран у консоли, выделить буфер кадра
 *   gfx_clear(RGB(0, 0, 40));           рисуем в буфере (gfx_buf, gfx_w x gfx_h)
 *   gfx_text(10, 10, "hi", WHITE, -1);
 *   gfx_flip();                         показать кадр (или gfx_flip_rect — только часть)
 *   gfx_close();                        вернуть экран консоли (при выходе — само)
 *
 * Пиксели — 0x00RRGGBB. Мышь — mouse_get(). Клавиши — readkey() в режиме
 * TTY_RAW | TTY_NONBLOCK (его включает gfx_open). */
#ifndef GFX_H
#define GFX_H
#include "myos.h"

typedef uint32_t color_t;
#define RGB(r, g, b) ((color_t)(((r) & 255) << 16 | ((g) & 255) << 8 | ((b) & 255)))
#define TRANSPARENT  0xFFFFFFFFu    /* для фона текста: не закрашивать */

#define C_BLACK   RGB(0, 0, 0)
#define C_WHITE   RGB(255, 255, 255)
#define C_GREY    RGB(128, 128, 128)
#define C_RED     RGB(220, 50, 50)
#define C_GREEN   RGB(60, 200, 80)
#define C_BLUE    RGB(50, 100, 230)
#define C_YELLOW  RGB(250, 220, 60)

extern int gfx_w, gfx_h;
extern color_t *gfx_buf;

int  gfx_open(void);                /* 0 или -errno (например, нет графики) */
int  gfx_load_font(void);           /* только шрифт (gfx_open делает это сам) */
void gfx_close(void);
void gfx_flip(void);
void gfx_flip_rect(int x, int y, int w, int h);

static inline void gfx_pixel(int x, int y, color_t c) {
    if ((unsigned)x < (unsigned)gfx_w && (unsigned)y < (unsigned)gfx_h) gfx_buf[y * gfx_w + x] = c;
}
static inline color_t gfx_get(int x, int y) {
    return ((unsigned)x < (unsigned)gfx_w && (unsigned)y < (unsigned)gfx_h) ? gfx_buf[y * gfx_w + x] : 0;
}

void gfx_clear(color_t c);
void gfx_fill_rect(int x, int y, int w, int h, color_t c);
void gfx_rect(int x, int y, int w, int h, color_t c);
void gfx_line(int x0, int y0, int x1, int y1, color_t c);
void gfx_thick_line(int x0, int y0, int x1, int y1, int r, color_t c);
void gfx_circle(int cx, int cy, int r, color_t c);
void gfx_fill_circle(int cx, int cy, int r, color_t c);
/* текст шрифтом 8x16; scale — увеличение; bg = TRANSPARENT — без фона. -> ширина в пикселях */
int  gfx_text(int x, int y, const char *s, color_t fg, color_t bg);
int  gfx_text_scaled(int x, int y, const char *s, color_t fg, int scale);

/* мышь */
struct mouse {
    int x, y;
    unsigned buttons;               /* MOUSE_LEFT | MOUSE_RIGHT | MOUSE_MIDDLE */
    unsigned events;
};
#define MOUSE_LEFT   1
#define MOUSE_RIGHT  2
#define MOUSE_MIDDLE 4
int  mouse_get(struct mouse *m);

/* курсор-стрелка: рисуется в буфер поверх картинки, под ним всё сохраняется */
void gfx_cursor_draw(int x, int y);
void gfx_cursor_erase(void);
#define CURSOR_W 12
#define CURSOR_H 19

#endif
