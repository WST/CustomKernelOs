/* desktop — рабочий стол MyOS: окна, панель задач, меню «Пуск», значки.
 *
 * Это обычная программа (сервер окон). Она забирает экран у консоли и
 * рисует всё сама. Программы с окнами (win.h) — отдельные процессы: их
 * пиксели лежат в общей памяти, а о том, что окно надо перерисовать, они
 * сообщают сообщением. Мы же собираем кадр из обоев, окон и панели и
 * переносим на экран только изменившиеся прямоугольники.
 *
 * Мышь: перетаскивание окон за заголовок, кнопки «_» и «X», клик по окну
 * поднимает его наверх. Двойной клик по значку запускает программу. */
#include "win.h"

#define TASKBAR_H 40
#define TITLE_H   28
#define MAXWIN    32
#define BTN_W     46                    /* кнопки в заголовке */

static int SW, SH;                      /* размер экрана */
static color_t *wallpaper;              /* обои, нарисованные один раз */
static color_t *back;                   /* сюда собираем кадр */

/* ------------------------------------------------------------ окна */

struct win {
    bool used;
    int  owner;                         /* pid программы */
    int  shm;
    color_t *px;
    int  w, h;                          /* размер содержимого */
    int  x, y;                          /* левый верхний угол рамки */
    char title[40];
    bool minimized;
};

static struct win wins[MAXWIN];
static int zorder[MAXWIN], nz;          /* снизу вверх */
static int focus = -1;
static int cascade;

static int outer_w(struct win *w) { return w->w + 2; }
static int outer_h(struct win *w) { return w->h + TITLE_H + 1; }

/* ------------------------------------------------------------ «грязные» прямоугольники */

struct rect { int x, y, w, h; };
#define MAXDIRTY 24
static struct rect dirty[MAXDIRTY];
static int ndirty;

static bool intersect(struct rect a, struct rect b, struct rect *out) {
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0) return false;
    if (out) { out->x = x0; out->y = y0; out->w = x1 - x0; out->h = y1 - y0; }
    return true;
}

static struct rect bounding(struct rect a, struct rect b) {
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    struct rect r = { x0, y0, x1 - x0, y1 - y0 };
    return r;
}

static void add_dirty(int x, int y, int w, int h) {
    struct rect screen = { 0, 0, SW, SH }, r = { x, y, w, h };
    if (!intersect(r, screen, &r)) return;
    for (int i = 0; i < ndirty; i++) {                  /* пересекается — объединяем */
        if (intersect(dirty[i], r, NULL)) {
            dirty[i] = bounding(dirty[i], r);
            return;
        }
    }
    if (ndirty == MAXDIRTY) {                           /* слишком много — один общий */
        for (int i = 1; i < ndirty; i++) dirty[0] = bounding(dirty[0], dirty[i]);
        ndirty = 1;
        dirty[0] = bounding(dirty[0], r);
        return;
    }
    dirty[ndirty++] = r;
}

static void dirty_win(struct win *w) { add_dirty(w->x, w->y, outer_w(w), outer_h(w)); }
static void dirty_taskbar(void) { add_dirty(0, SH - TASKBAR_H, SW, TASKBAR_H); }

/* ------------------------------------------------------------ значки (16x16, рисуем x2) */

struct icon_def { const char *name, *prog; const char *art[16]; };

static color_t pal(char c) {
    switch (c) {
    case 'k': return RGB(25, 25, 30);
    case 'w': return RGB(255, 255, 255);
    case 'g': return RGB(200, 200, 205);
    case 'G': return RGB(110, 110, 120);
    case 'b': return RGB(70, 150, 240);
    case 'B': return RGB(20, 70, 160);
    case 'y': return RGB(255, 210, 80);
    case 'Y': return RGB(220, 160, 30);
    case 'r': return RGB(230, 60, 60);
    case 'n': return RGB(80, 220, 100);
    case 'c': return RGB(90, 220, 230);
    case 'm': return RGB(200, 90, 220);
    default:  return TRANSPARENT;
    }
}

