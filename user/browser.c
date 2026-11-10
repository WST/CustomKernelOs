/* browser [адрес] — веб-браузер MyOS.
 *
 * Что умеет: HTTP (сам качает страницу через TCP/IP MyOS), разбирает HTML и
 * раскладывает текст по строкам: заголовки, абзацы, списки, ссылки, жирный,
 * <pre>, таблицы (упрощённо), горизонтальные линии, формы с полями ввода
 * (поиск работает). Переходы по ссылкам и редиректам, «Назад»/«Вперёд».
 * Картинки PNG/GIF/JPEG/BMP (user/lib/img.c) догружаются после показа страницы.
 *
 * HTTPS — свой, через user/lib/tls.c (TLS 1.3: X25519 + ChaCha20-Poly1305 или
 * AES-128-GCM), сертификат проверяется (user/lib/x509.c). Если сайт не умеет
 * TLS 1.3 (только 1.2 и старше), страница открывается через FrogFind — сервис
 * для старых компьютеров: он сам скачивает страницу и отдаёт её по HTTP.
 * Поиск — тоже через FrogFind.
 *
 * JavaScript — свой движок (user/lib/js*.c) и DOM (user/lib/dom*.c): страница
 * сначала разбирается в дерево, скрипты выполняются по ходу разбора и меняют
 * дерево, потом дерево превращается обратно в простой HTML для раскладки ниже.
 * После щелчков, набора текста и таймеров раскладка обновляется, если скрипты
 * что-то поменяли. Ctrl+J — включить/выключить JavaScript, Ctrl+K — консоль JS.
 *
 * CSS — user/lib/css.c: <style>, <link rel=stylesheet> (скачиваются), @import,
 * style="", каскад и наследование, var(), @media. Раскладка берёт из стиля
 * display, отступы, рамки и фон блоков, цвет, жирность, размер, выравнивание;
 * flex-строки ставит в ряд. Ctrl+U — выключить стили страницы.
 *
 * Шрифт без кириллицы, поэтому русский текст показывается латиницей (транслит).
 * Адрес file:/путь открывает HTML-файл с диска MyOS. */
#include "win.h"
#include "tls.h"
#include "dom.h"
#include "img.h"
#include "svg.h"

static int W = 800, H = 576;          /* окно можно растягивать */
#define BAR_H 76
#define NAV_Y 36
#define ADDR_X 154
#define ADDR_TEXT_X 184
#define STATUS_H 20
#define VIEW_Y BAR_H
#define VIEW_H (H - BAR_H - STATUS_H)
#define MARGIN 12
#define SB_W 12
#define MAX_PAGE (2048 * 1024)
#define HOME "about:home"
#define PROXY "http://frogfind.com/read.php?a="

static window *win;

/* ------------------------------------------------------------ память страницы
 * Всё, что нужно одной странице (слова, ссылки), берём из одного большого
 * куска и выбрасываем разом при переходе на другую. */

static char *arena;                /* текущий кусок; arena_used = 0 — выбросить всё */
static int arena_used, arena_cap;
static char *arena_more[15];       /* большой странице — ещё куски по 1 МБ (до 16 МБ) */
static int n_more, more_used;

static void *aalloc(int n) {
    n = (n + 3) & ~3;
    if (arena_used == 0 && n_more) {                 /* новая страница: лишние куски отдать */
        for (int i = 0; i < n_more; i++) free(arena_more[i]);
        n_more = 0;
    }
    if (n > arena_cap) return NULL;
    if (!n_more && arena_used + n <= arena_cap) { void *p = arena + arena_used; arena_used += n; return p; }
    if (!n_more || more_used + n > arena_cap) {
        if (n_more == 15) return NULL;
        char *c = malloc(arena_cap);
        if (!c) return NULL;
        arena_more[n_more++] = c;
        more_used = 0;
        if (!arena_used) arena_used = 4;              /* чтобы «arena_used = 0» значило сброс */
    }
    void *p = arena_more[n_more - 1] + more_used;
    more_used += n;
    return p;
}

static char *astrdup(const char *s, int n) {
    char *p = aalloc(n + 1);
    if (!p) return "";
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

/* ------------------------------------------------------------ элементы на странице */

enum { IT_TEXT, IT_HR, IT_BOX, IT_INPUT, IT_BUTTON, IT_BULLET, IT_FILL, IT_IMG };

struct item {
    int clip;
    int  x, y, w, h;
    unsigned char kind, scale, bold, underline, strike;
    unsigned char font, fsize;     /* IT_TEXT шрифтом TrueType: какой и какого размера (0 — моноширинный) */
    short below;                   /* IT_FILL: сколько уходит ниже строки (padding-bottom) */
    color_t color, bg;             /* bg — фон под текстом (TRANSPARENT — нет); у IT_FILL color — рамка */
    int  link;                     /* номер ссылки или -1 */
    int  input;                    /* IT_INPUT / IT_BUTTON: номер поля */
    int  node;                     /* элемент DOM, которому принадлежит (для щелчков в JS) */
    int  img;                      /* IT_IMG: номер в кэше картинок */
    char *text;
};

/* ------------------------------------------------------------ картинки
 * Кэш по адресу: сначала «в очереди», после загрузки — пиксели (уже уменьшенные, если
 * были огромными) и копия под размер на странице. Качаются после показа страницы,
 * по одной между событиями окна — страница не ждёт картинок. */
enum { IMG_QUEUED, IMG_OK, IMG_FAILED };
struct imgent {
    char *url;
    int state, w, h, alpha;
    unsigned *px;                  /* w x h, 0xAARRGGBB */
    unsigned *sc;                  /* под размер на странице: sw x sh (или == px) */
    int sw, sh;
    unsigned used;                 /* для выбрасывания давно не нужных */
    int page;                      /* номер страницы, на которой нужна */
};
#define IMG_N 400
static struct imgent imgs[IMG_N];
static int img_page;               /* растёт с каждой новой страницей */
static unsigned img_clock;
static int img_find(const char *u, bool create);

#define MAX_ITEMS 60000
#define MAX_LINKS 6000
#define MAX_INPUTS 200
#define MAX_FORMS 40

static struct item *items;
static int n_items;
static char *links[MAX_LINKS];
static int link_node[MAX_LINKS];           /* узел DOM ссылки (для событий JS) или -1 */
static int n_links;
static int doc_h, scroll;
static unsigned paint_epoch;
static int paint_damage_top,paint_damage_bottom;
static unsigned scroll_reuse_frames;

struct input { int form; char name[40]; char value[128]; char type; int item; int node; };   /* type: t текст, s кнопка, h скрытое */
static struct input inputs[MAX_INPUTS];
static int n_inputs, focus_input = -1;
static bool input_all;
static char form_action[MAX_FORMS][256];
static int form_node[MAX_FORMS];
static int n_forms, cur_form = -1;

static char page_title[80];
static bool loading;               /* идёт загрузка — кнопка R превращается в X */
static int nav_depth;              /* вложенность navigate (скрипт может перейти по адресу) */
static bool page_is_dom;           /* страница построена через DOM (а не простой текст) */
static char url[512];              /* адрес текущей страницы */
static char status[160];
static char hover_link[160];
static char addr[512];             /* текст в строке адреса */
static bool editing_addr;
static int addr_cur;
static bool addr_all;               /* весь адрес выделен: набор заменит его */
static int sel_item = -1, sel_byte, sel_end_item = -1, sel_end_byte;
static bool text_drag, text_dragged;
static int text_down_x, text_down_y;

static char *history[32];
static int n_hist, hist_pos = -1;

/* ------------------------------------------------------------ текст: UTF-8 -> наш шрифт */

static bool cp1251;                /* страница в кодировке windows-1251 */

/* символ Юникода -> UTF-8 (шрифты Ubuntu знают латиницу, кириллицу и знаки) */
static const char *map_cp(unsigned cp, char *tmp) {
    switch (cp) {
    case 0xA0: return " ";
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD: return "";
    }
    if (cp < 0x80) { tmp[0] = (char)cp; tmp[1] = 0; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | cp >> 6); tmp[1] = (char)(0x80 | (cp & 63)); tmp[2] = 0; }
    else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | cp >> 12); tmp[1] = (char)(0x80 | ((cp >> 6) & 63));
        tmp[2] = (char)(0x80 | (cp & 63)); tmp[3] = 0;
    } else return "?";
    return tmp;
}

/* &amp; &lt; &#1234; ... -> код символа (или -1) */
static int entity(const char *s, int n) {
    static const struct { const char *name; int cp; } ents[] = {
        { "amp", '&' }, { "lt", '<' }, { "gt", '>' }, { "quot", '"' }, { "apos", '\'' },
        { "nbsp", 0xA0 }, { "copy", 0xA9 }, { "reg", 0xAE }, { "laquo", 0xAB }, { "raquo", 0xBB },
        { "mdash", 0x2014 }, { "ndash", 0x2013 }, { "hellip", 0x2026 }, { "bull", 0x2022 },
        { "middot", 0xB7 }, { "lsquo", 0x2018 }, { "rsquo", 0x2019 }, { "ldquo", 0x201C },
        { "rdquo", 0x201D }, { "deg", 0xB0 }, { "rarr", 0x2192 }, { "larr", 0x2190 },
        { "euro", 0x20AC }, { "times", 'x' }, { "eacute", 0xE9 },
    };
    if (n > 1 && s[0] == '#') {
        int v = 0;
        if (s[1] == 'x' || s[1] == 'X') {
            for (int i = 2; i < n; i++) {
                char c = tolower(s[i]);
                v = v * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10);
            }
        } else {
            for (int i = 1; i < n; i++) v = v * 10 + (s[i] - '0');
        }
        return v;
    }
    for (int i = 0; i < (int)(sizeof(ents) / sizeof(ents[0])); i++)
        if ((int)strlen(ents[i].name) == n && !strncmp(ents[i].name, s, n)) return ents[i].cp;
    return -1;
}

/* перекодировать кусок текста страницы в «наши» байты; -> длина */
static int decode_text(const char *s, int n, char *out, int max) {
    int o = 0;
    char tmp[8];
    for (int i = 0; i < n && o < max - 4;) {
        unsigned char c = s[i];
        unsigned cp;
        if (c == '&') {
            int j = i + 1;
            while (j < n && j - i < 10 && s[j] != ';' && (isalnum(s[j]) || s[j] == '#')) j++;
            int cpv = (j < n && s[j] == ';') ? entity(s + i + 1, j - i - 1) : -1;
            if (cpv >= 0) { cp = cpv; i = j + 1; }
            else { cp = '&'; i++; }
        } else if (c < 0x80) {
            cp = c; i++;
        } else if (cp1251) {
            i++;
            if (c >= 0xC0) cp = 0x410 + (c - 0xC0);
            else if (c == 0xA8) cp = 0x401;
            else if (c == 0xB8) cp = 0x451;
            else if (c == 0xAB) cp = 0xAB;
            else if (c == 0xBB) cp = 0xBB;
            else if (c == 0x96 || c == 0x97) cp = 0x2014;
            else cp = '?';
        } else if ((c & 0xE0) == 0xC0 && i + 1 < n) {
            cp = ((c & 0x1F) << 6) | (s[i + 1] & 0x3F); i += 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < n) {
            cp = ((c & 0x0F) << 12) | ((s[i + 1] & 0x3F) << 6) | (s[i + 2] & 0x3F); i += 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < n) {
            cp = 0xFFFD; i += 4;
        } else {
            cp = '?'; i++;
        }
        const char *m = map_cp(cp, tmp);
        while (*m && o < max - 1) out[o++] = *m++;
    }
    out[o] = 0;
    return o;
}

/* ------------------------------------------------------------ раскладка (layout)
 *
 * Раскладка идёт по простому HTML из dom_serialize, а как выглядит каждый
 * элемент — берём из его вычисленного стиля CSS (dom_style по номеру data-n):
 * блок или строка, отступы, рамки и фон, цвет, жирность, размер, выравнивание.
 * Фон блоков рисуется прямоугольниками (boxes) под текстом, фон inline-block
 * (кнопки из ссылок) — элементом IT_FILL в той же строке. */

static int view_w(void) { return W - SB_W; }

static int lx, ly, line_h, line_below, line_start, lines_done;    /* курсор раскладки */
static int left, right, line_leading, layout_image_height, clip_box=-1;
static bool space_pending;
static int gap_pending;                  /* отступ перед следующим блоком */
static int cur_link = -1;
static int node_stack[256], node_sp;     /* открытые элементы DOM (номера из data-n) */
static int list_depth;
static int list_counter[8];
static bool in_title, in_skip;           /* <title>, <button>/<select>/<svg> */
static int inline_bg = -1;               /* фон текущего inline-элемента (<mark>, <code>...) */
static int page_bg;                      /* фон страницы (html/body) */
static int cur_bg = -1;                  /* фон под текущим текстом: -1 — фон страницы, -2 — картинка (неизвестно) */

static const struct dom_style *cs;       /* стиль текущего элемента */
static struct dom_style def_style, plain_style;

static void init_styles(void) {
    memset(&def_style, 0, sizeof(def_style));
    def_style.display = DS_INLINE;
    def_style.color = 0x141414;
    def_style.bg = -1;
    def_style.font_px = 16;
    def_style.width = def_style.max_width = def_style.height = -1;
    plain_style = def_style;
    plain_style.pre = 1;
}

/* прямоугольники фона и рамок блоков */
struct box { int x, y, w, h; int radius, clip, clip_height; int bg; short bw[4]; int bc[4]; };
#define MAX_BOXES 12000
static struct box *boxes;
static int n_boxes;

