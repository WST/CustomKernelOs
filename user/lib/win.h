/* win.h — окна на рабочем столе MyOS.
 *
 *   window *w = win_create("Hello", 300, 200);    NULL — рабочий стол не запущен
 *   gfx_clear(C_WHITE);                           рисуем как обычно (gfx.h) — в окно
 *   gfx_text(10, 10, "Hi!", C_BLACK, TRANSPARENT);
 *   win_update(w);                                показать нарисованное
 *   struct event e;
 *   while (win_event(&e, -1)) {                   ждать событие (-1 — сколько угодно)
 *       if (e.type == EV_CLOSE) break;            нажали крестик
 *       if (e.type == EV_MOUSE_DOWN) ...          e.x, e.y — внутри окна
 *       if (e.type == EV_KEY) ...                 e.key — как у readkey()
 *   }
 *
 * Как устроено: пиксели окна лежат в общей памяти (shm), которую видит и
 * программа, и рабочий стол. Программа рисует, потом сообщением говорит
 * «обнови такой-то прямоугольник», а рабочий стол копирует его на экран
 * поверх обоев и под другими окнами. События идут обратно тоже сообщениями. */
#ifndef WIN_H
#define WIN_H
#include "gfx.h"

typedef struct window {
    int id;                         /* номер у рабочего стола */
    int w, h;                       /* размер области рисования */
    color_t *pixels;                /* w * h пикселей */
    int shm;
} window;

enum {
    EV_NONE, EV_KEY, EV_MOUSE_DOWN, EV_MOUSE_UP, EV_MOUSE_MOVE,
    EV_CLOSE,                       /* нажали крестик — обычно надо закрыться */
    EV_FOCUS,                       /* e.key: 1 — окно стало активным, 0 — нет */
    EV_QUIT,                        /* рабочий стол завершается */
    EV_TIMEOUT,                     /* ничего не случилось за timeout_ms */
    EV_MESSAGE,                     /* сообщение от другой программы: e.msg */
    EV_RESIZE,                      /* окно растянули: e.x, e.y — новый размер; буфер уже новый,
                                       старое содержимое скопировано — перерисуй и win_update */
    EV_MOUSE_WHEEL,                 /* e.wheel: signed steps, positive = up */
};

struct event {
    int type;
    window *win;
    int key;                        /* EV_KEY */
    int x, y;                       /* мышь: координаты в окне */
    int wheel;                      /* EV_MOUSE_WHEEL */
    int buttons;                    /* MOUSE_LEFT | MOUSE_RIGHT ... */
    struct msg msg;                 /* EV_MESSAGE */
};

window *win_create(const char *title, int w, int h);
void    win_destroy(window *w);
void    win_select(window *w);      /* дальше gfx_* рисуют в это окно */
void    win_update(window *w);      /* показать всё окно */
void    win_update_rect(window *w, int x, int y, int rw, int rh);
void    win_set_title(window *w, const char *title);
/* окно можно растягивать мышью и разворачивать (кнопка «развернуть» в заголовке);
 * не меньше min_w x min_h. Программа должна уметь EV_RESIZE. */
void    win_set_resizable(window *w, int min_w, int min_h);
int     win_event(struct event *e, int timeout_ms);  /* 0 — рабочий стол пропал */
int     win_desktop_pid(void);

/* ---- простые элементы интерфейса (рисуют в выбранное окно) ----
 * Цвета и вид — как у темы Yaru в Ubuntu: светлый фон, скруглённые кнопки,
 * оранжевый акцент. Подписи — шрифтом Ubuntu (сглаженным). */
#define UI_FACE    RGB(250, 250, 250)       /* фон окна */
#define UI_LIGHT   RGB(255, 255, 255)
#define UI_SHADOW  RGB(205, 199, 194)       /* рамки */
#define UI_DARK    RGB(130, 130, 130)
#define UI_TEXT    RGB(61, 61, 61)
#define UI_ACCENT  RGB(233, 84, 32)         /* оранжевый Ubuntu */
#define UI_SELECT  RGB(252, 221, 208)
#define UI_FONT_SIZE 14

void ui_button(int x, int y, int w, int h, const char *label, bool pressed);
void ui_button_accent(int x, int y, int w, int h, const char *label, bool pressed);   /* оранжевая */
void ui_sunken(int x, int y, int w, int h, color_t bg);       /* поле ввода / список */
void ui_text_center(int x, int y, int w, int h, const char *s, color_t c);
int  ui_text(int x, int y, const char *s, color_t c);         /* шрифт интерфейса, -> ширина */
int  ui_text_width(const char *s);
bool ui_inside(int px, int py, int x, int y, int w, int h);

/* ---- протокол с рабочим столом (для любопытных: см. desktop.c) ---- */
enum {
    WM_CREATE = 100, WM_CREATED, WM_UPDATE, WM_TITLE, WM_DESTROY,
    WM_DISPLAY = 110,               /* программа -> стол: a = аппаратный курсор вкл/выкл */
    WM_WALLPAPER = 113,             /* reload /etc/wallpaper */
    WM_RESIZABLE = 111,             /* a = окно, b, c = наименьший размер */
    WM_SETBUF = 112,                /* a = окно, b = shm нового буфера, c = ширина, d = высота */
    WM_KEY = 200, WM_MOUSE, WM_CLOSE, WM_FOCUS, WM_QUIT,
    WM_MOUSE_WHEEL = 211,           /* a = window, b/c = position, d = signed steps */
    WM_RESIZE = 210,                /* стол -> программа: a = окно, b, c = новый размер */
};

#endif
