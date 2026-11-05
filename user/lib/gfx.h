/* gfx.h — графика для программ MyOS.
 *
 *   gfx_open();                         забрать экран у консоли, выделить буфер кадра
 *   gfx_clear(RGB(0, 0, 40));           рисуем в буфере (gfx_buf, gfx_w x gfx_h)
 *   gfx_text(10, 10, "hi", WHITE, -1);
 *   gfx_flip();                         показать кадр (или gfx_flip_rect — только часть)
 *   gfx_close();                        вернуть экран консоли (при выходе — само)
 *
 * Видеокарта и разрешение:
 *   gfx_info2(&gi)                      драйвер, режим, видеопамять, умения (GFX_CAP_*)
 *   gfx_set_mode(1280, 720)             сменить разрешение (для всех: консоль, стол)
 *   if (gfx_check_mode()) ...           режим сменился: gfx_w/gfx_h/gfx_buf уже новые
 *   gfx_hw_copy(...), gfx_hw_fill(...)  копирование/заливка прямо на экране — на GPU
 *   gfx_hw_cursor(1)                    аппаратный курсор (карта рисует стрелку сама)
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

struct gfx_info2 {
    unsigned width, height, bpp, pitch;
    unsigned vram_kb, max_w, max_h, caps, gen;
    int hw_cursor;
    char driver[24];
};
#define GFX_CAP_COPY      1         /* gfx_hw_copy работает (на GPU или процессором ядра) */
#define GFX_CAP_FILL      2
#define GFX_CAP_CURSOR    4         /* у карты есть аппаратный курсор */
#define GFX_CAP_GPU       8         /* копирование/заливку умеет сама видеокарта */
#define GFX_CAP_GPU_ON    16        /* ...и сейчас она это делает */
#define GFX_CAP_CURSOR_ON 32
#define GFX_CAP_ALPHA_CURSOR 64      /* аппаратный курсор с прозрачностью */
int  gfx_info2(struct gfx_info2 *gi);
int  gfx_set_mode(int w, int h);    /* 0 или -EINVAL */
int  gfx_check_mode(void);          /* 1 — разрешение сменилось, буфер пересоздан */
/* Прямо на экране, минуя буфер: свой gfx_buf программа поправляет сама. */
int  gfx_hw_copy(int sx, int sy, int dx, int dy, int w, int h);
int  gfx_hw_fill(int x, int y, int w, int h, color_t c);
int  gfx_hw_cursor(int on);         /* 0 или -ENODEV (нет аппаратного курсора) */
/* Список стандартных разрешений: -> сколько; поддерживаемые картой — с флагом ok */
struct gfx_mode { int w, h, ok; };
int  gfx_modes(struct gfx_mode *out, int max);
/* /etc/display: сохранить/прочитать выбор пользователя («1280x720» и «cursor=hw») */
int  gfx_save_config(int w, int h, int hw_cursor);
int  gfx_load_config(int *w, int *h, int *hw_cursor);

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
/* Текст моноширинным шрифтом в клетках 8x16 (сглаженный Ubuntu Mono, если он
 * есть в /lib/fonts, иначе растровый шрифт видеокарты). Понимает UTF-8
 * (кириллица); одиночные байты >= 0x80 — псевдографика CP437.
 * scale — увеличение; bg = TRANSPARENT — без фона. -> ширина в пикселях */
int  gfx_text(int x, int y, const char *s, color_t fg, color_t bg);
int  gfx_text_scaled(int x, int y, const char *s, color_t fg, int scale);
int  gfx_text_bitmap(int x, int y, const char *s, color_t fg, color_t bg, int scale);  /* всегда растровый */
extern uint8_t gfx_font[256 * 16];

/* Обрезка: все рисующие функции (кроме gfx_pixel) не выходят за этот прямоугольник */
void gfx_set_clip(int x, int y, int w, int h);
void gfx_no_clip(void);
int  gfx_clip_get(int *x, int *y, int *w, int *h);         /* -> 1, если обрезка включена */
void gfx_bounds(int *x0, int *y0, int *x1, int *y1);       /* куда можно рисовать сейчас */

/* ---------------------------------------------------------------- сглаженная графика (vec.c)
 * alpha — непрозрачность 0..255. Края фигур гладкие (без «лесенки»). */
color_t gfx_mix(color_t dst, color_t src, int a);           /* src поверх dst с непрозрачностью a */
void gfx_blend_pixel(int x, int y, color_t c, int a);
void gfx_fill_rect_a(int x, int y, int w, int h, color_t c, int alpha);
void gfx_gradient(int x, int y, int w, int h, color_t top, color_t bottom);
void gfx_round_rect(int x, int y, int w, int h, int r, color_t c);
void gfx_round_rect_a(int x, int y, int w, int h, int r, color_t c, int alpha);
/* corners: 1 — левый верхний, 2 — правый верхний, 4 — правый нижний, 8 — левый нижний */
void gfx_round_rect_ex(int x, int y, int w, int h, int r, int corners, color_t c, int alpha);
void gfx_round_frame(int x, int y, int w, int h, int r, color_t border, color_t fill);
void gfx_ellipse(int x, int y, int w, int h, color_t c, int alpha);       /* вписан в прямоугольник */
void gfx_disc16(int cx, int cy, int r, color_t c, int alpha);             /* всё в 1/16 пикселя */
void gfx_line16(int x0, int y0, int x1, int y1, int thick, color_t c, int alpha);
void gfx_shadow(int x, int y, int w, int h, int r, int blur, int dy, int alpha,
                int hole_x, int hole_y, int hole_w, int hole_h);