static const struct icon_def icons[] = {
    { "Terminal", "terminal", {
        "................", ".kkkkkkkkkkkkkk.", ".kGGGGGGGGGGGGk.", ".kkkkkkkkkkkkkk.",
        ".kkkkkkkkkkkkkk.", ".kknkkkkkkkkkkk.", ".kkknkkkkkkkkkk.", ".kkkknkkkkkkkkk.",
        ".kkknkkkkkkkkkk.", ".kknkknnnnkkkkk.", ".kkkkkkkkkkkkkk.", ".kkkkkkkkkkkkkk.",
        ".kkkkkkkkkkkkkk.", ".kkkkkkkkkkkkkk.", "................", "................" } },
    { "Browser", "browser", {
        "................", ".....bbbbbb.....", "...bbnnbbbbbb...", "..bnnnnbbbbnnb..",
        "..bnnnnnbbnnnb..", ".bbnnnnnbbbnnnb.", ".bbbnnnbbbbbnnb.", ".bbbbnbbbbbbbbb.",
        ".bbbbbbbnnbbbbb.", ".bbbbbbnnnnbbbb.", "..bbbbbnnnnnbb..", "..bbbbbbnnnbbb..",
        "...bbbbbbnbbb...", ".....bbbbbb.....", "................", "................" } },
    { "Files", "files", {
        "................", "................", ".YYYYY..........", ".YyyyyY.........",
        ".YyyyyyYYYYYYYY.", ".Yyyyyyyyyyyyyy.", ".YYYYYYYYYYYYYYY", ".Yyyyyyyyyyyyyy.",
        ".Yyyyyyyyyyyyyy.", ".Yyyyyyyyyyyyyy.", ".Yyyyyyyyyyyyyy.", ".Yyyyyyyyyyyyyy.",
        ".Yyyyyyyyyyyyyy.", ".YYYYYYYYYYYYYY.", "................", "................" } },
    { "Notepad", "notepad", {
        "...GGGGGGGGG....", "...GwwwwwwwGG...", "...GwwwwwwwGwG..", "...GwbbbbbwGGGG.",
        "...Gwwwwwwwwwwg.", "...GwbbbbbbbbwG.", "...Gwwwwwwwwwwg.", "...GwbbbbbbbbwG.",
        "...Gwwwwwwwwwwg.", "...GwbbbbbbbwwG.", "...Gwwwwwwwwwwg.", "...GwbbbbbbbbwG.",
        "...Gwwwwwwwwwwg.", "...GGGGGGGGGGGG.", "................", "................" } },
    { "Calculator", "calc", {
        "..GGGGGGGGGGGG..", "..GkkkkkkkkkkG..", "..GkcccccccckG..", "..GkkkkkkkkkkG..",
        "..GGGGGGGGGGGG..", "..GwwGwwGwwGrrG..", "..GwwGwwGwwGrrG..", "..GGGGGGGGGGGG..",
        "..GwwGwwGwwGbbG..", "..GwwGwwGwwGbbG..", "..GGGGGGGGGGGG..", "..GwwGwwGwwGbbG..",
        "..GwwGwwGwwGbbG..", "..GGGGGGGGGGGG..", "................", "................" } },
    { "Paint", "draw", {
        "................", ".....wwwwww.....", "...wwwwwwwwww...", "..wwrrwwwwbbww..",
        ".wwwrrwwwwbbwww.", ".wwwwwwwwwwwwww.", ".wnnwwwwwwwwyyw.", ".wnnwwwkkwwwyyw.",
        ".wwwwwwkkwwwwww.", "..wwwwwwwwwwmm..", "..wwwwwwwwwwmm..", "...wwwwwwwwww...",
        ".....wwwwww.....", "................", "................", "................" } },
    { "Minesweeper", "mines", {
        "gggggggggggggggg", "gwwwwwwwwwwwwwwG", "gwgggggkgggggggG", "gwggkgkkkgkggggG",
        "gwgggkkkkkgggggG", "gwggkkwwkkkggggG", "gwkkkkwkkkkkkggG", "gwggkkkkkkkggggG",
        "gwgggkkkkkgggggG", "gwggkgkkkgkggggG", "gwgggggkgggggggG", "gwggggggggggg ggG",
        "gwggggggggggggGG", "gwGGGGGGGGGGGGGG", "gGGGGGGGGGGGGGGG", "................" } },
    { "Task Manager", "taskmgr", {
        "................", ".BBBBBBBBBBBBBB.", ".BkkkkkkkkkkkkB.", ".BkkkkkkkkknkkB.",
        ".BkkkkkkkkknkkB.", ".BkkknkkkkknkkB.", ".BkkknkkknknkkB.", ".BkkknknknknkkB.",
        ".BknknknknknkkB.", ".BknknknknknknB.", ".BknknknknknknB.", ".BkkkkkkkkkkkkB.",
        ".BBBBBBBBBBBBBB.", "......GGGG......", "....GGGGGGGG....", "................" } },
    { "About MyOS", "about", {
        "................", ".....bbbbbb.....", "...bbbbbbbbbb...", "..bbbbbwwbbbbb..",
        "..bbbbbwwbbbbb..", ".bbbbbbbbbbbbbb.", ".bbbbbwwwbbbbbb.", ".bbbbbbwwbbbbbb.",
        ".bbbbbbwwbbbbbb.", ".bbbbbbwwbbbbbb.", "..bbbbbwwbbbbb..", "..bbbbwwwwbbbb..",
        "...bbbbbbbbbb...", ".....bbbbbb.....", "................", "................" } },
};
#define N_ICONS ((int)(sizeof(icons) / sizeof(icons[0])))

#define ICON_X 16
#define ICON_Y 16
#define ICON_CELL_W 88
#define ICON_CELL_H 84
static int selected_icon = -1;

static void icon_cell(int i, int *x, int *y) {
    int per_col = (SH - TASKBAR_H - ICON_Y) / ICON_CELL_H;
    *x = ICON_X + (i / per_col) * ICON_CELL_W;
    *y = ICON_Y + (i % per_col) * ICON_CELL_H;
}