static struct item *add_item(int kind, int w, int h) {
    if (n_items >= MAX_ITEMS) return NULL;
    struct item *it = &items[n_items++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
    it->clip=clip_box;
    it->x = lx;
    it->y = ly;
    it->w = w;
    it->h = h;
    it->link = -1;
    it->input = -1;
    it->bg = TRANSPARENT;
    it->node = node_sp ? node_stack[node_sp - 1] : -1;
    if (h > line_h) line_h = h;
    return it;
}

static void finish_line(void) {
    if (lx == left && line_start == n_items) return;
    int asc = line_h ? line_h : 16, desc = line_below;
    int leading = line_leading > asc+desc ? line_leading-asc-desc : 0;
    int shift = 0;
    if (cs->align) {                                       /* text-align: center / right */
        int used = left;
        for (int i = line_start; i < n_items; i++) if (items[i].x + items[i].w > used) used = items[i].x + items[i].w;
        shift = cs->align == 1 ? (right - used) / 2 : right - used;
        if (shift < 0) shift = 0;
    }
    for (int i = line_start; i < n_items; i++) { items[i].y = ly + leading/2 + asc - items[i].h; items[i].x += shift; }
    ly += asc + desc + leading;
    line_leading = 0;
    lx = left;
    line_h = 0;
    line_below = 0;
    line_start = n_items;
    space_pending = false;
    lines_done++;
}

static void block(int gap) {
    finish_line();
    if (gap > gap_pending) gap_pending = gap;
}

static void apply_gap(void) {
    if (gap_pending && ly > 8) ly += gap_pending;
    gap_pending = 0;
}

static int cur_scale(void) { return cs->font_px >= 26 ? 2 : 1; }

static int luma(int c) { return (((c >> 16) & 255) * 299 + ((c >> 8) & 255) * 587 + (c & 255) * 114) / 1000; }

/* цвет текста; если он почти сливается с фоном (белый текст на картинке, которую
 * мы не показываем), — берём читаемый */
static color_t text_color(void) {
    int c = cs->color, bg = inline_bg >= 0 ? inline_bg : cur_bg == -1 ? (page_bg >= 0 ? page_bg : 0xffffff) : cur_bg;
    if (bg == -2) return (color_t)c;
    int d = luma(c) - luma(bg);
    if (d < 0) d = -d;
    if (d < 40) return luma(bg) > 128 ? RGB(20, 20, 20) : RGB(240, 240, 240);
    return (color_t)c;
}

/* шрифт и размер текущего текста (из CSS) */
static int cur_font(void) { return cs->bold ? FONT_UI_BOLD : FONT_UI; }
static int cur_size(void) {
    int px = cs->font_px;
    if (px < 9) px = 9;
    if (px > 72) px = 72;
    return px;
}

/* сколько байт занимает первый символ UTF-8 */
static int utf8_len1(const char *w) {
    const char *p = w;
    utf8_next(&p);
    return (int)(p - w);
}

static void place_word(const char *w, int n) {
    if (n <= 0) return;
    apply_gap();
    int font = cur_font(), size = cur_size();
    int width = text_width_n(font, size, w, n);
    int sw = text_width(font, size, " ");
    int space = space_pending && lx > left ? sw : 0;
    int h = size * 5 / 4 + 1;                              /* межстрочный интервал ~1.25 */
    if (lx + space + width > right && lx > left) { finish_line(); space = 0; }
    if (width > right - left) {                            /* слово длиннее строки — рубим */
        int fit = text_fit(font, size, w, right - lx);
        if (fit < 1) fit = utf8_len1(w);
        if (fit > n) fit = n;
        place_word(w, fit);
        space_pending = false;
        place_word(w + fit, n - fit);
        return;
    }
    if (space && inline_bg >= 0 && n_items > line_start && items[n_items - 1].bg == (color_t)inline_bg)
        items[n_items - 1].w += space;                     /* фон <mark> — и под пробелом */
    int leading = cs->line_height < 0 ? -cs->line_height * cs->font_px / 100 : cs->line_height;
    if (leading > line_leading) line_leading = leading;
    lx += space;
    struct item *it = add_item(IT_TEXT, width, h);
    if (!it) return;
    it->text = astrdup(w, n);
    if (cs->transform) {                                   /* text-transform */
        for (int i = 0; i < n; i++) {
            char c = it->text[i];
            if (cs->transform == 1 || (cs->transform == 3 && i == 0)) it->text[i] = toupper(c);
            else if (cs->transform == 2) it->text[i] = tolower(c);
        }
        width = text_width(font, size, it->text);
        it->w = width;
    }
    it->scale = 1;
    it->font = font;
    it->fsize = size;
    it->bold = cs->bold;
    it->link = cur_link;
    it->underline = cs->underline;
    it->strike = cs->strike;
    it->color = text_color();
    if (inline_bg >= 0) it->bg = inline_bg;
    lx += width;
    space_pending = false;
}

/* обычный текст (между тегами) */
static void add_text(const char *s, int n) {
    static char buf[8192];
    if (in_skip) return;
    if (in_title) {
        int l = decode_text(s, n, buf, sizeof(buf));
        int have = strlen(page_title);
        for (int i = 0; i < l && have < 79; i++) page_title[have++] = buf[i] == '\n' ? ' ' : buf[i];
        page_title[have] = 0;
        return;
    }
    if (cs->notext) return;                                /* font-size:0, text-indent:-9999px */
    int len = decode_text(s, n, buf, sizeof(buf));
    if (cs->pre) {                                         /* white-space: pre — пробелы и переводы строк как есть */
        int start = 0;
        for (int i = 0; i <= len; i++) {
            if (i == len || buf[i] == '\n') {
                if (i > start) {
                    apply_gap();
                    int w = (i - start) * 8;
                    if (lx + w > right && lx > left) finish_line();
                    struct item *it = add_item(IT_TEXT, w, 16);
                    if (it) {
                        it->text = astrdup(buf + start, i - start);
                        for (char *p = it->text; *p; p++) if (*p == '\t') *p = ' ';
                        it->scale = 1;
                        it->color = text_color();
                        it->bold = cs->bold;
                        it->link = cur_link;
                        it->underline = cs->underline;
                        if (inline_bg >= 0) it->bg = inline_bg;
                    }
                    lx += w;
                }
                if (i < len) { if (line_h < 16) line_h = 16; finish_line(); }
                start = i + 1;
            }
        }
        return;
    }
    int i = 0;
    while (i < len) {
        if (buf[i] == ' ' || buf[i] == '\n' || buf[i] == '\t' || buf[i] == '\r') { space_pending = true; i++; continue; }
        int j = i;
        while (j < len && buf[j] != ' ' && buf[j] != '\n' && buf[j] != '\t' && buf[j] != '\r') j++;
        place_word(buf + i, j - i);
        i = j;
    }
}

/* ------------------------------------------------------------ теги */

/* значение атрибута name="..." внутри тега */
static bool get_attr(const char *tag, int n, const char *name, char *out, int max) {
    int ln = strlen(name);
    for (int i = 0; i + ln < n; i++) {
        if (i > 0 && !isspace(tag[i - 1])) continue;
        bool same = true;
        for (int k = 0; k < ln; k++) if (tolower(tag[i + k]) != name[k]) { same = false; break; }
        if (!same) continue;
        int j = i + ln;
        while (j < n && isspace(tag[j])) j++;
        if (j >= n || tag[j] != '=') continue;
        j++;
        while (j < n && isspace(tag[j])) j++;
        char q = (j < n && (tag[j] == '"' || tag[j] == '\'')) ? tag[j++] : 0;
        int o = 0;
        while (j < n && (q ? tag[j] != q : (!isspace(tag[j]) && tag[j] != '>')) && o < max - 1) out[o++] = tag[j++];
        out[o] = 0;
        if (strchr(out, '&')) {                           /* &amp; в адресах */
            static char tmp[65536];
            decode_text(out, o, tmp, sizeof(tmp));
            snprintf(out, max, "%s", tmp);
        }
        return true;
    }
    out[0] = 0;
    return false;
}

static void resolve_url(const char *base, const char *rel, char *out, int max);

static int node_attr(const char *t, int n) {          /* data-n="номер узла DOM" */
    char v[16];
    return get_attr(t, n, "data-n", v, sizeof(v)) ? atoi(v) : -1;
}

static bool styled_button(const struct dom_style *s) {
    return dom_css && (s->flex_row || s->bg>=0 || s->padding[0] || s->padding[1] || s->padding[2] || s->padding[3]);
}

static void tag_input(const char *t, int n, bool button_tag) {
    char type[16], name[40], value[128];
    get_attr(t, n, "type", type, sizeof(type));
    get_attr(t, n, "name", name, sizeof(name));
    get_attr(t, n, "value", value, sizeof(value));
    for (char *p = type; *p; p++) *p = tolower(*p);
    if (button_tag && !type[0]) strcpy(type, "submit");
    if (n_inputs >= MAX_INPUTS) return;
    struct input *in = &inputs[n_inputs];
    memset(in, 0, sizeof(*in));
    in->form = cur_form;
    snprintf(in->name, sizeof(in->name), "%s", name);
    snprintf(in->value, sizeof(in->value), "%s", value);
    in->item = -1;
    in->node = node_attr(t, n);
    if (button_tag && styled_button(cs)) {in->type='s';n_inputs++;return;}
    if (!strcmp(type, "hidden")) { in->type = 'h'; n_inputs++; return; }
    if (!strcmp(type, "submit") || !strcmp(type, "button") || !strcmp(type, "image")) {
        in->type = 's';
        if (!in->value[0]) strcpy(in->value, button_tag ? "Submit" : "Submit");
        apply_gap();
        int w = strlen(in->value) * 8 + 20;
        if (lx + w + 8 > right && lx > left) finish_line();
        if (space_pending && lx > left) lx += 8;
        struct item *it = add_item(IT_BUTTON, w, 24);
        if (!it) return;
        it->input = n_inputs;
        in->item = it - items;
        lx += w;
        space_pending = true;
        n_inputs++;
        return;
    }
    if (!strcmp(type, "checkbox") || !strcmp(type, "radio")) { add_text("[ ] ", 4); return; }
    in->type = 't';
    char sz[8];
    int chars = get_attr(t, n, "size", sz, sizeof(sz)) ? atoi(sz) : 24;
    if (chars < 8) chars = 8;
    if (chars > 50) chars = 50;
    apply_gap();
    int w = chars * 8 + 8;
    if (lx + w + 8 > right && lx > left) finish_line();
    if (space_pending && lx > left) lx += 8;
    struct item *it = add_item(IT_INPUT, w, 24);
    if (!it) return;
    it->input = n_inputs;
    in->item = it - items;
    lx += w;
    space_pending = true;
    n_inputs++;
}

/* <img>: размер из width/height, CSS или самой картинки (если уже скачана) -> IT_IMG.
 * false — картинку не показать (нет адреса, не скачалась): пусть будет подпись [alt] */
static bool img_tag(const char *t, int n, const char *alt) {
    static char src[65536];                                 /* data:-адреса бывают длинными */
    if (!get_attr(t, n, "src", src, sizeof(src)) || !src[0] ||
        strstr(src, "blank.gif") || strstr(src, "spacer.gif") || strstr(src, "pixel.gif")) {
        /* «ленивые» картинки: настоящий адрес в data-src, в src заглушка */
        char alt2[2048];
        if (get_attr(t, n, "data-src", alt2, sizeof(alt2)) || get_attr(t, n, "data-original", alt2, sizeof(alt2)) ||
            get_attr(t, n, "data-lazy-src", alt2, sizeof(alt2))) snprintf(src, sizeof(src), "%s", alt2);
        else if (get_attr(t, n, "srcset", alt2, sizeof(alt2))) {           /* первый вариант из srcset */
            int k = 0;
            while (alt2[k] && alt2[k] != ' ' && alt2[k] != ',') k++;
            alt2[k] = 0;
            snprintf(src, sizeof(src), "%s", alt2);
        }
    }
    if (!src[0]) return false;
    char full[1024];
    const char *u = src;
    if (strncmp(src, "data:", 5)) { resolve_url(url, src, full, sizeof(full)); u = full; }
    int k = img_find(u, true);
    if (k < 0) return false;
    struct imgent *e = &imgs[k];
    e->page = img_page;
    if (e->state == IMG_FAILED) return false;
    char v[16];
    int aw = get_attr(t, n, "width", v, sizeof(v)) && !strchr(v, '%') ? atoi(v) : 0;
    int ah = get_attr(t, n, "height", v, sizeof(v)) && !strchr(v, '%') ? atoi(v) : 0;
    if (aw <= 0 || aw > 8000) aw = 0;
    if (ah <= 0 || ah > 8000) ah = 0;
    int id = node_attr(t, n);
    const struct dom_style *st = id >= 0 ? dom_style(id) : NULL;
    int avail = right - left;
    if (st && st->width > 0 && st->width < 8000) aw = st->width_pct ? avail * st->width / 100 : st->width;
    if(st && st->height>0) {
        int parent_height=layout_image_height;
        ah=st->height_pct?parent_height:st->height;
    }
    int w, h;
    if (aw && ah) { w = aw; h = ah; }
    else if (e->state == IMG_OK) {
        if (aw) { w = aw; h = aw * e->h / e->w; }
        else if (ah) { h = ah; w = ah * e->w / e->h; }
        else { w = e->w; h = e->h; }
    } else if (aw || ah) { w = aw ? aw : ah; h = ah ? ah : aw; }       /* пока не скачалась — квадрат */
    else { w = 24; h = 24; }                                           /* размер узнаем потом */
    bool guess = e->state != IMG_OK && !(aw && ah);
    if (w <= 2 && h <= 2) return true;                                 /* пиксель-счётчик: не показываем */
    int maxw = avail;
    if (st && st->max_width >= 0) { int mw = st->max_width_pct ? avail * st->max_width / 100 : st->max_width; if (mw > 0 && mw < maxw) maxw = mw; }
    if (w > maxw && maxw > 16) { h = h * maxw / w; w = maxw; }        /* max-width: 100% и просто узкое окно */
    if (h > 4000) h = 4000;
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    apply_gap();
    if (space_pending && lx > left) lx += 4;
    if (lx + w > right && lx > left) finish_line();
    struct item *it = add_item(IT_IMG, w, h);
    if (!it) return true;
    it->img = k;
    it->scale = guess;                                     /* размер угадан — после загрузки разложить заново */
    it->text = astrdup(alt, strlen(alt));
    it->link = cur_link;
    if (id >= 0) it->node = id;
    lx += w;
    space_pending = true;
    return true;
}

static void handle_tag(const char *t, int n) {
    bool close = n > 0 && t[0] == '/';
    if (close) { t++; n--; }
    char name[16];
    int k = 0;
    while (k < n && k < 15 && (isalnum(t[k]))) { name[k] = tolower(t[k]); k++; }
    name[k] = 0;
    if (!name[0]) return;
    #define IS(s) (!strcmp(name, s))

    if (IS("script") || IS("style") || IS("noscript") || IS("svg") || IS("iframe")) { in_skip = !close; return; }
    if (IS("title")) { in_title = !close; return; }
    if (IS("head")) return;
    if (IS("br")) { if (lx == left && line_start == n_items) ly += 16; else finish_line(); return; }
    if (IS("form")) {
        if (!close && n_forms < MAX_FORMS) {
            char action[256];
            get_attr(t, n, "action", action, sizeof(action));
            resolve_url(url, action[0] ? action : url, form_action[n_forms], 256);
            form_node[n_forms] = node_attr(t, n);
            cur_form = n_forms++;
        } else if (close) cur_form = -1;
        return;
    }
    if (IS("ul") || IS("ol") || IS("menu") || IS("dir")) {   /* счётчики для нумерации; отступ — из CSS */
        if (!close && list_depth < 8) {
            char st[12];
            list_counter[list_depth] = get_attr(t, n, "start", st, sizeof(st)) ? atoi(st) - 1 : 0;
            list_depth++;
        } else if (close && list_depth > 0) list_depth--;
        return;
    }
    if (IS("hr")) {
        block(6);
        apply_gap();
        lx = left;
        add_item(IT_HR, right - left, 10);
        finish_line();
        gap_pending = 4;
        return;
    }
    if (IS("a")) {
        if (close) { cur_link = -1; return; }
        char href[512];
        if (get_attr(t, n, "href", href, sizeof(href)) && n_links < MAX_LINKS && strncmp(href, "javascript:", 11)) {
            char full[512];
            resolve_url(url, href, full, sizeof(full));
            links[n_links] = astrdup(full, strlen(full));
            link_node[n_links] = node_attr(t, n);
            cur_link = n_links++;
        }
        return;
    }
    if (IS("img")) {
        if (close) return;
        char alt[80];
        get_attr(t, n, "alt", alt, sizeof(alt));
        if (img_tag(t, n, alt)) return;                     /* настоящая картинка */
        if (cs->notext && !alt[0]) return;
        char label[96];
        snprintf(label, sizeof(label), "[%s]", alt[0] ? alt : "image");
        int len = strlen(label);
        if (len > 40) { label[39] = ']'; label[40] = 0; len = 40; }
        apply_gap();
        int w = len * 8 + 6;
        if (space_pending && lx > left) lx += 8;
        if (lx + w > right && lx > left) finish_line();
        struct item *it = add_item(IT_BOX, w, 20);
        if (it) { it->text = astrdup(label, len); it->link = cur_link; }
        lx += w;
        space_pending = true;
        return;
    }
    if (IS("input")) { if (!close) tag_input(t, n, false); return; }
    if (IS("button")) { if (!close) { tag_input(t, n, true); in_skip = !styled_button(cs); } else in_skip = false; return; }
    if (IS("textarea")) { if (!close) { tag_input(t, n, false); in_skip = true; } else in_skip = false; return; }
    if (IS("select")) { if (!close) add_text("[...]", 5); in_skip = !close; return; }
    if (IS("meta")) {
        char cs2[64];
        if (get_attr(t, n, "charset", cs2, sizeof(cs2)) || get_attr(t, n, "content", cs2, sizeof(cs2)))
            for (char *p = cs2; *p; p++) if (!strncmp(p, "1251", 4)) cp1251 = true;
        return;
    }
    #undef IS
}

/* --- элементы и их стили */

enum { FK_INLINE, FK_BLOCK, FK_IBLOCK, FK_ROW, FK_CELL };

struct frame {
    const struct dom_style *prev, *s;
    int kind, left, right, box, inline_bg, cur_bg, item_start;
    int fill_x, fill_start, fill_lines, flex_item, promoted;
    int flex_x, flex_y, flex_bottom, flex_count, column;
    int id, box_start, content_y, fragment_start;
    int row_left, row_right, row_count, button_input;
    int outflow, flow_y, flow_gap, target_height, containing_right, containing_bottom, position_y;
    int old_clip;
    int sizing_avail, flex_fixed, flex_autos, flex_grow;
    double flex_weight;
};
static struct frame frames[256];
struct flex_fragment {
    struct frame *parent;
    int item_begin, item_end, box_begin, box_end, box, bottom, align, stretch;
};
static struct flex_fragment flex_fragments[MAX_ITEMS];
static int n_flex_fragments;


static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int vmargin(int m) { return m == CSS_AUTO ? 0 : clampi(m, 0, 64); }

static bool has_border(const struct dom_style *s) { return s->border[0] || s->border[1] || s->border[2] || s->border[3]; }

static int display_kind(const struct dom_style *s) {
    switch (s->display) {
    case DS_BLOCK: case DS_LIST_ITEM: case DS_FLEX: case DS_GRID: case DS_TABLE: return FK_BLOCK;
    case DS_INLINE_BLOCK: return FK_IBLOCK;
    case DS_ROW: return FK_ROW;
    case DS_CELL: return FK_CELL;
    default: return FK_INLINE;
    }
}

static int measure_intrinsic(const char *s,int size,int bold) {
    if(size<0) {
        char full[1024];resolve_url(url,s,full,sizeof(full));int k=img_find(full,false);
        return k>=0 && imgs[k].state==IMG_OK?imgs[k].w:24;
    }
    return text_width(bold?FONT_UI_BOLD:FONT_UI,clampi(size,9,72),s)+(*s?2:0);
}
/* Measure auto bases before distributing positive or negative free space. */
static int flex_width(struct frame *p, const struct dom_style *s, int id, int avail, int gap) {
    if (p->s->grid_columns) {
        int cols=p->s->grid_columns;
        int width=(avail-gap*(cols-1))/cols;
        return width>0?width:1;
    }
    int count = p->flex_count > 0 ? p->flex_count : 1;
    int usable = avail - gap * (count - 1);
    if (usable < 0) usable = 0;
    if (p->sizing_avail != avail) {
        p->flex_fixed = p->flex_autos = p->flex_grow = 0;
        p->flex_weight = 0;
        for (int id = dom_layout_next(p->id, -1); id >= 0; id = dom_layout_next(p->id, id)) {
            const struct dom_style *c = dom_style(id);
            int w = c->width<0?dom_layout_intrinsic(id,measure_intrinsic):
                (c->width_pct?avail*c->width/100:c->width)+(c->border_box?0:c->padding[1]+c->padding[3]+c->border[1]+c->border[3]);
            p->flex_fixed += w;
            p->flex_weight += (double)w*c->flex_shrink;
            p->flex_fixed += vmargin(c->margin[1]) + vmargin(c->margin[3]);
            p->flex_grow += c->flex_grow;
        }
        p->sizing_avail = avail;
    }
    int fixed = p->flex_fixed, grow = p->flex_grow;
    int width = s->width < 0 ? dom_layout_intrinsic(id,measure_intrinsic) : (s->width_pct ? avail*s->width/100 : s->width) +
        (s->border_box ? 0 : s->padding[1]+s->padding[3]+s->border[1]+s->border[3]);
    if (grow && usable > fixed) width += (usable-fixed)*s->flex_grow/grow;
    if (!p->s->flex_wrap && fixed > usable && s->flex_shrink) {
        double weight = p->flex_weight;
        if (weight > 0) width -= (int)((fixed-usable)*(double)width*s->flex_shrink/weight);
    }
    if (s->max_width >= 0) {
        int max = s->max_width_pct ? avail*s->max_width/100 : s->max_width;
        if (!s->border_box) max += s->padding[1]+s->padding[3]+s->border[1]+s->border[3];
        if (width > max) width = max;
    }
    return clampi(width, 1, avail > 0 ? avail : 1);
}

/* Finalize a flex line only after all its children have their final heights.
 * Move text, backgrounds and controls together so painting and hit testing agree. */
static void flex_finish_row(struct frame *p) {
    int count = 0;
    for (int i=p->fragment_start; i<n_flex_fragments; i++) if (flex_fragments[i].parent == p) count++;
    int free = p->row_right - p->flex_x + clampi(p->s->column_gap,0,(p->row_right-p->row_left)/2);
    if (free < 0) free = 0;
    int index = 0;
    for (int i=p->fragment_start; i<n_flex_fragments; i++) {
        struct flex_fragment *r = &flex_fragments[i];
        if (r->parent != p) continue;
        int dx = p->s->justify == 1 ? free/2 : p->s->justify == 2 ? free :
            p->s->justify == 3 && count>1 ? free*index/(count-1) :
            p->s->justify == 4 ? free*(2*index+1)/(2*count) :
            p->s->justify == 5 ? free*(index+1)/(count+1) : 0;
        int extra = p->flex_bottom-r->bottom;
        int dy = r->align == 1 ? extra/2 : r->align == 2 ? extra : 0;
        for (int k=r->item_begin;k<r->item_end;k++) { items[k].x+=dx; items[k].y+=dy; }
        for (int k=r->box_begin;k<r->box_end;k++) { boxes[k].x+=dx; boxes[k].y+=dy; }
        if (r->stretch && r->box >= 0 && extra > 0) boxes[r->box].h += extra;
        index++;
    }
    p->fragment_start=n_flex_fragments;
}

/* маркер пункта списка: •, ◦, ▪ или «3.» */
static void list_marker(const struct dom_style *s) {
    if (s->list == LS_NONE) return;
    apply_gap();
    int d = list_depth ? list_depth - 1 : 0;
    if (s->list == LS_DECIMAL || s->list == LS_ALPHA) {
        char num[16];
        int v = ++list_counter[d];
        if (s->list == LS_ALPHA) snprintf(num, sizeof(num), "%c.", 'a' + (v - 1) % 26);
        else snprintf(num, sizeof(num), "%d.", v);
        int size = s->font_px < 9 ? 9 : s->font_px > 72 ? 72 : s->font_px;
        int w = text_width(FONT_UI, size, num);
        lx = left - w - 6;
        struct item *it = add_item(IT_TEXT, w, size * 5 / 4 + 1);
        if (it) {
            it->text = astrdup(num, strlen(num)); it->scale = 1; it->color = s->color;
            it->font = FONT_UI; it->fsize = size;
        }
    } else {
        int size = s->font_px < 9 ? 9 : s->font_px > 72 ? 72 : s->font_px;
        lx = left - 14;
        struct item *it = add_item(IT_BULLET, 6, size * 5 / 4 + 1);
        if (it) { it->scale = s->list; it->color = s->color; }
    }
    lx = left;
    space_pending = false;
}

/* в inline-block оказался блок (карточка в flex-строке): дальше считаем его блоком —
 * с новой строки, с фоном и рамкой на всю ширину */
static void promote(struct frame *f) {
    for (struct frame *p = f - 1; p >= frames; p--) {
        if (!p->s || p->kind == FK_INLINE) continue;
        if (p->kind != FK_IBLOCK || p->promoted) return;
        p->promoted = 1;
        finish_line();
        const struct dom_style *ps = p->s;
        if ((ps->bg >= 0 || has_border(ps) || ps->bg_image) && n_boxes < MAX_BOXES) {
            apply_gap();
            struct box *b = &boxes[n_boxes];
            p->box = n_boxes++;
            b->x = left; b->y = ly; b->w = right - left; b->h = 0;
            b->bg = ps->bg; b->radius=ps->radius;b->clip=clip_box;b->clip_height=-1;
            for (int i = 0; i < 4; i++) { b->bw[i] = clampi(ps->border[i], 0, 8); b->bc[i] = ps->border_color[i]; }
            ly += clampi(ps->padding[0] + ps->border[0], 0, 120);
        }
        return;
    }
}

static void style_open(struct frame *f, const struct dom_style *s, const char *tag, int id) {
    f->button_input=!strcmp(tag,"button") && styled_button(s) && n_inputs<MAX_INPUTS?n_inputs:-1;
    f->id = id;
    f->box_start = n_boxes;
    f->old_clip=clip_box;
    f->prev = cs;
    f->s = s;
    f->left = left;
    f->right = right;
    f->box = -1;
    f->inline_bg = inline_bg;
    f->cur_bg = cur_bg;
    f->item_start = n_items;
    f->flex_item = cs->flex_row;
    f->promoted = 0;
    f->kind = display_kind(s);
    f->column = 0;
    int vertical=s->padding[0]+s->padding[2]+s->border[0]+s->border[2];
    int parent_height=f>frames?(f-1)->target_height:dom_view_height;
    f->target_height=s->height_pct?(parent_height*s->height/100):(s->height>=0?s->height:0);
    if(s->border_box) f->target_height-=vertical;
    int minimum=s->min_height-(s->border_box?vertical:0);
    if(f->target_height<minimum)f->target_height=minimum;
    if(f->target_height<0)f->target_height=0;
    f->outflow=s->fixed==1 || s->fixed==2;
    if(f->outflow) {
        finish_line();f->flow_y=ly;f->flow_gap=gap_pending;
        int cx=0,cy=0,cw=view_w(),ch=dom_view_height;
        if(s->fixed==1)for(struct frame *p=f-1;p>=frames;p--)if(p->s && p->s->fixed) {
            cx=p->row_left;cy=p->content_y;cw=p->row_right-p->row_left;ch=p->target_height;
            if(ch<1)ch=dom_view_height;
            break;
        }
        int offsets[4];for(int i=0;i<4;i++)offsets[i]=s->position_edge[i]==CSS_AUTO?CSS_AUTO:
            (s->position_pct&(1<<i))?s->position_edge[i]*(i%2?cw:ch)/100:s->position_edge[i];
        left=cx+(offsets[3]==CSS_AUTO?0:offsets[3]);
        right=cx+cw-(offsets[1]==CSS_AUTO?0:offsets[1]);
        ly=cy+(offsets[0]==CSS_AUTO?0:offsets[0]);lx=left;gap_pending=0;
        f->containing_right=right;f->containing_bottom=cy+ch-(offsets[2]==CSS_AUTO?0:offsets[2]);
        f->position_y=ly;
        if(s->height<0 && offsets[0]!=CSS_AUTO && offsets[2]!=CSS_AUTO) f->target_height=f->containing_bottom-ly;
        f->kind=FK_BLOCK;
    }
    if (cs->flex_row && f > frames && !f->outflow) {
        struct frame *p = f - 1;
        int avail = right - left;
        int gap = clampi(cs->column_gap, 0, avail / 2);
        int anonymous = lx > left && p->flex_x == left;
        if (anonymous) {
            p->flex_x = lx + gap;
            const struct dom_style *saved = cs;
            struct dom_style text_style = *cs;
            text_style.align = 0; cs = &text_style;
            finish_line(); cs = saved;
            if (ly > p->flex_bottom) p->flex_bottom = ly;
        } else finish_line();
        int count = p->flex_count > 0 ? p->flex_count : 1;
        int width = flex_width(p,s,id,avail,gap);
        if (anonymous && s->width < 0) width = clampi((right-p->flex_x)/count,1,avail);
        int margins = vmargin(s->margin[1]) + vmargin(s->margin[3]);
        if (cs->flex_wrap && p->flex_x > left && (p->flex_x + width + margins > right + 1 || (cs->grid_columns && p->row_count >= cs->grid_columns))) {
            flex_finish_row(p);
            p->row_count=0;
            p->flex_x = left;
            p->flex_y = p->flex_bottom + clampi(cs->row_gap, 0, 200);
        }
        left = p->flex_x;
        right = left + width + margins;
        lx = left; ly = p->flex_y;
        gap_pending = 0;
        p->flex_x = right + gap;
        p->row_count++;
        f->column = 1;
        f->kind = FK_BLOCK;
    }
    if(f->kind==FK_IBLOCK && s->flex_row) {
        int width=s->width>=0 && !s->width_pct?s->width:dom_layout_intrinsic(id,measure_intrinsic);
        if(width>0 && left+width<right)right=left+width;
        f->kind=FK_BLOCK;
    }
    if (f->kind == FK_BLOCK) promote(f);
    bool root = !strcmp(tag, "html") || !strcmp(tag, "body");
    switch (f->kind) {
    case FK_BLOCK: {
        block(vmargin(s->margin[0]));
        inline_bg = -1;
        int avail = right - left;
        int ml = s->margin[3] == CSS_AUTO ? 0 : clampi(s->margin[3], -left, 2000);
        int mr = s->margin[1] == CSS_AUTO ? 0 : clampi(s->margin[1], -(view_w() - right), 2000);
        int bx = left + ml, bxr = right - mr;
        int want = -1;                                     /* ширина: max-width всегда, width — у колонки по центру */
        if (s->width >= 0 && !f->column) {
            want = s->width_pct ? avail * s->width / 100 : s->width;
            if (!s->border_box) want += s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
        }
        if (s->max_width >= 0) {
            int mw = s->max_width_pct ? avail * s->max_width / 100 : s->max_width;
            if (!s->border_box) mw += s->padding[1] + s->padding[3] + s->border[1] + s->border[3];
            if (want < 0 || mw < want) want = mw;
        }
        if (want >= 0 && want < bxr - bx) {
            int extra = bxr - bx - want;
            if (s->margin[1] == CSS_AUTO && s->margin[3] == CSS_AUTO) bx += extra / 2;
            else if (s->margin[3] == CSS_AUTO) bx += extra;
            bxr = bx + want;
        }
        if (bxr < bx) bxr = bx;
        bool painted = s->bg >= 0 || has_border(s) || s->bg_image || s->clip_overflow;
        if (root && s->bg >= 0 && page_bg < 0) { page_bg = s->bg; painted = has_border(s); }   /* фон страницы */
        int pt = s->padding[0] + s->border[0];
        if (painted || pt || s->flex_row) apply_gap();
        if (painted && n_boxes < MAX_BOXES) {
            struct box *b = &boxes[n_boxes];
            f->box = n_boxes++;
            b->x = bx; b->y = ly; b->w = bxr - bx; b->h = 0;
            b->bg = root && s->bg == page_bg ? -1 : s->bg; b->radius=s->radius;b->clip=clip_box;
            b->clip_height=s->clip_overflow && f->target_height>0?f->target_height+s->padding[0]+s->padding[2]+s->border[0]+s->border[2]:-1;
            if(s->clip_overflow)clip_box=f->box;
            for (int i = 0; i < 4; i++) { b->bw[i] = clampi(s->border[i], 0, 8); b->bc[i] = s->border_color[i]; }
        }
        if (s->bg >= 0 && !(root && s->bg == page_bg)) cur_bg = s->bg;
        else if (s->bg_image) cur_bg = -2;
        ly += clampi(pt, 0, 120);
        left = bx + clampi(s->border[3] + s->padding[3], 0, 120);
        right = bxr - clampi(s->border[1] + s->padding[1], 0, 120);
        if (right - left < 60) { left = bx; right = bxr; }
        lx = left;
        if (s->display == DS_LIST_ITEM) list_marker(s);
        break;
    }
    case FK_ROW:
        finish_line();
        break;
    case FK_CELL:
        if (lx > left) {                                   /* колонки — по «табуляции» через 88 точек */
            int stop = left + ((lx - left + 12) / 88 + 1) * 88;
            lx = stop < right ? stop : lx + 16;
            space_pending = false;
        }
        break;
    case FK_IBLOCK: {
        int target=s->width>=0?(s->width_pct?(right-left)*s->width/100:s->width):dom_layout_intrinsic(id,measure_intrinsic);
        if(target>0 && lx+target<right)right=lx+target;
        int sc = cur_scale();
        if (space_pending && lx > left) lx += 8 * sc;
        space_pending = false;
        if (lx > left) lx += vmargin(s->margin[3]);
        if (lx > right - 16 && lx > left) finish_line();
        f->fill_x = lx;
        f->fill_start = n_items;
        f->fill_lines = lines_done;
        lx += clampi(s->border[3] + s->padding[3], 0, 64);
        inline_bg = -1;
        if (s->bg >= 0) cur_bg = s->bg;
        else if (s->bg_image) cur_bg = -2;
        break;
    }
    default:
        if (s->bg >= 0 && s->bg != page_bg) inline_bg = s->bg;
        break;
    }
    f->row_count=0;
    f->content_y = ly;
    f->sizing_avail = -1;
    f->fragment_start = n_flex_fragments;
    f->row_left = left; f->row_right = right;
    f->flex_x = left;
    f->flex_y = f->flex_bottom = ly;
    f->flex_count = s->flex_row ? dom_layout_children(id) : 0;
}

/* блок закрылся: высота его фона; картинка фона со светлым текстом поверх — тёмный фон */
static void box_done(struct frame *f) {
    struct box *b = &boxes[f->box];
    b->h = ly - b->y;
    if (b->h < 0) b->h = 0;
    if (f->s->bg_image && b->bg < 0) {
        int light = 0, dark = 0;
        for (int i = f->item_start; i < n_items; i++)
            if (items[i].kind == IT_TEXT && items[i].bg == TRANSPARENT) { if (luma(items[i].color) > 170) light++; else dark++; }
        if (light > dark) b->bg = RGB(58, 64, 72);
    }
}

static void style_close(struct frame *f) {
    const struct dom_style *s = f->s;
    switch (f->kind) {
    case FK_BLOCK: {
        finish_line();
        if (f->column) apply_gap(); /* child paragraph margins belong inside the flex item's height */
        int target = f->target_height;
        if (ly < f->content_y+target) ly = f->content_y+target;
        if(s->clip_overflow && s->height>=0)ly=f->content_y+target;
        if (s->flex_row) {
            if (ly > f->flex_bottom) f->flex_bottom=ly;
            flex_finish_row(f);
        }
        int pb = s->padding[2] + s->border[2];
        ly += clampi(pb, 0, 120);
        if (f->box >= 0) {
            box_done(f);
        }
        left = f->left;
        right = f->right;
        lx = left;
        block(vmargin(s->margin[2]));
        break;
    }
    case FK_ROW:
        finish_line();
        break;
    case FK_IBLOCK: {
        if (f->promoted) {
            finish_line();
            if (f->box >= 0) {
                ly += clampi(s->padding[2] + s->border[2], 0, 120);
                box_done(f);
            }
            block(vmargin(s->margin[2]));
            break;
        }
        lx += clampi(s->padding[1] + s->border[1], 0, 64);
        if ((s->bg >= 0 || has_border(s)) && f->fill_lines == lines_done) {   /* кнопка в одну строку — рисуем фон */
            int inner = 0;
            for (int i = f->fill_start; i < n_items; i++) if (items[i].h > inner) inner = items[i].h;
            if (!inner) inner = 16;
            int above = clampi(s->padding[0] + s->border[0], 0, 32), below = clampi(s->padding[2] + s->border[2], 0, 32);
            int save_lx = lx;
            struct item *it = add_item(IT_FILL, lx - f->fill_x, inner + above);
            if (it) {
                it->x = f->fill_x;
                it->below = below;
                it->bg = s->bg >= 0 ? (color_t)s->bg : TRANSPARENT;
                it->color = has_border(s) ? (color_t)(s->border_color[0] >= 0 ? s->border_color[0] : s->border_color[2]) : TRANSPARENT;
                it->link = cur_link;
                if (below > line_below) line_below = below;
            }
            lx = save_lx;
        }
        lx += vmargin(s->margin[1]);
        if (f->flex_item) space_pending = true;           /* элементы flex-строки не слипаются */
        break;
    }
    default:
        break;
    }
    if(f->button_input>=0 && n_items<MAX_ITEMS) {
        int x=0,y=0,w=0,h=0;
        if(f->box>=0) {struct box *b=&boxes[f->box];x=b->x;y=b->y;w=b->w;h=b->h;}
        else if(n_items>f->item_start) {
            x=items[f->item_start].x;y=items[f->item_start].y;
            int x2=x,y2=y;
            for(int i=f->item_start;i<n_items;i++) {struct item *it=&items[i];if(it->x<x)x=it->x;if(it->y<y)y=it->y;if(it->x+it->w>x2)x2=it->x+it->w;if(it->y+it->h>y2)y2=it->y+it->h;}
            w=x2-x;h=y2-y;
        }
        struct item *it=&items[n_items++];memset(it,0,sizeof(*it));
        it->kind=IT_BUTTON;it->fsize=255;it->x=x;it->y=y;it->w=w;it->h=h;it->input=f->button_input;it->clip=clip_box;
        it->link=-1;it->node=f->id;inputs[f->button_input].item=it-items;
    }
    inline_bg = f->inline_bg;
    cur_bg = f->cur_bg;
    if (f->column) {
        struct frame *p = f - 1;
        apply_gap();
        if (n_flex_fragments < MAX_ITEMS) {
            struct flex_fragment *r = &flex_fragments[n_flex_fragments++];
            r->parent=p; r->item_begin=f->item_start; r->item_end=n_items;
            r->box_begin=f->box_start; r->box_end=n_boxes; r->box=f->box;
            r->bottom=ly;
            r->align=s->align_self ? (s->align_self == 4 ? 0 : s->align_self) : p->s->align_items;
            r->stretch=!r->align && s->height < 0;
        }
        if (ly > p->flex_bottom) p->flex_bottom = ly;
        ly = p->flex_bottom;
        left = f->left; right = f->right; lx = left;
        gap_pending = 0;
        line_start = n_items;
    }
    if(f->outflow) {
        int x=f->box>=0?boxes[f->box].x:f->row_left,y=f->position_y;
        int w=f->box>=0?boxes[f->box].w:0,h=ly-y;
        if(!w)for(int i=f->item_start;i<n_items;i++)if(items[i].x+items[i].w-x>w)w=items[i].x+items[i].w-x;
        int dx=0,dy=0;
        if(s->position_edge[3]==CSS_AUTO && s->position_edge[1]!=CSS_AUTO)dx=f->containing_right-x-w;
        if(s->position_edge[0]==CSS_AUTO && s->position_edge[2]!=CSS_AUTO)dy=f->containing_bottom-y-h;
        dx+=(s->translate_pct&1)?s->translate_x*w/100:s->translate_x;
        dy+=(s->translate_pct&2)?s->translate_y*h/100:s->translate_y;
        for(int i=f->item_start;i<n_items;i++){items[i].x+=dx;items[i].y+=dy;}
        for(int i=f->box_start;i<n_boxes;i++){boxes[i].x+=dx;boxes[i].y+=dy;}
        left=f->left;right=f->right;lx=left;ly=f->flow_y;gap_pending=f->flow_gap;
        line_h=line_below=line_leading=0;line_start=n_items;space_pending=false;
    }
    if(f->kind==FK_IBLOCK) {left=f->left;right=f->right;}
    clip_box=f->old_clip;
}

/* разобрать HTML и разложить */
static void layout_html(const char *s, int n, bool plain) {
    paint_epoch++;
    sel_item = sel_end_item = -1; text_drag = false;
    input_all = false;
    n_flex_fragments = 0;
    clip_box=-1;
    n_items = 0; n_links = 0; n_inputs = 0; n_forms = 0; cur_form = -1; n_boxes = 0;
    focus_input = -1;
    left = MARGIN - 8; right = view_w() - MARGIN + 8;      /* ещё 8 точек — отступ <body> по умолчанию */
    lx = left; ly = 8; line_h = 0; line_below = 0; line_start = 0; lines_done = 0;
    line_leading = 0;
    space_pending = false; gap_pending = 0;
    cur_link = -1; list_depth = 0;
    in_title = in_skip = false;
    node_sp = 0;
    inline_bg = -1;
    page_bg = -1;
    cur_bg = -1;
    cs = plain ? &plain_style : &def_style;
    page_title[0] = 0;
    if (plain) { left = MARGIN; lx = left; add_text(s, n); finish_line(); doc_h = ly + 16; return; }
    int i = 0;
    while (i < n) {
        if (s[i] == '<') {
            if (i + 3 < n && s[i + 1] == '!' && s[i + 2] == '-' && s[i + 3] == '-') {      /* комментарий */
                int j = i + 4;
                while (j + 2 < n && !(s[j] == '-' && s[j + 1] == '-' && s[j + 2] == '>')) j++;
                i = j + 3;
                continue;
            }
            int j = i + 1;
            char q = 0;
            while (j < n && (q || s[j] != '>')) {
                if (!q && (s[j] == '"' || s[j] == '\'')) q = s[j];
                else if (q && s[j] == q) q = 0;
                j++;
            }
            if (s[i + 1] == '!' || s[i + 1] == '?') { i = j + 1; continue; }
            bool closing = s[i + 1] == '/';
            char nm[16];
            int k = 0, st = i + 1 + closing;
            while (st + k < j && k < 15 && isalnum(s[st + k])) { nm[k] = tolower(s[st + k]); k++; }
            nm[k] = 0;
            static const char *voids = " area base br col embed hr img input link meta param source track wbr keygen ";
            char key[20];
            snprintf(key, sizeof(key), " %s ", nm);
            bool is_void = strstr(voids, key) != NULL;
            if (closing) {
                handle_tag(s + i + 1, j - i - 1);
                if (node_sp > 0) {
                    struct frame *f = &frames[--node_sp];
                    if (f->s) { style_close(f); cs = f->prev; }
                }
            } else if (nm[0]) {
                int id = node_attr(s + i + 1, j - i - 1);
                const struct dom_style *ds = id >= 0 && !in_skip ? dom_style(id) : NULL;
                if (!is_void && node_sp < 256) {
                    struct frame *f = &frames[node_sp];
                    node_stack[node_sp] = id >= 0 ? id : node_sp ? node_stack[node_sp - 1] : -1;
                    node_sp++;
                    f->s = NULL;
                    if (ds) { style_open(f, ds, nm, id); cs = ds; }
                    if (!in_skip) handle_tag(s + i + 1, j - i - 1);
                } else if (!in_skip) {                     /* <img>, <input>, <br>... */
                    if(ds && (ds->fixed==1 || ds->fixed==2 || (cs->flex_row && (!strcmp(nm,"img") || !strcmp(nm,"input")))) && node_sp<256) {
                        struct frame *f=&frames[node_sp];node_stack[node_sp++]=id;
                        style_open(f,ds,nm,id);layout_image_height=f->target_height;cs=ds;handle_tag(s+i+1,j-i-1);style_close(f);cs=f->prev;node_sp--;
                        i=j+1;continue;
                    }
                    bool blk = ds && display_kind(ds) == FK_BLOCK && strcmp(nm, "hr");
                    if (blk) block(vmargin(ds->margin[0]));
                    handle_tag(s + i + 1, j - i - 1);
                    if (blk) block(vmargin(ds->margin[2]));
                }
            }
            i = j + 1;
            continue;
        }
        int j = i;
        while (j < n && s[j] != '<') j++;
        add_text(s + i, j - i);
        i = j;
    }
    while (node_sp > 0) { struct frame *f = &frames[--node_sp]; if (f->s) { style_close(f); cs = f->prev; } }
    finish_line();
    doc_h = ly + 16;
}

/* ------------------------------------------------------------ адреса */

struct url_parts { bool https, file, about; char host[128]; int port; char path[512]; };

static bool parse_url(const char *u, struct url_parts *p) {
    memset(p, 0, sizeof(*p));
    p->port = 80;
    if (!strncmp(u, "about:", 6)) { p->about = true; snprintf(p->path, sizeof(p->path), "%s", u + 6); return true; }
    if (!strncmp(u, "file:", 5)) {
        p->file = true;
        u += 5;
        while (u[0] == '/' && u[1] == '/') u++;
        snprintf(p->path, sizeof(p->path), "%s", u);
        return true;
    }
    if (!strncmp(u, "http://", 7)) u += 7;
    else if (!strncmp(u, "https://", 8)) { u += 8; p->https = true; p->port = 443; }
    int i = 0;
    while (*u && *u != '/' && *u != ':' && *u != '?' && *u != '#' && i < 127) p->host[i++] = *u++;
    p->host[i] = 0;
    if (!i) return false;
    if (*u == ':') { u++; p->port = atoi(u); while (isdigit(*u)) u++; }
    snprintf(p->path, sizeof(p->path), "%s%s", *u == '/' ? "" : "/", u);
    char *hash = strchr(p->path, '#');
    if (hash) *hash = 0;
    if (!p->path[0]) strcpy(p->path, "/");
    return true;
}

/* относительный адрес (как в href) -> полный */
static void resolve_url(const char *base, const char *rel, char *out, int max) {
    while (*rel == ' ') rel++;
    if (strstr(rel, "://") || !strncmp(rel, "about:", 6) || !strncmp(rel, "file:", 5) || !strncmp(rel, "mailto:", 7) ||
        !strncmp(rel, "myosjs:", 7)) {
        snprintf(out, max, "%s", rel);
        return;
    }
    struct url_parts b;
    if (!parse_url(base, &b)) { snprintf(out, max, "%s", rel); return; }
    char prefix[200];
    if (b.file) strcpy(prefix, "file:");
    else if (b.about) strcpy(prefix, "about:");
    else if (b.port != (b.https ? 443 : 80)) snprintf(prefix, sizeof(prefix), "%s://%s:%d", b.https ? "https" : "http", b.host, b.port);
    else snprintf(prefix, sizeof(prefix), "%s://%s", b.https ? "https" : "http", b.host);
    if (rel[0] == '/' && rel[1] == '/') { snprintf(out, max, "%s:%s", b.https ? "https" : "http", rel); return; }
    if (rel[0] == '/') { snprintf(out, max, "%s%s", prefix, rel); return; }
    if (rel[0] == '#') { snprintf(out, max, "%s%s", prefix, b.path); return; }
    if (rel[0] == '?') {
        char path[512];
        snprintf(path, sizeof(path), "%s", b.path);
        char *q = strchr(path, '?');
        if (q) *q = 0;
        snprintf(out, max, "%s%s%s", prefix, path, rel);
        return;
    }
    /* относительно папки текущей страницы, с ./ и ../ */
    char path[1024];
    snprintf(path, sizeof(path), "%s", b.path);
    char *q = strchr(path, '?');
    if (q) *q = 0;
    char *slash = strrchr(path, '/');
    if (slash) slash[1] = 0; else strcpy(path, "/");
    for (;;) {
        if (!strncmp(rel, "./", 2)) { rel += 2; continue; }
        if (!strncmp(rel, "../", 3)) {
            rel += 3;
            int l = strlen(path);
            if (l > 1) {
                path[l - 1] = 0;
                char *s2 = strrchr(path, '/');
                if (s2) s2[1] = 0; else strcpy(path, "/");
            }
            continue;
        }
        break;
    }
    int l = strlen(path);
    snprintf(path + l, sizeof(path) - l, "%s", rel);
    snprintf(out, max, "%s%s", prefix, path);
}

static void url_encode(const char *s, char *out, int max) {
    int o = 0;
    static const char hex[] = "0123456789ABCDEF";
    for (; *s && o < max - 4; s++) {
        unsigned char c = *s;
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out[o++] = c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    out[o] = 0;
}

/* ------------------------------------------------------------ рисование */

static bool u8cont(char c) { return ((unsigned char)c & 0xC0) == 0x80; }
static int u8count(const char *s, int bytes) { int k = 0; for (int i = 0; i < bytes && s[i]; i++) if (!u8cont(s[i])) k++; return k; }

/* Navigation geometry is shared by painting and mouse hit testing. */
static void nav_icon(int x, int kind, bool enabled) {
    color_t c = enabled ? RGB(66, 77, 98) : RGB(184, 192, 205);
    int cy = NAV_Y + 14;
    if (kind < 2) {
        int d = kind ? -1 : 1, cx = x + 14;
        gfx_line(cx - 6*d, cy, cx + 6*d, cy, c);
        gfx_line(cx - 6*d, cy, cx, cy - 6, c);
        gfx_line(cx - 6*d, cy, cx, cy + 6, c);
    } else if (kind == 2) {
        if (loading) {
            gfx_line(x+9, cy-5, x+19, cy+5, c);
            gfx_line(x+9, cy+5, x+19, cy-5, c);
        } else {
            gfx_circle(x+14, cy, 6, c);
            gfx_fill_rect(x+18, cy-8, 5, 7, RGB(248,250,253));
            gfx_line(x+19, cy-6, x+19, cy, c);
            gfx_line(x+19, cy, x+13, cy, c);
        }
    } else {
        gfx_line(x+7, cy, x+14, cy-7, c);
        gfx_line(x+14, cy-7, x+21, cy, c);
        gfx_line(x+9, cy-2, x+9, cy+7, c);
        gfx_line(x+9, cy+7, x+19, cy+7, c);
        gfx_line(x+19, cy+7, x+19, cy-2, c);
    }
}

static void draw_toolbar(void) {
    gfx_fill_rect(0, 0, W, BAR_H, RGB(248,250,253));
    gfx_fill_rect(0, 0, W, 29, RGB(231,237,246));
    gfx_fill_circle(19, 14, 7, RGB(73,112,226));
    gfx_circle(19, 14, 4, C_WHITE);
    ui_text(34, 6, "MyOS", RGB(54,73,110));
    char title[100];
    int title_chars = (W - 126) / 8;
    if (title_chars > 72) title_chars = 72;
    snprintf(title, sizeof(title), "%.*s", title_chars, page_title[0] ? page_title : "Browser");
    ui_text(100, 6, title, RGB(91,105,128));
    gfx_fill_rect(0, BAR_H - 1, W, 1, RGB(220,226,236));
    nav_icon(8, 0, hist_pos > 0);
    nav_icon(42, 1, hist_pos + 1 < n_hist);
    nav_icon(76, 2, true);
    nav_icon(110, 3, true);
    gfx_round_frame(ADDR_X, NAV_Y-2, W-ADDR_X-12, 32, 14,
                    editing_addr ? RGB(91,128,234) : RGB(226,232,242),
                    editing_addr ? C_WHITE : RGB(236,241,248));
    /* A neutral site-information mark; it does not imply certificate trust. */
    gfx_circle(ADDR_X+15, NAV_Y+14, 6, RGB(112,128,152));
    gfx_text(ADDR_X+12, NAV_Y+6, "i", RGB(112,128,152), TRANSPARENT);
    static char shown[1100];
    int maxc = (W - ADDR_TEXT_X - 24) / 8;
    if (maxc < 1) maxc = 1;
    int ccur = u8count(addr, addr_cur), skip = 0, start = 0;
    if (editing_addr && ccur > maxc - 1) skip = ccur - maxc + 1;
    while (skip > 0 && addr[start]) { start++; while (u8cont(addr[start])) start++; skip--; }
    int n = 0, k = 0;
    for (int i = start; addr[i] && n < 1090; i++) {
        if (!u8cont(addr[i]) && k++ == maxc) break;
        shown[n++] = addr[i];
    }
    shown[n] = 0;
    if (editing_addr && addr_all && addr[0]) {
        gfx_fill_rect(ADDR_TEXT_X-1, NAV_Y+5, u8count(shown, n)*8+2, 18, RGB(65,105,220));
        gfx_text(ADDR_TEXT_X, NAV_Y+6, shown, C_WHITE, TRANSPARENT);
    } else gfx_text(ADDR_TEXT_X, NAV_Y+6, shown, RGB(61,77,103), TRANSPARENT);
    if (editing_addr && !addr_all)
        gfx_fill_rect(ADDR_TEXT_X+(ccur-u8count(addr,start))*8, NAV_Y+6, 1, 16, RGB(65,105,220));
}

static void draw_status(void) {
    gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, RGB(248, 250, 253));
    gfx_fill_rect(0, H - STATUS_H, W, 1, RGB(226, 232, 242));
    char s[100];
    snprintf(s, sizeof(s), "%.96s", hover_link[0] ? hover_link : status);
    gfx_text(6, H - STATUS_H + 2, s, UI_DARK, TRANSPARENT);
}

static void draw_img(struct item *it, int y);
static void set_status(const char *s);

static bool page_selection(void) {
    return sel_item >= 0 && sel_end_item >= 0 &&
        (sel_item != sel_end_item || sel_byte != sel_end_byte);
}
static void page_bounds(int *i0, int *b0, int *i1, int *b1) {
    bool forward = sel_item < sel_end_item || (sel_item == sel_end_item && sel_byte <= sel_end_byte);
    *i0 = forward ? sel_item : sel_end_item; *b0 = forward ? sel_byte : sel_end_byte;
    *i1 = forward ? sel_end_item : sel_item; *b1 = forward ? sel_end_byte : sel_byte;
}
static int item_text_width(struct item *it, int bytes) {
    if (it->fsize) return text_width_n(it->font, it->fsize, it->text, bytes);
    return u8count(it->text, bytes) * 8 * (it->scale > 1 ? it->scale : 1);
}
static void copy_page_selection(void) {
    if (!page_selection()) return;
    int i0, b0, i1, b1, n = 0, previous = -1;
    page_bounds(&i0, &b0, &i1, &b1);
    char *s = malloc(CLIPBOARD_MAX);
    if (!s) return;
    for (int i = i0; i <= i1; i++) {
        struct item *it = &items[i];
        if (it->kind != IT_TEXT || !it->text) continue;
        int start = i == i0 ? b0 : 0, end = i == i1 ? b1 : strlen(it->text);
        if (end <= start) continue;
        bool newline = previous >= 0 && (it->y >= items[previous].y + items[previous].h || it->y + it->h <= items[previous].y);
        int gap = previous >= 0 && (newline || it->x > items[previous].x + item_text_width(&items[previous], strlen(items[previous].text)));
        if (n + gap + end - start > CLIPBOARD_MAX) { free(s); set_status("Selection exceeds 64 KiB"); return; }
        if (gap) s[n++] = newline ? '\n' : ' ';
        memcpy(s + n, it->text + start, end - start); n += end - start; previous = i;
    }
    clipboard_set(s, n); free(s);
}

static void page_clip_bounds(int clip,int *x0,int *y0,int *x1,int *y1) {
    *x0=0;*y0=VIEW_Y;*x1=view_w();*y1=VIEW_Y+VIEW_H;
    if(paint_damage_bottom>paint_damage_top) {
        if(paint_damage_top>*y0)*y0=paint_damage_top;
        if(paint_damage_bottom<*y1)*y1=paint_damage_bottom;
    }
    for(int depth=0;clip>=0 && clip<n_boxes && depth<256;depth++) {
        struct box *b=&boxes[clip];int y=b->y-scroll+VIEW_Y,h=b->clip_height>=0?b->clip_height:b->h;
        if(b->x>*x0)*x0=b->x;
        if(y>*y0)*y0=y;
        if(b->x+b->w<*x1)*x1=b->x+b->w;
        if(y+h<*y1)*y1=y+h;
        clip=b->clip;
    }
    if(*x1<*x0)*x1=*x0;
    if(*y1<*y0)*y1=*y0;
}
static void page_clip(int clip) {
    int x0,y0,x1,y1;page_clip_bounds(clip,&x0,&y0,&x1,&y1);gfx_set_clip(x0,y0,x1-x0,y1-y0);
}
static int page_clip_contains(int clip,int x,int y) {
    int x0,y0,x1,y1;page_clip_bounds(clip,&x0,&y0,&x1,&y1);return x>=x0 && x<x1 && y>=y0 && y<y1;
}
static void draw_page(void) {
    int dirty_top=paint_damage_bottom>paint_damage_top?paint_damage_top:VIEW_Y;
    int dirty_bottom=paint_damage_bottom>paint_damage_top?paint_damage_bottom:VIEW_Y+VIEW_H;
    page_clip(-1);
    gfx_fill_rect(0, VIEW_Y, W, VIEW_H, page_bg >= 0 ? (color_t)page_bg : C_WHITE);
    if (page_is_dom && !n_items && dom_module_scripts() && dom_last_error()[0]) {
        gfx_fill_rect(12, VIEW_Y + 12, W - 36, 124, RGB(245, 245, 245));
        gfx_text(24, VIEW_Y + 24, "The module application could not be started.", UI_DARK, TRANSPARENT);
        gfx_text(24, VIEW_Y + 48, "A script failed to load, link or execute.", UI_DARK, TRANSPARENT);
        gfx_text(24, VIEW_Y + 72, "The page loaded, but its content was not built.", UI_DARK, TRANSPARENT);
        gfx_text(24, VIEW_Y + 96, "Ctrl+K: details. Use the Back button to return.", UI_DARK, TRANSPARENT);
    }
    for (int i = 0; i < n_boxes; i++) {                      /* фон и рамки блоков */
        struct box *b = &boxes[i];
        page_clip(b->clip);
        int y = b->y - scroll + VIEW_Y;
        if (y + b->h < dirty_top || y > dirty_bottom || b->h <= 0) continue;
        if (b->bg >= 0) {
            int radius=clampi(b->radius,0,(b->w<b->h?b->w:b->h)/2);
            if (radius) gfx_round_rect(b->x,y,b->w,b->h,radius,b->bg);
            else gfx_fill_rect(b->x,y,b->w,b->h,b->bg);
        }
        if (b->bw[0]) gfx_fill_rect(b->x, y, b->w, b->bw[0], b->bc[0]);
        if (b->bw[2]) gfx_fill_rect(b->x, y + b->h - b->bw[2], b->w, b->bw[2], b->bc[2]);
        if (b->bw[3]) gfx_fill_rect(b->x, y, b->bw[3], b->h, b->bc[3]);
        if (b->bw[1]) gfx_fill_rect(b->x + b->w - b->bw[1], y, b->bw[1], b->h, b->bc[1]);
    }
    for (int i = n_items - 1; i >= 0; i--) {                 /* фон inline-block (внешние раньше внутренних) */
        struct item *it = &items[i];
        if (it->kind != IT_FILL) continue;
        page_clip(it->clip);
        int y = it->y - scroll + VIEW_Y, h = it->h + it->below;
        if (y + h < dirty_top - 32 || y > dirty_bottom) continue;
        if (it->bg != TRANSPARENT) gfx_fill_rect(it->x, y, it->w, h, it->bg);
        if (it->color != TRANSPARENT) gfx_rect(it->x, y, it->w, h, it->color);
    }
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        page_clip(it->clip);
        int y = it->y - scroll + VIEW_Y;
        if (y + it->h < dirty_top - 32 || y > dirty_bottom) continue;
        switch (it->kind) {
        case IT_TEXT:
            if (it->bg != TRANSPARENT) gfx_fill_rect(it->x, y, it->w, it->h, it->bg);
            if (page_selection()) {
                int i0, b0, i1, b1;
                page_bounds(&i0, &b0, &i1, &b1);
                if (i >= i0 && i <= i1) {
                    int start = i == i0 ? b0 : 0, end = i == i1 ? b1 : strlen(it->text);
                    int x0 = item_text_width(it, start), x1 = item_text_width(it, end);
                    if (x1 > x0) gfx_fill_rect(it->x + x0, y, x1 - x0, it->h, UI_SELECT);
                }
            }
            if (it->fsize) {                                 /* шрифт Ubuntu нужного размера */
                int lh = text_line_h(it->font, it->fsize), ty = y + (it->h - lh) / 2;
                int base = ty + text_ascent(it->font, it->fsize);
                int th = it->fsize >= 24 ? 2 : 1;
                text_draw(it->font, it->fsize, it->x, ty, it->text, it->color);
                if (it->underline) gfx_fill_rect(it->x, base + 1 + it->fsize / 10, it->w, th, it->color);
                if (it->strike) gfx_fill_rect(it->x, base - it->fsize * 3 / 10, it->w, th, it->color);
                break;
            }
            if (it->scale > 1) {
                gfx_text_scaled(it->x, y, it->text, it->color, it->scale);
                if (it->bold) gfx_text_scaled(it->x + 1, y, it->text, it->color, it->scale);
            } else {
                gfx_text(it->x, y, it->text, it->color, TRANSPARENT);
                if (it->bold) gfx_text(it->x + 1, y, it->text, it->color, TRANSPARENT);
            }
            if (it->underline) gfx_fill_rect(it->x, y + it->h - 2, it->w, it->scale, it->color);
            if (it->strike) gfx_fill_rect(it->x, y + it->h / 2, it->w, it->scale, it->color);
            break;
        case IT_BULLET:
            if (it->scale == LS_CIRCLE) {
                gfx_ellipse(it->x, y + it->h / 2 - 3, 6, 6, it->color, 255);
                gfx_ellipse(it->x + 1, y + it->h / 2 - 2, 4, 4, page_bg >= 0 ? (color_t)page_bg : C_WHITE, 255);
            } else gfx_ellipse(it->x, y + it->h / 2 - 3, 6, 6, it->color, 255);
            break;
        case IT_HR:
            gfx_fill_rect(it->x, y + 4, it->w, 1, RGB(170, 170, 170));
            gfx_fill_rect(it->x, y + 5, it->w, 1, RGB(230, 230, 230));
            break;
        case IT_IMG:
            draw_img(it, y);
            break;
        case IT_BOX:
            gfx_fill_rect(it->x, y, it->w, it->h, RGB(245, 245, 235));
            gfx_rect(it->x, y, it->w, it->h, it->link >= 0 ? RGB(0, 0, 238) : RGB(180, 180, 170));
            gfx_text(it->x + 3, y + 2, it->text, it->link >= 0 ? RGB(0, 0, 238) : RGB(120, 120, 110), TRANSPARENT);
            break;
        case IT_INPUT: {
            struct input *in = &inputs[it->input];
            ui_sunken(it->x, y, it->w, it->h, C_WHITE);
            if (focus_input == it->input) gfx_rect(it->x, y, it->w, it->h, UI_ACCENT);
            int maxc = (it->w - 8) / 8, l = u8count(in->value, strlen(in->value)), skip = l > maxc - 1 ? l - maxc + 1 : 0;
            const char *v = in->value;                    /* в буквах UTF-8: видны последние maxc */
            while (skip > 0 && *v) { v++; while (u8cont(*v)) v++; skip--; }
            if (focus_input == it->input && input_all) gfx_fill_rect(it->x + 3, y + 3, it->w - 6, it->h - 6, UI_SELECT);
            gfx_text(it->x + 4, y + 4, v, UI_TEXT, TRANSPARENT);
            if (focus_input == it->input) gfx_fill_rect(it->x + 4 + u8count(v, strlen(v)) * 8, y + 4, 2, 16, UI_TEXT);
            break;
        }
        case IT_BUTTON:
            if (it->fsize!=255) ui_button(it->x, y, it->w, it->h, inputs[it->input].value, false);
            break;
        }
    }
    gfx_no_clip();
    if (doc_h > VIEW_H) {                                    /* полоса прокрутки */
        int kh = VIEW_H * VIEW_H / doc_h;
        if (kh < 24) kh = 24;
        int ky = VIEW_Y + (VIEW_H - kh) * scroll / (doc_h - VIEW_H);
        gfx_fill_rect(W - SB_W, VIEW_Y, SB_W, VIEW_H, RGB(238, 238, 238));
        gfx_fill_rect(W - SB_W + 2, ky, SB_W - 4, kh, RGB(180, 180, 180));
    }
}