void gfx_draw_mask(int x, int y, const uint8_t *m, int w, int h, color_t c, int alpha);
void gfx_draw_argb(int x, int y, const uint32_t *img, int w, int h);      /* 0xAARRGGBB */

/* Контур из линий и кривых. Координаты пользователя: X = ox + x * num / den
 * пикселей (например, den = 16 — рисуем в 1/16 пикселя).
 *   gfx_path p; path_begin(&p, 10, 10, 1, 1);
 *   path_move(&p, 0, 0); path_line(&p, 20, 0); path_quad(&p, 30, 10, 20, 20); path_close(&p);
 *   path_fill(&p, C_RED, 255); path_free(&p); */
typedef struct gfx_path {
    int *e, n, cap;                 /* рёбра: x0, y0, x1, y1, направление (в 1/256 пикселя) */
    int cx, cy, sx, sy, open;
    int ox, oy, num, den;
    int minx, miny, maxx, maxy;
} gfx_path;
void path_begin(gfx_path *p, int ox, int oy, int num, int den);
void path_move(gfx_path *p, int x, int y);
void path_line(gfx_path *p, int x, int y);
void path_quad(gfx_path *p, int cx, int cy, int x, int y);
void path_cubic(gfx_path *p, int c1x, int c1y, int c2x, int c2y, int x, int y);
void path_close(gfx_path *p);
void path_rect(gfx_path *p, int x, int y, int w, int h);
void path_round_rect(gfx_path *p, int x, int y, int w, int h, int r);
void path_round_rect_ex(gfx_path *p, int x, int y, int w, int h, int r, int corners);
void path_ellipse(gfx_path *p, int x, int y, int w, int h);
void path_fill(gfx_path *p, color_t c, int alpha);
void path_fill_grad(gfx_path *p, color_t top, int y0, color_t bottom, int y1, int alpha);
void path_mask(gfx_path *p, uint8_t *mask, int x, int y, int w, int h);
void path_free(gfx_path *p);
/* то же в «сырых» единицах 1/256 пикселя */
void path_fix_move(gfx_path *p, int x, int y);
void path_fix_line(gfx_path *p, int x0, int y0, int x1, int y1);
void path_fix_lineto(gfx_path *p, int x, int y);
void path_fix_quad(gfx_path *p, int x1, int y1, int x2, int y2);
void path_fix_cubic(gfx_path *p, int x1, int y1, int x2, int y2, int x3, int y3);

/* Рисование «на прозрачном» — для значков: рисуем дважды (pass 0 и 1), получаем 0xAARRGGBB
 *   for (int pass = 0; pass < 2; pass++) { gfx_canvas_begin(&cv, 48, 48, pass); ...рисуем...; }
 *   uint32_t *img = gfx_canvas_end(&cv);       // free(img), когда не нужен */
struct gfx_canvas {
    int w, h, save_w, save_h, saved_clip, cx, cy, cw, ch;
    color_t *save_buf, *black, *white;
};
void gfx_canvas_begin(struct gfx_canvas *cv, int w, int h, int pass);
uint32_t *gfx_canvas_end(struct gfx_canvas *cv);

/* ---------------------------------------------------------------- шрифты TrueType (font.c)
 * Шрифты Ubuntu из /lib/fonts. size — размер в пикселях, y — верх строки. */
enum { FONT_UI, FONT_UI_MEDIUM, FONT_UI_BOLD, FONT_MONO, FONT_MONO_BOLD, FONT_COUNT };
int  text_draw(int font, int size, int x, int y, const char *s, color_t c);   /* -> ширина */
int  text_draw_n(int font, int size, int x, int y, const char *s, int nbytes, color_t c);
int  text_draw_fit(int font, int size, int x, int y, const char *s, int max_w, color_t c);  /* с «…» */
int  text_width(int font, int size, const char *s);
int  text_width_n(int font, int size, const char *s, int nbytes);
int  text_fit(int font, int size, const char *s, int max_w);     /* -> сколько байт влезает */
int  text_line_h(int font, int size);
int  text_ascent(int font, int size);
int  font_available(int font);
void font_set_dir(const char *dir);                         /* по умолчанию /lib/fonts */
int  font_mono_draw(int x, int y, const char *s, color_t fg, color_t bg, int scale);
int  utf8_next(const char **s);

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
#define CURSOR_W 24
#define CURSOR_H 28
#define CURSOR_HOT_X 2               /* где «кончик» стрелки внутри картинки */
#define CURSOR_HOT_Y 3
/* вид курсора (для стола: «тянуть край окна»); gfx_hw_cursor(1) после смены — перезагрузить в карту */
enum { GFX_CURSOR_ARROW, GFX_CURSOR_RESIZE_H, GFX_CURSOR_RESIZE_V, GFX_CURSOR_RESIZE_D1, GFX_CURSOR_RESIZE_D2 };
void gfx_cursor_shape(int shape);
int  gfx_cursor_get_shape(void);
void gfx_cursor_hot(int *hx, int *hy);  /* «горячая точка» текущего вида */

#endif