static void draw_art(const char *const *art, int x, int y, int scale) {
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 16 && art[r][c]; c++) {
            color_t col = pal(art[r][c]);
            if (col != TRANSPARENT) gfx_fill_rect(x + c * scale, y + r * scale, scale, scale, col);
        }
}

/* текст с тенью — на обоях читается лучше */
static void shadow_text(int x, int y, const char *s, color_t c) {
    gfx_text(x + 1, y + 1, s, RGB(0, 0, 0), TRANSPARENT);
    gfx_text(x, y, s, c, TRANSPARENT);
}

static void draw_icon(int i) {
    int x, y;
    icon_cell(i, &x, &y);
    if (i == selected_icon) {
        for (int yy = y; yy < y + ICON_CELL_H - 6; yy++)          /* полупрозрачная подсветка */
            for (int xx = x; xx < x + ICON_CELL_W - 8; xx++) {
                color_t p = gfx_buf[yy * SW + xx];
                gfx_buf[yy * SW + xx] = ((p >> 1) & 0x7F7F7F) + RGB(50, 80, 120);
            }
    }
    draw_art(icons[i].art, x + (ICON_CELL_W - 8 - 32) / 2, y + 6, 2);
    int tw = strlen(icons[i].name) * 8;
    shadow_text(x + (ICON_CELL_W - 8 - tw) / 2, y + 46, icons[i].name, C_WHITE);
}

static int icon_at(int px, int py) {
    for (int i = 0; i < N_ICONS; i++) {
        int x, y;
        icon_cell(i, &x, &y);
        if (ui_inside(px, py, x, y, ICON_CELL_W - 8, ICON_CELL_H - 6)) return i;
    }
    return -1;
}

static void dirty_icon(int i) {
    if (i < 0) return;
    int x, y;
    icon_cell(i, &x, &y);
    add_dirty(x, y, ICON_CELL_W, ICON_CELL_H);
}

/* ------------------------------------------------------------ обои */

static void make_wallpaper(void) {
    wallpaper = malloc(SW * SH * 4);
    for (int y = 0; y < SH; y++) {
        int t = y * 256 / SH;
        color_t c = RGB(10 + t / 8, 40 + t / 3, 90 + t / 3);          /* сверху тёмно-синий, вниз — бирюза */
        for (int x = 0; x < SW; x++) {
            int glow = (x - SW / 2) * (x - SW / 2) / 4096 + (y - SH / 3) * (y - SH / 3) / 2048;
            int add = 60 - glow;
            if (add < 0) add = 0;
            int r = ((c >> 16) & 255) + add / 3, g = ((c >> 8) & 255) + add / 2, b = (c & 255) + add;
            if (b > 255) b = 255;
            if (g > 255) g = 255;
            wallpaper[y * SW + x] = RGB(r, g, b);
        }
    }
    color_t *save = gfx_buf;
    gfx_buf = wallpaper;
    int scale = 9, tw = 4 * 8 * scale;
    gfx_text_scaled(SW / 2 - tw / 2 + 4, SH / 2 - 100 + 4, "MyOS", RGB(15, 45, 90), scale);
    gfx_text_scaled(SW / 2 - tw / 2, SH / 2 - 100, "MyOS", RGB(70, 140, 210), scale);
    const char *sub = "written from scratch in C and assembly";
    gfx_text(SW / 2 - (int)strlen(sub) * 4, SH / 2 + 60, sub, RGB(150, 200, 240), TRANSPARENT);
    gfx_buf = save;
}

/* ------------------------------------------------------------ панель задач и «Пуск» */

static bool menu_open;
#define MENU_W 250
#define MENU_ITEM_H 34
#define MENU_HEAD 44
static int menu_hover = -1;
static int menu_items(void) { return N_ICONS + 1; }               /* + «Выход» */
static struct rect menu_rect(void) {
    int h = MENU_HEAD + menu_items() * MENU_ITEM_H + 12;
    struct rect r = { 0, SH - TASKBAR_H - h, MENU_W, h };
    return r;
}

static char clock_text[8], date_text[12];
static bool start_hover;

#define TB_START_W 96
#define TB_BTN_MAX 170
static int TB_BTN_W = TB_BTN_MAX;                                 /* сужаются, когда окон много */
static void taskbar_layout(void);

static int taskbar_order[MAXWIN], n_taskbar;                  /* кнопки окон: по порядку создания */

static void taskbar_layout(void) {
    int avail = SW - 110 - TB_START_W - 6;
    TB_BTN_W = n_taskbar ? avail / n_taskbar : TB_BTN_MAX;
    if (TB_BTN_W > TB_BTN_MAX) TB_BTN_W = TB_BTN_MAX;
    if (TB_BTN_W < 40) TB_BTN_W = 40;
}