static struct {
    unsigned epoch;
    int valid,w,h,scroll,focus,selection,editing,loading,dirty;
    color_t *pixels;
} painted;

static void redraw(void) {
    int delta=scroll-painted.scroll,step=delta<0?-delta:delta;
    int reuse=painted.valid && painted.pixels==win->pixels && painted.w==W && painted.h==H &&
        painted.epoch==paint_epoch && step>0 && step<VIEW_H &&
        focus_input<0 && painted.focus<0 && !page_selection() && !painted.selection &&
        !editing_addr && !painted.editing && !loading && !painted.loading && !dom_dirty && !painted.dirty;
    if(reuse) {
        /* Shared window pixels remain the backing store. Copy the overlap by
         * words, then rasterize only the newly exposed strip. The scrollbar is
         * repainted separately by draw_page, while the browser chrome stays put. */
        color_t *base=win->pixels+VIEW_Y*W;
        int count=(VIEW_H-step)*W;
        if(delta>0) {
            color_t *src=base+step*W;
            for(int i=0;i<count;i++)base[i]=src[i];
            paint_damage_top=VIEW_Y+VIEW_H-step;paint_damage_bottom=VIEW_Y+VIEW_H;
        } else {
            color_t *dst=base+step*W;
            for(int i=count;i>0;i--)dst[i-1]=base[i-1];
            paint_damage_top=VIEW_Y;paint_damage_bottom=VIEW_Y+step;
        }
        draw_page();paint_damage_top=paint_damage_bottom=0;
        win_update_rect(win,0,VIEW_Y,W,VIEW_H);
        scroll_reuse_frames++;
    } else {
        draw_page();draw_toolbar();draw_status();win_update(win);
    }
    painted.valid=1;painted.pixels=win->pixels;painted.w=W;painted.h=H;painted.scroll=scroll;
    painted.epoch=paint_epoch;painted.focus=focus_input;painted.selection=page_selection();
    painted.editing=editing_addr;painted.loading=loading;painted.dirty=dom_dirty;
}

