/* browser [адрес] — веб-браузер MyOS.
 *
 * Что умеет: HTTP (сам качает страницу через TCP/IP MyOS), разбирает HTML и
 * раскладывает текст по строкам: заголовки, абзацы, списки, ссылки, жирный,
 * <pre>, таблицы (упрощённо), горизонтальные линии, формы с полями ввода
 * (поиск работает). Переходы по ссылкам и редиректам, «Назад»/«Вперёд».
 * Картинки показываются рамкой с подписью.
 *
 * HTTPS (шифрования) в MyOS нет, поэтому https-адреса открываются через
 * FrogFind — сервис для старых компьютеров: он сам скачивает страницу по
 * HTTPS и отдаёт её упрощённой, по обычному HTTP. Там же есть поиск.
 *
 * Шрифт без кириллицы, поэтому русский текст показывается латиницей (транслит).
 * Адрес file:/путь открывает HTML-файл с диска MyOS. */
#include "win.h"

#define W 800
#define H 576
#define BAR_H 36
#define STATUS_H 20
#define VIEW_Y BAR_H
#define VIEW_H (H - BAR_H - STATUS_H)
#define MARGIN 12
#define SB_W 12
#define MAX_PAGE (768 * 1024)
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

enum { IT_TEXT, IT_HR, IT_BOX, IT_INPUT, IT_BUTTON, IT_BULLET };

struct item {
    int  x, y, w, h;
    unsigned char kind, scale, bold, underline;
    color_t color;
    int  link;                     /* номер ссылки или -1 */
    int  input;                    /* IT_INPUT / IT_BUTTON: номер поля */
    char *text;
};

#define MAX_ITEMS 12000
#define MAX_LINKS 1500
#define MAX_INPUTS 64
#define MAX_FORMS 16

static struct item *items;
static int n_items;
static char *links[MAX_LINKS];
static int n_links;
static int doc_h, scroll;

struct input { int form; char name[40]; char value[128]; char type; int item; };   /* type: t текст, s кнопка, h скрытое */
static struct input inputs[MAX_INPUTS];
static int n_inputs, focus_input = -1;
static char form_action[MAX_FORMS][256];
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

/* ------------------------------------------------------------ раскладка (layout) */

static int view_w(void) { return W - SB_W; }

static int lx, ly, line_h, line_start;    /* курсор раскладки */
static int left, right;
static bool space_pending;
static int gap_pending;                  /* отступ перед следующим блоком */
static int bold, italic, underline_n, pre, heading, center_n;
static int cur_link = -1;
static color_t text_color;
static int list_depth;
static int list_counter[8];
static bool list_ordered[8];
static bool in_title, in_skip;           /* <title>, <script>/<style> */

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
    if (h > line_h) line_h = h;
    return it;
}

static void finish_line(void) {
    if (lx == left && line_start == n_items) return;
    int lh = line_h ? line_h : 16;
    int shift = 0;
    if (center_n) {                                        /* <center>: сдвинуть строку к середине */
        int used = 0;
        for (int i = line_start; i < n_items; i++) if (items[i].x + items[i].w > used) used = items[i].x + items[i].w;
        shift = (right - used) / 2;
        if (shift < 0) shift = 0;
    }
    for (int i = line_start; i < n_items; i++) { items[i].y = ly + (lh - items[i].h); items[i].x += shift; }
    ly += lh;
    lx = left;
    line_h = 0;
    line_start = n_items;
    space_pending = false;
}

static void block(int gap) {
    finish_line();
    if (gap > gap_pending) gap_pending = gap;
}

static void apply_gap(void) {
    if (gap_pending && ly > 8) ly += gap_pending;
    gap_pending = 0;
}