static void draw_taskbar(void) {
    taskbar_layout();
    int y = SH - TASKBAR_H;
    gfx_fill_rect(0, y, SW, TASKBAR_H, RGB(32, 36, 48));
    gfx_fill_rect(0, y, SW, 1, RGB(70, 76, 96));
    /* «Пуск» */
    color_t sb = menu_open ? RGB(60, 110, 200) : start_hover ? RGB(55, 62, 82) : RGB(32, 36, 48);
    gfx_fill_rect(0, y + 1, TB_START_W, TASKBAR_H - 1, sb);
    gfx_fill_rect(12, y + 10, 9, 9, RGB(240, 80, 50));         /* «флажок» из четырёх квадратиков */
    gfx_fill_rect(23, y + 10, 9, 9, RGB(120, 200, 60));
    gfx_fill_rect(12, y + 21, 9, 9, RGB(40, 160, 240));
    gfx_fill_rect(23, y + 21, 9, 9, RGB(255, 200, 40));
    gfx_text(40, y + 12, "Start", C_WHITE, TRANSPARENT);
    /* окна */
    int x = TB_START_W + 6;
    for (int i = 0; i < n_taskbar; i++) {
        struct win *w = &wins[taskbar_order[i]];
        if (x + TB_BTN_W > SW - 110) break;
        bool active = taskbar_order[i] == focus && !w->minimized;
        gfx_fill_rect(x, y + 4, TB_BTN_W - 4, TASKBAR_H - 8, active ? RGB(70, 80, 105) : RGB(45, 50, 66));
        gfx_fill_rect(x, y + TASKBAR_H - 6, TB_BTN_W - 4, 2, active ? RGB(90, 170, 255) : RGB(110, 120, 140));
        char t[24];
        int n = 0, maxc = (TB_BTN_W - 20) / 8;
        while (w->title[n] && n < maxc && n < 22) { t[n] = w->title[n]; n++; }
        t[n] = 0;
        gfx_text(x + 8, y + 12, t, w->minimized ? RGB(170, 175, 190) : C_WHITE, TRANSPARENT);
        x += TB_BTN_W;
    }
    /* часы */
    gfx_text(SW - 64, y + 4, clock_text, C_WHITE, TRANSPARENT);
    gfx_text(SW - 92, y + 20, date_text, RGB(190, 195, 210), TRANSPARENT);
}

static int taskbar_button_at(int px, int py) {
    if (py < SH - TASKBAR_H) return -1;
    taskbar_layout();
    int x = TB_START_W + 6;
    for (int i = 0; i < n_taskbar; i++) {
        if (x + TB_BTN_W > SW - 110) break;
        if (px >= x && px < x + TB_BTN_W - 4) return taskbar_order[i];
        x += TB_BTN_W;
    }
    return -1;
}

static void draw_menu(void) {
    struct rect r = menu_rect();
    gfx_fill_rect(r.x, r.y, r.w, r.h, RGB(40, 44, 58));
    gfx_rect(r.x, r.y, r.w, r.h, RGB(80, 86, 106));
    for (int i = 0; i < MENU_HEAD; i++)                           /* шапка с градиентом */
        gfx_fill_rect(r.x + 1, r.y + 1 + i, r.w - 2, 1, RGB(20 + i, 80 + i * 2, 160 + i));
    gfx_text_scaled(r.x + 14, r.y + 6, "MyOS", C_WHITE, 2);
    gfx_text(r.x + 92, r.y + 22, "v0.12", RGB(200, 220, 255), TRANSPARENT);
    for (int i = 0; i < menu_items(); i++) {
        int iy = r.y + MENU_HEAD + 6 + i * MENU_ITEM_H;
        if (i == menu_hover) gfx_fill_rect(r.x + 4, iy, r.w - 8, MENU_ITEM_H, RGB(65, 75, 100));
        if (i < N_ICONS) {
            draw_art(icons[i].art, r.x + 14, iy + 1, 2);
            gfx_text(r.x + 56, iy + 9, icons[i].name, C_WHITE, TRANSPARENT);
        } else {
            gfx_fill_rect(r.x + 12, iy - 3, r.w - 24, 1, RGB(80, 86, 106));
            gfx_fill_circle(r.x + 30, iy + 17, 10, RGB(220, 70, 60));
            gfx_fill_rect(r.x + 29, iy + 8, 3, 9, C_WHITE);
            gfx_text(r.x + 56, iy + 9, "Exit to console", C_WHITE, TRANSPARENT);
        }
    }
}

static int menu_item_at(int px, int py) {
    struct rect r = menu_rect();
    if (!ui_inside(px, py, r.x, r.y, r.w, r.h)) return -2;            /* вне меню */
    int i = (py - r.y - MENU_HEAD - 6) / MENU_ITEM_H;
    if (py < r.y + MENU_HEAD + 6 || i >= menu_items()) return -1;
    return i;
}

/* ------------------------------------------------------------ рамки окон */

static int hover_close = -1, hover_min = -1;

