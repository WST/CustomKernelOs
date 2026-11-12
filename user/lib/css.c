/* css.c — стили страницы (CSS) для браузера MyOS.
 *
 * Что делает: собирает таблицы стилей страницы — <style>, <link rel=stylesheet>
 * (скачивает их, а также @import), атрибуты style="" — и для каждого элемента
 * вычисляет стиль по правилам каскада: специфичность селекторов, порядок,
 * !important, наследование, переменные var(--x), единицы px/em/rem/%/vw/pt,
 * calc()/min()/max()/clamp(), @media для окна шириной 788 точек.
 *
 * Свойств понимаем немного — те, что может показать наша раскладка: display,
 * visibility, цвет текста и фона, жирность, размер шрифта (крупный — вдвое),
 * подчёркивание, выравнивание, white-space, text-transform, маркеры списков,
 * отступы (margin/padding), рамки, width/max-width (колонка по центру), и
 * типичные способы спрятать элемент (position:absolute за краем экрана, clip,
 * height:0 + overflow:hidden). Flex-контейнер в строку ставит детей в ряд.
 * Остальное (float, grid-раскладка, позиционирование, картинки фона) — нет. */
#include "domint.h"

int dom_css = 1;

int dom_view_width = 788, dom_view_height = 520;
#define VIEW_W dom_view_width
#define VIEW_H dom_view_height

/* ------------------------------------------------------------------ свойства */

enum { S_DISPLAY, S_VIS, S_POINTER, S_COLOR, S_BG, S_FW, S_FSTYLE, S_FSIZE, S_FAMILY, S_SHADOW, S_MASKIMG, S_MASKSIZE, S_MASKPOS, S_MASKREPEAT, S_MASKORIGIN, S_MASKCLIP, S_MASKMODE, S_MASKCOMPOSITE, S_TDEC, S_TALIGN, S_TTRANS, S_WS, S_LIST,
       S_MT, S_MR, S_MB, S_ML, S_PT, S_PR, S_PB, S_PL,
       S_BTW, S_BRW, S_BBW, S_BLW, S_BTC, S_BRC, S_BBC, S_BLC, S_BTS, S_BRS, S_BBS, S_BLS,
       S_WIDTH, S_MAXW, S_MINW, S_FLOAT, S_CLEAR, S_ZINDEX, S_HEIGHT, S_MAXH, S_OVERFLOW, S_POS, S_LEFT, S_TOP, S_CLIP, S_CLIPPATH, S_TINDENT,
       S_FLEXDIR, S_BGIMG, S_BGSIZE, S_BGPOS, S_BGREPEAT, S_BGORIGIN, S_BGCLIP, S_OBJECTFIT, S_OBJECTPOS, S_WORDWRAP, S_WORDBREAK, S_BOXSIZING, S_COLGAP, S_ROWGAP, S_BASIS, S_FLEXWRAP, S_GROW, S_SHRINK, S_JUSTIFY, S_ALIGNITEMS, S_ALIGNSELF, S_MINH, S_LINEHEIGHT, S_GRIDCOLS, S_RADIUS, S_RIGHT, S_BOTTOM, S_OPACITY, S_TRANSLATE, S_TRANSFORM, S_COUNT };

/* как достать значение своего свойства из сокращённой записи */
enum { SH_NONE, SH_BOX, SH_PAIR, SH_BG, SH_FONT, SH_BORDER, SH_LIST, SH_TDEC, SH_FIRST, SH_FLEX, SH_BGEXTRA, SH_MASKEXTRA, SH_MASKMODE };

struct pmap { unsigned char slot, sh, idx; };
struct propdef { const char *name; unsigned char n; struct pmap m[12]; };

#define BOX4(a, b, c, d) 4, { { a, SH_BOX, 0 }, { b, SH_BOX, 1 }, { c, SH_BOX, 2 }, { d, SH_BOX, 3 } }
#define ONE(a) 1, { { a, SH_NONE, 0 } }
#define SIDE(w, c, s) 3, { { w, SH_BORDER, 0 }, { c, SH_BORDER, 1 }, { s, SH_BORDER, 2 } }

static const struct propdef props[] = {
    {"pointer-events",ONE(S_POINTER)},
    { "display", ONE(S_DISPLAY) }, { "visibility", ONE(S_VIS) }, { "color", ONE(S_COLOR) },
    { "background-color", ONE(S_BG) }, { "background", 7, { { S_BG, SH_BG, 0 }, { S_BGIMG, SH_NONE, 0 },
      {S_BGSIZE,SH_BGEXTRA,0},{S_BGPOS,SH_BGEXTRA,1},{S_BGREPEAT,SH_BGEXTRA,2},
      {S_BGORIGIN,SH_BGEXTRA,3},{S_BGCLIP,SH_BGEXTRA,4} } },
    { "background-image", ONE(S_BGIMG) }, {"background-size",ONE(S_BGSIZE)},
    {"background-position",ONE(S_BGPOS)}, {"background-repeat",ONE(S_BGREPEAT)},
    {"background-origin",ONE(S_BGORIGIN)}, {"background-clip",ONE(S_BGCLIP)},
    {"object-fit",ONE(S_OBJECTFIT)}, {"object-position",ONE(S_OBJECTPOS)},
    {"overflow-wrap",ONE(S_WORDWRAP)}, {"word-wrap",ONE(S_WORDWRAP)}, {"word-break",ONE(S_WORDBREAK)},
    {"mask",8,{{S_MASKIMG,SH_NONE,0},{S_MASKSIZE,SH_MASKEXTRA,0},{S_MASKPOS,SH_MASKEXTRA,1},
      {S_MASKREPEAT,SH_MASKEXTRA,2},{S_MASKORIGIN,SH_MASKEXTRA,3},{S_MASKCLIP,SH_MASKEXTRA,4},
      {S_MASKMODE,SH_MASKMODE,0},{S_MASKCOMPOSITE,SH_MASKMODE,1}}},
    {"-webkit-mask",8,{{S_MASKIMG,SH_NONE,0},{S_MASKSIZE,SH_MASKEXTRA,0},{S_MASKPOS,SH_MASKEXTRA,1},
      {S_MASKREPEAT,SH_MASKEXTRA,2},{S_MASKORIGIN,SH_MASKEXTRA,3},{S_MASKCLIP,SH_MASKEXTRA,4},
      {S_MASKMODE,SH_MASKMODE,0},{S_MASKCOMPOSITE,SH_MASKMODE,1}}},
    {"mask-image",ONE(S_MASKIMG)},{"-webkit-mask-image",ONE(S_MASKIMG)},
    {"mask-size",ONE(S_MASKSIZE)},{"-webkit-mask-size",ONE(S_MASKSIZE)},
    {"mask-position",ONE(S_MASKPOS)},{"-webkit-mask-position",ONE(S_MASKPOS)},
    {"mask-repeat",ONE(S_MASKREPEAT)},{"-webkit-mask-repeat",ONE(S_MASKREPEAT)},
    {"mask-origin",ONE(S_MASKORIGIN)},{"-webkit-mask-origin",ONE(S_MASKORIGIN)},
    {"mask-clip",ONE(S_MASKCLIP)},{"-webkit-mask-clip",ONE(S_MASKCLIP)},
    {"mask-mode",ONE(S_MASKMODE)},{"mask-composite",ONE(S_MASKCOMPOSITE)},
    {"-webkit-mask-composite",ONE(S_MASKCOMPOSITE)},
    {"box-shadow",ONE(S_SHADOW)},{"-webkit-box-shadow",ONE(S_SHADOW)},
    {"font-family",ONE(S_FAMILY)},
    { "font-weight", ONE(S_FW) }, { "font-style", ONE(S_FSTYLE) }, { "font-size", ONE(S_FSIZE) },
    { "font", 4, { { S_FW, SH_FONT, 0 }, { S_FSTYLE, SH_FONT, 1 }, { S_FSIZE, SH_FONT, 2 }, { S_FAMILY, SH_FONT, 3 } } },
    { "text-decoration", 1, { { S_TDEC, SH_TDEC, 0 } } }, { "text-decoration-line", 1, { { S_TDEC, SH_TDEC, 0 } } },
    { "text-align", ONE(S_TALIGN) }, { "text-transform", ONE(S_TTRANS) }, { "white-space", ONE(S_WS) },
    { "list-style-type", ONE(S_LIST) }, { "list-style", 1, { { S_LIST, SH_LIST, 0 } } },
    { "margin", BOX4(S_MT, S_MR, S_MB, S_ML) },
    { "margin-top", ONE(S_MT) }, { "margin-right", ONE(S_MR) }, { "margin-bottom", ONE(S_MB) }, { "margin-left", ONE(S_ML) },
    { "margin-block-start", ONE(S_MT) }, { "margin-inline-end", ONE(S_MR) }, { "margin-block-end", ONE(S_MB) },
    { "margin-inline-start", ONE(S_ML) },
    { "margin-inline", 2, { { S_ML, SH_PAIR, 0 }, { S_MR, SH_PAIR, 1 } } },
    { "margin-block", 2, { { S_MT, SH_PAIR, 0 }, { S_MB, SH_PAIR, 1 } } },
    { "padding", BOX4(S_PT, S_PR, S_PB, S_PL) },
    { "padding-top", ONE(S_PT) }, { "padding-right", ONE(S_PR) }, { "padding-bottom", ONE(S_PB) }, { "padding-left", ONE(S_PL) },
    { "padding-block-start", ONE(S_PT) }, { "padding-inline-end", ONE(S_PR) }, { "padding-block-end", ONE(S_PB) },
    { "padding-inline-start", ONE(S_PL) },
    { "padding-inline", 2, { { S_PL, SH_PAIR, 0 }, { S_PR, SH_PAIR, 1 } } },
    { "padding-block", 2, { { S_PT, SH_PAIR, 0 }, { S_PB, SH_PAIR, 1 } } },
    { "border", 12, { { S_BTW, SH_BORDER, 0 }, { S_BRW, SH_BORDER, 0 }, { S_BBW, SH_BORDER, 0 }, { S_BLW, SH_BORDER, 0 },
                      { S_BTC, SH_BORDER, 1 }, { S_BRC, SH_BORDER, 1 }, { S_BBC, SH_BORDER, 1 }, { S_BLC, SH_BORDER, 1 },
                      { S_BTS, SH_BORDER, 2 }, { S_BRS, SH_BORDER, 2 }, { S_BBS, SH_BORDER, 2 }, { S_BLS, SH_BORDER, 2 } } },
    { "border-top", SIDE(S_BTW, S_BTC, S_BTS) }, { "border-right", SIDE(S_BRW, S_BRC, S_BRS) },
    { "border-bottom", SIDE(S_BBW, S_BBC, S_BBS) }, { "border-left", SIDE(S_BLW, S_BLC, S_BLS) },
    { "border-block-start", SIDE(S_BTW, S_BTC, S_BTS) }, { "border-block-end", SIDE(S_BBW, S_BBC, S_BBS) },
    { "border-inline-start", SIDE(S_BLW, S_BLC, S_BLS) }, { "border-inline-end", SIDE(S_BRW, S_BRC, S_BRS) },
    { "border-width", BOX4(S_BTW, S_BRW, S_BBW, S_BLW) }, { "border-color", BOX4(S_BTC, S_BRC, S_BBC, S_BLC) },
    { "border-style", BOX4(S_BTS, S_BRS, S_BBS, S_BLS) },
    { "border-top-width", ONE(S_BTW) }, { "border-bottom-width", ONE(S_BBW) }, { "border-left-width", ONE(S_BLW) },
    { "border-right-width", ONE(S_BRW) }, { "border-top-color", ONE(S_BTC) }, { "border-bottom-color", ONE(S_BBC) },
    { "border-left-color", ONE(S_BLC) }, { "border-right-color", ONE(S_BRC) }, { "border-top-style", ONE(S_BTS) },
    { "border-bottom-style", ONE(S_BBS) }, { "border-left-style", ONE(S_BLS) }, { "border-right-style", ONE(S_BRS) },
    { "z-index", ONE(S_ZINDEX) }, { "float", ONE(S_FLOAT) }, { "clear", ONE(S_CLEAR) },
    { "min-width", ONE(S_MINW) }, { "min-inline-size", ONE(S_MINW) },
    { "width", ONE(S_WIDTH) }, { "max-width", ONE(S_MAXW) }, { "height", ONE(S_HEIGHT) }, { "max-height", ONE(S_MAXH) },
    { "inline-size", ONE(S_WIDTH) }, { "max-inline-size", ONE(S_MAXW) },
    { "overflow", 1, { { S_OVERFLOW, SH_FIRST, 0 } } }, { "overflow-y", ONE(S_OVERFLOW) },
    { "position", ONE(S_POS) }, { "left", ONE(S_LEFT) }, { "top", ONE(S_TOP) },
    { "right", ONE(S_RIGHT) }, { "bottom", ONE(S_BOTTOM) }, { "opacity", ONE(S_OPACITY) }, { "translate", ONE(S_TRANSLATE) }, { "transform", ONE(S_TRANSFORM) },
    { "inset-inline", 2, { { S_LEFT, SH_PAIR, 0 }, { S_RIGHT, SH_PAIR, 1 } } },
    { "inset-block", 2, { { S_TOP, SH_PAIR, 0 }, { S_BOTTOM, SH_PAIR, 1 } } },
    { "inset", BOX4(S_TOP,S_RIGHT,S_BOTTOM,S_LEFT) },
    { "clip", ONE(S_CLIP) }, { "clip-path", ONE(S_CLIPPATH) }, { "text-indent", ONE(S_TINDENT) },
    { "flex-direction", ONE(S_FLEXDIR) }, { "flex-flow", 2, { { S_FLEXDIR, SH_FIRST, 0 }, { S_FLEXWRAP, SH_NONE, 0 } } },
    { "flex-wrap", ONE(S_FLEXWRAP) }, { "-ms-flex-wrap", ONE(S_FLEXWRAP) },
    { "-webkit-box-orient", ONE(S_FLEXDIR) },
    { "box-sizing", ONE(S_BOXSIZING) }, { "-webkit-box-sizing", ONE(S_BOXSIZING) },
    { "column-gap", ONE(S_COLGAP) }, { "row-gap", ONE(S_ROWGAP) },
    { "gap", 2, { { S_ROWGAP, SH_PAIR, 0 }, { S_COLGAP, SH_PAIR, 1 } } },
    { "grid-template-columns", ONE(S_GRIDCOLS) }, { "border-radius", 1, { { S_RADIUS, SH_FIRST, 0 } } },
    { "justify-content", ONE(S_JUSTIFY) }, { "align-items", ONE(S_ALIGNITEMS) }, { "align-self", ONE(S_ALIGNSELF) },
    { "min-height", ONE(S_MINH) }, { "min-block-size", ONE(S_MINH) }, { "block-size", ONE(S_HEIGHT) },
    { "line-height", ONE(S_LINEHEIGHT) },
    { "flex-grow", ONE(S_GROW) }, { "flex-shrink", ONE(S_SHRINK) },
    { "flex", 3, { { S_GROW, SH_FLEX, 0 }, { S_SHRINK, SH_FLEX, 1 }, { S_BASIS, SH_FLEX, 2 } } },
    { "flex-basis", ONE(S_BASIS) }, { "-ms-flex-preferred-size", ONE(S_BASIS) },
};
#define N_PROPS ((int)(sizeof(props) / sizeof(props[0])))

