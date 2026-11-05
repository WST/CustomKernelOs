/* browser [адрес] — веб-браузер MyOS.
 *
 * Что умеет: HTTP (сам качает страницу через TCP/IP MyOS), разбирает HTML и
 * раскладывает текст по строкам: заголовки, абзацы, списки, ссылки, жирный,
 * <pre>, таблицы (упрощённо), горизонтальные линии, формы с полями ввода
 * (поиск работает). Переходы по ссылкам и редиректам, «Назад»/«Вперёд».
 * Картинки показываются рамкой с подписью.
 *
 * HTTPS — свой, через user/lib/tls.c (TLS 1.3: X25519 + ChaCha20-Poly1305 или
 * AES-128-GCM). Сертификат сервера пока не проверяется. Если сайт не умеет
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

#define W 800
#define H 576
#define BAR_H 36
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

static char *arena;
static int arena_used, arena_cap;

static void *aalloc(int n) {
    n = (n + 3) & ~3;
    if (arena_used + n > arena_cap) return NULL;
    void *p = arena + arena_used;
    arena_used += n;
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

enum { IT_TEXT, IT_HR, IT_BOX, IT_INPUT, IT_BUTTON, IT_BULLET, IT_FILL };

struct item {
    int  x, y, w, h;
    unsigned char kind, scale, bold, underline, strike;
    short below;                   /* IT_FILL: сколько уходит ниже строки (padding-bottom) */
    color_t color, bg;             /* bg — фон под текстом (TRANSPARENT — нет); у IT_FILL color — рамка */
    int  link;                     /* номер ссылки или -1 */
    int  input;                    /* IT_INPUT / IT_BUTTON: номер поля */
    int  node;                     /* элемент DOM, которому принадлежит (для щелчков в JS) */
    char *text;
};

#define MAX_ITEMS 12000
#define MAX_LINKS 1500
#define MAX_INPUTS 64
#define MAX_FORMS 16

static struct item *items;
static int n_items;
static char *links[MAX_LINKS];
static int link_node[MAX_LINKS];           /* узел DOM ссылки (для событий JS) или -1 */
static int n_links;
static int doc_h, scroll;

struct input { int form; char name[40]; char value[128]; char type; int item; int node; };   /* type: t текст, s кнопка, h скрытое */
static struct input inputs[MAX_INPUTS];
static int n_inputs, focus_input = -1;
static char form_action[MAX_FORMS][256];
static int form_node[MAX_FORMS];
static int n_forms, cur_form = -1;

static char page_title[80];
static char url[512];              /* адрес текущей страницы */
static char status[160];
static char hover_link[160];
static char addr[512];             /* текст в строке адреса */
static bool editing_addr;
static int addr_cur;
static bool addr_all;               /* весь адрес выделен: набор заменит его */

static char *history[32];
static int n_hist, hist_pos = -1;

/* ------------------------------------------------------------ текст: UTF-8 -> наш шрифт */

static const char *translit_upper[32] = {
    "A", "B", "V", "G", "D", "E", "Zh", "Z", "I", "Y", "K", "L", "M", "N", "O", "P",
    "R", "S", "T", "U", "F", "Kh", "Ts", "Ch", "Sh", "Sch", "\"", "Y", "'", "E", "Yu", "Ya",
};
static const char *translit_lower[32] = {
    "a", "b", "v", "g", "d", "e", "zh", "z", "i", "y", "k", "l", "m", "n", "o", "p",
    "r", "s", "t", "u", "f", "kh", "ts", "ch", "sh", "sch", "\"", "y", "'", "e", "yu", "ya",
};

static bool cp1251;                /* страница в кодировке windows-1251 */