static void draw_frame(int idx) {
    struct win *w = &wins[idx];
    bool active = idx == focus;
    int ow = outer_w(w), oh = outer_h(w);
    color_t tb = active ? RGB(0, 103, 192) : RGB(238, 238, 242);
    color_t tc = active ? C_WHITE : RGB(110, 110, 120);
    gfx_fill_rect(w->x, w->y, ow, TITLE_H, tb);
    gfx_rect(w->x, w->y, ow, oh, active ? RGB(0, 90, 170) : RGB(150, 150, 160));
    /* заголовок: обрезаем, чтобы не залез на кнопки */
    char t[40];
    int maxc = (ow - 2 * BTN_W - 16) / 8, n = 0;
    while (w->title[n] && n < maxc && n < 39) { t[n] = w->title[n]; n++; }
    t[n] = 0;
    gfx_text(w->x + 10, w->y + 6, t, tc, TRANSPARENT);
    /* кнопки «_» и «X» */
    int bx = w->x + ow - BTN_W;
    if (hover_close == idx) gfx_fill_rect(bx, w->y + 1, BTN_W - 1, TITLE_H - 1, RGB(232, 17, 35));
    gfx_line(bx + 18, w->y + 9, bx + 27, w->y + 18, hover_close == idx ? C_WHITE : tc);
    gfx_line(bx + 27, w->y + 9, bx + 18, w->y + 18, hover_close == idx ? C_WHITE : tc);
    bx -= BTN_W;
    if (hover_min == idx) gfx_fill_rect(bx, w->y + 1, BTN_W, TITLE_H - 1, active ? RGB(30, 125, 210) : RGB(220, 220, 226));
    gfx_fill_rect(bx + 18, w->y + 14, 10, 1, tc);
}

/* ------------------------------------------------------------ сборка кадра */

static int mouse_x, mouse_y;

static void compose(struct rect r) {
    for (int y = r.y; y < r.y + r.h; y++)                         /* обои */
        memcpy(back + y * SW + r.x, wallpaper + y * SW + r.x, r.w * 4);
    struct rect desk_icons = { 0, 0, ICON_CELL_W * 2 + ICON_X, SH - TASKBAR_H };
    if (intersect(r, desk_icons, NULL))
        for (int i = 0; i < N_ICONS; i++) {
            int x, y;
            icon_cell(i, &x, &y);
            struct rect ir = { x, y, ICON_CELL_W, ICON_CELL_H };
            if (intersect(r, ir, NULL)) draw_icon(i);
        }
    for (int k = 0; k < nz; k++) {                                /* окна снизу вверх */
        struct win *w = &wins[zorder[k]];
        if (w->minimized) continue;
        struct rect wr = { w->x, w->y, outer_w(w), outer_h(w) };
        if (!intersect(r, wr, NULL)) continue;
        draw_frame(zorder[k]);
        struct rect cr = { w->x + 1, w->y + TITLE_H, w->w, w->h }, part;
        if (intersect(r, cr, &part))                              /* содержимое — только нужный кусок */
            for (int y = part.y; y < part.y + part.h; y++)
                memcpy(back + y * SW + part.x, w->px + (y - cr.y) * w->w + (part.x - cr.x), part.w * 4);
    }
    struct rect tb = { 0, SH - TASKBAR_H, SW, TASKBAR_H };
    if (intersect(r, tb, NULL)) draw_taskbar();
    if (menu_open && intersect(r, menu_rect(), NULL)) draw_menu();
    struct rect cur = { mouse_x, mouse_y, CURSOR_W, CURSOR_H };
    if (intersect(r, cur, NULL)) gfx_cursor_draw(mouse_x, mouse_y);
}

static void flush(void) {
    for (int i = 0; i < ndirty; i++) {
        compose(dirty[i]);
        gfx_flip_rect(dirty[i].x, dirty[i].y, dirty[i].w, dirty[i].h);
    }
    ndirty = 0;
}

/* ------------------------------------------------------------ сообщения программам */

static void send(int pid, int type, int a, int b, int c, int d) {
    struct msg m;
    memset(&m, 0, sizeof(m));
    m.type = type; m.a = a; m.b = b; m.c = c; m.d = d;
    msg_send(pid, &m);
}

static void set_focus(int idx) {
    if (idx == focus) return;
    if (focus >= 0 && wins[focus].used) {
        send(wins[focus].owner, WM_FOCUS, focus, 0, 0, 0);
        add_dirty(wins[focus].x, wins[focus].y, outer_w(&wins[focus]), TITLE_H);
    }
    focus = idx;
    if (idx >= 0) {
        send(wins[idx].owner, WM_FOCUS, idx, 1, 0, 0);
        add_dirty(wins[idx].x, wins[idx].y, outer_w(&wins[idx]), TITLE_H);
    }
    dirty_taskbar();
}

static void raise(int idx) {
    int k = 0;
    while (k < nz && zorder[k] != idx) k++;
    if (k == nz) return;
    if (k != nz - 1) {
        for (; k < nz - 1; k++) zorder[k] = zorder[k + 1];
        zorder[nz - 1] = idx;
        dirty_win(&wins[idx]);
    }
    set_focus(idx);
}