static bool verbose;                /* browser -v: строки статуса ещё и в stdout (видно на COM1) */
static unsigned t_start;

static void set_status(const char *s) {
    snprintf(status, sizeof(status), "%s", s);
    if (verbose) printf("[browser %u ms] %s\n", ticks() - t_start, s);
    hover_link[0] = 0;
    draw_status();
    win_update_rect(win, 0, H - STATUS_H, W, STATUS_H);
}

/* ------------------------------------------------------------ загрузка */

static char *body;                 /* содержимое последнего ответа */
static int body_len;
static tls *conn;                  /* открытое HTTPS-соединение (иначе NULL) */
static char tls_err[96];           /* почему не вышло HTTPS */
static char secure[120];           /* "TLS 1.3, ChaCha20-Poly1305, certificate OK (Let's Encrypt)" для статуса */
static char cert_problem[160];     /* сертификат не прошёл проверку: почему (для страницы-предупреждения) */
static char cert_host[128];        /* ...у какого сайта */
static char trusted[16][128];      /* сайты, которые человек велел открыть, несмотря на сертификат */
static int n_trusted;

static bool host_trusted(const char *h) {
    for (int i = 0; i < n_trusted; i++) if (!strcmp(trusted[i], h)) return true;
    return false;
}

static unsigned req_total_ms = 60000, req_idle_lim = 20000;   /* сроки запроса (страница / скрипты) */
static const char *req_method = "GET";   /* метод и тело следующего запроса (XMLHttpRequest/fetch из JS) */
static const char *req_accept = "text/html, text/plain, */*";
static const char *req_body;
static int req_body_len;
static char resp_ctype[96];        /* Content-Type последнего ответа */