static int cur_scale(void) { return heading == 1 ? 2 : 1; }

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
    lx += space;
    struct item *it = add_item(IT_TEXT, width, 16 * sc);
    if (!it) return;
    it->text = astrdup(w, n);
    it->scale = sc;
    it->bold = bold > 0 || heading > 0;
    it->link = cur_link;
    it->underline = cur_link >= 0 || underline_n > 0;
    it->color = cur_link >= 0 ? RGB(0, 0, 238) : heading ? RGB(20, 20, 60) : italic ? RGB(80, 80, 80) : text_color;
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
    int len = decode_text(s, n, buf, sizeof(buf));
    if (pre) {                                             /* <pre>: пробелы и переводы строк как есть */
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
                        it->color = text_color;
                        it->link = cur_link;
                        it->underline = cur_link >= 0;
                        if (cur_link >= 0) it->color = RGB(0, 0, 238);
                    }
                    lx += w;
                }
                if (i < len) { line_h = 16; finish_line(); }
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
    if (IS("p") || IS("div") || IS("section") || IS("article") || IS("header") || IS("footer") ||
        IS("nav") || IS("main") || IS("aside") || IS("form") || IS("table") || IS("figure")) {
        block(IS("p") ? 12 : 4);
        if (IS("form")) {
            if (!close && n_forms < MAX_FORMS) {
                char action[256];
                get_attr(t, n, "action", action, sizeof(action));
                resolve_url(url, action[0] ? action : url, form_action[n_forms], 256);
                cur_form = n_forms++;
            } else if (close) cur_form = -1;
        }
        return;
    }
    if (name[0] == 'h' && name[1] >= '1' && name[1] <= '6' && !name[2]) {
        block(close ? 8 : 16);
        heading = close ? 0 : name[1] - '0';
        return;
    }
    if (IS("b") || IS("strong")) { bold += close ? -1 : 1; if (bold < 0) bold = 0; return; }
    if (IS("i") || IS("em")) { italic += close ? -1 : 1; if (italic < 0) italic = 0; return; }
    if (IS("u")) { underline_n += close ? -1 : 1; if (underline_n < 0) underline_n = 0; return; }
    if (IS("center")) { block(4); center_n += close ? -1 : 1; if (center_n < 0) center_n = 0; return; }
    if (IS("pre") || IS("listing") || IS("xmp")) { block(8); pre += close ? -1 : 1; if (pre < 0) pre = 0; return; }
    if (IS("blockquote") || IS("dd")) {
        block(6);
        left += close ? -32 : 32;
        if (left < MARGIN) left = MARGIN;
        lx = left;
        return;
    }
    if (IS("ul") || IS("ol") || IS("dl") || IS("menu")) {
        block(close ? 8 : 4);
        if (!close && list_depth < 8) {
            list_ordered[list_depth] = IS("ol");
            list_counter[list_depth] = 0;
            list_depth++;
            left += 28;
        } else if (close && list_depth > 0) {
            list_depth--;
            left -= 28;
        }
        lx = left;
        return;
    }
    if (IS("li")) {
        if (close) return;
        block(2);
        apply_gap();
        int d = list_depth ? list_depth - 1 : 0;
        if (list_depth && list_ordered[d]) {
            char num[12];
            snprintf(num, sizeof(num), "%d.", ++list_counter[d]);
            int w = strlen(num) * 8;
            lx = left - w - 6;
            struct item *it = add_item(IT_TEXT, w, 16);
            if (it) { it->text = astrdup(num, strlen(num)); it->scale = 1; it->color = text_color; }
        } else {
            lx = left - 14;
            add_item(IT_BULLET, 6, 16);
        }
        lx = left;
        space_pending = false;
        return;
    }
    if (IS("dt")) { block(4); return; }
    if (IS("tr")) { finish_line(); return; }
    if (IS("td") || IS("th")) {
        if (!close && lx > left) {                         /* колонки — по «табуляции» через 88 точек */
            int stop = left + ((lx - left + 12) / 88 + 1) * 88;
            lx = stop < right ? stop : lx + 16;
            space_pending = false;
        }
        if (IS("th")) bold += close ? -1 : 1;
        if (bold < 0) bold = 0;
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
            cur_link = n_links++;
        }
        return;
    }
    if (IS("img")) {
        if (close) return;
        char alt[80];
        get_attr(t, n, "alt", alt, sizeof(alt));
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
    if (IS("select")) { in_skip = !close; if (!close) add_text("[...]", 5); return; }
    if (IS("meta")) {
        char cs[64];
        if (get_attr(t, n, "charset", cs, sizeof(cs)) || get_attr(t, n, "content", cs, sizeof(cs)))
            for (char *p = cs; *p; p++) if (!strncmp(p, "1251", 4)) cp1251 = true;
        return;
    }
    #undef IS
}

