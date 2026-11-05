/* dom.h — дерево документа (DOM) для браузера MyOS и его связь с JavaScript.
 *
 * Браузер отдаёт сюда HTML страницы; dom.c строит дерево узлов, выполняет
 * скрипты (<script>, в том числе внешние), даёт им document/window, события,
 * таймеры, XMLHttpRequest/fetch, cookie и т. д. Для раскладки дерево снова
 * превращается в простой HTML (dom_serialize) — его понимает старая раскладка
 * браузера. Узлы, по которым можно щёлкнуть, получают атрибут data-n="номер". */
#ifndef DOM_H
#define DOM_H
#include "js.h"

struct dom_host {
    /* скачать адрес (GET или POST): 0 — ок; *data — malloc, освободит dom.c */
    int  (*fetch)(const char *url, const char *method, const char *body, int body_len,
                  char **data, int *len, int *status, char *ctype, int ctype_max);
    void (*status)(const char *msg);            /* показать в строке состояния */
    void (*console)(const char *s, int n);      /* вывод console.log */
};

extern int dom_scripting;                       /* 1 — JavaScript включён */
extern int dom_css;                             /* 1 — стили страницы (CSS) включены */

/* Вычисленный стиль элемента (css.c) — то, что понимает раскладка браузера.
 * Цвета 0xRRGGBB, -1 — нет (прозрачный). Размеры в точках, стороны: верх, право, низ, лево. */
enum { DS_INLINE, DS_BLOCK, DS_LIST_ITEM, DS_INLINE_BLOCK, DS_FLEX, DS_GRID, DS_TABLE, DS_ROW, DS_CELL,
       DS_CONTENTS, DS_NONE };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_ALPHA, LS_NONE };
#define CSS_AUTO (-30000)                       /* margin: auto */
struct dom_style {
    unsigned char display;
    unsigned char bold, italic, underline, strike;
    unsigned char align;                        /* 0 влево, 1 по центру, 2 вправо */
    unsigned char transform;                    /* 1 ПРОПИСНЫЕ, 2 строчные, 3 Каждое Слово */
    unsigned char pre;                          /* white-space: pre / pre-wrap / pre-line */
    unsigned char nowrap;
    unsigned char list;                         /* LS_... */
    unsigned char notext;                       /* текст не показывать (font-size:0, text-indent:-9999px) */
    unsigned char hidden;                       /* элемент не показывать совсем */
    unsigned char flex_row;                     /* flex-контейнер в строку: дети встают в ряд */
    unsigned char width_pct, max_width_pct;
    unsigned char bg_image;                     /* фон — картинка (её не показываем) */
    int   color, bg;
    short font_px;
    short margin[4], padding[4], border[4];
    int   border_color[4];
    short width, max_width;                     /* -1 — не задано */
};
/* стиль узла с номером id (из data-n) после dom_serialize; NULL — нет */
const struct dom_style *dom_style(int id);

void  dom_init(struct dom_host *h);
/* разобрать страницу и выполнить её скрипты */
void  dom_load(const char *html, int len, const char *url);
/* дерево -> простой HTML для раскладки (malloc; длина в *len) */
char *dom_serialize(int *len);
void  dom_title(char *out, int max);

extern int dom_dirty;                               /* дерево менялось — нужна новая раскладка */
int   dom_click(int node);                       /* -> 1, если скрипт отменил действие по умолчанию */
int   dom_submit(int form_node);                 /* -> 1, если отменили отправку */
/* клавиша в поле ввода: keydown, новое значение (если изменилось) -> input, keyup */
void  dom_key(int node, int key, const char *value);
int   dom_next_timer(void);                      /* мс до ближайшего таймера, -1 — нет */
void  dom_run_timers(void);
int   dom_pending_nav(char *url, int max);       /* скрипт попросил перейти по адресу */
void  dom_stats(char *out, int max);             /* «JS: 3 scripts, 1 error» */
const char *dom_last_error(void);
void  dom_set_url(const char *url);              /* адрес изменился без перезагрузки */
void  dom_get_url(char *out, int max);
int   dom_url_changed(void);                     /* history.pushState поменял адрес */

/* cookie (живут, пока открыт браузер) */
void  cookie_set(const char *host, const char *header);  /* строка Set-Cookie */
void  cookie_get(const char *host, char *out, int max);

#endif