#define USER_AGENT "Mozilla/5.0 (MyOS; i386) MyOS-Browser/0.19"

/* консоль JavaScript: последние строки console.log и ошибок (about:console, Ctrl+K) */
static char console_buf[12288];
static int console_len;

static void console_add(const char *s, int n) {
    if (n > (int)sizeof(console_buf) / 2) { s += n - (int)sizeof(console_buf) / 2; n = sizeof(console_buf) / 2; }
    if (console_len + n > (int)sizeof(console_buf)) {         /* выбросить старое */
        int drop = console_len + n - sizeof(console_buf);
        while (drop < console_len && console_buf[drop] != '\n') drop++;
        if (drop < console_len) drop++;
        memmove(console_buf, console_buf + drop, console_len - drop);
        console_len -= drop;
    }
    memcpy(console_buf + console_len, s, n);
    console_len += n;
}

static int console_page(char *out, int max) {
    int o = snprintf(out, max, "<title>JavaScript console</title><h1>JavaScript console</h1>"
                     "<p>Output of console.log() and script errors of the last page "
                     "(<a href=\"about:js\">try JavaScript</a>, <a href=\"about:home\">home</a>).</p><hr><pre>");
    for (int i = 0; i < console_len && o < max - 16; i++) {
        char c = console_buf[i];
        if (c == '<') o += snprintf(out + o, max - o, "&lt;");
        else if (c == '&') o += snprintf(out + o, max - o, "&amp;");
        else out[o++] = c;
    }
    if (!console_len) o += snprintf(out + o, max - o, "(empty)");
    o += snprintf(out + o, max - o, "</pre>");
    return o;
}

/* ---- ожидание сети без «зависания» окна ----
 * У сокета тайм-аут чтения 100 мс (sockctl). Каждый раз, когда данных нет, зовётся
 * net_idle(): она смотрит события окна (Esc и кнопка «X» — остановить, крестик — выйти,
 * растягивание — запомнить размер) и следит за сроками: сервер молчит дольше idle_ms
 * или весь запрос идёт дольше total — бросаем. */
static bool stop_req;              /* Esc / «X»: остановить загрузку страницы */
static bool resized_while_busy;    /* окно растянули, пока были заняты: потом разложить заново */
static unsigned req_last_data, req_idle_ms, req_deadline;
static char req_fail[64];          /* почему запрос оборвался */
static void draw_toolbar(void);
static void redraw(void);

/* события, пришедшие, пока работал долгий скрипт (не во время загрузки): главный цикл их потом обработает */
static struct event later[16];
static int n_later;

static void busy_events(void) {
    static unsigned last;
    if (ticks() - last < 50) return;
    last = ticks();
    struct event e;
    for (int k = 0; k < 16 && win_event(&e, 0); k++) {
        if (e.type == EV_TIMEOUT) break;
        if (e.type == EV_CLOSE || e.type == EV_QUIT) { win_destroy(win); exit(0); }
        if (e.type == EV_KEY && e.key == 27) stop_req = true;
        else if (e.type == EV_MOUSE_DOWN && e.y < BAR_H && ui_inside(e.x, e.y, 76, NAV_Y, 28, 28) && loading) stop_req = true;
        else if (!loading && e.type != EV_RESIZE && e.type != EV_MOUSE_MOVE && n_later < 16) later[n_later++] = e;
        if (e.type == EV_RESIZE) {                            /* буфер уже новый — нарисовать, что есть */
            W = e.x; H = e.y;
            resized_while_busy = true;
            redraw();
        }
    }
}

static int net_idle(void) {
    busy_events();
    unsigned now = ticks();
    if (stop_req) { snprintf(req_fail, sizeof(req_fail), "stopped"); return 0; }
    if (now - req_last_data > req_idle_ms) { snprintf(req_fail, sizeof(req_fail), "server does not answer"); return 0; }
    if ((int)(now - req_deadline) > 0) { snprintf(req_fail, sizeof(req_fail), "took too long"); return 0; }
    return 1;
}

/* JS-движок зовёт это примерно раз в 100 мс, пока работает скрипт: 1 — прервать скрипт */
static int js_busy_poll(void) {
    busy_events();
    return stop_req;
}

static int net_read(int fd, void *b, int n) {
    for (;;) {
        int r = conn ? tls_read(conn, b, n) : read(fd, b, n);
        if (!conn && r < 0) r = -errno; /* read is POSIX; this layer uses -errno */
        if (r > 0) { req_last_data = ticks(); return r; }
        if (r == -ETIMEDOUT && !conn) { if (net_idle()) continue; return -ETIMEDOUT; }
        if (r < 0 && req_fail[0]) return -ETIMEDOUT;            /* tls_read: net_idle сказала «хватит» */
        return r;
    }
}
static int net_write(int fd, const void *b, int n) { return conn ? tls_write(conn, b, n) : write(fd, b, n); }

/* реклама и счётчики: тяжёлые скрипты, которые ничего не показывают */
static bool blocked_host(const char *u) {
    static const char *bad[] = {
        "google-analytics.com", "googletagmanager.com", "googletagservices.com", "doubleclick.net",
        "googlesyndication.com", "googleadservices.com", "adservice.google.", "pagead2.", "connect.facebook.net",
        "mc.yandex.ru", "an.yandex.ru", "yandex.ru/ads", "top-fwz1.mail.ru", "counter.yadro.ru", "ad.mail.ru",
        "scorecardresearch.com", "quantserve.com", "chartbeat.", "hotjar.com", "criteo.", "taboola.com",
        "outbrain.com", "amazon-adsystem.com", "adnxs.com", "moatads.com", "js-agent.newrelic.com",
        "bam.nr-data.net", "cdn.segment.com", "mixpanel.com", "adfox.", "adriver.ru", "tns-counter.ru",
        "smartadserver.", "pubmatic.com", "rubiconproject.com", "openx.net", "yieldmo.", "clarity.ms",
        "static.cloudflareinsights.com", "sentry-cdn.com", "browser.sentry-cdn",
    };
    const char *h = strstr(u, "://");
    if (!h) return false;
    h += 3;
    char host[160];
    int n = 0;
    while (h[n] && h[n] != '/' && h[n] != '?' && h[n] != '#' && n < 159) { host[n] = tolower(h[n]); n++; }
    host[n] = 0;
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) if (strstr(host, bad[i]) || (strchr(bad[i], '/') && strstr(u, bad[i]))) return true;
    return false;
}

/* разобрать Transfer-Encoding: chunked на месте; -> новая длина */
static bool response_header(const char *line, const char *name) {
    while (*name && *line && tolower((unsigned char)*line) == *name) { line++; name++; }
    return !*name && *line == ':';
}

/* A complete chunked response need not close its TCP connection. */
static int chunks_complete(const char *b, int len) {
    int r = 0;
    while (r < len) {
        unsigned size = 0;
        int digits = 0;
        while (r < len) {
            char c = tolower((unsigned char)b[r]);
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) break;
            if (size > MAX_PAGE / 16) return -1;
            size = size * 16 + d; r++; digits++;
        }
        if (r == len) return 0;
        if (!digits) return -1;
        if (b[r] == ';') { while (r < len && b[r] != '\r') r++; }
        if (r + 1 >= len) return 0;
        if (b[r] != '\r' || b[r + 1] != '\n') return -1;
        r += 2;
        if (!size) {             /* optional trailers, then an empty line */
            while (r + 1 < len) {
                if (b[r] == '\r' && b[r + 1] == '\n') return 1;
                while (r + 1 < len && !(b[r] == '\r' && b[r + 1] == '\n')) r++;
                r += 2;
            }
            return 0;
        }
        if (size > (unsigned)(len - r)) return 0;
        r += size;
        if (r + 1 >= len) return 0;
        if (b[r] != '\r' || b[r + 1] != '\n') return -1;
        r += 2;
    }
    return 0;
}

static int dechunk(char *b, int len) {
    int r = 0, w = 0;
    while (r < len) {
        unsigned size = 0;
        int digits = 0;
        while (r < len) {
            char c = tolower(b[r]);
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (d < 0) break;
            if (size > 0x0FFFFFFF) return w;                   /* чепуха вместо размера */
            size = size * 16 + d; r++; digits++;
        }
        if (!digits) return w;                                  /* не похоже на кусок — что есть */
        while (r < len && b[r] != '\n') r++;                    /* ";расширения" и \r\n */
        r++;
        if (size == 0 || r >= len) break;
        if (size > (unsigned)(len - r)) size = len - r;         /* оборвалось на середине */
        memmove(b + w, b + r, size);
        w += size; r += size;
        if (r < len && b[r] == '\r') r++;
        if (r < len && b[r] == '\n') r++;
    }
    return w;
}