/* наследуемые свойства */
static const unsigned char inherited[S_COUNT] = {
    [S_POINTER]=1, [S_FAMILY]=1, [S_VIS] = 1, [S_COLOR] = 1, [S_FW] = 1, [S_FSTYLE] = 1, [S_FSIZE] = 1, [S_TDEC] = 1, [S_TALIGN] = 1,
    [S_WORDWRAP]=1, [S_WORDBREAK]=1, [S_TTRANS] = 1, [S_WS] = 1, [S_LIST] = 1, [S_LINEHEIGHT] = 1,
};

#define PROP_HASH 256
static short prop_hash[PROP_HASH];      /* номер в props + 1, открытая адресация */

static unsigned khash(const char *s, int n) { unsigned h = 5381; for (int i = 0; i < n; i++) h = h * 33 + (ju8)s[i]; return h; }

static int prop_find(const char *name, int n) {
    if (!prop_hash[khash("display", 7) % PROP_HASH]) {
        for (int i = 0; i < N_PROPS; i++) {
            unsigned h = khash(props[i].name, strlen(props[i].name)) % PROP_HASH;
            while (prop_hash[h]) h = (h + 1) % PROP_HASH;
            prop_hash[h] = i + 1;
        }
    }
    unsigned h = khash(name, n) % PROP_HASH;
    while (prop_hash[h]) {
        const char *pn = props[prop_hash[h] - 1].name;
        if ((int)strlen(pn) == n && !strncmp(pn, name, n)) return prop_hash[h] - 1;
        h = (h + 1) % PROP_HASH;
    }
    return -1;
}

/* ------------------------------------------------------------------ память */

/* всё, что живёт до пересборки правил (объявления), — в rule_arena;
 * то, что живёт до изменения дерева (переменные, style="") — в tmp_arena */
struct arena { char *blk; int used, cap; struct arena *prev; };
static struct arena *rule_arena, *tmp_arena;

static void *ar_alloc(struct arena **a, int n) {
    n = (n + 7) & ~7;
    if (!*a || (*a)->used + n > (*a)->cap) {
        int cap = n > 65536 ? n : 65536;
        struct arena *b = malloc(sizeof(struct arena));
        b->blk = malloc(cap);
        b->used = 0;
        b->cap = cap;
        b->prev = *a;
        *a = b;
    }
    void *p = (*a)->blk + (*a)->used;
    (*a)->used += n;
    return p;
}