/* разобрать HTML и разложить */
static void layout_html(const char *s, int n, bool plain) {
    n_items = 0; n_links = 0; n_inputs = 0; n_forms = 0; cur_form = -1;
    focus_input = -1;
    left = MARGIN; right = view_w() - MARGIN;
    lx = left; ly = 8; line_h = 0; line_start = 0;
    space_pending = false; gap_pending = 0;
    bold = italic = underline_n = heading = center_n = 0;
    pre = plain ? 1 : 0;
    cur_link = -1; list_depth = 0;
    in_title = in_skip = false;
    text_color = RGB(20, 20, 20);
    page_title[0] = 0;
    if (plain) { add_text(s, n); finish_line(); doc_h = ly + 16; return; }
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
            if (in_skip) {                                  /* внутри <script>: ищем только закрывающий тег */
                if (s[i + 1] == '/') handle_tag(s + i + 1, j - i - 1);
            } else if (s[i + 1] != '!' && s[i + 1] != '?') {
                handle_tag(s + i + 1, j - i - 1);
            }
            i = j + 1;
            continue;
        }
        int j = i;
        while (j < n && s[j] != '<') j++;
        add_text(s + i, j - i);
        i = j;
    }
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
    if (strstr(rel, "://") || !strncmp(rel, "about:", 6) || !strncmp(rel, "file:", 5) || !strncmp(rel, "mailto:", 7)) {
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
    gfx_fill_rect(0, VIEW_Y, W, VIEW_H, C_WHITE);
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        int y = it->y - scroll + VIEW_Y;
        if (y + it->h < VIEW_Y - 32 || y > VIEW_Y + VIEW_H) continue;
        switch (it->kind) {
        case IT_TEXT:
            if (it->scale > 1) {
                gfx_text_scaled(it->x, y, it->text, it->color, it->scale);
                if (it->bold) gfx_text_scaled(it->x + 1, y, it->text, it->color, it->scale);
            } else {
                gfx_text(it->x, y, it->text, it->color, TRANSPARENT);
                if (it->bold) gfx_text(it->x + 1, y, it->text, it->color, TRANSPARENT);
            }
            if (it->underline) gfx_fill_rect(it->x, y + it->h - 2, it->w, 1, it->color);
            break;
        case IT_BULLET:
            gfx_fill_rect(it->x, y + 6, 5, 5, RGB(60, 60, 60));
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

static const char *home_page =
    "<title>MyOS Browser</title>"
    "<center><h1>MyOS Browser</h1></center>"
    "<p>A small web browser written from scratch for MyOS: it speaks HTTP over the MyOS "
    "TCP/IP stack and lays out HTML by itself.</p>"
    "<form action=\"http://frogfind.com/\"><b>Search the web:</b> <input name=\"q\" size=\"40\"> "
    "<input type=\"submit\" value=\"Search\"></form>"
    "<p><i>(search and https:// sites go through FrogFind, a free service that simplifies modern "
    "sites for old computers - MyOS has no HTTPS yet)</i></p>"
    "<h2>Try these</h2><ul>"
    "<li><a href=\"http://info.cern.ch/hypertext/WWW/TheProject.html\">The first web site</a> - CERN, 1991</li>"
    "<li><a href=\"http://frogfind.com/\">FrogFind</a> - search engine for vintage computers</li>"
    "<li><a href=\"http://68k.news/\">68k.news</a> - news headlines, plain HTML</li>"
    "<li><a href=\"https://en.wikipedia.org/wiki/Operating_system\">Wikipedia: Operating system</a> (via FrogFind)</li>"
    "<li><a href=\"https://ru.wikipedia.org/wiki/MS-DOS\">Wikipedia (ru): MS-DOS</a> - Russian is shown in Latin letters</li>"
    "<li><a href=\"http://example.com/\">example.com</a></li>"
    "<li><a href=\"http://neverssl.com/\">neverssl.com</a></li>"
    "<li><a href=\"http://localhost/\">http://localhost/</a> - this computer (start <b>httpd &amp;</b> in Terminal first)</li>"
    "<li><a href=\"file:/\">file:/</a> - files on the MyOS disk</li>"
    "</ul><hr><p>Keys: Ctrl+L - address, Backspace - back, PgUp/PgDn, arrows, Home/End - scroll.</p>";

static int http_get(const char *u, char *location, int loc_max, bool *is_html) {
    struct url_parts p;
    if (!parse_url(u, &p)) return -1;
    *is_html = true;
    location[0] = 0;
    body_len = 0;
    if (p.about) {
        int n = strlen(home_page);
        memcpy(body, home_page, n);
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
    char req[1024];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: MyOS-Browser/0.12 (Lynx-like)\r\n"
        "Accept: text/html, text/plain, */*\r\nAccept-Language: ru, en\r\nConnection: close\r\n\r\n",
        p.path, p.host);
    write(fd, req, rl);
    int n, total = 0, expect = -1;                       /* expect: где кончится ответ по Content-Length */
    unsigned last = ticks();
    while (total < MAX_PAGE && (n = read(fd, body + total, MAX_PAGE - total)) > 0) {
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
        if (val && !strcmp(low, "content-type")) {
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
    for (int hop = 0; hop < 6; hop++) {
        if (!strncmp(u, "https://", 8)) {                 /* шифрования нет — через FrogFind */
            char enc[400], t[512];
            url_encode(u, enc, sizeof(enc));
            snprintf(t, sizeof(t), PROXY "%s", enc);
            snprintf(u, sizeof(u), "%s", t);
        }
        snprintf(url, sizeof(url), "%s", u);
        snprintf(addr, sizeof(addr), "%s", u);
        draw_toolbar();
        win_update_rect(win, 0, 0, W, BAR_H);
        code = http_get(u, loc, sizeof(loc), &html);
        if (code >= 300 && code < 400 && loc[0]) {
            resolve_url(u, loc, u, sizeof(u));
            continue;
        }
        break;
    }
    arena_used = 0;
    if (code < 0) {
        const char *err = "<title>Error</title><h1>Cannot open this page</h1><p>See the status line below. "
                          "Check the address, or go <a href=\"about:home\">home</a>.</p>";
        body_len = strlen(err);
        memcpy(body, err, body_len);
        html = true;
    }
    layout_html(body, body_len, !html);
    scroll = 0;
    char t[96];
    snprintf(t, sizeof(t), "%.60s - Browser", page_title[0] ? page_title : url);
    win_set_title(win, t);
    if (code >= 0) {
        snprintf(status, sizeof(status), code == 200 ? "Done: %d KB, %d links" : "HTTP %d", code == 200 ? body_len / 1024 : code, n_links);
        if (code == 200) snprintf(status, sizeof(status), "Done: %d KB, %d links", body_len / 1024, n_links);
    }
    hover_link[0] = 0;
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
    int dy = y - VIEW_Y + scroll;
    for (int i = 0; i < n_items; i++) {
        struct item *it = &items[i];
        if (x >= it->x && x < it->x + it->w && dy >= it->y && dy < it->y + it->h) return i;
    }
    return -1;
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
        if (it->kind == IT_BUTTON) { submit_form(inputs[it->input].form); return; }
        if (it->link >= 0) {
            char target[512];
            snprintf(target, sizeof(target), "%s", links[it->link]);
            if (!strncmp(target, "mailto:", 7)) { set_status("mail links are not supported"); return; }
            navigate(target, true);
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
        if (k == '\n' || k == '\r') { submit_form(in->form); return; }
        if (k == 27) { focus_input = -1; redraw(); return; }
        int cur = strlen(in->value);
        edit_string(in->value, sizeof(in->value), &cur, k);
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
    body = malloc(MAX_PAGE + 1);
    arena_cap = 1024 * 1024;
    arena = malloc(arena_cap);
    if (!items || !body || !arena) { dprintf(STDERR, "browser: out of memory\n"); return 1; }
    navigate(argc > 1 ? argv[1] : HOME, true);
    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_DOWN && (e.buttons & MOUSE_LEFT)) on_click(e.x, e.y);
        else if (e.type == EV_MOUSE_MOVE && !e.buttons) on_move(e.x, e.y);
        else if (e.type == EV_KEY) on_key(e.key);
    }
    win_destroy(win);
    return 0;
}