static unsigned response_crc(const unsigned char *p, int n) {
    unsigned crc = ~0u;
    for (int i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static unsigned response_le32(const unsigned char *p) {
    return (unsigned)p[0] | (unsigned)p[1] << 8 | (unsigned)p[2] << 16 | (unsigned)p[3] << 24;
}

/* Servers may compress HTML, scripts and styles even for HTTP/1.0 clients. */
static int decode_response(const char *encoding) {
    if (!encoding[0] || !strcmp(encoding, "identity")) return 0;
    unsigned char *in = (unsigned char *)body;
    int gzip = !strcmp(encoding, "gzip") || !strcmp(encoding, "x-gzip");
    int deflate = !strcmp(encoding, "deflate");
    if (!gzip && !deflate) { set_status("Unsupported HTTP Content-Encoding"); return -1; }
    int start = 0, end = body_len;
    if (gzip) {
        if (end < 18 || in[0] != 31 || in[1] != 139 || in[2] != 8 || (in[3] & 0xe0)) goto bad;
        int flags = in[3];
        start = 10; end -= 8;
        if (flags & 4) {
            if (start + 2 > end) goto bad;
            int extra = in[start] | in[start + 1] << 8;
            start += 2 + extra;
            if (start > end) goto bad;
        }
        for (int flag = 8; flag <= 16; flag <<= 1) if (flags & flag) {
            while (start < end && in[start]) start++;
            if (start >= end) goto bad;
            start++;
        }
        if (flags & 2) {
            if (start + 2 > end || (response_crc(in, start) & 65535) != (unsigned)(in[start] | in[start + 1] << 8)) goto bad;
            start += 2;
        }
        if (response_le32(in + end + 4) > MAX_PAGE) goto bad;
    } else if (end < 6) goto bad;
    if (start >= end) goto bad;
    unsigned char *out = malloc(MAX_PAGE + 1);
    if (!out) { set_status("Not enough memory to decompress page"); return -1; }
    int len = img_inflate(in + start, end - start, out, MAX_PAGE, deflate);
    int valid = len >= 0;
    if (valid && gzip) valid = response_le32(in + end + 4) == (unsigned)len && response_le32(in + end) == response_crc(out, len);
    if (valid && deflate) {
        unsigned a = 1, b = 0;
        for (int i = 0; i < len; i++) { a = (a + out[i]) % 65521; b = (b + a) % 65521; }
        unsigned expected = (unsigned)in[end - 4] << 24 | (unsigned)in[end - 3] << 16 | (unsigned)in[end - 2] << 8 | in[end - 1];
        valid = expected == (b << 16 | a);
    }
    if (valid) { memcpy(body, out, len); body_len = len; body[len] = 0; }
    free(out);
    if (valid) return 0;
bad:
    set_status("Invalid, truncated or oversized compressed response");
    return -1;
}

static const char *home_page =
    "<title>Новая страница</title><style>"
    "body { background:#f8fafd; color:#273750; font-family:sans-serif; padding:20px; }"
    "main { max-width:520px; margin:0 auto; padding-top:62px; text-align:center; }"
    "h1 { font-size:44px; color:#466bda; margin-bottom:32px; text-align:center; }"
    ".search { padding:14px 0; margin-bottom:22px; text-align:center; }"
    "a { color:#74839b; text-decoration:none; }"
    ".links { text-align:center; color:#bac4d3; font-size:14px; }"
    ".hint { color:#a0acbf; font-size:12px; text-align:center; margin-top:48px; }"
    "@media (max-width:600px) { main { padding-top:48px; } h1 { font-size:38px; } }"
    "</style><main><h1>MyOS</h1><div class=\"search\"><center><form action=\"http://frogfind.com/\">"
    "<input name=\"q\" size=\"24\" placeholder=\"Поиск в интернете\"><input type=\"submit\" value=\"Найти\">"
    "</form></center></div><p class=\"links\"><a href=\"https://ru.wikipedia.org/\">Википедия</a> &middot; "
    "<a href=\"http://68k.news/\">Новости</a> &middot; <a href=\"file:/\">Мои файлы</a></p>"
    "<p class=\"hint\">Ctrl+L — ввести адрес</p></main>";

static const char *js_page =
    "<title>JavaScript in MyOS</title><h1>JavaScript in MyOS</h1>"
    "<p>Everything below is made by scripts running in the MyOS JavaScript engine.</p>"
    "<h2>Counter</h2><p><button id=\"minus\">-</button> <b id=\"count\">0</b> <button id=\"plus\">+</button></p>"
    "<h2>Clock</h2><p id=\"clock\">...</p>"
    "<h2>Calculator</h2><form id=\"calc\"><input id=\"expr\" size=\"30\" value=\"2 ** 10 + Math.sqrt(16)\"> "
    "<input type=\"submit\" value=\"=\"></form><p id=\"result\"></p>"
    "<h2>Type here</h2><p><input id=\"typing\" size=\"30\"></p><p>Reversed: <span id=\"rev\"></span></p>"
    "<h2>Primes</h2><p id=\"primes\"></p>"
    "<h2>Lists</h2><ul id=\"list\"></ul><p><a href=\"#\" id=\"add\">add an item</a> | <a href=\"#\" id=\"sort\">sort</a></p>"
    "<p><a href=\"about:console\">JavaScript console</a> - <a href=\"about:home\">home</a></p>"
    "<script>\n"
    "var n = 0, count = document.getElementById('count');\n"
    "document.getElementById('plus').onclick = function () { count.textContent = ++n; };\n"
    "document.getElementById('minus').addEventListener('click', () => { count.textContent = --n; });\n"
    "function tick() { document.getElementById('clock').textContent = new Date().toLocaleTimeString(); }\n"
    "tick(); setInterval(tick, 1000);\n"
    "document.getElementById('calc').addEventListener('submit', function (e) {\n"
    "  e.preventDefault();\n"
    "  var out = document.getElementById('result');\n"
    "  try { out.textContent = '= ' + eval(document.getElementById('expr').value); }\n"
    "  catch (err) { out.textContent = err.name + ': ' + err.message; }\n"
    "});\n"
    "document.getElementById('typing').addEventListener('input', function () {\n"
    "  document.getElementById('rev').textContent = this.value.split('').reverse().join('');\n"
    "});\n"
    "var primes = [];\n"
    "for (var i = 2; primes.length < 25; i++) if (primes.every(p => i % p)) primes.push(i);\n"
    "document.getElementById('primes').textContent = primes.join(', ');\n"
    "var fruits = ['pear', 'apple', 'plum'];\n"
    "function draw() { document.getElementById('list').innerHTML = fruits.map(f => `<li>${f}</li>`).join(''); }\n"
    "draw();\n"
    "var more = ['kiwi', 'banana', 'cherry', 'orange', 'lime'];\n"
    "document.getElementById('add').onclick = function (e) { e.preventDefault(); fruits.push(more[fruits.length % more.length]); draw(); };\n"
    "document.getElementById('sort').onclick = function (e) { e.preventDefault(); fruits.sort(); draw(); };\n"
    "console.log('about:js is ready');\n"
    "</script>";

static int http_get(const char *u, char *location, int loc_max, bool *is_html) {
#ifdef BROWSER_HOST
    extern int browser_host_fetch(const char *, char *, int, bool *);
    int host_result = browser_host_fetch(u, location, loc_max, is_html);
    if (host_result != -999) return host_result;
#endif
    struct url_parts p;
    if (!parse_url(u, &p)) return -1;
    *is_html = true;
    location[0] = 0;
    body_len = 0;
    resp_ctype[0] = 0;
    if (p.about) {
        if (!strcmp(p.path, "console")) { body_len = console_page(body, MAX_PAGE); return 200; }
        if (!strncmp(p.path, "trust?", 6)) {                /* «всё равно открыть» сайт с плохим сертификатом */
            struct url_parts t;
            if (parse_url(p.path + 6, &t) && t.host[0] && !host_trusted(t.host)) {
                snprintf(trusted[n_trusted % 16], sizeof(trusted[0]), "%s", t.host);
                if (n_trusted < 16) n_trusted++;
            }
            snprintf(location, loc_max, "%s", p.path + 6);
            return 302;
        }
        const char *pg = !strcmp(p.path, "js") ? js_page : home_page;
        int n = strlen(pg);
        memcpy(body, pg, n);
        body_len = n;
        return 200;
    }
    if (p.file) {                                           /* файл или папка на диске */
        struct dirent_info st;
        char path[512];
        snprintf(path, sizeof(path), "%s", p.path[0] ? p.path : "/");
        if (dirstat(path, &st) < 0) { set_status("file not found"); return 404; }
        if (st.type == 2) {
            int o = snprintf(body, MAX_PAGE, "<title>%s</title><h1>Index of %s</h1><ul>", path, path);
            struct dirent_info e;
            for (int i = 0; dirread(path, i, &e) == 1 && o < MAX_PAGE - 512; i++) {
                if (!strcmp(e.name, ".")) continue;
                o += snprintf(body + o, MAX_PAGE - o, "<li><a href=\"file:%s%s%s%s\">%s%s</a> %s</li>",
                              path, path[strlen(path) - 1] == '/' ? "" : "/", e.name, e.type == 2 ? "/" : "",
                              e.name, e.type == 2 ? "/" : "", "");
            }
            o += snprintf(body + o, MAX_PAGE - o, "</ul>");
            body_len = o;
            return 200;
        }
        int fd = open(path, O_READ);
        if (fd < 0) return 404;
        int n;
        while (body_len < MAX_PAGE && (n = read(fd, body + body_len, MAX_PAGE - body_len)) > 0) body_len += n;
        close(fd);
        int l = strlen(path);
        *is_html = (l > 5 && !strcmp(path + l - 5, ".html")) || (l > 4 && !strcmp(path + l - 4, ".htm"));
        return 200;
    }
    char msg[160];
    if (stop_req) { set_status("Stopped"); return -1; }
    snprintf(msg, sizeof(msg), "Connecting to %s...", p.host);
    set_status(msg);
    req_fail[0] = 0;
    int fd = tcp_connect_timeout(p.host, p.port, 10000);
    if (fd < 0) {
        snprintf(msg, sizeof(msg), "%s: %s", p.host, fd == -ENOENT ? "host not found" :
                 fd == -ETIMEDOUT ? "no answer (timed out)" : strerror(-fd));
        set_status(msg);
        return -1;
    }
    sockctl(fd, SOCK_RCVTIMEO, 100);                        /* ждём по 100 мс, между ними — net_idle */
    req_last_data = ticks();
    req_deadline = ticks() + req_total_ms;
    req_idle_ms = req_idle_lim;
    tls_idle = net_idle;
    secure[0] = 0;
    if (p.https) {                                          /* HTTPS: рукопожатие TLS 1.3 */
        snprintf(msg, sizeof(msg), "TLS handshake with %s...", p.host);
        set_status(msg);
        conn = tls_open(fd, p.host, tls_err, sizeof(tls_err));
        if (!conn) {
            close(fd);
            if (req_fail[0]) { snprintf(msg, sizeof(msg), "%s: %s", p.host, req_fail); set_status(msg); return -1; }
            return -2;
        }
        if (tls_cert_ok(conn)) snprintf(secure, sizeof(secure), "%s, certificate OK (%.40s)", tls_info(conn), tls_cert_issuer(conn));
        else if (host_trusted(p.host)) snprintf(secure, sizeof(secure), "%s, CERTIFICATE NOT TRUSTED (opened anyway)", tls_info(conn));
        else {                                              /* не проверен — не отдаём ни байта */
            snprintf(cert_problem, sizeof(cert_problem), "%s", tls_cert_error(conn));
            snprintf(cert_host, sizeof(cert_host), "%s", p.host);
            tls_close(conn); conn = NULL; tls_idle = NULL;
            close(fd);
            snprintf(msg, sizeof(msg), "%s: certificate problem - %s", p.host, cert_problem);
            set_status(msg);
            return -3;
        }
    }
    char cookies[2048], req[4096];
    cookie_get(p.host, cookies, sizeof(cookies));
    int rl = snprintf(req, sizeof(req),
        "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
        "Accept: %s\r\nAccept-Encoding: gzip, deflate\r\nAccept-Language: ru, en\r\nConnection: close\r\n",
        req_method, p.path, p.host, req_accept);
    if (cookies[0] && rl < (int)sizeof(req)) rl += snprintf(req + rl, sizeof(req) - rl, "Cookie: %s\r\n", cookies);
    if (req_body && rl < (int)sizeof(req)) rl += snprintf(req + rl, sizeof(req) - rl, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n", req_body_len);
    if (rl < (int)sizeof(req)) rl += snprintf(req + rl, sizeof(req) - rl, "\r\n");
    if (rl >= (int)sizeof(req)) rl = sizeof(req) - 1;
    net_write(fd, req, rl);
    if (req_body && req_body_len) net_write(fd, req_body, req_body_len);
    int n, total = 0, expect = -1;                       /* expect: где кончится ответ по Content-Length */
    int chunk_start = -1;
    unsigned last = ticks();
    bool cut = false;                                     /* оборвали по тайм-ауту или Esc */
    while (total < MAX_PAGE) {
        n = net_read(fd, body + total, MAX_PAGE - total);
        if (n == -ETIMEDOUT) { cut = true; break; }
        if (n <= 0) break;
        total += n;
        if (expect < 0) {                                   /* заголовки пришли? узнаем длину */
            body[total] = 0;
            char *he = strstr(body, "\r\n\r\n");
            if (he) {
                expect = 0;
                for (char *p2 = body; p2 < he;) {
                    if (response_header(p2, "content-length")) {
                        int cl = atoi(p2 + 15);
                        if (cl >= 0 && cl <= MAX_PAGE) expect = (he - body) + 4 + cl;
                    }
                    if (response_header(p2, "transfer-encoding")) {
                        char value[64]; int k = 0;
                        const char *v = p2 + 18;
                        while (v < he && *v != '\r' && k < 63) value[k++] = tolower((unsigned char)*v++);
                        value[k] = 0;
                        if (strstr(value, "chunked")) chunk_start = (he - body) + 4;
                    }
                    char *next = strstr(p2, "\r\n");
                    if (!next) break;
                    p2 = next + 2;
                }
                if (chunk_start >= 0) expect = 0;          /* transfer coding overrides Content-Length */
                /* что-то пришло — дальше ждём не дольше, чем позволяет «молчание» сервера */
            }
        }
        if (expect > 0 && total >= expect) break;           /* всё получили — не ждём закрытия */
        if (chunk_start >= 0 && chunks_complete(body + chunk_start, total - chunk_start) == 1) break;
        if (ticks() - last > 300) {
            last = ticks();
            snprintf(msg, sizeof(msg), "Reading %s... %d KB%s", p.host, total / 1024, nav_depth ? " (Esc - stop)" : "");
            set_status(msg);
        }
    }
    if (conn) { tls_close(conn); conn = NULL; }
    tls_idle = NULL;
    close(fd);
    if (total == MAX_PAGE && (expect <= 0 || expect > total)) {
        set_status("Response exceeds browser resource size limit");
        return -1;
    }
    if (cut) {
        snprintf(msg, sizeof(msg), "%s: %s", p.host, req_fail[0] ? req_fail : "timed out");
        set_status(msg);
    }
    /* заголовки */
    int hend = -1;
    for (int i = 0; i + 3 < total; i++)
        if (body[i] == '\r' && body[i + 1] == '\n' && body[i + 2] == '\r' && body[i + 3] == '\n') { hend = i; break; }
    if (hend < 0) { if (!cut) set_status(total ? "bad response" : "empty response"); return -1; }
    body[hend] = 0;
    int code = 0;
    char *sp = strchr(body, ' ');
    if (sp) code = atoi(sp + 1);
    if (code <= 0) code = 200;
    bool chunked = false;
    char encoding[64] = "";
    cp1251 = false;
    for (char *line = body; line && *line;) {
        char *eol = strstr(line, "\r\n");
        if (eol) *eol = 0;
        char low[64];
        int k = 0;
        while (line[k] && line[k] != ':' && k < 63) { low[k] = tolower(line[k]); k++; }
        low[k] = 0;
        char *val = line[k] == ':' ? line + k + 1 : NULL;
        while (val && *val == ' ') val++;
        if (val && !strcmp(low, "location")) snprintf(location, loc_max, "%s", val);
        if (val && !strcmp(low, "set-cookie")) cookie_set(p.host, val);
        if (val && !strcmp(low, "content-type")) {
            snprintf(resp_ctype, sizeof(resp_ctype), "%s", val);
            *is_html = strstr(val, "html") != NULL;
            if (strstr(val, "1251")) cp1251 = true;
            if (strstr(val, "image/")) { *is_html = true; }
        }
        if (val && !strcmp(low, "transfer-encoding")) {
            char transfer[64]; snprintf(transfer, sizeof(transfer), "%s", val);
            for (char *q = transfer; *q; q++) *q = tolower((unsigned char)*q);
            if (strstr(transfer, "chunked")) chunked = true;
        }
        if (val && !strcmp(low, "content-encoding")) {
            snprintf(encoding, sizeof(encoding), "%s", val);
            for (char *q = encoding; *q; q++) *q = tolower(*q);
            int len = strlen(encoding);
            while (len && (encoding[len - 1] == ' ' || encoding[len - 1] == '\t')) encoding[--len] = 0;
        }
        line = eol ? eol + 2 : NULL;
    }
    int start = hend + 4;
    memmove(body, body + start, total - start);
    body_len = total - start;
    if (chunked) body_len = dechunk(body, body_len);
    if (decode_response(encoding) < 0) return -1;
    if (cut && (code < 200 || code >= 300 || !nav_depth || !body_len)) return -1;   /* обрывок годится только как страница */
    return code;
}

static void navigate(const char *target, bool add_history);
static char page_status[160];

/* windows-1251 -> UTF-8 (DOM и скрипты работают с UTF-8) */
static void cp1251_to_utf8(void) {
    static const unsigned short hi[64] = {   /* 0x80..0xBF */
        0x402, 0x403, 0x201A, 0x453, 0x201E, 0x2026, 0x2020, 0x2021, 0x20AC, 0x2030, 0x409, 0x2039, 0x40A, 0x40C, 0x40B, 0x40F,
        0x452, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014, '?', 0x2122, 0x459, 0x203A, 0x45A, 0x45C, 0x45B, 0x45F,
        0xA0, 0x40E, 0x45E, 0x408, 0xA4, 0x490, 0xA6, 0xA7, 0x401, 0xA9, 0x404, 0xAB, 0xAC, 0xAD, 0xAE, 0x407,
        0xB0, 0xB1, 0x406, 0x456, 0x491, 0xB5, 0xB6, 0xB7, 0x451, 0x2116, 0x454, 0xBB, 0x458, 0x405, 0x455, 0x457 };
    char *out = malloc(body_len * 2 + 1);
    if (!out) return;
    int o = 0;
    for (int i = 0; i < body_len; i++) {
        unsigned char c = body[i];
        unsigned cp = c < 0x80 ? c : c >= 0xC0 ? 0x410 + (c - 0xC0) : hi[c - 0x80];
        if (cp < 0x80) out[o++] = cp;
        else if (cp < 0x800) { out[o++] = 0xC0 | (cp >> 6); out[o++] = 0x80 | (cp & 63); }
        else { out[o++] = 0xE0 | (cp >> 12); out[o++] = 0x80 | ((cp >> 6) & 63); out[o++] = 0x80 | (cp & 63); }
    }
    if (o > MAX_PAGE) o = MAX_PAGE;
    memcpy(body, out, o);
    body_len = o;
    free(out);
}

/* скачать адрес для скрипта (внешний <script>, XMLHttpRequest, fetch) */
/* кэш скачанного для скриптов и стилей: одно и то же (jquery, общий CSS) не качаем заново */
#define CACHE_N 48
#define CACHE_BYTES (6 << 20)
static struct { char *url, *data, ctype[64]; int len, status; unsigned used; } cache[CACHE_N];
static unsigned cache_clock;

static int cache_find(const char *u) {
    for (int i = 0; i < CACHE_N; i++) if (cache[i].url && !strcmp(cache[i].url, u)) { cache[i].used = ++cache_clock; return i; }
    return -1;
}

static void cache_put(const char *u, const char *data, int len, int status, const char *ctype) {
    if (len > CACHE_BYTES / 4) return;
    int total = len;
    for (int i = 0; i < CACHE_N; i++) if (cache[i].url) total += cache[i].len;
    for (;;) {                                            /* выбросить самые давние, пока не влезет */
        int old = -1, empty = -1;
        for (int i = 0; i < CACHE_N; i++) {
            if (!cache[i].url) { if (empty < 0) empty = i; continue; }
            if (old < 0 || cache[i].used < cache[old].used) old = i;
        }
        if (empty >= 0 && total <= CACHE_BYTES) {
            char *d = malloc(len + 1), *k = malloc(strlen(u) + 1);
            if (!d || !k) { free(d); free(k); return; }
            memcpy(d, data, len); d[len] = 0;
            strcpy(k, u);
            cache[empty].url = k; cache[empty].data = d; cache[empty].len = len; cache[empty].status = status;
            snprintf(cache[empty].ctype, sizeof(cache[empty].ctype), "%s", ctype);
            cache[empty].used = ++cache_clock;
            return;
        }
        if (old < 0) return;
        total -= cache[old].len;
        free(cache[old].url); free(cache[old].data);
        cache[old].url = cache[old].data = NULL;
    }
}

static unsigned page_t0;            /* когда начали грузить текущую страницу */
#define SUBRES_BUDGET 25000         /* скрипты и стили страницы качаем не дольше 25 с в сумме */
static int n_skipped;               /* сколько скриптов/стилей не стали качать */
static bool painted_early;          /* страницу уже показали, не дожидаясь всех скриптов */
static void early_paint(void);
static void relayout(bool keep);

/* скачать адрес для скрипта (внешний <script>, XMLHttpRequest, fetch) */
static int js_fetch(const char *u, const char *method, const char *data, int dlen,
                    char **out, int *len, int *status, char *ctype, int cmax) {
    char cur[1024], loc[512];
    bool html;
    *out = NULL;
    *len = 0;
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8) && strncmp(u, "file:", 5)) return -1;
    bool get = !method || !*method || !strcmp(method, "GET");
    if (get && !data) {
        int c = cache_find(u);
        if (c >= 0) {
            *out = malloc(cache[c].len + 1);
            if (!*out) return -1;
            memcpy(*out, cache[c].data, cache[c].len + 1);
            *len = cache[c].len; *status = cache[c].status;
            snprintf(ctype, cmax, "%s", cache[c].ctype);
            return 0;
        }
    }
    if (blocked_host(u)) {                                 /* счётчики и реклама — мимо */
        char m[200]; int l = snprintf(m, sizeof(m), "blocked (ads/analytics): %.150s\n", u);
        console_add(m, l);
        return -1;
    }
    if (nav_depth && (stop_req || ticks() - page_t0 > SUBRES_BUDGET)) {   /* страница грузится слишком долго */
        n_skipped++;
        char m[200]; int l = snprintf(m, sizeof(m), "skipped (%s): %.150s\n", stop_req ? "stopped" : "page loads too long", u);
        console_add(m, l);
        return -1;
    }
    if (nav_depth) early_paint();                          /* пусть человек видит страницу, пока качаем */
    snprintf(cur, sizeof(cur), "%s", u);
    req_method = method && *method ? method : "GET";
    req_body = data;
    req_body_len = dlen;
    unsigned save_total = req_total_ms, save_idle = req_idle_lim;
    req_total_ms = 15000; req_idle_lim = 6000;
    if (nav_depth) {                                       /* не дольше, чем осталось от бюджета страницы */
        unsigned left = SUBRES_BUDGET - (ticks() - page_t0);
        if (left < req_total_ms) req_total_ms = left < 2000 ? 2000 : left;
    }
    int code = -1;
    for (int hop = 0; hop < 5; hop++) {
        code = http_get(cur, loc, sizeof(loc), &html);
        if (code >= 300 && code < 400 && loc[0]) {
            char next[1024];
            resolve_url(cur, loc, next, sizeof(next));
            snprintf(cur, sizeof(cur), "%s", next);
            req_method = "GET";
            req_body = NULL;
            continue;
        }
        break;
    }
    req_total_ms = save_total; req_idle_lim = save_idle;
    req_method = "GET";
    req_body = NULL;
    req_body_len = 0;
    if (!nav_depth) { stop_req = false; set_status(page_status); }   /* запрос из скрипта после загрузки — вернуть «Done» */
    if (code < 0) return -1;
    *out = malloc(body_len + 1);
    if (!*out) return -1;
    memcpy(*out, body, body_len);
    (*out)[body_len] = 0;
    *len = body_len;
    *status = code;
    snprintf(ctype, cmax, "%s", resp_ctype);
    if (get && !data && code == 200 && (strstr(resp_ctype, "javascript") || strstr(resp_ctype, "css") ||
                                        strstr(resp_ctype, "ecmascript")))
        cache_put(u, body, body_len, code, resp_ctype);
    return 0;
}

static void js_status(const char *m) { set_status(m); }

/* ---- картинки: кэш, загрузка, рисование ---- */
#define IMG_BUDGET (20 << 20)       /* байт пикселей на все картинки */
static bool img_more;               /* на странице, может быть, есть нескачанные картинки */
static unsigned img_last_relayout;

static void img_drop(struct imgent *e) {
    paint_epoch++;
    if (e->sc && e->sc != e->px) free(e->sc);
    free(e->px);
    e->px = e->sc = NULL;
    e->sw = e->sh = 0;
}

static int img_bytes(void) {
    int b = 0;
    for (int i = 0; i < IMG_N; i++) {
        struct imgent *e = &imgs[i];
        if (e->px) b += e->w * e->h * 4;
        if (e->sc && e->sc != e->px) b += e->sw * e->sh * 4;
    }
    return b;
}

/* памяти мало: выбросить давно не нужные картинки (сначала — не с этой страницы) */
static void img_trim(int need) {
    while (img_bytes() + need > IMG_BUDGET) {
        int old = -1;
        for (int pass = 0; pass < 2 && old < 0; pass++)
            for (int i = 0; i < IMG_N; i++) {
                struct imgent *e = &imgs[i];
                if (!e->px || (pass == 0 && e->page == img_page)) continue;
                if (old < 0 || e->used < imgs[old].used) old = i;
            }
        if (old < 0) return;
        img_drop(&imgs[old]);
        imgs[old].state = IMG_QUEUED;                       /* понадобится — скачаем снова */
    }
}

static void img_store(struct imgent *e, const unsigned char *data, int len) {
    paint_epoch++;
    img im;
    int side = W > 1600 ? W : 1600;
    int iw = 0, ih = 0;
    if (img_size(data, len, &iw, &ih) == 0 && iw > 0 && ih > 0) {
        int dw = iw, dh = ih, sd = iw > ih ? iw : ih;
        if (sd > side) { dw = iw * side / sd; dh = ih * side / sd; }
        if (dw > 0 && dh > 0 && dw < 20000 && dh < 20000) img_trim(dw * dh * 4);
    }
    if (img_decode(data, len, &im, side) < 0 && svg_decode(data, len, &im, side) < 0) { e->state = IMG_FAILED; return; }
    e->px = im.px; e->w = im.w; e->h = im.h; e->alpha = im.has_alpha;
    e->state = IMG_OK;
}

static int b64v(int c) {
    return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 :
           c == '+' || c == '-' ? 62 : c == '/' || c == '_' ? 63 : -1;
}

/* data:image/png;base64,.... — картинка прямо в адресе */
static void img_data_url(struct imgent *e) {
    const char *c = strchr(e->url, ',');
    if (!c || !strstr(e->url, ";base64")) { e->state = IMG_FAILED; return; }
    c++;
    int n = strlen(c);
    unsigned char *d = malloc(n * 3 / 4 + 4);
    if (!d) { e->state = IMG_FAILED; return; }
    int o = 0, acc = 0, bits = 0;
    for (; *c; c++) {
        int v = b64v((unsigned char)*c);
        if (v < 0) continue;
        acc = ((acc << 6) | v) & 0xFFFFFF; bits += 6;
        if (bits >= 8) { bits -= 8; d[o++] = (acc >> bits) & 255; }
    }
    img_store(e, d, o);
    free(d);
}

static int img_find(const char *u, bool create) {
    int empty = -1;
    for (int i = 0; i < IMG_N; i++) {
        if (!imgs[i].url) { if (empty < 0) empty = i; continue; }
        if (!strcmp(imgs[i].url, u)) { imgs[i].used = ++img_clock; return i; }
    }
    if (!create) return -1;
    if (empty < 0) {                                       /* место кончилось: самая давняя не с этой страницы */
        for (int i = 0; i < IMG_N; i++)
            if (imgs[i].page != img_page && (empty < 0 || imgs[i].used < imgs[empty].used)) empty = i;
        if (empty < 0) return -1;
        img_drop(&imgs[empty]);
        free(imgs[empty].url);
        imgs[empty].url = NULL;
    }
    struct imgent *e = &imgs[empty];
    memset(e, 0, sizeof(*e));
    e->url = malloc(strlen(u) + 1);
    if (!e->url) return -1;
    strcpy(e->url, u);
    e->state = IMG_QUEUED;
    e->used = ++img_clock;
    e->page = img_page;
    if (!strncmp(u, "data:", 5)) img_data_url(e);
    else img_more = true;
    return empty;
}

/* следующая нескачанная картинка этой страницы: сначала видимые; -1 — все готовы */
static int img_next(void) {
    int best = -1, best_d = 0x7fffffff;
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        if (it->kind != IT_IMG || imgs[it->img].state != IMG_QUEUED || !imgs[it->img].url) continue;
        int d = it->y + it->h < scroll ? scroll - it->y : it->y > scroll + VIEW_H ? it->y - scroll : 0;
        if (d < best_d) { best_d = d; best = it->img; if (!d) break; }
    }
    return best;
}