/* самое верхнее видимое окно (или -1) */
static int top_visible(void) {
    for (int k = nz - 1; k >= 0; k--) if (!wins[zorder[k]].minimized) return zorder[k];
    return -1;
}

static void remove_window(int idx) {
    struct win *w = &wins[idx];
    if (!w->used) return;
    if (!w->minimized) dirty_win(w);
    shm_unmap(w->px);
    w->used = false;
    int k = 0, j = 0;
    for (; k < nz; k++) if (zorder[k] != idx) zorder[j++] = zorder[k];
    nz = j;
    for (k = 0, j = 0; k < n_taskbar; k++) if (taskbar_order[k] != idx) taskbar_order[j++] = taskbar_order[k];
    n_taskbar = j;
    if (hover_close == idx) hover_close = -1;
    if (hover_min == idx) hover_min = -1;
    if (focus == idx) { focus = -1; set_focus(top_visible()); }
    dirty_taskbar();
}

static void minimize(int idx) {
    wins[idx].minimized = true;
    dirty_win(&wins[idx]);
    if (focus == idx) { focus = -1; set_focus(top_visible()); send(wins[idx].owner, WM_FOCUS, idx, 0, 0, 0); }
    dirty_taskbar();
}

static void restore(int idx) {
    if (wins[idx].minimized) { wins[idx].minimized = false; dirty_win(&wins[idx]); }
    raise(idx);
    dirty_taskbar();
}

static void handle_msg(struct msg *m) {
    switch (m->type) {
    case WM_CREATE: {
        int idx = -1;
        for (int i = 0; i < MAXWIN; i++) if (!wins[i].used) { idx = i; break; }
        color_t *px = idx >= 0 ? shm_map(m->a) : NULL;
        if (!px || m->b <= 0 || m->c <= 0 || m->b > SW - 2 || m->c > SH - TASKBAR_H - TITLE_H - 1) {
            if (px) shm_unmap(px);
            send(m->from, WM_CREATED, -1, 0, 0, 0);
            return;
        }
        struct win *w = &wins[idx];
        memset(w, 0, sizeof(*w));
        w->used = true;
        w->owner = m->from;
        w->shm = m->a;
        w->px = px;
        w->w = m->b; w->h = m->c;
        memcpy(w->title, m->data, 39);
        w->title[39] = 0;
        /* лесенкой, начиная правее значков */
        w->x = 200 + (cascade % 8) * 32;
        w->y = 40 + (cascade % 8) * 28;
        cascade++;
        if (w->x + outer_w(w) > SW) w->x = SW - outer_w(w);
        if (w->y + outer_h(w) > SH - TASKBAR_H) w->y = SH - TASKBAR_H - outer_h(w);
        if (w->x < 0) w->x = 0;
        if (w->y < 0) w->y = 0;
        zorder[nz++] = idx;
        taskbar_order[n_taskbar++] = idx;
        send(m->from, WM_CREATED, idx, 0, 0, 0);
        dirty_win(w);
        set_focus(idx);
        return;
    }
    case WM_UPDATE: {
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        struct win *w = &wins[m->a];
        if (w->minimized) return;
        int rw = m->d & 0xFFFF, rh = (m->d >> 16) & 0xFFFF;
        struct rect r = { m->b, m->c, rw, rh }, all = { 0, 0, w->w, w->h };
        if (!intersect(r, all, &r)) return;
        add_dirty(w->x + 1 + r.x, w->y + TITLE_H + r.y, r.w, r.h);
        return;
    }
    case WM_TITLE:
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        memcpy(wins[m->a].title, m->data, 39);
        wins[m->a].title[39] = 0;
        add_dirty(wins[m->a].x, wins[m->a].y, outer_w(&wins[m->a]), TITLE_H);
        dirty_taskbar();
        return;
    case WM_DESTROY:
        if (m->a < 0 || m->a >= MAXWIN || !wins[m->a].used || wins[m->a].owner != m->from) return;
        remove_window(m->a);
        return;
    }
}

/* ------------------------------------------------------------ программы */

static void launch(const char *prog) {
    char path[64];
    snprintf(path, sizeof(path), "/bin/%s", prog);
    int pid = fork();
    if (pid == 0) {
        int p[2];                                             /* ввод — пустой канал: клавиатура только у стола */
        if (pipe(p) == 0) { close(p[1]); dup2(p[0], 0); close(p[0]); }
        char *argv[2] = { (char *)prog, NULL };
        exec(path, argv);
        exit(127);
    }
}

/* окна умерших программ — убрать */
static void check_owners(void) {
    struct proc_info pi[64];
    int n = ps(pi, 64);
    if (n <= 0) return;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used) continue;
        bool alive = false;
        for (int j = 0; j < n; j++)
            if ((int)pi[j].pid == wins[i].owner && pi[j].state != 4) { alive = true; break; }
        if (!alive) remove_window(i);
    }
}

