/* paint — рисовалка мышью.
 *   левая кнопка — рисовать   правая — ластик   панель сверху — цвет и толщина
 *   1..5 — толщина   C — очистить   Q, Esc — выход */
#include "gfx.h"

#define BAR 44                                  /* высота панели инструментов */

static const color_t colors[16] = {
    RGB(0, 0, 0),       RGB(255, 255, 255), RGB(128, 128, 128), RGB(192, 192, 192),
    RGB(220, 40, 40),   RGB(255, 140, 0),   RGB(255, 220, 0),   RGB(60, 200, 60),
    RGB(0, 120, 40),    RGB(0, 200, 200),   RGB(40, 90, 230),   RGB(20, 30, 120),
    RGB(150, 60, 200),  RGB(255, 100, 180), RGB(130, 80, 30),   RGB(250, 210, 170),
};
static const int sizes[5] = { 1, 3, 6, 12, 24 };
static int color = 0, size = 1;
static const color_t PAPER = RGB(255, 255, 255);

static void draw_bar(void) {
    gfx_fill_rect(0, 0, gfx_w, BAR, RGB(45, 45, 55));
    for (int i = 0; i < 16; i++) {
        int x = 8 + i * 34;
        gfx_fill_rect(x, 6, 30, 30, colors[i]);
        gfx_rect(x - 1, 5, 32, 32, i == color ? C_YELLOW : RGB(90, 90, 100));
        if (i == color) gfx_rect(x - 2, 4, 34, 34, C_YELLOW);
    }
    int sx = 8 + 16 * 34 + 16;
    for (int i = 0; i < 5; i++) {
        int x = sx + i * 34;
        gfx_fill_rect(x, 6, 30, 30, RGB(70, 70, 80));
        int r = sizes[i] / 2 + 1 > 13 ? 13 : sizes[i] / 2 + 1;
        gfx_fill_circle(x + 15, 21, r, C_WHITE);
        if (i == size) gfx_rect(x - 2, 4, 34, 34, C_YELLOW);
    }
    int bx = sx + 5 * 34 + 16;
    gfx_fill_rect(bx, 6, 70, 30, RGB(150, 40, 40));
    gfx_text(bx + 15, 13, "Clear", C_WHITE, TRANSPARENT);
    gfx_text(bx + 82, 6, "LMB draw  RMB erase", RGB(190, 190, 200), TRANSPARENT);
    gfx_text(bx + 82, 22, "1-5 size  C  Q quit", RGB(190, 190, 200), TRANSPARENT);
}

static void clear_canvas(void) { gfx_fill_rect(0, BAR, gfx_w, gfx_h - BAR, PAPER); }

/* щелчок по панели */
static void click_bar(int x, int y) {
    if (y < 4 || y > 38) return;
    for (int i = 0; i < 16; i++) if (x >= 8 + i * 34 && x < 38 + i * 34) color = i;
    int sx = 8 + 16 * 34 + 16;
    for (int i = 0; i < 5; i++) if (x >= sx + i * 34 && x < sx + 30 + i * 34) size = i;
    int bx = sx + 5 * 34 + 16;
    if (x >= bx && x < bx + 70) clear_canvas();
    draw_bar();
}

int main(void) {
    if (gfx_open() < 0) { printf("paint: graphics mode is not available\n"); return 1; }
    clear_canvas();
    draw_bar();
    struct mouse m, prev;
    mouse_get(&m);
    prev = m;
    bool dirty = true;
    for (;;) {
        int k;
        while ((k = readkey()) >= 0) {
            if (k == 'q' || k == 'Q' || k == 27) goto quit;
            if (k >= '1' && k <= '5') { size = k - '1'; draw_bar(); dirty = true; }
            if (k == 'c' || k == 'C') { clear_canvas(); dirty = true; }
        }
        prev = m;
        mouse_get(&m);
        if (m.events != prev.events) dirty = true;

        bool down = m.buttons & (MOUSE_LEFT | MOUSE_RIGHT);
        bool was_down = prev.buttons & (MOUSE_LEFT | MOUSE_RIGHT);
        if (down && m.y < BAR) {
            if (!was_down) click_bar(m.x, m.y);
        } else if (down && m.y >= BAR) {
            color_t c = (m.buttons & MOUSE_LEFT) ? colors[color] : PAPER;
            int r = sizes[size] / 2;
            int px = was_down && prev.y >= BAR ? prev.x : m.x;
            int py = was_down && prev.y >= BAR ? prev.y : m.y;
            gfx_thick_line(px, py, m.x, m.y, (m.buttons & MOUSE_RIGHT) ? r + 6 : r, c);
            draw_bar();                                 /* толстая кисть могла залезть на панель */
        }
        if (dirty) {
            gfx_cursor_draw(m.x, m.y);
            gfx_flip();
            gfx_cursor_erase();
            dirty = false;
        } else {
            sleep_ms(10);
        }
    }
quit:
    gfx_close();
    return 0;
}