/* скачать и показать одну картинку; false — больше нечего */
static bool img_pump(void) {
    if (!img_more || !page_is_dom) return false;
    int k = img_next();
    if (k < 0) {
        img_more = false;
        set_status(page_status);
        return false;
    }
    struct imgent *e = &imgs[k];
    int left = 0;
    for (int i = 0; i < IMG_N; i++) if (imgs[i].url && imgs[i].state == IMG_QUEUED && imgs[i].page == img_page) left++;
    char msg[160];
    snprintf(msg, sizeof(msg), "Loading images: %d left (Esc - stop)", left);
    set_status(msg);
    stop_req = false;
    unsigned save_total = req_total_ms, save_idle = req_idle_lim;
    req_total_ms = 15000; req_idle_lim = 6000;
    req_accept = "image/png, image/gif, image/jpeg, image/bmp, */*;q=0.5";
    char cur[1024], loc[512];
    bool html;
    int code = -1;
    snprintf(cur, sizeof(cur), "%s", e->url);
    for (int hop = 0; hop < 5; hop++) {
        code = http_get(cur, loc, sizeof(loc), &html);
        if (code >= 300 && code < 400 && loc[0]) { char nx[1024]; resolve_url(cur, loc, nx, sizeof(nx)); snprintf(cur, sizeof(cur), "%s", nx); continue; }
        break;
    }
    req_accept = "text/html, text/plain, */*";
    req_total_ms = save_total; req_idle_lim = save_idle;
    if (stop_req) {                                        /* Esc: остальные картинки не качать */
        for (int i = 0; i < IMG_N; i++) if (imgs[i].url && imgs[i].state == IMG_QUEUED && imgs[i].page == img_page) imgs[i].state = IMG_FAILED;
        stop_req = false;
        img_more = false;
        set_status(page_status);
        redraw();
        return false;
    }
    if (code != 200 || body_len <= 0) e->state = IMG_FAILED;
    else img_store(e, (const unsigned char *)body, body_len);
    bool guessed = false;                                  /* размер на странице угадывали — разложить заново */
    for (int i = 0; i < n_items; i++) if (items[i].kind == IT_IMG && items[i].img == k && (items[i].scale || e->state != IMG_OK)) guessed = true;
    if (guessed && (ticks() - img_last_relayout > 500 || img_next() < 0)) {
        relayout(true);
        img_last_relayout = ticks();
        redraw();
    } else if (guessed) {
        redraw();
        img_more = true;
    } else redraw();
    return true;
}

/* нарисовать картинку item на экране в (x, y), обрезав по области страницы */
static void draw_img(struct item *it, int y) {
    struct imgent *e = &imgs[it->img];
    int x = it->x, w = it->w, h = it->h;
    int cx0,cx1,cy0,cy1;page_clip_bounds(it->clip,&cx0,&cy0,&cx1,&cy1);
    if (e->state != IMG_OK || !e->px) {                    /* ещё качается или не вышло: рамка и подпись */
        gfx_fill_rect(x, y, w, h, RGB(242, 242, 238));
        gfx_rect(x, y, w, h, it->link >= 0 ? RGB(120, 120, 230) : RGB(200, 200, 195));
        if (it->text && it->text[0] && w > 24 && h >= 18) {
            char t[64];
            int maxc = (w - 6) / 8;
            snprintf(t, sizeof(t), "%.*s", maxc < 63 ? maxc : 63, it->text);
            gfx_text(x + 3, y + 2, t, RGB(120, 120, 110), TRANSPARENT);
        }
        return;
    }
    e->used = ++img_clock;
    if (!e->sc || e->sw != w || e->sh != h) {              /* копия под размер на странице */
        if (e->sc && e->sc != e->px) free(e->sc);
        e->sc = NULL;
        if (w == e->w && h == e->h) e->sc = e->px;
        else if (w * h <= 4 << 20) e->sc = img_scale(e->px, e->w, e->h, w, h);
        if (!e->sc) return;
        e->sw = w; e->sh = h;
    }
    int ys = y < cy0 ? cy0 - y : 0, ye = y + h > cy1 ? cy1 - y : h;
    int xs = x < cx0 ? cx0 - x : 0, xe = x + w > cx1 ? cx1 - x : w;
    if (xe > gfx_w - x) xe = gfx_w - x;
    if (ye > gfx_h - y) ye = gfx_h - y;
    for (int r = ys; r < ye; r++) {
        const unsigned *s = e->sc + r * w;
        color_t *d = gfx_buf + (y + r) * gfx_w + x;
        if (!e->alpha) { for (int c = xs; c < xe; c++) d[c] = s[c] & 0xFFFFFF; continue; }
        for (int c = xs; c < xe; c++) {
            unsigned p = s[c], a = p >> 24;
            if (a == 255) d[c] = p & 0xFFFFFF;
            else if (a) {
                unsigned q = d[c], ia = 255 - a;
                unsigned rr = (((p >> 16) & 255) * a + ((q >> 16) & 255) * ia) / 255;
                unsigned gg = (((p >> 8) & 255) * a + ((q >> 8) & 255) * ia) / 255;
                unsigned bb = ((p & 255) * a + (q & 255) * ia) / 255;
                d[c] = rr << 16 | gg << 8 | bb;
            }
        }
    }
}

/* скрипты страницы ещё качаются, а прошло уже больше секунды: показать то, что разобрано */
static void early_paint(void) {
    if (painted_early || !page_is_dom || ticks() - page_t0 < 1000) return;
    painted_early = true;
    relayout(false);
    redraw();
}

static struct dom_host dom_host = { js_fetch, js_status, console_add };

static unsigned last_layout;

/* дерево DOM -> раскладка; keep — сохранить прокрутку и поле ввода */
static void relayout(bool keep) {
    dom_set_viewport(view_w(), VIEW_H);
    int len;
    char *h = dom_serialize(&len);
    if (!h) return;
    int save_scroll = scroll, fnode = focus_input >= 0 ? inputs[focus_input].node : -1;
    arena_used = 0;
    cp1251 = false;
    layout_html(h, len, false);
    free(h);
    scroll = keep ? save_scroll : 0;
    if (scroll > doc_h - VIEW_H) scroll = doc_h - VIEW_H;
    if (scroll < 0) scroll = 0;
    if (fnode >= 0) for (int i = 0; i < n_inputs; i++) if (inputs[i].node == fnode) focus_input = i;
    char t[96];
    snprintf(t, sizeof(t), "%.60s - Browser", page_title[0] ? page_title : url);
    win_set_title(win, t);
    last_layout = ticks();
}

static void go_history(int d);

/* после работы скриптов: переход по location.href, новый адрес, перерисовка.
 * lazy — от таймеров: перерисовывать не чаще 10 раз в секунду */
static void after_js_ex(bool lazy) {
    char nav[1024];
    if (dom_pending_nav(nav, sizeof(nav))) {
        if (!strcmp(nav, "myos:back")) { go_history(-1); return; }
        if (!strcmp(nav, "myos:forward")) { go_history(1); return; }
        if (nav_depth < 5) { navigate(nav, true); return; }
    }
    if (page_is_dom && dom_url_changed()) {
        dom_get_url(url, sizeof(url));
        if (!editing_addr) snprintf(addr, sizeof(addr), "%s", url);
        if (hist_pos >= 0 && hist_pos < n_hist) { free(history[hist_pos]); history[hist_pos] = malloc(strlen(url) + 1); strcpy(history[hist_pos], url); }
        draw_toolbar();
        win_update_rect(win, 0, 0, W, BAR_H);
    }
    if (page_is_dom && dom_dirty && !(lazy && ticks() - last_layout < 100)) { relayout(true); redraw(); }
}

static void after_js(void) { after_js_ex(false); }

/* тот же документ, только другой #якорь? */
static bool same_page(const char *a, const char *b) {
    int la = strchr(a, '#') ? strchr(a, '#') - a : (int)strlen(a), lb = strchr(b, '#') ? strchr(b, '#') - b : (int)strlen(b);
    return la == lb && !strncmp(a, b, la) && strchr(b, '#');
}

static void navigate(const char *target, bool add_history) {
    char u[512], loc[512];
    snprintf(u, sizeof(u), "%s", target);
    if (!strstr(u, "://") && strncmp(u, "about:", 6) && strncmp(u, "file:", 5)) {   /* «example.com» */
        if (strchr(u, '.') || !strncmp(u, "localhost", 9)) snprintf(u, sizeof(u), "http://%s", target);
        else {                                                                    /* просто слова — поиск */
            char q[400];
            url_encode(target, q, sizeof(q));
            snprintf(u, sizeof(u), "http://frogfind.com/?q=%s", q);
        }
    }
    bool html = true;
    int code = -1;
    char fallback[160];
    fallback[0] = 0;
    bool outer = !loading;                                /* не вложенный переход (location.href из скрипта) */
    if (outer) { stop_req = false; page_t0 = ticks(); painted_early = false; n_skipped = 0; }
    img_page++;                                           /* картинки прошлой страницы можно выбрасывать */
    img_more = false;
    loading = true;
    for (int hop = 0; hop < 6; hop++) {
        snprintf(url, sizeof(url), "%s", u);
        snprintf(addr, sizeof(addr), "%s", u);
        draw_toolbar();
        win_update_rect(win, 0, 0, W, BAR_H);
        code = http_get(u, loc, sizeof(loc), &html);
        if (code == -2) {                                 /* TLS 1.3 не вышел — через FrogFind */
            char enc[400], t[512];
            snprintf(fallback, sizeof(fallback), "TLS: %s - trying FrogFind instead", tls_err);
            url_encode(u, enc, sizeof(enc));
            snprintf(t, sizeof(t), PROXY "%s", enc);
            snprintf(u, sizeof(u), "%s", t);
            continue;
        }
        if (code >= 300 && code < 400 && loc[0]) {
            resolve_url(u, loc, u, sizeof(u));
            continue;
        }
        break;
    }
    arena_used = 0;
    char page_secure[120];                                 /* HTTPS самой страницы (скрипты перезапишут secure) */
    snprintf(page_secure, sizeof(page_secure), "%s", code >= 0 ? secure : "");
    if (code == -3) {                                     /* сертификат не годится: предупредить */
        char esc_url[600];
        int k = 0;
        for (int i = 0; url[i] && k < 590; i++) { char c = url[i]; if (c == '"' || c == '<' || c == '>') c = '_'; esc_url[k++] = c; }
        esc_url[k] = 0;
        body_len = snprintf(body, MAX_PAGE, "<title>Certificate problem</title><h1>This site's certificate is not trusted</h1>"
                            "<p><b>%s</b>: %s</p><p>Someone may be pretending to be this site, or its certificate is just "
                            "set up wrong (or our list of root certificates in /etc/certs.der is old).</p>"
                            "<p><a href=\"about:home\">Go home</a> &nbsp; | &nbsp; <a href=\"about:trust?%s\">Open the site anyway</a> "
                            "(only until the browser is closed)</p>", cert_host, cert_problem, esc_url);
        html = true;
        code = 0;
    }
    if (code < 0) {
        char why[200];
        int k = 0;
        for (int i = 0; status[i] && k < 190; i++) {        /* причина из строки состояния, без < и & */
            char c = status[i];
            if (c == '<' || c == '>' || c == '&') c = ' ';
            why[k++] = c;
        }
        why[k] = 0;
        body_len = snprintf(body, MAX_PAGE, "<title>Error</title><h1>Cannot open this page</h1><p><b>%s</b></p>"
                            "<p>Check the address and try again (R), or go <a href=\"about:home\">home</a>.</p>%s%s%s",
                            why, fallback[0] ? "<p><b>HTTPS:</b> " : "", fallback, fallback[0] ? "</p>" : "");
        html = true;
    }
    if (strncmp(url, "about:console", 13)) console_len = 0;
    focus_input = -1;
    page_is_dom = html;
    int body_kb = body_len / 1024;
    if (html) {                                            /* HTML: дерево DOM + скрипты */
        if (!cp1251) {                                     /* <meta charset=windows-1251> */
            int lim = body_len < 4096 ? body_len : 4096;
            for (int i = 0; i + 4 <= lim; i++) if (!memcmp(body + i, "1251", 4)) { cp1251 = true; break; }
        }
        if (cp1251) cp1251_to_utf8();
        cp1251 = false;
        set_status(dom_scripting ? "Running scripts..." : "Building page...");
        nav_depth++;
        dom_set_viewport(view_w(), VIEW_H);
        dom_load(body, body_len, url);
        relayout(false);
        nav_depth--;
    } else layout_html(body, body_len, true);
    scroll = 0;
    if (code > 0) {                                       /* 0 — своя страница-предупреждение со своим статусом */
        if (code == 200) snprintf(status, sizeof(status), "Done: %d KB, %d links", body_kb, n_links);
        else snprintf(status, sizeof(status), "HTTP %d", code);
        int sl = strlen(status);
        if (html) {
            char js[64];
            dom_stats(js, sizeof(js));
            sl += snprintf(status + sl, sizeof(status) - sl, " - %s%s", js, strstr(js, "error") ? " (Ctrl+K)" : "");
        }
        if (n_skipped && sl < (int)sizeof(status)) sl += snprintf(status + sl, sizeof(status) - sl, ", %d not loaded", n_skipped);
        if (stop_req && sl < (int)sizeof(status)) sl += snprintf(status + sl, sizeof(status) - sl, " - stopped");
        if (sl >= (int)sizeof(status)) sl = sizeof(status) - 1;
        if (page_secure[0]) snprintf(status + sl, sizeof(status) - sl, " - %s", page_secure);
        else if (fallback[0]) snprintf(status + sl, sizeof(status) - sl, " - %s", fallback);
    }
    if (code == 0) snprintf(status, sizeof(status), "Certificate problem: %s", cert_problem);
    if (outer) { loading = false; resized_while_busy = false; }
    hover_link[0] = 0;
    snprintf(page_status, sizeof(page_status), "%s", status);
    if (add_history) {
        if (hist_pos < n_hist - 1) {                      /* после «Назад» новая ветка */
            for (int i = hist_pos + 1; i < n_hist; i++) free(history[i]);
            n_hist = hist_pos + 1;
        }
        if (n_hist == 32) { free(history[0]); memmove(history, history + 1, sizeof(char *) * 31); n_hist--; }
        history[n_hist] = malloc(strlen(url) + 1);
        strcpy(history[n_hist], url);
        hist_pos = n_hist++;
    }
    redraw();
    if (html && nav_depth == 0) {                          /* скрипт сразу попросил другой адрес */
        nav_depth++;
        after_js();
        nav_depth--;
    }
}