static void update_clock(void) {
    struct datetime t;
    if (get_time(&t) < 0) return;
    char c[8], d[12];
    snprintf(c, sizeof(c), "%02u:%02u", t.hour, t.min);
    snprintf(d, sizeof(d), "%02u.%02u.%04u", t.day, t.month, t.year);
    if (strcmp(c, clock_text) || strcmp(d, date_text)) {
        strcpy(clock_text, c);
        strcpy(date_text, d);
        add_dirty(SW - 100, SH - TASKBAR_H, 100, TASKBAR_H);
    }
}

/* ------------------------------------------------------------ мышь */

static int drag = -1, drag_dx, drag_dy;
static int capture = -1;                                          /* окно, где нажали кнопку */
static unsigned prev_buttons;
static unsigned last_click_tick;
static int last_click_icon = -1;
static bool quit;

static int window_at(int px, int py) {
    for (int k = nz - 1; k >= 0; k--) {
        struct win *w = &wins[zorder[k]];
        if (!w->minimized && ui_inside(px, py, w->x, w->y, outer_w(w), outer_h(w))) return zorder[k];
    }
    return -1;
}

static void set_hover(int *var, int value) {
    if (*var == value) return;
    if (*var >= 0 && wins[*var].used) add_dirty(wins[*var].x, wins[*var].y, outer_w(&wins[*var]), TITLE_H);
    *var = value;
    if (value >= 0) add_dirty(wins[value].x, wins[value].y, outer_w(&wins[value]), TITLE_H);
}

static void close_menu(void) {
    if (!menu_open) return;
    menu_open = false;
    struct rect r = menu_rect();
    add_dirty(r.x, r.y, r.w, r.h);
    add_dirty(0, SH - TASKBAR_H, TB_START_W, TASKBAR_H);
}

static void mouse_down(int x, int y) {
    if (menu_open) {
        int item = menu_item_at(x, y);
        if (item >= 0) {
            close_menu();
            if (item < N_ICONS) launch(icons[item].prog);
            else quit = true;
            return;
        }
        if (item == -1) return;                                   /* по шапке меню */
        close_menu();
        if (x < TB_START_W && y >= SH - TASKBAR_H) return;        /* «Пуск» ещё раз — просто закрыть */
    }
    if (y >= SH - TASKBAR_H) {                                    /* панель задач */
        if (x < TB_START_W) {
            menu_open = true;
            menu_hover = -1;
            struct rect r = menu_rect();
            add_dirty(r.x, r.y, r.w, r.h);
            add_dirty(0, SH - TASKBAR_H, TB_START_W, TASKBAR_H);
            return;
        }
        int b = taskbar_button_at(x, y);
        if (b >= 0) {
            if (b == focus && !wins[b].minimized) minimize(b);
            else restore(b);
        }
        return;
    }
    int idx = window_at(x, y);
    if (idx >= 0) {
        struct win *w = &wins[idx];
        raise(idx);
        if (y < w->y + TITLE_H) {                                 /* заголовок */
            int ow = outer_w(w);
            if (x >= w->x + ow - BTN_W) { send(w->owner, WM_CLOSE, idx, 0, 0, 0); return; }
            if (x >= w->x + ow - 2 * BTN_W) { minimize(idx); return; }
            drag = idx;
            drag_dx = x - w->x;
            drag_dy = y - w->y;
            return;
        }
        capture = idx;
        send(w->owner, WM_MOUSE, idx, x - w->x - 1, y - w->y - TITLE_H, prev_buttons | (1 << 8));
        return;
    }
    /* рабочий стол: значки */
    if (focus >= 0) set_focus(-1);
    int ic = icon_at(x, y);
    if (ic != selected_icon) { dirty_icon(selected_icon); selected_icon = ic; dirty_icon(ic); }
    if (ic >= 0 && ic == last_click_icon && ticks() - last_click_tick < 500) {
        launch(icons[ic].prog);
        last_click_icon = -1;
        return;
    }
    last_click_icon = ic;
    last_click_tick = ticks();
}

