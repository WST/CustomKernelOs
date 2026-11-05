/* calc — калькулятор. Мышью по кнопкам или с клавиатуры: цифры, + - * /, Enter, Backspace, Esc. */
#include "win.h"

#define W 252
#define H 330
#define DISP_H 64
#define BW 60
#define BH 48
#define GAP 2

static window *win;
static char disp[32] = "0";
static double acc;                         /* что уже посчитано */
static char op;                            /* ждущая операция: + - * / или 0 */
static bool fresh = true;                  /* следующая цифра начинает новое число */
static bool error_state;
static int pressed = -1;

static const char *keys[20] = {
    "C", "<-", "%", "/",
    "7", "8", "9", "*",
    "4", "5", "6", "-",
    "1", "2", "3", "+",
    "+/-", "0", ".", "=",
};

static double parse(const char *s) {
    double v = 0, f = 0.1;
    bool neg = *s == '-', frac = false;
    if (neg) s++;
    for (; *s; s++) {
        if (*s == '.') { frac = true; continue; }
        if (!frac) v = v * 10 + (*s - '0');
        else { v += (*s - '0') * f; f /= 10; }
    }
    return neg ? -v : v;
}

/* число -> текст: целое без «.0», дробное — до 10 знаков без хвостовых нулей */
static void show(double v) {
    if (v > 1e15 || v < -1e15) { strcpy(disp, "too big"); error_state = true; return; }
    snprintf(disp, sizeof(disp), "%.10f", v);
    char *dot = strchr(disp, '.');
    if (dot) {
        char *e = disp + strlen(disp) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e == dot) *e = 0;
    }
    if (!strcmp(disp, "-0")) strcpy(disp, "0");
}

static void apply_op(void) {
    double v = parse(disp);
    switch (op) {
    case '+': acc += v; break;
    case '-': acc -= v; break;
    case '*': acc *= v; break;
    case '/':
        if (v == 0) { strcpy(disp, "can't divide by 0"); error_state = true; op = 0; return; }
        acc /= v;
        break;
    default: acc = v;
    }
    show(acc);
}

static void press(const char *k) {
    if (error_state && strcmp(k, "C")) return;
    if (!strcmp(k, "C")) { strcpy(disp, "0"); acc = 0; op = 0; fresh = true; error_state = false; return; }
    if (!strcmp(k, "<-")) {
        if (fresh) return;
        int n = strlen(disp);
        if (n > 1) disp[n - 1] = 0; else strcpy(disp, "0");
        return;
    }
    if (!strcmp(k, "+/-")) {
        if (!strcmp(disp, "0")) return;
        if (disp[0] == '-') memmove(disp, disp + 1, strlen(disp));
        else if (strlen(disp) < 20) { memmove(disp + 1, disp, strlen(disp) + 1); disp[0] = '-'; }
        return;
    }
    if (!strcmp(k, "%")) { show(parse(disp) / 100); fresh = true; return; }
    if (k[0] >= '0' && k[0] <= '9') {
        if (fresh || !strcmp(disp, "0")) { disp[0] = 0; fresh = false; }
        if (strlen(disp) < 16) strcat(disp, k);
        return;
    }
    if (!strcmp(k, ".")) {
        if (fresh) { strcpy(disp, "0"); fresh = false; }
        if (!strchr(disp, '.')) strcat(disp, ".");
        return;
    }
    if (!strcmp(k, "=")) { apply_op(); op = 0; fresh = true; return; }
    /* + - * / */
    if (!fresh || !op) apply_op();
    op = k[0];
    fresh = true;
}

static void draw(void) {
    gfx_clear(UI_FACE);
    if (op) { char o[2] = { op, 0 }; text_draw(FONT_UI, 16, 12, 8, o, UI_DARK); }
    int size = 40, tw;
    while ((tw = text_width(FONT_UI, size, disp)) > W - 24 && size > 14) size -= 2;
    text_draw(FONT_UI, size, W - 12 - tw, DISP_H - 6 - text_line_h(FONT_UI, size), disp, UI_TEXT);
    for (int i = 0; i < 20; i++) {
        int x = GAP + (i % 4) * (BW + GAP), y = DISP_H + GAP + (i / 4) * (BH + GAP);
        bool digit = keys[i][0] >= '0' && keys[i][0] <= '9';
        if (i == 19) { ui_button_accent(x + 2, y + 2, BW - 4, BH - 4, keys[i], i == pressed); continue; }
        color_t bg = digit ? RGB(255, 255, 255) : RGB(236, 236, 236);
        if (i == pressed) bg = RGB(214, 214, 214);
        gfx_round_rect(x + 2, y + 2, BW - 4, BH - 4, 8, bg);
        int th = text_line_h(FONT_UI, 18), kw = text_width(FONT_UI, 18, keys[i]);
        text_draw(FONT_UI, 18, x + (BW - kw) / 2, y + (BH - th) / 2, keys[i], UI_TEXT);
    }
    win_update(win);
}

int main(void) {
    win = win_create("Calculator", W, H);
    if (!win) { dprintf(STDERR, "calc: the desktop is not running\n"); return 1; }
    draw();
    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_DOWN) {
            int c = (e.x - GAP) / (BW + GAP), r = (e.y - DISP_H - GAP) / (BH + GAP);
            if (e.y > DISP_H && c >= 0 && c < 4 && r >= 0 && r < 5) { pressed = r * 4 + c; press(keys[pressed]); }
        } else if (e.type == EV_MOUSE_UP) {
            pressed = -1;
        } else if (e.type == EV_KEY) {
            int k = e.key;
            char s[2] = { (char)k, 0 };
            if ((k >= '0' && k <= '9') || k == '.' || k == '+' || k == '-' || k == '*' || k == '/' || k == '%') press(s);
            else if (k == '\n' || k == '\r' || k == '=') press("=");
            else if (k == 8 || k == 127) press("<-");
            else if (k == 27 || k == 'c' || k == 'C') press("C");
            else continue;
        } else continue;
        draw();
    }
    win_destroy(win);
    return 0;
}