static void go_history(int d) {
    int p = hist_pos + d;
    if (p < 0 || p >= n_hist) return;
    hist_pos = p;
    navigate(history[p], false);
}

static void submit_form(int form) {
    if (form < 0) return;
    char u[512];
    snprintf(u, sizeof(u), "%s", form_action[form]);
    char *q = strchr(u, '?');
    if (q) *q = 0;
    int l = strlen(u);
    char sep = '?';
    for (int i = 0; i < n_inputs; i++) {
        struct input *in = &inputs[i];
        if (in->form != form || !in->name[0] || in->type == 's') continue;
        char ev[300];
        url_encode(in->value, ev, sizeof(ev));
        l += snprintf(u + l, sizeof(u) - l, "%c%s=%s", sep, in->name, ev);
        sep = '&';
    }
    navigate(u, true);
}

/* ------------------------------------------------------------ мышь и клавиши */

static int item_at(int x, int y) {
    int dy = y - VIEW_Y + scroll, fill = -1;
    for(int i=n_items-1;i>=0;i--) {
        struct item *it=&items[i];
        if(it->kind==IT_BUTTON && it->fsize==255 && page_clip_contains(it->clip,x,y) && x>=it->x && x<it->x+it->w && dy>=it->y && dy<it->y+it->h)return i;
    }
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        if (page_clip_contains(it->clip,x,y) && x >= it->x && x < it->x + it->w && dy >= it->y && dy < it->y + it->h + it->below) {
            if (it->kind != IT_FILL) return i;
            if (fill < 0) fill = i;                          /* фон кнопки — если не попали в текст */
        }
    }
    return fill;
}

static void scroll_by(int d) {
    scroll += d;
    if (scroll > doc_h - VIEW_H) scroll = doc_h - VIEW_H;
    if (scroll < 0) scroll = 0;
}

/* Locate a UTF-8 character boundary in the nearest rendered text run. */
static int text_position(int x, int y, int *byte) {
    int best = -1, distance = 0x7fffffff;
    y += scroll - VIEW_Y;
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        if (it->kind != IT_TEXT || !it->text || !it->text[0]) continue;
        int dy = y < it->y ? it->y - y : y >= it->y + it->h ? y - it->y - it->h + 1 : 0;
        int dx = x < it->x ? it->x - x : x > it->x + it->w ? x - it->x - it->w : 0;
        int d = dy * 4096 + dx;
        if (d < distance) { distance = d; best = i; }
    }
    *byte = 0;
    if (best < 0) return -1;
    struct item *it = &items[best];
    int prev = 0;
    while (it->text[*byte]) {
        int next = *byte + 1;
        while (u8cont(it->text[next])) next++;
        int width = item_text_width(it, next);
        if (x - it->x < (prev + width) / 2) break;
        *byte = next; prev = width;
    }
    return best;
}

static void on_click(int x, int y) {
    input_all = false;
    sel_item = sel_end_item = -1; text_drag = false;
    if (y < BAR_H) {
        editing_addr = false;
        if (ui_inside(x, y, 8, NAV_Y, 28, 28)) { go_history(-1); return; }
        if (ui_inside(x, y, 42, NAV_Y, 28, 28)) { go_history(1); return; }
        if (ui_inside(x, y, 76, NAV_Y, 28, 28)) { navigate(url, false); return; }
        if (ui_inside(x, y, 110, NAV_Y, 28, 28)) { navigate(HOME, true); return; }
        if (ui_inside(x, y, ADDR_X, NAV_Y-2, W-ADDR_X-12, 32)) { addr_all = true; editing_addr = true; focus_input = -1; addr_cur = strlen(addr); }
        redraw();
        return;
    }
    if (y >= H - STATUS_H) return;
    editing_addr = false;
    if (x >= W - SB_W && doc_h > VIEW_H) {                  /* полоса прокрутки: страница вверх/вниз */
        int ky = VIEW_Y + (VIEW_H - 24) * scroll / (doc_h - VIEW_H);
        scroll_by(y < ky ? -VIEW_H + 32 : VIEW_H - 32);
        redraw();
        return;
    }
    int i = item_at(x, y);
    focus_input = -1;
    if (i >= 0) {
        struct item *it = &items[i];
        if (it->kind == IT_INPUT) { focus_input = it->input; redraw(); return; }
        if (it->kind == IT_BUTTON) {
            struct input *in = &inputs[it->input];
            if (page_is_dom && dom_scripting && in->node >= 0) { dom_click(in->node); after_js(); return; }
            submit_form(in->form);
            return;
        }
        if (it->link >= 0) {
            char target[512];
            snprintf(target, sizeof(target), "%s", links[it->link]);
            int node = link_node[it->link];
            if (!strncmp(target, "myosjs:", 7)) {             /* элемент с обработчиком onclick */
                if (page_is_dom) { dom_click(atoi(target + 7)); after_js(); }
                return;
            }
            if (page_is_dom && dom_scripting && node >= 0) {
                if (dom_click(node)) { after_js(); return; }   /* скрипт отменил переход */
                char nav[1024];
                if (dom_pending_nav(nav, sizeof(nav))) { navigate(nav, true); return; }
            }
            if (page_is_dom && same_page(url, target)) {          /* #якорь: страницу не перезагружаем */
                dom_set_url(target);
                snprintf(url, sizeof(url), "%s", target);
                snprintf(addr, sizeof(addr), "%s", target);
                after_js();
                redraw();
                return;
            }
            if (!strncmp(target, "mailto:", 7)) { set_status("mail links are not supported"); return; }
            if (!strncmp(target, "javascript:", 11)) return;
            navigate(target, true);
            return;
        }
        if (page_is_dom && dom_scripting && it->node >= 0) {    /* щелчок по тексту: событие элементу (и выше) */
            dom_click(it->node);
            after_js();
            return;
        }
    }
    redraw();
}

/* строка в UTF-8: курсор — байтовое смещение, но ходит и стирает целыми буквами */

static void edit_string(char *s, int max, int *cur, int k) {
    int l = strlen(s);
    if (k == KEY_LEFT && *cur > 0) { (*cur)--; while (*cur > 0 && u8cont(s[*cur])) (*cur)--; }
    else if (k == KEY_RIGHT && *cur < l) { (*cur)++; while (*cur < l && u8cont(s[*cur])) (*cur)++; }
    else if (k == KEY_HOME) *cur = 0;
    else if (k == KEY_END) *cur = l;
    else if ((k == 8 || k == 127) && *cur > 0) {
        int st = *cur - 1;
        while (st > 0 && u8cont(s[st])) st--;
        memmove(s + st, s + *cur, l - *cur + 1);
        *cur = st;
    } else if (k == KEY_DEL && *cur < l) {
        int e = *cur + 1;
        while (e < l && u8cont(s[e])) e++;
        memmove(s + *cur, s + e, l - e + 1);
    } else if ((k >= 32 && k < 127) || (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS))) {
        char u[4];
        int n = utf8_put(u, k);
        if (l + n >= max) return;
        memmove(s + *cur + n, s + *cur, l - *cur + 1);
        memcpy(s + *cur, u, n);
        *cur += n;
    }
}


static void on_key(int k) {
    if (!editing_addr && focus_input < 0 && (k == CTRL('c') || k == KEY_COPY)) { copy_page_selection(); return; }
    if (!editing_addr && focus_input < 0 && k == CTRL('a')) {
        sel_item = sel_end_item = -1;
        for (int i = 0; i < n_items; i++) if (items[i].kind == IT_TEXT && items[i].text) {
            if (sel_item < 0) { sel_item = i; sel_byte = 0; }
            sel_end_item = i; sel_end_byte = strlen(items[i].text);
        }
        redraw(); return;
    }
    if (k == CTRL('l')) { editing_addr = true; addr_all = true; focus_input = -1; addr_cur = strlen(addr); redraw(); return; }
    if (k == CTRL('j')) {                                  /* JavaScript вкл/выкл — и перезагрузить */
        dom_scripting = !dom_scripting;
        navigate(url, false);
        set_status(dom_scripting ? "JavaScript is ON" : "JavaScript is OFF (Ctrl+J to turn on)");
        return;
    }
    if (k == CTRL('k')) { navigate("about:console", true); return; }
    if (k == CTRL('u')) {                                  /* стили страницы вкл/выкл — и перезагрузить */
        dom_css = !dom_css;
        navigate(url, false);
        set_status(dom_css ? "Page styles (CSS) are ON" : "Page styles (CSS) are OFF (Ctrl+U to turn on)");
        return;
    }
    if (editing_addr) {
        if (k == CTRL('a')) { addr_all = true; addr_cur = strlen(addr); draw_toolbar(); win_update_rect(win, 0, 0, W, BAR_H); return; }
        if (k == CTRL('c') || k == KEY_COPY || k == CTRL('x')) {
            if (addr_all && clipboard_set(addr, strlen(addr)) >= 0 && k == CTRL('x')) {
                addr[0] = 0; addr_cur = 0; addr_all = false;
            }
            draw_toolbar(); win_update_rect(win, 0, 0, W, BAR_H); return;
        }
        if (k == CTRL('v') || k == KEY_PASTE || k == (KEY_INS | KEY_SHIFT)) {
            char *s = malloc(CLIPBOARD_MAX);
            if (!s) return;
            int n = clipboard_get(s, CLIPBOARD_MAX), length = strlen(addr);
            /* A URL is a single line; stop before pasted line breaks. */
            int count = 0;
            while (count < n && (unsigned char)s[count] >= 32 && s[count] != 127) count++;
            if (count > 0 && count + (addr_all ? 0 : length) < (int)sizeof(addr)) {
                if (addr_all) { addr[0] = 0; addr_cur = length = 0; addr_all = false; }
                memmove(addr + addr_cur + count, addr + addr_cur, length - addr_cur + 1);
                memcpy(addr + addr_cur, s, count); addr_cur += count;
            }
            free(s); draw_toolbar(); win_update_rect(win, 0, 0, W, BAR_H); return;
        }
        if (k == '\n' || k == '\r') { editing_addr = false; navigate(addr, true); return; }
        if (k == 27) { editing_addr = false; snprintf(addr, sizeof(addr), "%s", url); redraw(); return; }
        if (addr_all) {
            addr_all = false;
            bool ch = (k >= 32 && k < 127) || (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS));
            if (ch || k == 8 || k == 127 || k == KEY_DEL) { addr[0] = 0; addr_cur = 0; if (!ch) k = 0; }
        }
        if (k) edit_string(addr, sizeof(addr), &addr_cur, k);
        draw_toolbar();
        win_update_rect(win, 0, 0, W, BAR_H);
        return;
    }
    if (focus_input >= 0) {
        struct input *in = &inputs[focus_input];
        bool js = page_is_dom && dom_scripting && in->node >= 0;
        if (k == CTRL('a')) { input_all = true; redraw(); return; }
        if (k == CTRL('c') || k == KEY_COPY || k == CTRL('x')) {
            if (!input_all) return;
            if (clipboard_set(in->value, strlen(in->value)) < 0 || k != CTRL('x')) return;
        }
        if (k == '\n' || k == '\r') {
            if (js) {
                int f = in->form;
                dom_key(in->node, '\r', NULL);
                if (f >= 0 && form_node[f] >= 0) {
                    if (!dom_submit(form_node[f])) { submit_form(f); return; }
                }
                after_js();
                return;
            }
            submit_form(in->form);
            return;
        }
        if (k == 27) { focus_input = -1; redraw(); return; }
        char before[128];
        snprintf(before, sizeof(before), "%s", in->value);
        int cur = strlen(in->value);
        if (k == CTRL('v') || k == KEY_PASTE || k == (KEY_INS | KEY_SHIFT)) {
            char *s = malloc(CLIPBOARD_MAX);
            if (!s) return;
            int n = clipboard_get(s, CLIPBOARD_MAX), count = 0;
            while (count < n && (unsigned char)s[count] >= 32 && s[count] != 127) count++;
            if (count > 0 && count + (input_all ? 0 : cur) < (int)sizeof(in->value)) {
                if (input_all) cur = 0;
                memcpy(in->value + cur, s, count); in->value[cur + count] = 0; input_all = false;
            }
            free(s); k = 0;
        } else {
            bool printable = (k >= 32 && k < 127) || (k >= 0xA0 && k < 0x10000 && (k < KEY_UP || k > KEY_INS));
            if (input_all && (printable || k == 8 || k == 127 || k == KEY_DEL || k == CTRL('x'))) {
                in->value[0] = 0; cur = 0;
                if (!printable) k = 0;
            }
            input_all = false;
            if (k) edit_string(in->value, sizeof(in->value), &cur, k);
        }
        if (page_is_dom && in->node >= 0) {
            dom_key(in->node, k < 256 ? k : 0, strcmp(before, in->value) ? in->value : NULL);
            after_js();
        }
        redraw();
        return;
    }
    switch (k) {
    case KEY_DOWN: scroll_by(48); break;
    case KEY_UP: scroll_by(-48); break;
    case KEY_PGDN: case ' ': scroll_by(VIEW_H - 32); break;
    case KEY_PGUP: scroll_by(-(VIEW_H - 32)); break;
    case KEY_HOME: scroll = 0; break;
    case KEY_END: scroll_by(doc_h); break;
    case 8: case 127: go_history(-1); return;
    case KEY_RIGHT: go_history(1); return;
    case CTRL('r'): navigate(url, false); return;
    default: return;
    }
    redraw();
}

static void on_move(int x, int y) {
    char before[160];
    snprintf(before, sizeof(before), "%s", hover_link);
    hover_link[0] = 0;
    if (y >= VIEW_Y && y < H - STATUS_H) {
        int i = item_at(x, y);
        if (i >= 0 && items[i].link >= 0) snprintf(hover_link, sizeof(hover_link), "%s", links[items[i].link]);
    }
    if (strcmp(before, hover_link)) {
        draw_status();
        win_update_rect(win, 0, H - STATUS_H, W, STATUS_H);
    }
}

int main(int argc, char **argv) {
    win = win_create("Browser", W, H);
    if (!win) { dprintf(STDERR, "browser: the desktop is not running\n"); return 1; }
    win_set_resizable(win, 480, 300);
    items = malloc(sizeof(struct item) * MAX_ITEMS);
    boxes = malloc(sizeof(struct box) * MAX_BOXES);
    init_styles();
    body = malloc(MAX_PAGE + 1);
    arena_cap = 1024 * 1024;
    arena = malloc(arena_cap);
    if (!items || !boxes || !body || !arena) { dprintf(STDERR, "browser: out of memory\n"); return 1; }
    dom_init(&dom_host);
    js_poll_hook = js_busy_poll;
    if (argc > 1 && !strcmp(argv[1], "-v")) { verbose = true; argc--; argv++; }
    t_start = ticks();
    navigate(argc > 1 ? argv[1] : HOME, true);
    if (verbose) printf("[browser %u ms] %s\n", ticks() - t_start, status);
    struct event e;
    for (;;) {
        int wait = page_is_dom ? dom_next_timer() : -1;
        if (img_more && !n_later) wait = 0;                /* есть нескачанные картинки: только заглянуть в события */
        if (page_is_dom && dom_dirty) {                    /* отложенная перерисовка */
            int left = 100 - (int)(ticks() - last_layout);
            if (left < 0) left = 0;
            if (wait < 0 || left < wait) wait = left;
        }
        if (n_later) { e = later[0]; memmove(later, later + 1, sizeof(later[0]) * --n_later); }
        else if (!win_event(&e, wait)) break;
        stop_req = false;                                  /* Esc во время таймера касался только его */
        if (e.type == EV_TIMEOUT) {                        /* setTimeout / setInterval */
            if (page_is_dom) { if (dom_next_timer() == 0) dom_run_timers(); after_js_ex(true); }
            if (img_more) img_pump();                      /* событий нет — следующая картинка */
            continue;
        }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_WHEEL) {
            if (e.y >= VIEW_Y && e.y < H - STATUS_H) { scroll_by(-e.wheel * 48); redraw(); }
        }
        else if (e.type == EV_MOUSE_DOWN && (e.buttons & MOUSE_LEFT)) {
            int hit = item_at(e.x, e.y);
            if (e.y >= VIEW_Y && e.y < H - STATUS_H && hit >= 0 && items[hit].kind == IT_TEXT) {
                sel_item = sel_end_item = text_position(e.x, e.y, &sel_byte);
                sel_end_byte = sel_byte; text_drag = true; text_dragged = false;
                text_down_x = e.x; text_down_y = e.y;
                editing_addr = false; focus_input = -1; redraw();
            } else on_click(e.x, e.y);
        }
        else if (e.type == EV_MOUSE_MOVE && text_drag) {
            if (e.x < text_down_x - 3 || e.x > text_down_x + 3 || e.y < text_down_y - 3 || e.y > text_down_y + 3) text_dragged = true;
            if (text_dragged) {
                if (e.y < VIEW_Y) scroll_by(-16);
                if (e.y >= H - STATUS_H) scroll_by(16);
                sel_end_item = text_position(e.x, e.y, &sel_end_byte); redraw();
            }
        }
        else if (e.type == EV_MOUSE_UP && text_drag) {
            text_drag = false;
            if (!text_dragged) on_click(text_down_x, text_down_y);
            else { sel_end_item = text_position(e.x, e.y, &sel_end_byte); redraw(); }
        }
        else if (e.type == EV_FOCUS && !e.key) text_drag = false;
        else if (e.type == EV_MOUSE_MOVE && !e.buttons) on_move(e.x, e.y);
        else if (e.type == EV_KEY) on_key(e.key);
        else if (e.type == EV_RESIZE) {                    /* новая ширина — разложить страницу заново */
            W = e.x; H = e.y;
            if (page_is_dom) {dom_set_viewport(view_w(),VIEW_H);dom_resize();relayout(true);}
            else { arena_used = 0; layout_html(body, body_len, true); }
            if (scroll > doc_h - VIEW_H) scroll = doc_h - VIEW_H;
            if (scroll < 0) scroll = 0;
            redraw();
        }
    }
    win_destroy(win);
    return 0;
}