static void mouse_move(int ox, int oy, int x, int y) {
    add_dirty(ox, oy, CURSOR_W, CURSOR_H);
    add_dirty(x, y, CURSOR_W, CURSOR_H);
    if (drag >= 0) {
        struct win *w = &wins[drag];
        dirty_win(w);
        w->x = x - drag_dx;
        w->y = y - drag_dy;
        if (w->y < 0) w->y = 0;
        if (w->y > SH - TASKBAR_H - TITLE_H) w->y = SH - TASKBAR_H - TITLE_H;
        if (w->x < 60 - outer_w(w)) w->x = 60 - outer_w(w);
        if (w->x > SW - 60) w->x = SW - 60;
        dirty_win(w);
        return;
    }
    if (menu_open) {
        int item = menu_item_at(x, y);
        if (item < 0) item = -1;
        if (item != menu_hover) {
            menu_hover = item;
            struct rect r = menu_rect();
            add_dirty(r.x, r.y, r.w, r.h);
        }
    }
    bool sh_ = x < TB_START_W && y >= SH - TASKBAR_H;
    if (sh_ != start_hover) { start_hover = sh_; add_dirty(0, SH - TASKBAR_H, TB_START_W, TASKBAR_H); }
    int idx = window_at(x, y);
    int hc = -1, hm = -1;
    if (idx >= 0 && y < wins[idx].y + TITLE_H && y >= wins[idx].y) {
        int right = wins[idx].x + outer_w(&wins[idx]);
        if (x >= right - BTN_W) hc = idx;
        else if (x >= right - 2 * BTN_W) hm = idx;
    }
    set_hover(&hover_close, hc);
    set_hover(&hover_min, hm);
    int target = capture >= 0 ? capture : (idx == focus ? idx : -1);
    if (target >= 0 && wins[target].used) {
        struct win *w = &wins[target];
        int lx = x - w->x - 1, ly = y - w->y - TITLE_H;
        if (capture >= 0 || (lx >= 0 && ly >= 0 && lx < w->w && ly < w->h))
            send(w->owner, WM_MOUSE, target, lx, ly, prev_buttons | (3 << 8));
    }
}

static void mouse_up(int x, int y) {
    drag = -1;
    if (capture >= 0 && wins[capture].used) {
        struct win *w = &wins[capture];
        send(w->owner, WM_MOUSE, capture, x - w->x - 1, y - w->y - TITLE_H, prev_buttons | (2 << 8));
    }
    capture = -1;
}

/* ------------------------------------------------------------ главный цикл */

int main(void) {
    if (service_find("desktop")) { dprintf(STDERR, "desktop: already running\n"); return 1; }
    int r = gfx_open();
    if (r < 0) { dprintf(STDERR, "desktop: no graphics (%s)\n", strerror(-r)); return 1; }
    if (service_register("desktop") < 0) { gfx_close(); dprintf(STDERR, "desktop: cannot register\n"); return 1; }
    ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG);
    SW = gfx_w; SH = gfx_h;
    back = gfx_buf;
    make_wallpaper();
    struct mouse ms;
    mouse_get(&ms);
    mouse_x = ms.x; mouse_y = ms.y;
    prev_buttons = ms.buttons;
    update_clock();
    add_dirty(0, 0, SW, SH);
    flush();

    unsigned last_check = ticks();
    while (!quit) {
        struct msg m;
        if (msg_recv(&m, 10) == 0) {
            handle_msg(&m);
            while (msg_recv(&m, 0) == 0) handle_msg(&m);
        }
        mouse_get(&ms);
        if (ms.x != mouse_x || ms.y != mouse_y) {
            int ox = mouse_x, oy = mouse_y;
            mouse_x = ms.x; mouse_y = ms.y;
            mouse_move(ox, oy, mouse_x, mouse_y);
        }
        unsigned pressed = ms.buttons & ~prev_buttons, released = prev_buttons & ~ms.buttons;
        prev_buttons = ms.buttons;
        if (pressed & MOUSE_LEFT) mouse_down(mouse_x, mouse_y);
        else if (pressed && capture < 0) {                        /* правая кнопка — тоже в окно */
            int idx = window_at(mouse_x, mouse_y);
            if (idx >= 0 && mouse_y >= wins[idx].y + TITLE_H) {
                raise(idx);
                capture = idx;
                send(wins[idx].owner, WM_MOUSE, idx, mouse_x - wins[idx].x - 1,
                     mouse_y - wins[idx].y - TITLE_H, ms.buttons | (1 << 8));
            }
        }
        if (released && !ms.buttons) mouse_up(mouse_x, mouse_y);

        int k;
        while ((k = readkey()) > 0) {
            if (k == 27 && menu_open) { close_menu(); continue; }
            if (focus >= 0 && wins[focus].used) send(wins[focus].owner, WM_KEY, focus, k, 0, 0);
        }

        while (waitpid(-1, NULL, WNOHANG) > 0) {}                 /* прибрать запущенные нами программы */
        if (ticks() - last_check > 500) {
            last_check = ticks();
            update_clock();
            check_owners();
        }
        flush();
    }

    /* выход: попросить программы закрыться, подождать, остальных — kill */
    int owners[MAXWIN], n_owners = 0;
    for (int i = 0; i < MAXWIN; i++) {
        if (!wins[i].used) continue;
        bool seen = false;
        for (int j = 0; j < n_owners; j++) if (owners[j] == wins[i].owner) seen = true;
        if (!seen) { owners[n_owners++] = wins[i].owner; send(wins[i].owner, WM_QUIT, i, 0, 0, 0); }
    }
    sleep_ms(300);
    for (int j = 0; j < n_owners; j++) kill(owners[j]);
    for (int i = 0; i < MAXWIN; i++) if (wins[i].used) shm_unmap(wins[i].px);
    while (waitpid(-1, NULL, WNOHANG) > 0) {}
    gfx_close();
    printf(ANSI_CLEAR "Desktop closed.\n");
    return 0;
}