static char *ar_str(struct arena **a, const char *s, int n) {
    char *p = ar_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

static void ar_free(struct arena **a) {
    while (*a) { struct arena *p = (*a)->prev; free((*a)->blk); free(*a); *a = p; }
}

/* ------------------------------------------------------------------ мелочи разбора */

static int is_sp(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : (c = lc(c)) >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static int word_is(const char *s, int n, const char *w) {      /* без учёта регистра */
    int l = strlen(w);
    if (n != l) return 0;
    for (int i = 0; i < n; i++) if (lc(s[i]) != w[i]) return 0;
    return 1;
}

static int starts(const char *s, const char *w) {
    for (; *w; s++, w++) if (lc(*s) != *w) return 0;
    return 1;
}

/* число в начале строки: «-1.5e2» -> *v; -> сколько символов */
static int read_num(const char *s, double *v) {
    int i = 0, sign = 1, any = 0;
    double r = 0;
    if (s[i] == '-') { sign = -1; i++; } else if (s[i] == '+') i++;
    while (s[i] >= '0' && s[i] <= '9') { r = r * 10 + (s[i++] - '0'); any = 1; }
    if (s[i] == '.' && s[i + 1] >= '0' && s[i + 1] <= '9') {
        i++;
        double f = 0.1;
        while (s[i] >= '0' && s[i] <= '9') { r += (s[i++] - '0') * f; f /= 10; any = 1; }
    }
    if (!any) return 0;
    if ((s[i] == 'e' || s[i] == 'E') && ((s[i + 1] >= '0' && s[i + 1] <= '9') || ((s[i + 1] == '-' || s[i + 1] == '+') && s[i + 2] >= '0' && s[i + 2] <= '9'))) {
        int j = i + 1, es = 1, e = 0;
        if (s[j] == '-') { es = -1; j++; } else if (s[j] == '+') j++;
        while (s[j] >= '0' && s[j] <= '9') e = e * 10 + (s[j++] - '0');
        while (e-- > 0) r = es > 0 ? r * 10 : r / 10;
        i = j;
    }
    *v = r * sign;
    return i;
}

/* разбить значение на слова (с учётом скобок и кавычек): -> число слов */
static int split_words(const char *s, const char **w, int *wl, int max) {
    int n = 0;
    while (*s) {
        while (is_sp(*s) || *s == ',') { if (*s == ',' && n < max) { w[n] = s; wl[n++] = 1; } s++; }
        if (!*s) break;
        const char *st = s;
        int depth = 0;
        char q = 0;
        while (*s && (depth || q || (!is_sp(*s) && *s != ','))) {
            if (q) { if (*s == q) q = 0; }
            else if (*s == '"' || *s == '\'') q = *s;
            else if (*s == '(') depth++;
            else if (*s == ')') depth--;
            s++;
        }
        if (n < max) { w[n] = st; wl[n++] = s - st; }
    }
    return n;
}

/* ------------------------------------------------------------------ цвета */

static const struct { const char *name; int rgb; } named_colors[] = {
    { "black", 0x000000 }, { "white", 0xffffff }, { "red", 0xff0000 }, { "green", 0x008000 }, { "blue", 0x0000ff },
    { "yellow", 0xffff00 }, { "gray", 0x808080 }, { "grey", 0x808080 }, { "silver", 0xc0c0c0 }, { "maroon", 0x800000 },
    { "purple", 0x800080 }, { "fuchsia", 0xff00ff }, { "magenta", 0xff00ff }, { "lime", 0x00ff00 }, { "olive", 0x808000 },
    { "navy", 0x000080 }, { "teal", 0x008080 }, { "aqua", 0x00ffff }, { "cyan", 0x00ffff }, { "orange", 0xffa500 },
    { "pink", 0xffc0cb }, { "brown", 0xa52a2a }, { "gold", 0xffd700 }, { "beige", 0xf5f5dc }, { "ivory", 0xfffff0 },
    { "khaki", 0xf0e68c }, { "lavender", 0xe6e6fa }, { "violet", 0xee82ee }, { "indigo", 0x4b0082 }, { "coral", 0xff7f50 },
    { "salmon", 0xfa8072 }, { "tomato", 0xff6347 }, { "crimson", 0xdc143c }, { "darkred", 0x8b0000 },
    { "darkblue", 0x00008b }, { "darkgreen", 0x006400 }, { "darkgray", 0xa9a9a9 }, { "darkgrey", 0xa9a9a9 },
    { "dimgray", 0x696969 }, { "dimgrey", 0x696969 }, { "lightgray", 0xd3d3d3 }, { "lightgrey", 0xd3d3d3 },
    { "gainsboro", 0xdcdcdc }, { "whitesmoke", 0xf5f5f5 }, { "lightblue", 0xadd8e6 }, { "skyblue", 0x87ceeb },
    { "steelblue", 0x4682b4 }, { "royalblue", 0x4169e1 }, { "dodgerblue", 0x1e90ff }, { "deepskyblue", 0x00bfff },
    { "midnightblue", 0x191970 }, { "slategray", 0x708090 }, { "slategrey", 0x708090 }, { "darkslategray", 0x2f4f4f },
    { "lightyellow", 0xffffe0 }, { "lightgreen", 0x90ee90 }, { "forestgreen", 0x228b22 }, { "seagreen", 0x2e8b57 },
    { "darkorange", 0xff8c00 }, { "orangered", 0xff4500 }, { "firebrick", 0xb22222 }, { "chocolate", 0xd2691e },
    { "tan", 0xd2b48c }, { "wheat", 0xf5deb3 }, { "linen", 0xfaf0e6 }, { "snow", 0xfffafa }, { "honeydew", 0xf0fff0 },
    { "aliceblue", 0xf0f8ff }, { "ghostwhite", 0xf8f8ff }, { "mintcream", 0xf5fffa }, { "azure", 0xf0ffff },
    { "seashell", 0xfff5ee }, { "oldlace", 0xfdf5e6 }, { "floralwhite", 0xfffaf0 }, { "cornsilk", 0xfff8dc },
    { "lemonchiffon", 0xfffacd }, { "papayawhip", 0xffefd5 }, { "mistyrose", 0xffe4e1 }, { "darkviolet", 0x9400d3 },
    { "rebeccapurple", 0x663399 }, { "turquoise", 0x40e0d0 }, { "darkcyan", 0x008b8b }, { "cadetblue", 0x5f9ea0 },
    { "hotpink", 0xff69b4 }, { "deeppink", 0xff1493 }, { "goldenrod", 0xdaa520 }, { "sienna", 0xa0522d },
    { "plum", 0xdda0dd }, { "orchid", 0xda70d6 }, { "limegreen", 0x32cd32 }, { "olivedrab", 0x6b8e23 },
    { "canvas", 0xffffff }, { "canvastext", 0x000000 }, { "linktext", 0x0000ee }, { "buttonface", 0xefefef },
    { "buttontext", 0x000000 }, { "field", 0xffffff }, { "fieldtext", 0x000000 }, { "graytext", 0x808080 },
    { "highlight", 0x3390ff }, { "highlighttext", 0xffffff }, { "mark", 0xffff00 }, { "marktext", 0x000000 },
};

static int clamp255(double v) { return v < 0 ? 0 : v > 255 ? 255 : (int)(v + 0.5); }

/* компонента rgb()/hsl(): число или проценты */
static double comp(const char *s, int n, double pct_scale) {
    double v = 0;
    int k = read_num(s, &v);
    if (k && k < n && s[k] == '%') return v * pct_scale / 100;
    return v;
}

static double hue2rgb(double p, double q, double t) {
    if (t < 0) t += 1;
    if (t > 1) t -= 1;
    if (t < 1.0 / 6) return p + (q - p) * 6 * t;
    if (t < 0.5) return q;
    if (t < 2.0 / 3) return p + (q - p) * (2.0 / 3 - t) * 6;
    return p;
}

/* цвет -> 1 (в *rgb и *alpha 0..1); 2 — currentColor; 0 — не цвет */
static int parse_color(const char *s, int n, int *rgb, double *alpha) {
    while (n && is_sp(*s)) { s++; n--; }
    while (n && is_sp(s[n - 1])) n--;
    *alpha = 1;
    if (!n) return 0;
    if (s[0] == '#') {
        int h[8], k = n - 1;
        if (k != 3 && k != 4 && k != 6 && k != 8) return 0;
        for (int i = 0; i < k; i++) if ((h[i] = hexv(s[i + 1])) < 0) return 0;
        if (k <= 4) {
            *rgb = (h[0] * 17) << 16 | (h[1] * 17) << 8 | h[2] * 17;
            if (k == 4) *alpha = h[3] * 17 / 255.0;
        } else {
            *rgb = (h[0] * 16 + h[1]) << 16 | (h[2] * 16 + h[3]) << 8 | (h[4] * 16 + h[5]);
            if (k == 8) *alpha = (h[6] * 16 + h[7]) / 255.0;
        }
        return 1;
    }
    if (word_is(s, n, "transparent")) { *rgb = 0; *alpha = 0; return 1; }
    if (word_is(s, n, "currentcolor")) return 2;
    if (starts(s,"oklch(") || starts(s,"oklab(")) {
        int polar=starts(s,"oklch(");
        char buf[160]; int count=n<159?n:159; memcpy(buf,s,count);buf[count]=0;
        char *p=strchr(buf,'(')+1,*end=strrchr(p,')'); if (!end) return 0; *end=0;
        for (char *q=p;*q;q++) if (*q=='/') *q=' ';
        const char *w[4];int wl[4],nw=split_words(p,w,wl,4);
        if (nw<3) return 0;
        double L=comp(w[0],wl[0],1),a=comp(w[1],wl[1],.4),b=comp(w[2],wl[2],.4);
        if (polar) {
            double h=comp(w[2],wl[2],360);
            int used=read_num(w[2],&b);
            if (used<wl[2] && starts(w[2]+used,"turn")) h=b*360;
            else if (used<wl[2] && starts(w[2]+used,"rad")) h=b*180/3.141592653589793;
            b=a*js_sin(h*3.141592653589793/180); a*=js_cos(h*3.141592653589793/180);
        }
        double l=L+.3963377774*a+.2158037573*b, m=L-.1055613458*a-.0638541728*b, ss=L-.0894841775*a-1.291485548*b;
        l=l*l*l;m=m*m*m;ss=ss*ss*ss;
        double channels[3]={4.0767416621*l-3.3077115913*m+.2309699292*ss,
            -1.2684380046*l+2.6097574011*m-.3413193965*ss,-.0041960863*l-.7034186147*m+1.707614701*ss};
        *rgb = 0;
        int value=0;
        for (int i=0;i<3;i++) {
            double c=channels[i]; c=c<=.0031308?12.92*c:1.055*js_pow(c,1/2.4)-.055;
            value=(value<<8)|clamp255(c*255);
        }
        *rgb=value;
        if (nw==4) *alpha=comp(w[3],wl[3],1);
        return 1;
    }
    int rgbf = starts(s, "rgb"), hslf = starts(s, "hsl");
    if (rgbf || hslf) {
        const char *p = s;
        while (p < s + n && *p != '(') p++;
        if (p >= s + n) return 0;
        p++;
        const char *e = s + n;
        if (e > p && e[-1] == ')') e--;
        const char *w[8];
        int wl[8], k = 0;
        char tmp[128];
        int tl = e - p < 127 ? e - p : 127;
        memcpy(tmp, p, tl);
        tmp[tl] = 0;
        for (int i = 0; i < tl; i++) if (tmp[i] == '/') tmp[i] = ' ';
        int nw = split_words(tmp, w, wl, 8);
        const char *v[4];
        int vl[4];
        for (int i = 0; i < nw && k < 4; i++) if (!(wl[i] == 1 && w[i][0] == ',')) { v[k] = w[i]; vl[k++] = wl[i]; }
        if (k < 3) return 0;
        for (int i = 0; i < k; i++) { double t; if (!read_num(v[i], &t) && !word_is(v[i], vl[i], "none")) return 0; }
        if (rgbf) {
            *rgb = clamp255(comp(v[0], vl[0], 255)) << 16 | clamp255(comp(v[1], vl[1], 255)) << 8 | clamp255(comp(v[2], vl[2], 255));
        } else {
            double hh = comp(v[0], vl[0], 360) / 360, ss = comp(v[1], vl[1], 100) / 100, ll = comp(v[2], vl[2], 100) / 100;
            if (starts(v[0] + read_num(v[0], &(double){ 0 }), "turn")) hh *= 360;
            hh -= (int)hh;
            if (hh < 0) hh += 1;
            double q = ll < 0.5 ? ll * (1 + ss) : ll + ss - ll * ss, pp = 2 * ll - q;
            *rgb = clamp255(hue2rgb(pp, q, hh + 1.0 / 3) * 255) << 16 | clamp255(hue2rgb(pp, q, hh) * 255) << 8 |
                   clamp255(hue2rgb(pp, q, hh - 1.0 / 3) * 255);
        }
        if (k == 4) *alpha = comp(v[3], vl[3], 1);
        return 1;
    }
    for (int i = 0; i < (int)(sizeof(named_colors) / sizeof(named_colors[0])); i++)
        if (word_is(s, n, named_colors[i].name)) { *rgb = named_colors[i].rgb; return 1; }
    return 0;
}

static int blend(int fg, int bg, double a) {
    if (a >= 0.999 || bg < 0) return fg;
    int r = ((fg >> 16) & 255) * a + ((bg >> 16) & 255) * (1 - a);
    int g = ((fg >> 8) & 255) * a + ((bg >> 8) & 255) * (1 - a);
    int b = (fg & 255) * a + (bg & 255) * (1 - a);
    return clamp255(r) << 16 | clamp255(g) << 8 | clamp255(b);
}

/* ------------------------------------------------------------------ длины */

struct lctx { double em, rem, pct; };         /* pct — от чего считаются проценты (<0 — нельзя) */

static int eval_expr(const char **s, struct lctx *c, double *v);

/* одна величина: число с единицей, (выражение), calc/min/max/clamp(...) */
static int eval_term(const char **sp, struct lctx *c, double *v) {
    const char *s = *sp;
    while (is_sp(*s)) s++;
    if (*s == '(') {
        s++;
        if (!eval_expr(&s, c, v)) return 0;
        while (is_sp(*s)) s++;
        if (*s != ')') return 0;
        *sp = s + 1;
        return 1;
    }
    int fn = 0;
    if (starts(s, "calc(")) { fn = 1; s += 5; }
    else if (starts(s, "-webkit-calc(")) { fn = 1; s += 13; }
    else if (starts(s, "min(")) { fn = 2; s += 4; }
    else if (starts(s, "max(")) { fn = 3; s += 4; }
    else if (starts(s, "clamp(")) { fn = 4; s += 6; }
    if (fn) {
        double a[3];
        int k = 0;
        for (;;) {
            if (k >= 3 || !eval_expr(&s, c, &a[k])) return 0;
            k++;
            while (is_sp(*s)) s++;
            if (*s == ',') { s++; continue; }
            if (*s == ')') { s++; break; }
            return 0;
        }
        if (fn == 1) *v = a[0];
        else if (fn == 2) { *v = a[0]; for (int i = 1; i < k; i++) if (a[i] < *v) *v = a[i]; }
        else if (fn == 3) { *v = a[0]; for (int i = 1; i < k; i++) if (a[i] > *v) *v = a[i]; }
        else { if (k != 3) return 0; *v = a[1] < a[0] ? a[0] : a[1] > a[2] ? a[2] : a[1]; }
        *sp = s;
        return 1;
    }
    double x;
    int k = read_num(s, &x);
    if (!k) return 0;
    s += k;
    char u[8];
    int ul = 0;
    while (ul < 7 && ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || *s == '%')) u[ul++] = lc(*s++);
    u[ul] = 0;
    if (!ul || !strcmp(u, "px")) *v = x;
    else if (!strcmp(u, "em")) *v = x * c->em;
    else if (!strcmp(u, "rem")) *v = x * c->rem;
    else if (!strcmp(u, "%")) { if (c->pct < 0) return 0; *v = x * c->pct / 100; }
    else if (!strcmp(u, "pt")) *v = x * 4 / 3;
    else if (!strcmp(u, "pc")) *v = x * 16;
    else if (!strcmp(u, "in")) *v = x * 96;
    else if (!strcmp(u, "cm")) *v = x * 96 / 2.54;
    else if (!strcmp(u, "mm")) *v = x * 96 / 25.4;
    else if (!strcmp(u, "vw") || !strcmp(u, "dvw") || !strcmp(u, "svw") || !strcmp(u, "lvw") || !strcmp(u, "vi")) *v = x * VIEW_W / 100;
    else if (!strcmp(u, "vh") || !strcmp(u, "dvh") || !strcmp(u, "svh") || !strcmp(u, "lvh") || !strcmp(u, "vb")) *v = x * VIEW_H / 100;
    else if (!strcmp(u, "vmin")) *v = x * (VIEW_W < VIEW_H ? VIEW_W : VIEW_H) / 100;
    else if (!strcmp(u, "vmax")) *v = x * (VIEW_W > VIEW_H ? VIEW_W : VIEW_H) / 100;
    else if (!strcmp(u, "ch") || !strcmp(u, "ex")) *v = x * 8;
    else if (!strcmp(u, "lh")) *v = x * c->em * 1.2;
    else return 0;
    *sp = s;
    return 1;
}

static int eval_prod(const char **s, struct lctx *c, double *v) {
    if (!eval_term(s, c, v)) return 0;
    for (;;) {
        const char *p = *s;
        while (is_sp(*p)) p++;
        if (*p != '*' && *p != '/') return 1;
        char op = *p++;
        double r;
        struct lctx plain = *c;
        if (!eval_term(&p, &plain, &r)) return 0;
        if (op == '*') *v *= r; else if (r != 0) *v /= r;
        *s = p;
    }
}

static int eval_expr(const char **s, struct lctx *c, double *v) {
    if (!eval_prod(s, c, v)) return 0;
    for (;;) {
        const char *p = *s;
        while (is_sp(*p)) p++;
        if ((*p != '+' && *p != '-') || !is_sp(p[1])) return 1;        /* в calc() знаки окружены пробелами */
        char op = *p++;
        double r;
        if (!eval_prod(&p, c, &r)) return 0;
        *v = op == '+' ? *v + r : *v - r;
        *s = p;
    }
}

/* длина -> 1 (*px), 2 — auto/none/normal, 0 — не длина */
static int parse_len(const char *s, int n, struct lctx *c, double *px) {
    char b[256];
    while (n && is_sp(*s)) { s++; n--; }
    while (n && is_sp(s[n - 1])) n--;
    if (!n || n > 255) return 0;
    if (word_is(s, n, "auto") || word_is(s, n, "none") || word_is(s, n, "normal") || word_is(s, n, "fit-content") ||
        word_is(s, n, "max-content") || word_is(s, n, "min-content")) return 2;
    if (word_is(s, n, "thin")) { *px = 1; return 1; }
    if (word_is(s, n, "medium")) { *px = 3; return 1; }
    if (word_is(s, n, "thick")) { *px = 5; return 1; }
    memcpy(b, s, n);
    b[n] = 0;
    const char *p = b;
    if (!eval_expr(&p, c, px)) return 0;
    while (is_sp(*p)) p++;
    if (*p) return 0;
    if (*px > 20000) *px = 20000;
    if (*px < -20000) *px = -20000;
    return 1;
}

/* ------------------------------------------------------------------ @media */

static int media_one(const char *q, int n) {
    char b[300];
    int l = 0;
    for (int i = 0; i < n && l < 299; i++) b[l++] = lc(q[i]);
    b[l] = 0;
    char *p = b;
    while (is_sp(*p)) p++;
    int neg = 0;
    if (!strncmp(p, "not ", 4)) { neg = 1; p += 4; }
    if (!strncmp(p, "only ", 5)) p += 5;
    int ok = 1;
    while (*p && ok) {
        while (is_sp(*p) || !strncmp(p, "and ", 4)) p += is_sp(*p) ? 1 : 4;
        if (!*p) break;
        if (*p != '(') {                                         /* тип: screen, all, print... */
            char *s = p;
            while (*p && !is_sp(*p) && *p != '(') p++;
            int tl = p - s;
            if (!(tl == 6 && !strncmp(s, "screen", 6)) && !(tl == 3 && !strncmp(s, "all", 3))) ok = 0;
            continue;
        }
        char *e = p + 1;
        int depth = 1;
        while (*e && depth) { if (*e == '(') depth++; else if (*e == ')') depth--; if (depth) e++; }
        char f[200];
        int fl = e - p - 1 < 199 ? e - p - 1 : 199;
        memcpy(f, p + 1, fl);
        f[fl] = 0;
        p = *e ? e + 1 : e;
        /* (min-width: 40em), (width >= 600px), (prefers-color-scheme: dark) ... */
        char *colon = strchr(f, ':');
        struct lctx c = { 16, 16, -1 };
        if (colon) {
            *colon = 0;
            char *name = f, *val = colon + 1;
            while (is_sp(*name)) name++;
            char *ne = name + strlen(name);
            while (ne > name && is_sp(ne[-1])) *--ne = 0;
            while (is_sp(*val)) val++;
            char *ve = val + strlen(val);
            while (ve > val && is_sp(ve[-1])) *--ve = 0;
            double v;
            int isw = strstr(name, "width") != NULL, ish = strstr(name, "height") != NULL;
            if ((isw || ish) && !strstr(name, "device-pixel") && parse_len(val, strlen(val), &c, &v) == 1) {
                double have = isw ? VIEW_W : VIEW_H;
                if (!strncmp(name, "min-", 4)) ok = have >= v;
                else if (!strncmp(name, "max-", 4)) ok = have <= v;
                else ok = have == v;
            } else if (!strcmp(name, "prefers-color-scheme")) ok = !strcmp(val, "light");
            else if (!strcmp(name, "prefers-reduced-motion")) ok = !strcmp(val, "no-preference");
            else if (!strcmp(name, "hover") || !strcmp(name, "any-hover")) ok = !strcmp(val, "hover");
            else if (!strcmp(name, "pointer") || !strcmp(name, "any-pointer")) ok = !strcmp(val, "fine");
            else if (!strcmp(name, "orientation")) ok = !strcmp(val, VIEW_W >= VIEW_H ? "landscape" : "portrait");
            else if (!strcmp(name, "color")) ok = 1;
            else if (strstr(name, "resolution") || strstr(name, "pixel-ratio")) {
                double r = 0;
                read_num(val, &r);
                if (strstr(val, "dpi")) r /= 96;
                ok = strncmp(name, "min-", 4) && strncmp(name, "-webkit-min", 11) ? 1 : r <= 1;
            } else if (!strcmp(name, "prefers-contrast")) ok = !strcmp(val, "no-preference");
            else if (!strcmp(name, "scripting")) ok = !strcmp(val, dom_scripting ? "enabled" : "none");
            else if (!strcmp(name, "display-mode")) ok = !strcmp(val, "browser");
            else ok = 0;
        } else {
            /* диапазоны: (width >= 600px), (400px < width <= 700px) */
            char *w = strstr(f, "width");
            char *h = w ? NULL : strstr(f, "height");
            char *nm = w ? w : h;
            if (!nm) { char *t = f; while (is_sp(*t)) t++; ok = !strcmp(t, "color") || !strcmp(t, "hover") || !strcmp(t, "pointer"); continue; }
            double have = w ? VIEW_W : VIEW_H;
            /* справа от имени */
            char *r = nm + (w ? 5 : 6);
            while (is_sp(*r)) r++;
            char op[3] = { 0 };
            if (*r == '<' || *r == '>' || *r == '=') { op[0] = *r++; if (*r == '=') op[1] = *r++; }
            double v;
            if (op[0] && parse_len(r, strlen(r), &c, &v) == 1) {
                if (op[0] == '<') ok = op[1] ? have <= v : have < v;
                else if (op[0] == '>') ok = op[1] ? have >= v : have > v;
                else ok = have == v;
            }
            /* слева от имени: «400px <» */
            char *l2 = f;
            while (is_sp(*l2)) l2++;
            if (ok && l2 < nm) {
                char *e2 = nm;
                while (e2 > l2 && is_sp(e2[-1])) e2--;
                char op2[3] = { 0 };
                if (e2 > l2 && e2[-1] == '=') { op2[1] = '='; e2--; }
                if (e2 > l2 && (e2[-1] == '<' || e2[-1] == '>')) { op2[0] = e2[-1]; e2--; }
                if (op2[0] && parse_len(l2, e2 - l2, &c, &v) == 1) {
                    if (op2[0] == '<') ok = op2[1] ? v <= have : v < have;
                    else ok = op2[1] ? v >= have : v > have;
                }
            }
        }
    }
    return neg ? !ok : ok;
}

int css_media_ok(const char *q, int n) {
    if (n < 0) n = strlen(q);
    int depth = 0, st = 0, any = 0;
    for (int i = 0; i <= n; i++) {
        if (i < n && q[i] == '(') depth++;
        if (i < n && q[i] == ')') depth--;
        if (i == n || (q[i] == ',' && !depth)) {
            int k = st;
            while (k < i && is_sp(q[k])) k++;
            if (k == i) { if (!any && i == n) return 1; }           /* пустой список — для всех */
            else if (media_one(q + k, i - k)) return 1;
            any = 1;
            st = i + 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------ правила */

struct decl { short prop; unsigned char imp; char *name; char *val; };     /* prop -1: --переменная (name) */
struct dblock { int n; struct decl *d; };
struct rule { struct selector *sel; unsigned spec; int order; unsigned char author; struct dblock *b; int next; };

static struct rule *rules;
static int n_rules, rules_cap, rule_order;
#define BUCKETS 1024
static int buckets[BUCKETS], universal = -1;         /* индексы правил + 1 / -1 */
static int built, built_mut = -1;
static unsigned built_sig;
static int sheet_bytes;

/* скачанные таблицы стилей (на время страницы) */
struct sheet { char *url; char *text; int len; };
static struct sheet sheets[24];
static int n_sheets;

static void rules_free(void) {
    for (int i = 0; i < n_rules; i++) sel_free(rules[i].sel);
    free(rules);
    rules = NULL;
    n_rules = rules_cap = rule_order = 0;
    for (int i = 0; i < BUCKETS; i++) buckets[i] = -1;
    universal = -1;
    ar_free(&rule_arena);
}

/* --- селектор: специфичность и «ключ» (по чему искать правило) */

static unsigned spec_simple(struct simple *s) {
    unsigned a = s->id ? 1 : 0, b = s->ncls + s->nat + (s->pseudo ? 1 : 0), c = s->tag ? 1 : 0;
    unsigned r = a << 16 | b << 8 | c;
    if (s->not_) r += spec_simple(s->not_);
    return r;
}

static void add_rule(struct selector *sel, struct dblock *b, int author) {
    struct simple *chain = sel->parts[0];
    int np = sel->nparts[0];
    unsigned spec = 0;
    for (int i = 0; i < np; i++) {
        if (chain[i].pseudo == 9) { sel_free(sel); return; }      /* :hover, ::before... — у нас никогда не совпадёт */
        spec += spec_simple(&chain[i]);
    }
    if (n_rules == rules_cap) {
        rules_cap = rules_cap ? rules_cap * 2 : 1024;
        rules = realloc(rules, sizeof(struct rule) * rules_cap);
    }
    struct rule *r = &rules[n_rules];
    r->sel = sel;
    r->spec = spec;
    r->order = rule_order++;
    r->author = author;
    r->b = b;
    struct simple *last = &chain[np - 1];
    char key[80];
    key[0] = 0;
    if (last->id) snprintf(key, sizeof(key), "#%s", last->id);
    else if (last->ncls) snprintf(key, sizeof(key), ".%s", last->cls[0]);
    else if (last->tag) snprintf(key, sizeof(key), "%s", last->tag);
    if (key[0]) {
        unsigned h = khash(key, strlen(key)) % BUCKETS;
        r->next = buckets[h];
        buckets[h] = n_rules;
    } else {
        r->next = universal;
        universal = n_rules;
    }
    n_rules++;
}

/* --- значения: годится ли для свойства (неподходящие объявления браузер выбрасывает) */

static int value_ok(int slot, const char *v, int n) {
    struct lctx c = { 16, 16, 100 };
    double d;
    int rgb;
    double a;
    while (n && is_sp(*v)) { v++; n--; }
    while (n && is_sp(v[n - 1])) n--;
    if (!n) return 0;
    if (word_is(v, n, "inherit") || word_is(v, n, "initial") || word_is(v, n, "unset") || word_is(v, n, "revert") ||
        word_is(v, n, "revert-layer")) return 1;
    switch (slot) {
    case S_COLOR: case S_BG: case S_BTC: case S_BRC: case S_BBC: case S_BLC:
        return parse_color(v, n, &rgb, &a) != 0;
    case S_MT: case S_MR: case S_MB: case S_ML: case S_PT: case S_PR: case S_PB: case S_PL:
    case S_BTW: case S_BRW: case S_BBW: case S_BLW: case S_WIDTH: case S_MAXW: case S_HEIGHT: case S_MAXH:
    case S_LEFT: case S_TOP: case S_TINDENT:
        return parse_len(v, n, &c, &d) != 0;
    case S_FSIZE: {
        static const char *kw[] = { "xx-small", "x-small", "small", "medium", "large", "x-large", "xx-large", "xxx-large",
                                    "smaller", "larger", NULL };
        for (int i = 0; kw[i]; i++) if (word_is(v, n, kw[i])) return 1;
        return parse_len(v, n, &c, &d) == 1;
    }
    case S_DISPLAY: {
        static const char *kw[] = { "none", "block", "inline", "inline-block", "flex", "inline-flex", "grid", "inline-grid",
                                    "list-item", "table", "inline-table", "table-row", "table-cell", "table-row-group",
                                    "table-header-group", "table-footer-group", "table-caption", "table-column",
                                    "table-column-group", "contents", "flow-root", "-webkit-box", "-webkit-inline-box",
                                    "-ms-flexbox", "-webkit-flex", "-ms-inline-flexbox", "-webkit-inline-flex", "ruby",
                                    "run-in", "flow", "block flex", "inline flex", "block flow", "inline flow-root", NULL };
        for (int i = 0; kw[i]; i++) if (word_is(v, n, kw[i])) return 1;
        return 0;
    }
    default:
        return 1;
    }
}

/* Resolve URL tokens while their stylesheet base is still known. Keep URL
 * spelling/case and skip quoted text and escaped parentheses. */
static char *declaration_urls(const char *s,int n,const char *base,struct arena **ar) {
    jsbuf b={0};int start=0;char quote=0;
    for(int i=0;i<n;i++) {
        if(s[i]=='\\' && i+1<n){i++;continue;}
        if(quote){if(s[i]==quote)quote=0;continue;}
        if(s[i]=='\'' || s[i]=='"'){quote=s[i];continue;}
        if(i+4>n || lc(s[i])!='u' || lc(s[i+1])!='r' || lc(s[i+2])!='l' || s[i+3]!='(')continue;
        int j=i+4;while(j<n && is_sp(s[j]))j++;
        char q=j<n && (s[j]=='\'' || s[j]=='"')?s[j++]:0;
        jsbuf u={0};
        while(j<n && (q?s[j]!=q:s[j]!=')')) {
            if(s[j]=='\\' && j+1<n)j++;
            jb_addc(&u,s[j++]);
        }
        if(q){if(j>=n){jb_free(&u);break;}j++;while(j<n && is_sp(s[j]))j++;}
        if(j>=n || s[j]!=')'){jb_free(&u);continue;}
        while(u.n && is_sp(u.p[u.n-1]))u.n--;
        jb_addc(&u,0);
        jb_add(&b,s+start,i-start);jb_adds(&b,"url(\"");
        if(u.n>1) {
            if(!strncmp(u.p,"data:",5))jb_adds(&b,u.p);
            else {char full[2048];dn_resolve(base,u.p,full,sizeof(full));jb_adds(&b,full);}
        }
        jb_adds(&b,"\")");jb_free(&u);i=j;start=j+1;
    }
    jb_add(&b,s+start,n-start);char *out=ar_str(ar,b.p,b.n);jb_free(&b);return out;
}

/* объявления «a: b; c: d !important» -> блок; ar — где хранить строки */
static struct dblock *parse_decls(const char *s, int n, const char *base, struct arena **ar) {
    struct decl tmp[512];
    int nd = 0;
    int i = 0;
    while (i < n && nd < (int)(sizeof(tmp)/sizeof(tmp[0]))) {
        while (i < n && (is_sp(s[i]) || s[i] == ';')) i++;
        if (i >= n) break;
        int st = i, depth = 0;
        char q = 0;
        int nested = 0;
        while (i < n) {                                        /* до ; вне скобок */
            char c = s[i];
            if (q) { if (c == '\\') i++; else if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') depth++;
            else if (c == ')' || c == ']') depth--;
            else if (c == '{') {                               /* вложенное правило (CSS nesting) — пропустить */
                int d = 1;
                i++;
                while (i < n && d) { if (s[i] == '{') d++; else if (s[i] == '}') d--; i++; }
                nested = 1;
                break;
            } else if (c == ';' && depth <= 0) break;
            i++;
        }
        if (nested) continue;
        int en = i;
        int colon = st;
        while (colon < en && s[colon] != ':') colon++;
        if (colon >= en) continue;
        int ns = st, ne = colon;
        while (ne > ns && is_sp(s[ne - 1])) ne--;
        int vs = colon + 1, ve = en;
        while (vs < ve && is_sp(s[vs])) vs++;
        while (ve > vs && is_sp(s[ve - 1])) ve--;
        int imp = 0;
        for (int k = ve - 1; k > vs; k--) {                    /* !important в конце */
            if (s[k] == '!') {
                int m = k + 1;
                while (m < ve && is_sp(s[m])) m++;
                if (word_is(s + m, ve - m, "important")) { imp = 1; ve = k; while (ve > vs && is_sp(s[ve - 1])) ve--; }
                break;
            }
            if (!((s[k] >= 'a' && s[k] <= 'z') || (s[k] >= 'A' && s[k] <= 'Z') || is_sp(s[k]))) break;
        }
        if (ne <= ns || ve <= vs) continue;
        struct decl *d = &tmp[nd];
        d->imp = imp;
        d->name = NULL;
        if (ne - ns > 2 && s[ns] == '-' && s[ns + 1] == '-') {   /* --переменная */
            d->prop = -1;
            d->name = ar_str(ar, s + ns, ne - ns);
            d->val = declaration_urls(s+vs,ve-vs,base,ar);
            nd++;
            continue;
        }
        char nm[48];
        if (ne - ns >= 47) continue;
        for (int k = 0; k < ne - ns; k++) nm[k] = lc(s[ns + k]);
        nm[ne - ns] = 0;
        int p = prop_find(nm, ne - ns);
        if (p < 0) continue;
        int has_var = 0;
        for (int k = vs; k + 3 < ve; k++) if (s[k] == 'v' && !strncmp(s + k, "var(", 4)) { has_var = 1; break; }
        if (!has_var && props[p].n == 1 && props[p].m[0].sh == SH_NONE && !value_ok(props[p].m[0].slot, s + vs, ve - vs)) continue;
        d->prop = p;
        d->val = declaration_urls(s+vs,ve-vs,base,ar);
        nd++;
    }
    if (!nd) return NULL;
    struct dblock *b = ar_alloc(ar, sizeof(struct dblock));
    b->n = nd;
    b->d = ar_alloc(ar, sizeof(struct decl) * nd);
    memcpy(b->d, tmp, sizeof(struct decl) * nd);
    return b;
}

static void parse_sheet(const char *s, int n, const char *base, int author, int depth);

/* скачать таблицу стилей (или взять уже скачанную) */
static struct sheet *get_sheet(const char *url) {
    for (int i = 0; i < n_sheets; i++) if (!strcmp(sheets[i].url, url)) return &sheets[i];
    if (n_sheets >= (int)(sizeof(sheets) / sizeof(sheets[0])) || !d_host || !d_host->fetch) return NULL;
    struct sheet *sh = &sheets[n_sheets++];
    sh->url = malloc(strlen(url) + 1);
    strcpy(sh->url, url);
    sh->text = NULL;
    sh->len = 0;
    if (sheet_bytes > (4 << 20)) return sh;
    char msg[200], ctype[64];
    snprintf(msg, sizeof(msg), "Loading style sheet %.150s", url);
    if (d_host->status) d_host->status(msg);
    char *data = NULL;
    int len = 0, status = 0;
    if (d_host->fetch(url, "GET", NULL, 0, &data, &len, &status, ctype, sizeof(ctype)) < 0 || status >= 400 || !data) {
        free(data);
        return sh;
    }
    if (len > 4 && (ju8)data[0] == 0xEF && (ju8)data[1] == 0xBB) { memmove(data, data + 3, len - 3); len -= 3; }
    sh->text = data;
    sh->len = len;
    sheet_bytes += len;
    return sh;
}

/* «@import url("a.css") screen;» */
static void do_import(const char *s, int n, const char *base, int author, int depth) {
    if (depth > 3) return;
    int i = 0;
    while (i < n && is_sp(s[i])) i++;
    char u[1024];
    int ul = 0;
    if (i + 4 <= n && starts(s + i, "url(")) {
        i += 4;
        while (i < n && is_sp(s[i])) i++;
        char q = (s[i] == '"' || s[i] == '\'') ? s[i++] : 0;
        while (i < n && (q ? s[i] != q : s[i] != ')') && ul < 1023) u[ul++] = s[i++];
        if (q && i < n) i++;
        while (i < n && s[i] != ')') i++;
        if (i < n) i++;
    } else if (s[i] == '"' || s[i] == '\'') {
        char q = s[i++];
        while (i < n && s[i] != q && ul < 1023) u[ul++] = s[i++];
        if (i < n) i++;
    } else return;
    u[ul] = 0;
    while (ul && is_sp(u[ul - 1])) u[--ul] = 0;
    /* условие после адреса: layer(...), supports(...), media */
    while (i < n && is_sp(s[i])) i++;
    if (i + 5 <= n && starts(s + i, "layer")) { while (i < n && !is_sp(s[i])) i++; }
    while (i < n && is_sp(s[i])) i++;
    if (i < n && !css_media_ok(s + i, n - i)) return;
    char full[1024];
    dn_resolve(base, u, full, sizeof(full));
    struct sheet *sh = get_sheet(full);
    if (sh && sh->text) parse_sheet(sh->text, sh->len, full, author, depth + 1);
}

/* одно правило: селекторы через запятую { объявления } */
static void add_style_rule(const char *sel, int sn, const char *body, int bn, const char *base, int author) {
    struct dblock *b = parse_decls(body, bn, base, &rule_arena);
    if (!b) return;
    char *s = malloc(sn + 1);
    memcpy(s, sel, sn);
    s[sn] = 0;
    char *p = s;
    while (*p) {                                               /* каждый вариант — отдельное правило */
        char *e = p;
        int depth = 0;
        char q = 0;
        while (*e && (q || *e != ',' || depth)) {
            if (*e == '\\' && e[1]) { e += 2; continue; }
            if (q) { if (*e == q) q = 0; }
            else if (*e == '"' || *e == '\'') q = *e;
            else if (*e == '(' || *e == '[') depth++;
            else if (*e == ')' || *e == ']') depth--;
            e++;
        }
        char save = *e;
        *e = 0;
        while (is_sp(*p)) p++;
        char *pe = e;
        while (pe > p && is_sp(pe[-1])) *--pe = 0;
        if (*p && n_rules < 60000) {
            struct selector *ss = sel_parse(p);
            if (ss) add_rule(ss, b, author);
        }
        if (!save) break;
        p = e + 1;
    }
    free(s);
}

static void parse_block(const char *s, int n, const char *base, int author, int depth) {
    int i = 0;
    while (i < n) {
        while (i < n && (is_sp(s[i]) || s[i] == ';' || s[i] == '}')) i++;
        if (i >= n) break;
        if (s[i] == '<' && i + 3 < n && !strncmp(s + i, "<!--", 4)) { i += 4; continue; }
        if (s[i] == '-' && i + 2 < n && !strncmp(s + i, "-->", 3)) { i += 3; continue; }
        /* до { или ; (вне строк и скобок) */
        int j = i, dp = 0;
        char q = 0;
        while (j < n) {
            char c = s[j];
            if (c == '\\' && j+1<n) { j+=2; continue; }
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '(' || c == '[') dp++;
            else if (c == ')' || c == ']') dp--;
            else if ((c == '{' || c == ';') && dp <= 0) break;
            else if (c == '}' && dp <= 0) break;
            j++;
        }
        if (j >= n) break;
        if (s[j] == '}') { i = j + 1; continue; }                 /* мусор */
        if (s[j] == ';') {
            if (s[i] == '@' && starts(s + i, "@import")) do_import(s + i + 7, j - i - 7, base, author, depth);
            i = j + 1;
            continue;
        }
        int k = j + 1, d = 1;                                     /* парная } */
        q = 0;
        while (k < n && d) {
            char c = s[k];
            if (c == '\\' && k+1<n) { k+=2; continue; }
            if (q) { if (c == q) q = 0; }
            else if (c == '"' || c == '\'') q = c;
            else if (c == '{') d++;
            else if (c == '}') d--;
            k++;
        }
        int bs = j + 1, be = d ? k : k - 1;
        if (s[i] == '@') {
            if (starts(s + i, "@media")) { if (css_media_ok(s + i + 6, j - i - 6)) parse_block(s + bs, be - bs, base, author, depth); }
            else if (starts(s + i, "@supports")) {
                int t = i + 9;
                while (t < j && is_sp(s[t])) t++;
                if (!starts(s + t, "not") && !(j-t>=9 && strstr(s+t,"color-mix(") && strstr(s+t,"color-mix(")<s+j)) parse_block(s + bs, be - bs, base, author, depth);
            } else if (starts(s + i, "@layer") || starts(s + i, "@scope") || starts(s + i, "@document") ||
                       starts(s + i, "@-moz-document")) parse_block(s + bs, be - bs, base, author, depth);
            /* @font-face, @keyframes, @page, @container... — пропускаем */
        } else add_style_rule(s + i, j - i, s + bs, be - bs, base, author);
        i = k;
    }
}

static void parse_sheet(const char *s, int n, const char *base, int author, int depth) {
    char *t = malloc(n + 1);                                      /* без комментариев */
    int o = 0;
    char q = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i+1<n) { t[o++]=s[i++]; t[o++]=s[i]; continue; }
        if (!q && s[i] == '/' && i + 1 < n && s[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(s[i] == '*' && s[i + 1] == '/')) i++;
            i++;
            t[o++] = ' ';
            continue;
        }
        if (q) { if (s[i] == '\\' && i + 1 < n) { t[o++] = s[i++]; } else if (s[i] == q || s[i] == '\n') q = 0; }
        else if (s[i] == '"' || s[i] == '\'') q = s[i];
        t[o++] = s[i];
    }
    t[o] = 0;
    parse_block(t, o, base, author, depth);
    free(t);
}

/* Стиль браузера по умолчанию (как в настоящих браузерах, чуть упрощённо) */
static const char *ua_sheet =
    "button,input,select,textarea{display:inline-block}"
    "html,address,blockquote,body,center,div,figure,figcaption,footer,form,header,hr,legend,listing,main,p,plaintext,"
    "pre,xmp,search,article,aside,h1,h2,h3,h4,h5,h6,hgroup,nav,section,dir,dd,dl,dt,menu,ol,ul,fieldset,details,"
    "summary,optgroup,address,frameset,frame,dialog[open]{display:block}"
    "li{display:list-item}"
    "table{display:table}caption{display:table-caption}tr{display:table-row}td,th{display:table-cell}"
    "thead,tbody,tfoot{display:table-row-group}col,colgroup{display:table-column}"
    "head,script,style,template,title,meta,link,base,datalist,param,noembed,noframes,area,map,dialog,[hidden],"
    "input[type=hidden]{display:none}"
    "dialog[open]{display:block}"
    "html{color:#141414;font-size:16px}"
    "body{margin:8px}"
    "p,dl{margin:1em 0}blockquote,figure{margin:1em 32px}dd{margin-left:32px}"
    "h1{font-size:2em;margin:.67em 0;font-weight:bold}h2{font-size:1.5em;margin:.83em 0;font-weight:bold}"
    "h3{font-size:1.17em;margin:1em 0;font-weight:bold}h4{margin:1.33em 0;font-weight:bold}"
    "h5{font-size:.83em;margin:1.67em 0;font-weight:bold}h6{font-size:.67em;margin:2.33em 0;font-weight:bold}"
    "ul,ol,menu,dir{margin:1em 0;padding-left:32px}ul,menu,dir{list-style-type:disc}ol{list-style-type:decimal}"
    "li ul,li ol,ul ul,ul ol,ol ul,ol ol,li menu{margin:0}ul ul,ol ul,menu ul{list-style-type:circle}"
    "b,strong,th{font-weight:bold}i,em,cite,var,dfn,address{font-style:italic}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "a:link{color:#0000ee;text-decoration:underline}"
    "center{text-align:center}"
    "pre,code,kbd,samp,tt{font-family:monospace}pre,listing,xmp,plaintext{white-space:pre;margin:1em 0}textarea{white-space:pre-wrap}"
    "hr{margin:.5em 0}big{font-size:larger}small,sub,sup{font-size:smaller}"
    "mark{background-color:#ff0;color:#000}"
    "fieldset{margin:0 2px;padding:.35em .75em .625em;border:2px groove #c0c0c0}"
    "details:not([open])>:not(summary){display:none}"
    "nobr{white-space:nowrap}";

/* правила устарели? (страница поменяла <style>/<link>) — собрать заново */
static void css_ensure(void) {
    if (built && built_mut == dom_mut) return;
    built_mut = dom_mut;
    if (!d_document) return;
    unsigned sig = dom_css ? 1 : 2;
    for (dnode *n = d_document->first; n; n = walk_next(n, d_document)) {
        if (n->type != DN_ELEMENT) continue;
        if (!strcmp(n->tag, "style")) {
            int l = 0;
            for (dnode *k = n->first; k; k = k->next) if (k->type == DN_TEXT) l += k->tlen * 31 + k->id;
            sig = sig * 131 + n->id * 7 + l;
        } else if (!strcmp(n->tag, "link")) {
            const char *h = dn_get_attr(n, "href"), *r = dn_get_attr(n, "rel");
            if (h && r) sig = sig * 131 + khash(h, strlen(h)) + khash(r, strlen(r));
        }
    }
    if (built && sig == built_sig) return;
    built = 1;
    built_sig = sig;
    rules_free();
    parse_sheet(ua_sheet, strlen(ua_sheet), "", 0, 0);
    if (!dom_css) return;
    for (dnode *n = d_document->first; n; n = walk_next(n, d_document)) {
        if (n->type != DN_ELEMENT) continue;
        const char *media = dn_get_attr(n, "media");
        if (media && !css_media_ok(media, -1)) continue;
        if (!strcmp(n->tag, "style")) {
            jsbuf b = { 0 };
            for (dnode *k = n->first; k; k = k->next) if (k->type == DN_TEXT) jb_add(&b, k->text, k->tlen);
            if (b.n) parse_sheet(b.p, b.n, d_url, 1, 0);
            jb_free(&b);
        } else if (!strcmp(n->tag, "link")) {
            const char *h = dn_get_attr(n, "href"), *r = dn_get_attr(n, "rel"), *as = dn_get_attr(n, "as");
            if (!h || !r || dn_get_attr(n, "disabled")) continue;
            char rel[64];
            snprintf(rel, sizeof(rel), " %s ", r);
            for (char *p = rel; *p; p++) *p = lc(*p);
            int is_sheet = strstr(rel, " stylesheet ") && !strstr(rel, " alternate ");
            if (!is_sheet && strstr(rel, " preload ") && as && !strcmp(as, "style")) is_sheet = 1;   /* загрузится и станет stylesheet */
            if (!is_sheet) continue;
            char full[1024];
            dn_resolve(d_url, h, full, sizeof(full));
            if (strncmp(full, "http", 4) && strncmp(full, "file:", 5)) continue;
            struct sheet *sh = get_sheet(full);
            if (sh && sh->text) parse_sheet(sh->text, sh->len, full, 1, 0);
        }
    }
}

/* ------------------------------------------------------------------ вычисление стиля */

struct cvar { const char *name; const char *val; struct cvar *next; };

struct cstyle {
    struct dom_style s;
    struct cvar *vars;              /* переменные --x (свои, потом унаследованные) */
    int gen;
    int eff_bg;                     /* фон под элементом (свой или предков) — для полупрозрачных цветов */
    unsigned char vis_hidden;
};

static struct cstyle *cache;
static int cache_cap, gen = 1, cache_mut = -1;
static double root_px = 16;

void dom_set_viewport(int width, int height) {
    if (width < 1 || height < 1 || (width == VIEW_W && height == VIEW_H)) return;
    dom_view_width = width;
    dom_view_height = height;
    built = 0;                 /* @media rules must be parsed again; downloaded sheets stay cached */
    built_mut = cache_mut = -1;
    dom_dirty = 1;
    if (js_global) {
        jsval window = js_objv(js_global);
        js_set_c(window, "innerWidth", js_num(width));
        js_set_c(window, "innerHeight", js_num(height));
        js_set_c(window, "outerWidth", js_num(width + 12));
        js_set_c(window, "outerHeight", js_num(height + 56));
    }
}

int dom_layout_children(int id) {
    if (id < 0 || id >= d_count) return 0;
    int count = 0;
    for (dnode *n = d_nodes[id]->first; n; n = n->next)
        if (n->type == DN_ELEMENT && strcmp(n->tag, "script") && strcmp(n->tag, "style") &&
            strcmp(n->tag, "template") && !css_hidden(n) && (css_style(n)->fixed==0 || css_style(n)->fixed==3)) count++;
    return count;
}

/* Immediate visible element children, also used by the browser's flex sizing pass. */
int dom_layout_next(int parent, int previous) {
    if (parent < 0 || parent >= d_count) return -1;
    dnode *n = previous >= 0 && previous < d_count ? d_nodes[previous]->next : d_nodes[parent]->first;
    for (; n; n = n->next)
        if (n->type == DN_ELEMENT && strcmp(n->tag,"script") && strcmp(n->tag,"style") &&
            strcmp(n->tag,"template") && !css_hidden(n) && (css_style(n)->fixed==0 || css_style(n)->fixed==3)) return n->id;
    return -1;
}

static int intrinsic_node(dnode *n,int (*measure)(const char *,int,int),int depth) {
    if(depth>32)return 0;
    if(n->type==DN_TEXT) {
        char text[256];int used=0,space=0;
        for(int i=0;i<n->tlen && used<254;i++) {
            if(is_sp(n->text[i])) {space=used>0;continue;}
            if(space)text[used++]=' ';
            space=0;
            text[used++]=n->text[i];
        }
        text[used]=0;const struct dom_style *s=css_style(n->parent);
        return measure(text,s->font_px,s->bold|(s->font_family<<1));
    }
    if(n->type!=DN_ELEMENT || css_hidden(n) || !strcmp(n->tag,"script") || !strcmp(n->tag,"style"))return 0;
    const struct dom_style *s=css_style(n);
    if(s->fixed==1 || s->fixed==2)return 0;
    int extra=s->padding[1]+s->padding[3]+s->border[1]+s->border[3];
    if(s->width>=0 && !s->width_pct)return s->width+(s->border_box?0:extra);
    if(!strcmp(n->tag,"input")) {
        const char *type=dn_get_attr(n,"type"),*value=dn_get_attr(n,"value");
        if(type && !strcmp(type,"hidden"))return 0;
        if(type && (!strcmp(type,"submit") || !strcmp(type,"button")))
            return measure(value && *value?value:"Submit",s->font_px,s->bold|(s->font_family<<1))+20+extra;
        const char *size=dn_get_attr(n,"size");int chars=size?atoi(size):24;
        if(chars<1)chars=1;
        if(chars>200)chars=200;
        return chars*8+8+extra;
    }
    if(!strcmp(n->tag,"svg") || !strcmp(n->tag,"img")) {
        const char *width=dn_get_attr(n,"width"),*src=dn_get_attr(n,"src");
        int w=width?atoi(width):src?measure(src,-1,0):24;return (w>0?w:24)+extra;
    }
    int width=0,line=0,count=0;
    for(dnode *k=n->first;k;k=k->next) {
        int w=intrinsic_node(k,measure,depth+1);
        if(!w)continue;
        if(s->flex_row || (k->type==DN_ELEMENT && css_style(k)->float_side)) {if(count++)line+=s->column_gap;line+=w;}
        else if(k->type==DN_ELEMENT && css_style(k)->display!=DS_INLINE && css_style(k)->display!=DS_INLINE_BLOCK) {
            if(line>width)width=line;
            line=0;
            if(w>width)width=w;
        } else line+=w;
    }
    if(line>width)width=line;
    if(s->max_width>=0 && !s->max_width_pct && width>s->max_width)width=s->max_width;
    return width+extra;
}
int dom_layout_intrinsic(int id,int (*measure)(const char *,int,int)) {
    if(id<0 || id>=d_count)return 0;
    return intrinsic_node(d_nodes[id],measure,0);
}

void css_reset(void) {
    rules_free();
    built = 0;
    built_mut = -1;
    for (int i = 0; i < n_sheets; i++) { free(sheets[i].url); free(sheets[i].text); }
    n_sheets = 0;
    sheet_bytes = 0;
    ar_free(&tmp_arena);
    free(cache);
    cache = NULL;
    cache_cap = 0;
    gen++;
    cache_mut = -1;
}

struct win { unsigned long long prio; const char *val; unsigned char sh, idx; };

struct cascade {
    struct win w[S_COUNT];
    struct { const char *name; const char *val; unsigned long long prio; } v[1024];
    int nv;
};

static void apply_block(struct cascade *c, struct dblock *b, unsigned long long base) {
    for (int i = 0; i < b->n; i++) {
        struct decl *d = &b->d[i];
        unsigned long long prio = base | ((unsigned long long)d->imp << 50);
        if (d->prop < 0) {
            int k;
            for (k = 0; k < c->nv; k++) if (!strcmp(c->v[k].name, d->name)) break;
            if (k == c->nv) { if (c->nv >= (int)(sizeof(c->v)/sizeof(c->v[0]))) continue; c->nv++; c->v[k].name = d->name; c->v[k].prio = 0; c->v[k].val = NULL; }
            if (prio >= c->v[k].prio) { c->v[k].prio = prio; c->v[k].val = d->val; }
            continue;
        }
        const struct propdef *p = &props[d->prop];
        for (int m = 0; m < p->n; m++) {
            struct win *w = &c->w[p->m[m].slot];
            if (prio >= w->prio) { w->prio = prio; w->val = d->val; w->sh = p->m[m].sh; w->idx = p->m[m].idx; }
        }
    }
}

static void check_rules(struct cascade *c, int idx, dnode *n) {
    for (; idx >= 0; idx = rules[idx].next) {
        struct rule *r = &rules[idx];
        if (!sel_match(r->sel, n)) continue;
        unsigned long long base = ((unsigned long long)r->author << 49) | ((unsigned long long)r->spec << 24) | (unsigned)r->order;
        apply_block(c, r->b, base);
    }
}

/* var(--x, запасное) -> значение; 0 — нет такой переменной */
static int subst_vars(const char *v, char *out, int max, struct cvar *vars, int depth) {
    int o = 0;
    while (*v) {
        if(o>=max-1)return 0;
        if (v[0] == 'v' && !strncmp(v, "var(", 4)) {
            const char *p = v + 4;
            while (is_sp(*p)) p++;
            const char *ns = p;
            while (*p && *p != ',' && *p != ')' && !is_sp(*p)) p++;
            int nl = p - ns;
            while (is_sp(*p)) p++;
            const char *fb = NULL;
            int fbl = 0;
            const char *e = p;
            if (*p == ',') {                                    /* запасное значение — до парной ) */
                fb = p + 1;
                int d = 1;
                e = fb;
                while (*e && d) { if (*e == '(') d++; else if (*e == ')') { if (--d == 0) break; } e++; }
                fbl = e - fb;
            } else e = p;
            if (*e == ')') e++;
            const char *val = NULL;
            for (struct cvar *k = vars; k; k = k->next)
                if ((int)strlen(k->name) == nl && !strncmp(k->name, ns, nl)) { val = k->val; break; }
            if (val && depth < 8 && subst_vars(val, out+o, max-o, vars, depth+1)) {
                o+=strlen(out+o);
            } else if (fb && depth<8 && fbl<65536) {
                char *fallback=malloc(fbl+1);if(!fallback)return 0;
                memcpy(fallback,fb,fbl);fallback[fbl]=0;
                int valid=subst_vars(fallback,out+o,max-o,vars,depth+1);free(fallback);
                if(!valid)return 0;
                o+=strlen(out+o);
            } else return 0;
            v = e;
            continue;
        }
        out[o++] = *v++;
    }
    out[o] = 0;
    return 1;
}

/* часть сокращённой записи (margin: 1px 2px -> второе слово и т. д.) */
#include "css_background.h"
#include "css_effects.h"

static int sh_part(const char *v, int sh, int idx, char *out, int max) {
    const char *w[24];
    int wl[24];
    int n = split_words(v, w, wl, 24);
    int k = 0;                                                  /* выкинем запятые */
    for (int i = 0; i < n; i++) if (!(wl[i] == 1 && w[i][0] == ',')) { w[k] = w[i]; wl[k++] = wl[i]; }
    n = k;
    #define PUT(i) do { int _l = wl[i] < max - 1 ? wl[i] : max - 1; memcpy(out, w[i], _l); out[_l] = 0; return 1; } while (0)
    #define PUTS(s) do { snprintf(out, max, "%s", s); return 1; } while (0)
    int rgb;
    double a;
    switch (sh) {
    case SH_BOX: {
        if (!n) return 0;
        static const int map[4][4] = { { 0, 0, 0, 0 }, { 0, 1, 0, 1 }, { 0, 1, 2, 1 }, { 0, 1, 2, 3 } };
        int c = n > 4 ? 4 : n;
        PUT(map[c - 1][idx]);
    }
    case SH_PAIR:
        if (!n) return 0;
        PUT(idx < n ? idx : 0);
    case SH_FLEX: {
        if (n == 1 && word_is(w[0], wl[0], "none")) PUTS(idx == 2 ? "auto" : "0");
        if (n == 1 && word_is(w[0], wl[0], "auto")) PUTS(idx == 2 ? "auto" : "1");
        int numbers = 0, basis = -1;
        for (int i = 0; i < n; i++) {
            double d;
            if (read_num(w[i], &d) == wl[i]) { if (numbers++ == idx && idx < 2) PUT(i); }
            else basis = i;
        }
        if (idx == 2 && basis >= 0) PUT(basis);
        PUTS(idx == 0 ? "1" : idx == 1 ? "1" : "0%");
    }
    case SH_FIRST:
        if (!n) return 0;
        PUT(0);
    case SH_BGEXTRA: return bg_extract(v,idx,out,max,0);
    case SH_MASKEXTRA: return bg_extract(v,idx,out,max,1);
    case SH_MASKMODE: return mask_extra(v,idx,out,max);
    case SH_BG:
        if (n == 1 && (word_is(w[0], wl[0], "inherit") || word_is(w[0], wl[0], "initial") || word_is(w[0], wl[0], "unset"))) PUT(0);
        for (int i = n - 1; i >= 0; i--) if (parse_color(w[i], wl[i], &rgb, &a)) PUT(i);
        PUTS("transparent");
    case SH_FONT: {
        if (n == 1 && (word_is(w[0], wl[0], "inherit") || word_is(w[0], wl[0], "initial"))) PUT(0);
        /* [style] [variant] [weight] size[/line-height] family */
        for (int i = 0; i < n; i++) {
            double d;
            int isnum = read_num(w[i], &d) == wl[i];
            if (idx == 0 && (word_is(w[i], wl[i], "bold") || word_is(w[i], wl[i], "bolder") || word_is(w[i], wl[i], "lighter") ||
                             (isnum && d >= 100 && d <= 1000))) PUT(i);
            if (idx == 1 && (word_is(w[i], wl[i], "italic") || word_is(w[i], wl[i], "oblique"))) PUT(i);
            if ((idx == 2 || idx==3) && !isnum) {
                struct lctx c = { 16, 16, 100 };
                char t[64];
                int l = wl[i] < 63 ? wl[i] : 63;
                memcpy(t, w[i], l);
                t[l] = 0;
                char *sl = strchr(t, '/');
                if (sl) *sl = 0;
                if(parse_len(t,strlen(t),&c,&d)==1 || value_ok(S_FSIZE,t,strlen(t))) {
                    if(idx==3 && i+1<n)PUTS(w[i+1]);
                    if(idx==2)PUTS(t);
                }
            }
        }
        PUTS(idx==3?"sans-serif":"normal");
    }
    case SH_BORDER:
        for (int i = 0; i < n; i++) {
            struct lctx c = { 16, 16, -1 };
            double d;
            static const char *styles[] = { "none", "hidden", "solid", "dashed", "dotted", "double", "groove", "ridge",
                                            "inset", "outset", NULL };
            int is_style = 0;
            for (int s = 0; styles[s]; s++) if (word_is(w[i], wl[i], styles[s])) is_style = 1;
            if (idx == 2 && is_style) PUT(i);
            if (idx == 0 && !is_style && parse_len(w[i], wl[i], &c, &d) == 1) PUT(i);
            if (idx == 1 && !is_style && parse_color(w[i], wl[i], &rgb, &a)) PUT(i);
        }
        if (n == 1 && (word_is(w[0], wl[0], "inherit") || word_is(w[0], wl[0], "initial"))) PUT(0);
        PUTS(idx == 0 ? "3px" : idx == 1 ? "currentcolor" : "none");
    case SH_LIST:
        for (int i = 0; i < n; i++)
            if (!starts(w[i], "url(") && !word_is(w[i], wl[i], "inside") && !word_is(w[i], wl[i], "outside")) PUT(i);
        PUTS("disc");
    case SH_TDEC:
        for (int i = 0; i < n; i++)
            if (word_is(w[i], wl[i], "underline") || word_is(w[i], wl[i], "line-through") || word_is(w[i], wl[i], "none") ||
                word_is(w[i], wl[i], "overline") || word_is(w[i], wl[i], "inherit") || word_is(w[i], wl[i], "initial") ||
                word_is(w[i], wl[i], "unset")) PUT(i);
        PUTS("none");
    }
    #undef PUT
    #undef PUTS
    return 0;
}

static void initial_style(struct cstyle *c) {
    memset(c, 0, sizeof(*c));
    c->s.display = DS_INLINE;
    c->s.color = 0x141414;c->s.opacity=10000;
    c->s.bg = -1;
    c->s.font_px = 16;
    c->s.width = c->s.max_width = -1;
    c->s.height = -1; c->s.min_height = 0; c->s.flex_shrink = 100;
    for (int i = 0; i < 4; i++) c->s.border_color[i] = -1;
    c->eff_bg = 0xffffff;
}

static int parse_display(const char *v) {
    int n = strlen(v);
    static const struct { const char *k; int d; } map[] = {
        { "none", DS_NONE }, { "block", DS_BLOCK }, { "inline", DS_INLINE }, { "inline-block", DS_INLINE_BLOCK },
        { "flex", DS_FLEX }, { "-webkit-flex", DS_FLEX }, { "-ms-flexbox", DS_FLEX }, { "-webkit-box", DS_FLEX },
        { "inline-flex", DS_INLINE_BLOCK }, { "-webkit-inline-flex", DS_INLINE_BLOCK }, { "-ms-inline-flexbox", DS_INLINE_BLOCK },
        { "-webkit-inline-box", DS_INLINE_BLOCK }, { "grid", DS_GRID }, { "inline-grid", DS_INLINE_BLOCK },
        { "list-item", DS_LIST_ITEM }, { "table", DS_TABLE }, { "inline-table", DS_INLINE_BLOCK }, { "table-row", DS_ROW },
        { "table-cell", DS_CELL }, { "table-row-group", DS_BLOCK }, { "table-header-group", DS_BLOCK },
        { "table-footer-group", DS_BLOCK }, { "table-caption", DS_BLOCK }, { "table-column", DS_NONE },
        { "table-column-group", DS_NONE }, { "contents", DS_CONTENTS }, { "flow-root", DS_BLOCK }, { "flow", DS_BLOCK },
        { "ruby", DS_INLINE }, { "run-in", DS_INLINE }, { "block flex", DS_FLEX }, { "inline flex", DS_INLINE_BLOCK },
        { "block flow", DS_BLOCK }, { "inline flow-root", DS_INLINE_BLOCK },
    };
    for (int i = 0; i < (int)(sizeof(map) / sizeof(map[0])); i++) if (word_is(v, n, map[i].k)) return map[i].d;
    return -1;
}

static short clamp_px(double v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : (short)(v + (v < 0 ? -0.5 : 0.5)); }

/* вычислить стиль элемента n по стилю родителя ps */
static void compute(dnode *n, struct cstyle *ps, struct cstyle *out) {
    static struct cascade c;                                  /* большая — не на стеке */
    memset(c.w, 0, sizeof(c.w));
    c.nv = 0;
    if (universal >= 0) check_rules(&c, universal, n);
    check_rules(&c, buckets[khash(n->tag, strlen(n->tag)) % BUCKETS], n);
    char key[96];
    const char *id = dn_get_attr(n, "id");
    if (id && *id && strlen(id) < sizeof(key) - 2) { int l = snprintf(key, sizeof(key), "#%s", id); check_rules(&c, buckets[khash(key, l) % BUCKETS], n); }
    const char *cl = dn_get_attr(n, "class");
    unsigned seen[16];
    int nseen = 0;
    while (cl && *cl) {
        while (*cl && is_sp(*cl)) cl++;
        const char *st = cl;
        while (*cl && !is_sp(*cl)) cl++;
        if (cl > st && cl - st < 90) {
            key[0] = '.';
            memcpy(key + 1, st, cl - st);
            unsigned h = khash(key, cl - st + 1) % BUCKETS;
            int dup = 0;
            for (int i = 0; i < nseen; i++) if (seen[i] == h) dup = 1;
            if (!dup) { if (nseen < 16) seen[nseen++] = h; check_rules(&c, buckets[h], n); }
        }
    }
    const char *inl = dom_css ? dn_get_attr(n, "style") : NULL;
    if (inl && *inl) {                                          /* style="" — сильнее любого селектора */
        struct dblock *b = parse_decls(inl, strlen(inl), d_url, &tmp_arena);
        if (b) apply_block(&c, b, (1ULL << 49) | (0xFFFFFFULL << 24));
    }

    /* переменные: свои поверх унаследованных */
    out->vars = ps->vars;
    for (int i = 0; i < c.nv; i++) {
        if (!c.v[i].val) continue;
        struct cvar *v = ar_alloc(&tmp_arena, sizeof(struct cvar));
        v->name = c.v[i].name;
        v->val = c.v[i].val;
        v->next = out->vars;
        out->vars = v;
    }

    /* значения по слотам */
    char vals[S_COUNT][256];
    unsigned char have[S_COUNT];
    for (int s = 0; s < S_COUNT; s++) {
        have[s] = 0;
        struct win *w = &c.w[s];
        if (!w->val) continue;
        static char tmp[8192];
        const char *v = w->val;
        if((s==S_BGIMG || s==S_MASKIMG) && (strlen(v)>=sizeof(vals[s]) || strstr(v,"var("))) {
            strcpy(vals[s],"image");have[s]=1;continue;
        }
        if (strstr(v, "var(")) {
            if (!subst_vars(v, tmp, sizeof(tmp), out->vars, 0)) continue;   /* неизвестная переменная — как не задано */
            v = tmp;
        }
        if (w->sh) {
            if (!sh_part(v, w->sh, w->idx, vals[s], sizeof(vals[s]))) continue;
        } else {
            int length=strlen(v);if(length>=(int)sizeof(vals[s]))continue;
            memcpy(vals[s],v,length+1);
        }
        char *p = vals[s];                                      /* обрезать пробелы */
        while (is_sp(*p)) p++;
        if (p != vals[s]) memmove(vals[s], p, strlen(p) + 1);
        int l = strlen(vals[s]);
        while (l && is_sp(vals[s][l - 1])) vals[s][--l] = 0;
        if (!l) continue;
        if (word_is(vals[s], l, "inherit")) { have[s] = 2; continue; }
        if (word_is(vals[s], l, "initial") || word_is(vals[s], l, "revert") || word_is(vals[s], l, "revert-layer")) { have[s] = 3; continue; }
        if (word_is(vals[s], l, "unset")) { have[s] = inherited[s] ? 2 : 3; continue; }
        if (w->sh == 0 && strstr(w->val, "var(") && !value_ok(s, vals[s], l)) continue;
        have[s] = 1;
    }
    #define V(s) (have[s] == 1 ? vals[s] : NULL)

    struct dom_style *o = &out->s, *p = &ps->s;
    /* унаследованное */
    o->pointer_none=p->pointer_none; o->font_family=p->font_family; o->color = p->color; o->bold = p->bold; o->italic = p->italic; o->underline = p->underline; o->strike = p->strike;
    o->align = p->align; o->transform = p->transform; o->pre = p->pre; o->nowrap = p->nowrap; o->wrap_word=p->wrap_word; o->list = p->list;
    o->line_height = p->line_height;
    o->font_px = p->font_px; o->notext = p->notext; out->vis_hidden = ps->vis_hidden;
    /* сброшенное */
    o->display = DS_INLINE; o->bg = -1; o->hidden = 0; o->flex_row = o->flex_column = o->flex_reverse = 0; o->bg_image = o->bg_clip = 0; o->background=o->mask=NULL;o->shadows=NULL;o->opacity=10000;o->mask_luminance=0;memset(o->mask_composite,0,sizeof(o->mask_composite));
    o->object_fit=0;o->object_pos[0]=o->object_pos[1]=50;
    o->float_side=o->clear_side=0; o->has_z_index=0; o->z_index=0; o->min_width=0; o->min_width_pct=0;
    o->width = o->max_width = -1; o->width_pct = o->max_width_pct = 0;
    o->height = -1; o->min_height = 0; o->grid_columns = 0;o->grid_auto=0;o->grid_min=0;memset(o->grid_track,0,sizeof(o->grid_track));memset(o->grid_track_type,0,sizeof(o->grid_track_type)); o->radius = 0; o->fixed=0; o->height_pct=o->position_pct=o->translate_pct=0;
    o->translate_x=o->translate_y=0;
    for(int i=0;i<4;i++)o->position_edge[i]=CSS_AUTO;
    o->flex_grow = 0; o->flex_shrink = 100;
    o->justify = o->align_items = o->align_self = 0;
    o->border_box = o->flex_wrap = 0; o->column_gap = o->row_gap = 0;
    for (int i = 0; i < 4; i++) { o->margin[i] = o->padding[i] = o->border[i] = 0; o->border_color[i] = -1; }
    struct cstyle init;
    initial_style(&init);
    for (int s = 0; s < S_COUNT; s++) {                         /* inherit у ненаследуемых / initial у наследуемых */
        if (have[s] == 2 && !inherited[s]) {
            if (s == S_BG) o->bg = p->bg;
            if (s == S_DISPLAY) o->display = p->display;
            if (s == S_OPACITY) o->opacity=p->opacity;
            if (s == S_SHADOW) o->shadows=p->shadows;
        }
        if (have[s] == 3 && inherited[s]) {
            if (s == S_COLOR) o->color = init.s.color;
            if (s == S_FW) o->bold = 0;
            if (s == S_FSTYLE) o->italic = 0;
            if (s == S_FSIZE) o->font_px = 16;
            if (s == S_FAMILY) o->font_family=0;
            if (s == S_POINTER) o->pointer_none=0;
            if (s == S_LINEHEIGHT) o->line_height = 0;
            if (s == S_WORDWRAP) o->wrap_word&=~1;
            if (s == S_WORDBREAK) o->wrap_word&=~2;
            if (s == S_TDEC) o->underline = o->strike = 0;
            if (s == S_TALIGN) o->align = 0;
            if (s == S_TTRANS) o->transform = 0;
            if (s == S_WS) o->pre = o->nowrap = 0;
            if (s == S_LIST) o->list = LS_DISC;
            if (s == S_VIS) out->vis_hidden = 0;
        }
    }

    /* шрифт */
    const char *v;
    double d;
    if ((v = V(S_FSIZE))) {
        static const struct { const char *k; double px; } kw[] = {
            { "xx-small", 9 }, { "x-small", 10 }, { "small", 13 }, { "medium", 16 }, { "large", 18 }, { "x-large", 24 },
            { "xx-large", 32 }, { "xxx-large", 48 } };
        int done = 0;
        for (int i = 0; i < 8; i++) if (word_is(v, strlen(v), kw[i].k)) { o->font_px = kw[i].px; done = 1; }
        if (!done && word_is(v, strlen(v), "smaller")) { o->font_px = p->font_px * 5 / 6; done = 1; }
        if (!done && word_is(v, strlen(v), "larger")) { o->font_px = p->font_px * 6 / 5; done = 1; }
        struct lctx lc2 = { p->font_px, root_px, p->font_px };
        if (!done && parse_len(v, strlen(v), &lc2, &d) == 1) o->font_px = clamp_px(d, 0, 400);
    }
    if (!n->parent || n->parent->type != DN_ELEMENT) root_px = o->font_px;      /* <html> */
    struct lctx L = { o->font_px, root_px, VIEW_W };
    if ((v = V(S_LINEHEIGHT))) {
        int l = strlen(v), num = read_num(v, &d);
        if (starts(v, "normal")) o->line_height = 0;
        else if (num == l && d >= 0) o->line_height = -clamp_px(d * 100, 1, 10000);
        else if (parse_len(v, l, &(struct lctx){o->font_px, root_px, o->font_px}, &d) == 1)
            o->line_height = clamp_px(d, 1, 20000);
    }
    if ((v = V(S_FW))) {
        if (word_is(v, strlen(v), "bold") || word_is(v, strlen(v), "bolder")) o->bold = 1;
        else if (word_is(v, strlen(v), "normal") || word_is(v, strlen(v), "lighter")) o->bold = 0;
        else if (read_num(v, &d)) o->bold = d >= 600;
    }
    if ((v = V(S_FSTYLE))) o->italic = starts(v, "italic") || starts(v, "oblique");
    if ((v = V(S_TDEC))) {
        if (starts(v, "underline")) { o->underline = 1; o->strike = 0; }
        else if (starts(v, "line-through")) { o->strike = 1; o->underline = 0; }
        else if (starts(v, "none") || starts(v, "overline")) o->underline = o->strike = 0;
    }
    if ((v = V(S_TALIGN))) {
        if (starts(v, "center") || starts(v, "-webkit-center") || starts(v, "-moz-center")) o->align = 1;
        else if (starts(v, "right") || starts(v, "end") || starts(v, "-webkit-right")) o->align = 2;
        else if (starts(v, "left") || starts(v, "start") || starts(v, "justify") || starts(v, "-webkit-left")) o->align = 0;
    }
    if ((v = V(S_TTRANS))) o->transform = starts(v, "uppercase") ? 1 : starts(v, "lowercase") ? 2 : starts(v, "capitalize") ? 3 : 0;
    if ((v = V(S_WS))) {
        o->pre = starts(v, "pre") || starts(v, "break-spaces");
        o->nowrap = starts(v, "nowrap");
    }
    if ((v = V(S_LIST))) {
        if (starts(v, "none")) o->list = LS_NONE;
        else if (starts(v, "circle")) o->list = LS_CIRCLE;
        else if (starts(v, "square")) o->list = LS_SQUARE;
        else if (starts(v, "decimal") || starts(v, "lower-roman") || starts(v, "upper-roman") || starts(v, "cjk")) o->list = LS_DECIMAL;
        else if (starts(v, "lower-alpha") || starts(v, "upper-alpha") || starts(v, "lower-latin") || starts(v, "upper-latin")) o->list = LS_ALPHA;
        else if (starts(v, "disc")) o->list = LS_DISC;
        else if (v[0] == '"' || v[0] == '\'') o->list = LS_DISC;
    }
    if((v=V(S_POINTER)))o->pointer_none=starts(v,"none");
    if ((v = V(S_VIS))) out->vis_hidden = starts(v, "hidden") || starts(v, "collapse");

    /* фон и цвет */
    int rgb;
    double a;
    out->eff_bg = ps->eff_bg;
    if ((v = V(S_BG))) {
        int r = parse_color(v, strlen(v), &rgb, &a);
        if (r == 2) { rgb = p->color; a = 1; r = 1; }
        if (r == 1 && a >= 0.05) { o->bg = blend(rgb, ps->eff_bg, a); out->eff_bg = o->bg; }
    }
    /* Images need a larger buffer than ordinary computed property values.
     * Inline SVG/data URLs must not be truncated at the old 255-byte boundary. */
    static char bgvalue[8192];const char *image=NULL;
    if(have[S_BGIMG]==1 && c.w[S_BGIMG].val) {
        const char *raw=c.w[S_BGIMG].val;
        if(strstr(raw,"var(")) {if(subst_vars(raw,bgvalue,sizeof(bgvalue),out->vars,0))image=bgvalue;}
        else if(strlen(raw)<sizeof(bgvalue)){strcpy(bgvalue,raw);image=bgvalue;}
    }
    o->background=bg_parse(image,V(S_BGSIZE),V(S_BGPOS),V(S_BGREPEAT),V(S_BGORIGIN),V(S_BGCLIP),&L);
    o->bg_image=o->background!=NULL;
    if(V(S_BGCLIP)) {
        const char *parts[CSS_BG_LAYERS];int lengths[CSS_BG_LAYERS];
        int count=image?bg_parts(image,parts,lengths,CSS_BG_LAYERS):1;
        const char *clip=bg_at(V(S_BGCLIP),count>0?count-1:0);
        if(clip)o->bg_clip=starts(clip,"content-box")?2:starts(clip,"padding-box")?1:0;
    }
    if((v=V(S_OBJECTFIT)))o->object_fit=starts(v,"contain")?1:starts(v,"cover")?2:starts(v,"none")?3:starts(v,"scale-down")?4:0;
    if((v=V(S_OBJECTPOS))) {unsigned char pct=3;bg_pair(v,o->object_pos,&pct,&L,1);if(pct!=3)o->object_pos[0]=o->object_pos[1]=50;}
    if((v=V(S_WORDWRAP)))o->wrap_word=(o->wrap_word&2)|((starts(v,"anywhere") || starts(v,"break-word"))?1:0);
    if((v=V(S_WORDBREAK)))o->wrap_word=(o->wrap_word&1)|(starts(v,"break-all")?2:0)|(starts(v,"break-word")?1:0);
    if ((v = V(S_COLOR))) {
        int r = parse_color(v, strlen(v), &rgb, &a);
        if (r == 1) {
            if (a < 0.05) o->notext = 1;                        /* color: transparent — текст «спрятан» */
            else o->color = blend(rgb, out->eff_bg, a);
        }
    }

    if((v=V(S_FAMILY)))o->font_family=css_font_family(v);
    if((v=V(S_SHADOW)))o->shadows=css_shadows(v,&L,o->color);
    static char maskvalue[8192];const char *mask=NULL;
    if(have[S_MASKIMG]==1 && c.w[S_MASKIMG].val) {
        const char *raw=c.w[S_MASKIMG].val;
        if(strstr(raw,"var(")){if(subst_vars(raw,maskvalue,sizeof(maskvalue),out->vars,0))mask=maskvalue;}
        else if(strlen(raw)<sizeof(maskvalue)){strcpy(maskvalue,raw);mask=maskvalue;}
    }
    const char *mask_origin=V(S_MASKORIGIN);if(!mask_origin)mask_origin="border-box";
    o->mask=bg_parse(mask,V(S_MASKSIZE),V(S_MASKPOS),V(S_MASKREPEAT),mask_origin,V(S_MASKCLIP),&L);
    for(int i=0;o->mask && i<o->mask->count;i++) {
        const char *mode=bg_at(V(S_MASKMODE),i),*composite=bg_at(V(S_MASKCOMPOSITE),i);
        if(mode && starts(mode,"luminance"))o->mask_luminance|=1<<i;
        if(composite)o->mask_composite[i]=starts(composite,"subtract") || starts(composite,"source-out")?1:
            starts(composite,"intersect") || starts(composite,"source-in") || starts(composite,"destination-in")?2:
            starts(composite,"exclude") || starts(composite,"xor")?3:0;
    }

    /* display */
    int disp = DS_INLINE;
    if (have[S_DISPLAY] == 2) disp = p->display;
    if ((v = V(S_DISPLAY))) { int dd = parse_display(v); if (dd >= 0) disp = dd; }
    int grid_raw = disp == DS_GRID || (v && starts(v,"inline-grid"));
    int flex_raw = disp == DS_FLEX || (v && (starts(v, "inline-flex") || starts(v, "-webkit-inline") || starts(v, "-ms-inline-flex") || starts(v, "inline flex")));
    if (flex_raw) {
        const char *fd = V(S_FLEXDIR);
        o->flex_row = !fd || starts(fd, "row") || starts(fd, "horizontal") || starts(fd, "wrap") || starts(fd, "nowrap");
        if (fd && starts(fd, "row") && strstr(fd, "column")) o->flex_row = 0;
        o->flex_column = !o->flex_row;
        o->flex_reverse=fd && strstr(fd,"reverse");
        const char *wrap = V(S_FLEXWRAP);
        o->flex_wrap = wrap && strstr(wrap, "wrap") && !strstr(wrap, "nowrap");
    }
    if (ps->s.flex_row && (disp == DS_BLOCK || disp == DS_LIST_ITEM || disp == DS_FLEX || disp == DS_GRID || disp == DS_TABLE))
        disp = DS_INLINE_BLOCK;                                 /* элементы flex-строки — в ряд */
    if ((v=V(S_FLOAT))) o->float_side=starts(v,"left")?1:starts(v,"right")?2:0;
    if ((v=V(S_CLEAR))) o->clear_side=starts(v,"left")?1:starts(v,"right")?2:starts(v,"both")?3:0;
    if (ps->s.flex_row || ps->s.flex_column) o->float_side=0;
    o->display = disp;
    if (grid_raw) {
        grid_tracks(o,V(S_GRIDCOLS),&L,0);
        if(!o->grid_columns){o->grid_columns=1;o->grid_track[0]=100;}
        o->flex_row=1; o->flex_wrap=1;
    }

    /* отступы и рамки */
    static const int ms[4] = { S_MT, S_MR, S_MB, S_ML }, pss[4] = { S_PT, S_PR, S_PB, S_PL };
    static const int bw[4] = { S_BTW, S_BRW, S_BBW, S_BLW }, bc[4] = { S_BTC, S_BRC, S_BBC, S_BLC }, bs[4] = { S_BTS, S_BRS, S_BBS, S_BLS };
    for (int i = 0; i < 4; i++) {
        int r;
        if ((v = V(ms[i]))) {
            r = parse_len(v, strlen(v), &L, &d);
            if (r == 2) o->margin[i] = CSS_AUTO;
            else if (r == 1) o->margin[i] = clamp_px(d, -2000, 2000);
        }
        if ((v = V(pss[i])) && parse_len(v, strlen(v), &L, &d) == 1) o->padding[i] = clamp_px(d, 0, 2000);
        const char *st = V(bs[i]);
        if (st && !starts(st, "none") && !starts(st, "hidden")) {
            double wd = 3;
            if ((v = V(bw[i])) && parse_len(v, strlen(v), &L, &d) == 1) wd = d;
            o->border[i] = clamp_px(wd, 0, 100);
            o->border_color[i] = o->color;
            if ((v = V(bc[i]))) {
                int r2 = parse_color(v, strlen(v), &rgb, &a);
                if (r2 == 1) o->border_color[i] = a < 0.05 ? -1 : blend(rgb, out->eff_bg, a);
            }
            if (o->border_color[i] < 0) o->border[i] = 0;
        }
    }
    if ((v = V(S_GROW)) && read_num(v, &d) == (int)strlen(v) && d >= 0) o->flex_grow = clamp_px(d * 100, 0, 10000);
    if ((v = V(S_SHRINK)) && read_num(v, &d) == (int)strlen(v) && d >= 0) o->flex_shrink = clamp_px(d * 100, 0, 10000);
    if ((v = V(S_JUSTIFY))) o->justify = strstr(v, "center") ? 1 : strstr(v, "end") ? 2 :
        starts(v, "space-between") ? 3 : starts(v, "space-around") ? 4 : starts(v, "space-evenly") ? 5 : 0;
    if ((v = V(S_ALIGNITEMS))) o->align_items = strstr(v, "center") ? 1 : strstr(v, "end") ? 2 : strstr(v, "start") ? 3 : 0;
    if ((v = V(S_ALIGNSELF))) o->align_self = strstr(v, "center") ? 1 : strstr(v, "end") ? 2 : strstr(v, "start") ? 3 : strstr(v, "stretch") ? 4 : 0;
    if ((v = V(S_HEIGHT))) {
        if (parse_len(v, strlen(v), &(struct lctx){L.em,L.rem,-1}, &d) == 1) o->height = clamp_px(d, 0, 20000);
        else if(strchr(v,'%') && read_num(v,&d)>0 && !strstr(v,"calc")) {o->height=clamp_px(d,0,2000);o->height_pct=1;}
    }
    if ((v = V(S_MINH)) && parse_len(v, strlen(v), &(struct lctx){L.em,L.rem,-1}, &d) == 1) o->min_height = clamp_px(d, 0, 20000);
    if ((v=V(S_RADIUS)) && parse_len(v,strlen(v),&L,&d)==1) o->radius=clamp_px(d,0,20000);
    o->border_box = (v = V(S_BOXSIZING)) && starts(v, "border-box");
    if ((v = V(S_COLGAP)) && parse_len(v, strlen(v), &L, &d) == 1) o->column_gap = clamp_px(d, 0, 2000);
    if ((v = V(S_ROWGAP)) && parse_len(v, strlen(v), &L, &d) == 1) o->row_gap = clamp_px(d, 0, 2000);
    v = ps->s.flex_row ? V(S_BASIS) : NULL;
    if (!v || starts(v, "auto") || starts(v, "content")) v = V(S_WIDTH);
    if (v) {
        int r = parse_len(v, strlen(v), &(struct lctx){ L.em, L.rem, -1 }, &d);
        if (r == 1) o->width = clamp_px(d, 0, 20000);
        else if (strchr(v, '%') && read_num(v, &d) && !strstr(v, "calc")) { o->width = clamp_px(d, 0, 100); o->width_pct = 1; }
    }
    if ((v = V(S_MINW))) {
        int r = parse_len(v, strlen(v), &(struct lctx){ L.em, L.rem, -1 }, &d);
        if (r == 1) o->min_width = clamp_px(d, 0, 20000);
        else if (strchr(v, '%') && read_num(v, &d) && !strstr(v, "calc")) { o->min_width = clamp_px(d, 0, 2000); o->min_width_pct = 1; }
    }
    if ((v = V(S_MAXW))) {
        int r = parse_len(v, strlen(v), &(struct lctx){ L.em, L.rem, -1 }, &d);
        if (r == 1) o->max_width = clamp_px(d, 0, 20000);
        else if (strchr(v, '%') && read_num(v, &d) && !strstr(v, "calc")) { o->max_width = clamp_px(d, 0, 100); o->max_width_pct = 1; }
    }

    /* спрятан? */
    int hide = disp == DS_NONE || out->vis_hidden;
    const char *pos = V(S_POS);
    if((v=V(S_ZINDEX)) && read_num(v,&d)==(int)strlen(v)) {o->has_z_index=1;o->z_index=d<-1000000?-1000000:d>1000000?1000000:(int)d;}
    o->fixed=pos?(starts(pos,"fixed")?2:starts(pos,"absolute")?1:starts(pos,"relative")?3:0):0;
    static const int edge_slots[4]={S_TOP,S_RIGHT,S_BOTTOM,S_LEFT};
    for(int i=0;i<4;i++)if((v=V(edge_slots[i]))) {
        if(strchr(v,'%') && read_num(v,&d)>0 && !strstr(v,"calc")) {o->position_edge[i]=clamp_px(d,-2000,2000);o->position_pct|=1<<i;}
        else if(parse_len(v,strlen(v),&(struct lctx){L.em,L.rem,-1},&d)==1)o->position_edge[i]=clamp_px(d,-20000,20000);
    }
    if((v=V(S_TRANSLATE))) {
        const char *w[3];int wl[3],n=split_words(v,w,wl,3);
        for(int i=0;i<n && i<2;i++) {
            short value=0;
            if((strchr(w[i],'%') && strchr(w[i],'%')<w[i]+wl[i]) && parse_len(w[i],wl[i],&(struct lctx){L.em,L.rem,100},&d)==1) {value=clamp_px(d,-2000,2000);o->translate_pct|=1<<i;}
            else if(parse_len(w[i],wl[i],&(struct lctx){L.em,L.rem,-1},&d)==1)value=clamp_px(d,-20000,20000);
            if(i)o->translate_y=value;else o->translate_x=value;
        }
    }
    if(have[S_TRANSLATE]!=1 && (v=V(S_TRANSFORM))) {
        int axis=starts(v,"translatex(")?0:starts(v,"translatey(")?1:-1;
        if(axis>=0) {
            const char *start=strchr(v,'(')+1,*end=strchr(start,')');
            if(end && parse_len(start,end-start,&(struct lctx){L.em,L.rem,100},&d)==1) {
                if(strchr(start,'%') && strchr(start,'%')<end)o->translate_pct|=1<<axis;
                if(axis)o->translate_y=clamp_px(d,-20000,20000);else o->translate_x=clamp_px(d,-20000,20000);
            }
        }
    }
    if((v=V(S_OPACITY))) {
        int used=read_num(v,&d),len=strlen(v);
        if(used==len)o->opacity=clamp_px(d*10000,0,10000);
        else if(used>0 && used+1==len && v[used]=='%')o->opacity=clamp_px(d*100,0,10000);
    }
    int abs = pos && (starts(pos, "absolute") || starts(pos, "fixed"));
    double wv = -1, hv = -1, mh = -1, lv = 0, tv = 0;
    if ((v = V(S_WIDTH))) { if (parse_len(v, strlen(v), &L, &d) == 1) wv = d; }
    if ((v = V(S_HEIGHT))) { struct lctx L2 = { L.em, L.rem, -1 }; if (parse_len(v, strlen(v), &L2, &d) == 1) hv = d; }
    if ((v = V(S_MAXH))) { struct lctx L2 = { L.em, L.rem, -1 }; if (parse_len(v, strlen(v), &L2, &d) == 1) mh = d; }
    const char *ov = V(S_OVERFLOW);
    int clipped = ov && (starts(ov, "hidden") || starts(ov, "clip"));
    o->clip_overflow=clipped;
    if (abs) {
        if ((v = V(S_LEFT)) && parse_len(v, strlen(v), &L, &d) == 1) lv = d;
        if ((v = V(S_TOP)) && parse_len(v, strlen(v), &L, &d) == 1) tv = d;
        if (lv <= -900 || tv <= -900) hide = 1;
        if (wv >= 0 && wv <= 1.5 && hv >= 0 && hv <= 1.5) hide = 1;                 /* .sr-only */
        if ((v = V(S_CLIP)) && starts(v, "rect(")) hide = 1;
    }
    if ((v = V(S_CLIPPATH)) && (starts(v, "inset(50%") || starts(v, "inset(100%") || starts(v, "circle(0") ||
                                starts(v, "polygon(0 0, 0 0") || starts(v, "polygon(0px 0px, 0px 0px"))) hide = 1;
    if (clipped && ((hv >= 0 && hv < 1) || (mh >= 0 && mh < 1) || (wv >= 0 && wv < 1))) hide = 1;
    if (o->font_px < 3 && have[S_FSIZE] == 1) o->notext = 1;
    if ((v = V(S_TINDENT)) && parse_len(v, strlen(v), &L, &d) == 1 && d <= -900) o->notext = 1;
    o->hidden = hide;
    #undef V
}

static struct cstyle initial_c;

static struct cstyle *style_r(dnode *n) {
    if (!n || n->type != DN_ELEMENT) return &initial_c;
    if (n->id < cache_cap && cache[n->id].gen == gen) return &cache[n->id];
    struct cstyle *ps = style_r(n->parent);
    struct cstyle *c = &cache[n->id];
    compute(n, ps, c);
    if (ps->s.hidden) c->s.hidden = 1;                          /* у спрятанного родителя — спрятаны и дети */
    c->gen = gen;
    return c;
}

static struct cstyle *style_of(dnode *n) {
    css_ensure();
    if (cache_mut != dom_mut) {                                 /* дерево изменилось — всё считаем заново */
        gen++;
        cache_mut = dom_mut;
        ar_free(&tmp_arena);
        root_px = 16;
    }
    if (d_count > cache_cap) {
        int cap = d_count + 1024;
        struct cstyle *p = malloc(sizeof(struct cstyle) * cap);
        if (!p) return &initial_c;
        if (cache_cap) memcpy(p, cache, sizeof(struct cstyle) * cache_cap);
        memset(p + cache_cap, 0, sizeof(struct cstyle) * (cap - cache_cap));
        free(cache);
        cache = p;
        cache_cap = cap;
    }
    initial_style(&initial_c);
    return style_r(n);
}

const struct dom_style *css_style(dnode *n) { return &style_of(n)->s; }

const struct dom_style *dom_style(int id) {
    if (id < 0 || id >= d_count) return NULL;
    dnode *n = d_nodes[id];
    if (!n || n->type != DN_ELEMENT) return NULL;
    return &style_of(n)->s;
}

int css_hidden(dnode *n) {
    if (!n || n->type != DN_ELEMENT) return 0;
    return style_of(n)->s.hidden;
}

/* элемент виден (не спрятан ни им самим, ни предками)? */
int dn_visible(dnode *n) {
    if (!n) return 0;
    if (n->type != DN_ELEMENT) n = n->parent;
    if (!n || !dn_connected(n)) return 0;
    if (n->type != DN_ELEMENT) return 1;
    return !css_hidden(n);
}

/* getComputedStyle(el).свойство */
void css_computed(dnode *n, const char *prop, char *out, int max) {
    out[0] = 0;
    if (!n || n->type != DN_ELEMENT) return;
    struct cstyle *c = style_of(n);
    const struct dom_style *s = &c->s;
    #define RGBS(x) snprintf(out, max, "rgb(%d, %d, %d)", ((x) >> 16) & 255, ((x) >> 8) & 255, (x) & 255)
    if (!strcmp(prop, "display")) {
        static const char *names[] = { "inline", "block", "list-item", "inline-block", "flex", "grid", "table", "table-row",
                                       "table-cell", "contents", "none" };
        snprintf(out, max, "%s", s->hidden && s->display != DS_NONE && !c->vis_hidden ? "none" : names[s->display]);
    } else if (!strcmp(prop, "visibility")) snprintf(out, max, "%s", c->vis_hidden ? "hidden" : "visible");
    else if(!strcmp(prop,"pointer-events"))snprintf(out,max,"%s",s->pointer_none?"none":"auto");
    else if (!strcmp(prop, "color")) RGBS(s->color);
    else if (!strcmp(prop, "background-color")) { if (s->bg < 0) snprintf(out, max, "rgba(0, 0, 0, 0)"); else RGBS(s->bg); }
    else if (!strcmp(prop, "font-size")) snprintf(out, max, "%dpx", s->font_px);
    else if (!strcmp(prop, "font-weight")) snprintf(out, max, "%s", s->bold ? "700" : "400");
    else if (!strcmp(prop, "font-style")) snprintf(out, max, "%s", s->italic ? "italic" : "normal");
    else if (!strcmp(prop, "text-align")) snprintf(out, max, "%s", s->align == 1 ? "center" : s->align == 2 ? "right" : "start");
    else if (!strcmp(prop, "opacity")) snprintf(out,max,"%.3g",s->opacity/10000.0);
    else if(!strcmp(prop,"font-family"))snprintf(out,max,"%s",s->font_family?"monospace":"sans-serif");
    else if (!strcmp(prop,"position"))snprintf(out,max,"%s",s->fixed==1?"absolute":s->fixed==2?"fixed":s->fixed==3?"relative":"static");
    else if (!strncmp(prop, "margin-", 7) || !strncmp(prop, "padding-", 8)) {
        const short *a = prop[0] == 'm' ? s->margin : s->padding;
        const char *side = strchr(prop, '-') + 1;
        int i = !strcmp(side, "top") ? 0 : !strcmp(side, "right") ? 1 : !strcmp(side, "bottom") ? 2 : 3;
        snprintf(out, max, "%dpx", a[i] == CSS_AUTO ? 0 : a[i]);
    } else if (!strncmp(prop, "--", 2)) {
        for (struct cvar *v = c->vars; v; v = v->next) if (!strcmp(v->name, prop)) { snprintf(out, max, "%s", v->val); break; }
    }
    #undef RGBS
}