/* символ Юникода -> строка для нашего шрифта (CP437) */
static const char *map_cp(unsigned cp, char *tmp) {
    if (cp < 128) { tmp[0] = (char)cp; tmp[1] = 0; return tmp; }
    if (cp >= 0x410 && cp <= 0x42F) return translit_upper[cp - 0x410];
    if (cp >= 0x430 && cp <= 0x44F) return translit_lower[cp - 0x430];
    if (cp == 0x401) return "Yo";
    if (cp == 0x451) return "yo";
    switch (cp) {
    case 0xA0: return " ";
    case 0xAB: return "\xAE";      /* « */
    case 0xBB: return "\xAF";      /* » */
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0xB0: return "\xF8";      /* ° */
    case 0xB7: case 0x2022: case 0x25CF: return "\x07";
    case 0xE9: return "\x82";
    case 0xE8: return "\x8A";
    case 0xE0: return "\x85";
    case 0xE4: return "\x84";
    case 0xF6: return "\x94";
    case 0xFC: return "\x81";
    case 0xC4: return "\x8E";
    case 0xD6: return "\x99";
    case 0xDC: return "\x9A";
    case 0xDF: return "\xE1";
    case 0xE7: return "\x87";
    case 0xF1: return "\xA4";
    case 0x2013: case 0x2014: case 0x2212: return "-";
    case 0x2018: case 0x2019: case 0x201A: return "'";
    case 0x201C: case 0x201D: case 0x201E: return "\"";
    case 0x2026: return "...";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    case 0x20AC: return "EUR";
    case 0x2116: return "No";
    case 0x200B: case 0xFEFF: return "";
    default: return "?";
    }
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
    char tmp[4];
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
            cp = '?'; i += 4;
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
static int left, right;
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
    def_style.width = def_style.max_width = -1;
    plain_style = def_style;
    plain_style.pre = 1;
}

/* прямоугольники фона и рамок блоков */
struct box { int x, y, w, h; int bg; short bw[4]; int bc[4]; };
#define MAX_BOXES 3000
static struct box *boxes;
static int n_boxes;

static struct item *add_item(int kind, int w, int h) {
    if (n_items >= MAX_ITEMS) return NULL;
    struct item *it = &items[n_items++];
    memset(it, 0, sizeof(*it));
    it->kind = kind;
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
    int shift = 0;
    if (cs->align) {                                       /* text-align: center / right */
        int used = left;
        for (int i = line_start; i < n_items; i++) if (items[i].x + items[i].w > used) used = items[i].x + items[i].w;
        shift = cs->align == 1 ? (right - used) / 2 : right - used;
        if (shift < 0) shift = 0;
    }
    for (int i = line_start; i < n_items; i++) { items[i].y = ly + asc - items[i].h; items[i].x += shift; }
    ly += asc + desc;
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

static void place_word(const char *w, int n) {
    if (n <= 0) return;
    apply_gap();
    int sc = cur_scale(), cw = 8 * sc;
    int width = n * cw, space = space_pending && lx > left ? cw : 0;
    if (lx + space + width > right && lx > left) { finish_line(); space = 0; }
    if (width > right - left) {                            /* слово длиннее строки — рубим */
        int fit = (right - lx) / cw;
        if (fit < 1) fit = 1;
        place_word(w, fit);
        space_pending = false;
        place_word(w + fit, n - fit);
        return;
    }
    if (space && inline_bg >= 0 && n_items > line_start && items[n_items - 1].bg == (color_t)inline_bg)
        items[n_items - 1].w += space;                     /* фон <mark> — и под пробелом */
    lx += space;
    struct item *it = add_item(IT_TEXT, width, 16 * sc);
    if (!it) return;
    it->text = astrdup(w, n);
    if (cs->transform) {                                   /* text-transform */
        for (int i = 0; i < n; i++) {
            char c = it->text[i];
            if (cs->transform == 1 || (cs->transform == 3 && i == 0)) it->text[i] = toupper(c);
            else if (cs->transform == 2) it->text[i] = tolower(c);
        }
    }
    it->scale = sc;
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
        char tmp[512];
        decode_text(out, o, tmp, sizeof(tmp));             /* &amp; в адресах */
        snprintf(out, max, "%s", tmp);
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
    if (IS("button")) { if (!close) { tag_input(t, n, true); in_skip = true; } else in_skip = false; return; }
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
};
static struct frame frames[256];

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
        int w = strlen(num) * 8;
        lx = left - w - 6;
        struct item *it = add_item(IT_TEXT, w, 16);
        if (it) { it->text = astrdup(num, strlen(num)); it->scale = 1; it->color = s->color; }
    } else {
        lx = left - 14;
        struct item *it = add_item(IT_BULLET, 6, 16);
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
            b->bg = ps->bg;
            for (int i = 0; i < 4; i++) { b->bw[i] = clampi(ps->border[i], 0, 8); b->bc[i] = ps->border_color[i]; }
            ly += clampi(ps->padding[0] + ps->border[0], 0, 120);
        }
        return;
    }
}

