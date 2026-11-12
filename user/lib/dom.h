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
    void (*console)(const char *s, int n);
    int (*storage_read)(const char *origin,char *out,int max); /* length, 0 missing, negative error */
    int (*storage_write)(const char *origin,const char *data,int len); /* atomic localStorage snapshot */
};

extern int dom_scripting;                       /* 1 — JavaScript включён */
extern int dom_css;                             /* 1 — стили страницы (CSS) включены */
/* сколько мс всего могут работать скрипты при загрузке страницы (остальные пропускаются) */
extern int dom_load_budget_ms;

/* Вычисленный стиль элемента (css.c) — то, что понимает раскладка браузера.
 * Цвета 0xRRGGBB, -1 — нет (прозрачный). Размеры в точках, стороны: верх, право, низ, лево. */
enum { DS_INLINE, DS_BLOCK, DS_LIST_ITEM, DS_INLINE_BLOCK, DS_FLEX, DS_GRID, DS_TABLE, DS_ROW, DS_CELL,
       DS_CONTENTS, DS_NONE };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_ALPHA, LS_NONE };
#define CSS_AUTO (-30000)                       /* margin: auto */
#define CSS_BG_LAYERS 4
#define CSS_GRADIENT_STOPS 8
struct css_bg_layer {
    const char *url;
    short size[2], pos[2], angle;
    unsigned char size_pct, pos_pct, mode, repeat, origin, clip, nstops;
    short stops[CSS_GRADIENT_STOPS];
    unsigned colors[CSS_GRADIENT_STOPS]; /* straight ARGB */
};
struct css_background { int count; struct css_bg_layer layer[CSS_BG_LAYERS]; };
#define CSS_SHADOWS 4
struct css_shadow {short x,y,blur,spread;unsigned color;unsigned char inset;};
struct css_shadows {int count;struct css_shadow layer[CSS_SHADOWS];};
struct dom_style {
    unsigned char display;
    unsigned char bold, italic, underline, strike, font_family;
    unsigned short opacity; /* 0..10000, rendering quantizes to 8-bit coverage */
    unsigned char align;                        /* 0 влево, 1 по центру, 2 вправо */
    unsigned char transform;                    /* 1 ПРОПИСНЫЕ, 2 строчные, 3 Каждое Слово */
    unsigned char pre;                          /* white-space: pre / pre-wrap / pre-line */
    unsigned char nowrap, wrap_word;
    unsigned char list;                         /* LS_... */
    unsigned char notext;                       /* текст не показывать (font-size:0, text-indent:-9999px) */
    unsigned char pointer_none, hidden;                       /* элемент не показывать совсем */
    unsigned char flex_row, flex_column, flex_reverse;                     /* flex-контейнер в строку: дети встают в ряд */
    unsigned char width_pct, max_width_pct, min_width_pct;
    const struct css_background *background,*mask;
    const struct css_shadows *shadows;
    unsigned char mask_luminance,mask_composite[CSS_BG_LAYERS];
    unsigned char bg_image, bg_clip;                     /* имеются фоновые изображения; область background-clip */
    unsigned char border_box;
    unsigned char flex_wrap, float_side, clear_side;
    unsigned char justify, align_items, align_self; /* 0 start/stretch, 1 center, 2 end, 3 between, 4 around, 5 evenly */
    short flex_grow, flex_shrink;                /* factors multiplied by 100 */
    short height, min_height;                  /* -1 auto; pixels, not inherited */
    short line_height;                         /* 0 normal; negative = unitless factor * -100 */
    unsigned char grid_auto,grid_track_type[32];
    short grid_min,grid_track[32];
    short grid_columns;                         /* repeat(N,minmax(0,1fr)) or equal fr tracks */
    short radius;
    unsigned char object_fit;
    short object_pos[2];
    unsigned char fixed;                        /* 1 absolute, 2 fixed, 3 relative */
    unsigned char height_pct, position_pct, translate_pct;
    short position_edge[4], translate_x, translate_y;
    unsigned char clip_overflow, has_z_index;
    int z_index;
    short column_gap, row_gap;
    int   color, bg;
    short font_px;
    short margin[4], padding[4], border[4];
    int   border_color[4];
    short width, max_width, min_width;                     /* -1 — не задано */
};
/* стиль узла с номером id (из data-n) после dom_serialize; NULL — нет */
const struct dom_style *dom_style(int id);
extern int dom_view_width, dom_view_height;
void dom_set_viewport(int width, int height);
void dom_resize(void);                         /* dispatch resize after a window size change */
int dom_layout_children(int id);
int dom_layout_next(int parent, int previous);
int dom_layout_intrinsic(int id, int (*measure)(const char *,int,int));
int dom_module_scripts(void);

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