static void style_open(struct frame *f, const struct dom_style *s, const char *tag) {
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
    if (f->kind == FK_BLOCK) promote(f);
    bool root = !strcmp(tag, "html") || !strcmp(tag, "body");
    switch (f->kind) {
    case FK_BLOCK: {
        block(vmargin(s->margin[0]));
        inline_bg = -1;
        int avail = right - left;
        int ml = s->margin[3] == CSS_AUTO ? 0 : clampi(s->margin[3], -left, 200);
        int mr = s->margin[1] == CSS_AUTO ? 0 : clampi(s->margin[1], -(view_w() - right), 200);
        int bx = left + ml, bxr = right - mr;
        int want = -1;                                     /* ширина: max-width всегда, width — у колонки по центру */
        bool autom = s->margin[1] == CSS_AUTO || s->margin[3] == CSS_AUTO;
        if (s->width >= 0 && autom) want = s->width_pct ? avail * s->width / 100 : s->width;
        if (s->max_width >= 0) {
            int mw = s->max_width_pct ? avail * s->max_width / 100 : s->max_width;
            if (want < 0 || mw < want) want = mw;
        }
        if (want >= 80 && want < bxr - bx) {
            int extra = bxr - bx - want;
            if (s->margin[1] == CSS_AUTO && s->margin[3] == CSS_AUTO) bx += extra / 2;
            else if (s->margin[3] == CSS_AUTO) bx += extra;
            bxr = bx + want;
        }
        if (bxr - bx < 80) { bx = left; bxr = right; }
        bool painted = s->bg >= 0 || has_border(s) || s->bg_image;
        if (root && s->bg >= 0 && page_bg < 0) { page_bg = s->bg; painted = has_border(s); }   /* фон страницы */
        int pt = s->padding[0] + s->border[0];
        if (painted || pt) apply_gap();
        if (painted && n_boxes < MAX_BOXES) {
            struct box *b = &boxes[n_boxes];
            f->box = n_boxes++;
            b->x = bx; b->y = ly; b->w = bxr - bx; b->h = 0;
            b->bg = root && s->bg == page_bg ? -1 : s->bg;
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
        int pb = s->padding[2] + s->border[2];
        if (f->box >= 0 || pb) ly += clampi(pb, 0, 120);
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
    inline_bg = f->inline_bg;
    cur_bg = f->cur_bg;
}

/* разобрать HTML и разложить */
static void layout_html(const char *s, int n, bool plain) {
    n_items = 0; n_links = 0; n_inputs = 0; n_forms = 0; cur_form = -1; n_boxes = 0;
    focus_input = -1;
    left = MARGIN - 8; right = view_w() - MARGIN + 8;      /* ещё 8 точек — отступ <body> по умолчанию */
    lx = left; ly = 8; line_h = 0; line_below = 0; line_start = 0; lines_done = 0;
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
                    if (ds) { style_open(f, ds, nm); cs = ds; }
                    if (!in_skip) handle_tag(s + i + 1, j - i - 1);
                } else if (!in_skip) {                     /* <img>, <input>, <br>... */
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

static void draw_toolbar(void) {
    gfx_fill_rect(0, 0, W, BAR_H, RGB(242, 242, 242));
    gfx_fill_rect(0, BAR_H - 1, W, 1, RGB(200, 200, 200));
    ui_button(6, 6, 30, 24, "<", false);
    ui_button(40, 6, 30, 24, ">", false);
    ui_button(74, 6, 30, 24, "R", false);
    ui_button(108, 6, 30, 24, "H", false);
    ui_sunken(144, 6, W - 150, 24, C_WHITE);
    char shown[96];
    int maxc = (W - 162) / 8;
    int start = 0, len = strlen(addr);
    if (editing_addr && addr_cur > maxc - 1) start = addr_cur - maxc + 1;
    snprintf(shown, sizeof(shown), "%.*s", maxc, addr + start);
    (void)len;
    if (editing_addr && addr_all && addr[0]) {
        int l = strlen(shown);
        gfx_fill_rect(149, 9, l * 8 + 2, 18, UI_ACCENT);
        gfx_text(150, 10, shown, C_WHITE, TRANSPARENT);
    } else gfx_text(150, 10, shown, UI_TEXT, TRANSPARENT);
    if (editing_addr && !addr_all) gfx_fill_rect(150 + (addr_cur - start) * 8, 10, 2, 16, UI_TEXT);
}

static void draw_status(void) {
    gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, RGB(242, 242, 242));
    gfx_fill_rect(0, H - STATUS_H, W, 1, RGB(200, 200, 200));
    char s[100];
    snprintf(s, sizeof(s), "%.96s", hover_link[0] ? hover_link : status);
    gfx_text(6, H - STATUS_H + 2, s, UI_DARK, TRANSPARENT);
}

static void draw_page(void) {
    gfx_fill_rect(0, VIEW_Y, W, VIEW_H, page_bg >= 0 ? (color_t)page_bg : C_WHITE);
    for (int i = 0; i < n_boxes; i++) {                      /* фон и рамки блоков */
        struct box *b = &boxes[i];
        int y = b->y - scroll + VIEW_Y;
        if (y + b->h < VIEW_Y || y > VIEW_Y + VIEW_H || b->h <= 0) continue;
        if (b->bg >= 0) gfx_fill_rect(b->x, y, b->w, b->h, b->bg);
        if (b->bw[0]) gfx_fill_rect(b->x, y, b->w, b->bw[0], b->bc[0]);
        if (b->bw[2]) gfx_fill_rect(b->x, y + b->h - b->bw[2], b->w, b->bw[2], b->bc[2]);
        if (b->bw[3]) gfx_fill_rect(b->x, y, b->bw[3], b->h, b->bc[3]);
        if (b->bw[1]) gfx_fill_rect(b->x + b->w - b->bw[1], y, b->bw[1], b->h, b->bc[1]);
    }
    for (int i = n_items - 1; i >= 0; i--) {                 /* фон inline-block (внешние раньше внутренних) */
        struct item *it = &items[i];
        if (it->kind != IT_FILL) continue;
        int y = it->y - scroll + VIEW_Y, h = it->h + it->below;
        if (y + h < VIEW_Y - 32 || y > VIEW_Y + VIEW_H) continue;
        if (it->bg != TRANSPARENT) gfx_fill_rect(it->x, y, it->w, h, it->bg);
        if (it->color != TRANSPARENT) gfx_rect(it->x, y, it->w, h, it->color);
    }
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        int y = it->y - scroll + VIEW_Y;
        if (y + it->h < VIEW_Y - 32 || y > VIEW_Y + VIEW_H) continue;
        switch (it->kind) {
        case IT_TEXT:
            if (it->bg != TRANSPARENT) gfx_fill_rect(it->x, y, it->w, it->h, it->bg);
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
            if (it->scale == LS_CIRCLE) gfx_rect(it->x, y + 6, 5, 5, it->color);
            else gfx_fill_rect(it->x, y + 6, 5, 5, it->color);
            break;
        case IT_HR:
            gfx_fill_rect(it->x, y + 4, it->w, 1, RGB(170, 170, 170));
            gfx_fill_rect(it->x, y + 5, it->w, 1, RGB(230, 230, 230));
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
            int maxc = (it->w - 8) / 8, l = strlen(in->value), st = l > maxc - 1 ? l - maxc + 1 : 0;
            gfx_text(it->x + 4, y + 4, in->value + st, UI_TEXT, TRANSPARENT);
            if (focus_input == it->input) gfx_fill_rect(it->x + 4 + (l - st) * 8, y + 4, 2, 16, UI_TEXT);
            break;
        }
        case IT_BUTTON:
            ui_button(it->x, y, it->w, it->h, inputs[it->input].value, false);
            break;
        }
    }
    if (doc_h > VIEW_H) {                                    /* полоса прокрутки */
        int kh = VIEW_H * VIEW_H / doc_h;
        if (kh < 24) kh = 24;
        int ky = VIEW_Y + (VIEW_H - kh) * scroll / (doc_h - VIEW_H);
        gfx_fill_rect(W - SB_W, VIEW_Y, SB_W, VIEW_H, RGB(238, 238, 238));
        gfx_fill_rect(W - SB_W + 2, ky, SB_W - 4, kh, RGB(180, 180, 180));
    }
}

static void redraw(void) {
    draw_page();
    draw_toolbar();
    draw_status();
    win_update(win);
}

static void set_status(const char *s) {
    snprintf(status, sizeof(status), "%s", s);
    hover_link[0] = 0;
    draw_status();
    win_update_rect(win, 0, H - STATUS_H, W, STATUS_H);
}

/* ------------------------------------------------------------ загрузка */

static char *body;                 /* содержимое последнего ответа */
static int body_len;
static tls *conn;                  /* открытое HTTPS-соединение (иначе NULL) */
static char tls_err[96];           /* почему не вышло HTTPS */
static char secure[48];            /* "TLS 1.3, ChaCha20-Poly1305" для статуса */

static const char *req_method = "GET";   /* метод и тело следующего запроса (XMLHttpRequest/fetch из JS) */
static const char *req_body;
static int req_body_len;
static char resp_ctype[96];        /* Content-Type последнего ответа */

#define USER_AGENT "Mozilla/5.0 (MyOS; i386) MyOS-Browser/0.16"

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

static int net_read(int fd, void *b, int n) { return conn ? tls_read(conn, b, n) : read(fd, b, n); }
static int net_write(int fd, const void *b, int n) { return conn ? tls_write(conn, b, n) : write(fd, b, n); }

static const char *home_page =
    "<title>MyOS Browser</title>"
    "<center><h1>MyOS Browser</h1></center>"
    "<p>A small web browser written from scratch for MyOS: it speaks HTTP over the MyOS "
    "TCP/IP stack and lays out HTML by itself.</p>"
    "<form action=\"http://frogfind.com/\"><b>Search the web:</b> <input name=\"q\" size=\"40\"> "
    "<input type=\"submit\" value=\"Search\"></form>"
    "<p><i>(search goes through FrogFind, a free service that simplifies modern sites for old "
    "computers. https:// sites are opened directly with our own TLS 1.3; sites that only know "
    "older TLS are opened through FrogFind)</i></p>"
    "<p><b>JavaScript</b> works: the browser has its own JS engine and DOM. "
    "<a href=\"about:js\">JavaScript demo</a> | <a href=\"about:console\">JS console</a> "
    "<span id=\"jsinfo\">(JavaScript is off - press Ctrl+J)</span></p>"
    "<script>document.getElementById('jsinfo').textContent = '- enabled, ' + new Date().toLocaleString();</script>"
    "<h2>Try these</h2><ul>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first web site</a> - CERN, 1991</li>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search engine for vintage computers</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> - news headlines, plain HTML</li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: Operating system</a> (HTTPS)</li>"
    "<li><a href=\"https://ru.wikipedia.org/wiki/MS-DOS\">Wikipedia (ru): MS-DOS</a> - Russian is shown in Latin letters</li>"
    "<li><a href=\"" PROXY "https://en.wikipedia.org/wiki/Operating_system\">The same Wikipedia page via FrogFind</a> - simplified</li>"
    "<li><a href=\"https://example.com/\">https://example.com/</a> - a tiny page over HTTPS</li>"
    "<li><a href=\"https://lite.duckduckgo.com/lite/\">DuckDuckGo Lite</a> - search over HTTPS</li>"
    "<li><a href=\"http://neverssl.com/\">neverssl.com</a></li>"
    "<li><a href=\"http://localhost/\">http://localhost/</a> - this computer (start <b>httpd &amp;</b> in Terminal first)</li>"
    "<li><a href=\"file:/\">file:/</a> - files on the MyOS disk</li>"
    "</ul><hr><p>Keys: Ctrl+L - address, Backspace - back, PgUp/PgDn, arrows, Home/End - scroll, "
    "Ctrl+J - JavaScript on/off, Ctrl+K - JavaScript console, Ctrl+U - page styles (CSS) on/off.</p>";

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
    struct url_parts p;
    if (!parse_url(u, &p)) return -1;
    *is_html = true;
    location[0] = 0;
    body_len = 0;
    resp_ctype[0] = 0;
    if (p.about) {
        if (!strcmp(p.path, "console")) { body_len = console_page(body, MAX_PAGE); return 200; }
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
        if (stat(path, &st) < 0) { set_status("file not found"); return 404; }
        if (st.type == 2) {
            int o = snprintf(body, MAX_PAGE, "<title>%s</title><h1>Index of %s</h1><ul>", path, path);
            struct dirent_info e;
            for (int i = 0; readdir(path, i, &e) == 1 && o < MAX_PAGE - 512; i++) {
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
    snprintf(msg, sizeof(msg), "Connecting to %s...", p.host);
    set_status(msg);
    int fd = tcp_connect(p.host, p.port);
    if (fd < 0) {
        snprintf(msg, sizeof(msg), "%s: %s", p.host, fd == -ENOENT ? "host not found" : strerror(-fd));
        set_status(msg);
        return -1;
    }
    secure[0] = 0;
    if (p.https) {                                          /* HTTPS: рукопожатие TLS 1.3 */
        snprintf(msg, sizeof(msg), "TLS handshake with %s...", p.host);
        set_status(msg);
        conn = tls_open(fd, p.host, tls_err, sizeof(tls_err));
        if (!conn) { close(fd); return -2; }
        snprintf(secure, sizeof(secure), "%s", tls_info(conn));
    }
    char cookies[2048], req[4096];
    cookie_get(p.host, cookies, sizeof(cookies));
    int rl = snprintf(req, sizeof(req),
        "%s %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
        "Accept: text/html, text/plain, */*\r\nAccept-Language: ru, en\r\nConnection: close\r\n",
        req_method, p.path, p.host);
    if (cookies[0]) rl += snprintf(req + rl, sizeof(req) - rl, "Cookie: %s\r\n", cookies);
    if (req_body) rl += snprintf(req + rl, sizeof(req) - rl, "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: %d\r\n", req_body_len);
    rl += snprintf(req + rl, sizeof(req) - rl, "\r\n");
    net_write(fd, req, rl);
    if (req_body && req_body_len) net_write(fd, req_body, req_body_len);
    int n, total = 0, expect = -1;                       /* expect: где кончится ответ по Content-Length */
    unsigned last = ticks();
    while (total < MAX_PAGE && (n = net_read(fd, body + total, MAX_PAGE - total)) > 0) {
        total += n;
        if (expect < 0) {                                   /* заголовки пришли? узнаем длину */
            body[total] = 0;
            char *he = strstr(body, "\r\n\r\n");
            if (he) {
                expect = 0;
                for (char *p2 = body; p2 < he; p2++)
                    if ((*p2 == 'C' || *p2 == 'c') && !strncmp(p2 + 1, "ontent-", 7) &&
                        (p2[8] == 'L' || p2[8] == 'l') && !strncmp(p2 + 9, "ength:", 6)) {
                        expect = (he - body) + 4 + atoi(p2 + 15);
                        break;
                    }
            }
        }
        if (expect > 0 && total >= expect) break;           /* всё получили — не ждём закрытия */
        if (ticks() - last > 300) {
            last = ticks();
            snprintf(msg, sizeof(msg), "Reading %s... %d KB", p.host, total / 1024);
            set_status(msg);
        }
    }
    if (conn) { tls_close(conn); conn = NULL; }
    close(fd);
    /* заголовки */
    int hend = -1;
    for (int i = 0; i + 3 < total; i++)
        if (body[i] == '\r' && body[i + 1] == '\n' && body[i + 2] == '\r' && body[i + 3] == '\n') { hend = i; break; }
    if (hend < 0) { set_status("bad response"); return -1; }
    body[hend] = 0;
    int code = 0;
    char *sp = strchr(body, ' ');
    if (sp) code = atoi(sp + 1);
    bool chunked = false;
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
        if (val && !strcmp(low, "transfer-encoding") && strstr(val, "chunked")) chunked = true;
        line = eol ? eol + 2 : NULL;
    }
    int start = hend + 4;
    memmove(body, body + start, total - start);
    body_len = total - start;
    if (chunked) {                                          /* склеить куски «размер\r\nданные\r\n» */
        int r = 0, w = 0;
        while (r < body_len) {
            int size = 0;
            while (r < body_len && isalnum(body[r])) {
                char c = tolower(body[r++]);
                size = size * 16 + (isdigit(c) ? c - '0' : c - 'a' + 10);
            }
            while (r < body_len && body[r] != '\n') r++;
            r++;
            if (size == 0) break;
            if (r + size > body_len) size = body_len - r;
            memmove(body + w, body + r, size);
            w += size; r += size + 2;
        }
        body_len = w;
    }
    return code;
}

static void navigate(const char *target, bool add_history);
static int nav_depth;
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
static int js_fetch(const char *u, const char *method, const char *data, int dlen,
                    char **out, int *len, int *status, char *ctype, int cmax) {
    char cur[1024], loc[512];
    bool html;
    *out = NULL;
    *len = 0;
    if (strncmp(u, "http://", 7) && strncmp(u, "https://", 8) && strncmp(u, "file:", 5)) return -1;
    snprintf(cur, sizeof(cur), "%s", u);
    req_method = method && *method ? method : "GET";
    req_body = data;
    req_body_len = dlen;
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
    req_method = "GET";
    req_body = NULL;
    req_body_len = 0;
    if (!nav_depth) set_status(page_status);              /* запрос из скрипта после загрузки — вернуть «Done» */
    if (code < 0) return -1;
    *out = malloc(body_len + 1);
    if (!*out) return -1;
    memcpy(*out, body, body_len);
    (*out)[body_len] = 0;
    *len = body_len;
    *status = code;
    snprintf(ctype, cmax, "%s", resp_ctype);
    return 0;
}

static void js_status(const char *m) { set_status(m); }

static struct dom_host dom_host = { js_fetch, js_status, console_add };

static bool page_is_dom;            /* страница построена через DOM (а не простой текст) */
static unsigned last_layout;

/* дерево DOM -> раскладка; keep — сохранить прокрутку и поле ввода */
static void relayout(bool keep) {
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
    if (code < 0) {
        body_len = snprintf(body, MAX_PAGE, "<title>Error</title><h1>Cannot open this page</h1><p>See the status line below. "
                            "Check the address, or go <a href=\"about:home\">home</a>.</p>%s%s%s",
                            fallback[0] ? "<p><b>HTTPS:</b> " : "", fallback, fallback[0] ? "</p>" : "");
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
        dom_load(body, body_len, url);
        relayout(false);
        nav_depth--;
    } else layout_html(body, body_len, true);
    scroll = 0;
    if (code >= 0) {
        if (code == 200) snprintf(status, sizeof(status), "Done: %d KB, %d links", body_kb, n_links);
        else snprintf(status, sizeof(status), "HTTP %d", code);
        int sl = strlen(status);
        if (html) {
            char js[64];
            dom_stats(js, sizeof(js));
            sl += snprintf(status + sl, sizeof(status) - sl, " - %s%s", js, strstr(js, "error") ? " (Ctrl+K)" : "");
        }
        if (secure[0]) snprintf(status + sl, sizeof(status) - sl, " - %s (certificate not checked)", secure);
        else if (fallback[0]) snprintf(status + sl, sizeof(status) - sl, " - %s", fallback);
    }
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
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        if (x >= it->x && x < it->x + it->w && dy >= it->y && dy < it->y + it->h + it->below) {
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

static void on_click(int x, int y) {
    if (y < BAR_H) {
        editing_addr = false;
        if (ui_inside(x, y, 6, 6, 30, 24)) { go_history(-1); return; }
        if (ui_inside(x, y, 40, 6, 30, 24)) { go_history(1); return; }
        if (ui_inside(x, y, 74, 6, 30, 24)) { navigate(url, false); return; }
        if (ui_inside(x, y, 108, 6, 30, 24)) { navigate(HOME, true); return; }
        if (x >= 144) { addr_all = !editing_addr; editing_addr = true; focus_input = -1; addr_cur = strlen(addr); }
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

static void edit_string(char *s, int max, int *cur, int k) {
    int l = strlen(s);
    if (k == KEY_LEFT && *cur > 0) (*cur)--;
    else if (k == KEY_RIGHT && *cur < l) (*cur)++;
    else if (k == KEY_HOME) *cur = 0;
    else if (k == KEY_END) *cur = l;
    else if ((k == 8 || k == 127) && *cur > 0) { memmove(s + *cur - 1, s + *cur, l - *cur + 1); (*cur)--; }
    else if (k == KEY_DEL && *cur < l) memmove(s + *cur, s + *cur + 1, l - *cur);
    else if (k >= 32 && k < 127 && l < max - 1) { memmove(s + *cur + 1, s + *cur, l - *cur + 1); s[(*cur)++] = k; }
}

static void on_key(int k) {
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
        if (k == '\n' || k == '\r') { editing_addr = false; navigate(addr, true); return; }
        if (k == 27) { editing_addr = false; snprintf(addr, sizeof(addr), "%s", url); redraw(); return; }
        if (addr_all) {
            addr_all = false;
            if ((k >= 32 && k < 127) || k == 8 || k == 127 || k == KEY_DEL) { addr[0] = 0; addr_cur = 0; if (k < 32 || k >= 127) k = 0; }
        }
        if (k) edit_string(addr, sizeof(addr), &addr_cur, k);
        draw_toolbar();
        win_update_rect(win, 0, 0, W, BAR_H);
        return;
    }
    if (focus_input >= 0) {
        struct input *in = &inputs[focus_input];
        bool js = page_is_dom && dom_scripting && in->node >= 0;
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
        edit_string(in->value, sizeof(in->value), &cur, k);
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
    items = malloc(sizeof(struct item) * MAX_ITEMS);
    boxes = malloc(sizeof(struct box) * MAX_BOXES);
    init_styles();
    body = malloc(MAX_PAGE + 1);
    arena_cap = 1024 * 1024;
    arena = malloc(arena_cap);
    if (!items || !boxes || !body || !arena) { dprintf(STDERR, "browser: out of memory\n"); return 1; }
    dom_init(&dom_host);
    navigate(argc > 1 ? argv[1] : HOME, true);
    struct event e;
    for (;;) {
        int wait = page_is_dom ? dom_next_timer() : -1;
        if (page_is_dom && dom_dirty) {                    /* отложенная перерисовка */
            int left = 100 - (int)(ticks() - last_layout);
            if (left < 0) left = 0;
            if (wait < 0 || left < wait) wait = left;
        }
        if (!win_event(&e, wait)) break;
        if (e.type == EV_TIMEOUT) {                        /* setTimeout / setInterval */
            dom_run_timers();
            after_js_ex(true);
            continue;
        }
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_DOWN && (e.buttons & MOUSE_LEFT)) on_click(e.x, e.y);
        else if (e.type == EV_MOUSE_MOVE && !e.buttons) on_move(e.x, e.y);
        else if (e.type == EV_KEY) on_key(e.key);
    }
    win_destroy(win);
    return 0;
}
