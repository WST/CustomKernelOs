/* draw — рисовалка в окне: палитра, толщина кисти, ластик, «Clear». */
#include "win.h"

#define W 600
#define H 420
#define BAR_H 40

static window *win;
static color_t canvas[W * (H - BAR_H)];
static const color_t palette[] = {
    RGB(0, 0, 0), RGB(127, 127, 127), RGB(136, 0, 21), RGB(237, 28, 36), RGB(255, 127, 39),
    RGB(255, 242, 0), RGB(34, 177, 76), RGB(0, 162, 232), RGB(63, 72, 204), RGB(163, 73, 164),
    RGB(255, 255, 255), RGB(185, 122, 87),
};
#define NPAL ((int)(sizeof(palette) / sizeof(palette[0])))
static int cur_col = 0, brush = 2;
static int sizes[] = { 1, 2, 4, 8 };
static int last_x = -1, last_y;

static void canvas_dot(int cx, int cy, int r, color_t c) {
    for (int y = -r; y <= r; y++)
        for (int x = -r; x <= r; x++)
            if (x * x + y * y <= r * r + r) {
                int px = cx + x, py = cy + y;
                if (px >= 0 && py >= 0 && px < W && py < H - BAR_H) canvas[py * W + px] = c;
            }
}

static void canvas_line(int x0, int y0, int x1, int y1, int r, color_t c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        canvas_dot(x0, y0, r, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_bar(void) {
    gfx_fill_rect(0, 0, W, BAR_H, UI_FACE);
    gfx_fill_rect(0, BAR_H - 1, W, 1, UI_SHADOW);
    for (int i = 0; i < NPAL; i++) {
        int x = 8 + i * 26;
        gfx_fill_rect(x, 8, 22, 22, palette[i]);
        gfx_rect(x, 8, 22, 22, i == cur_col ? UI_ACCENT : UI_SHADOW);
        if (i == cur_col) gfx_rect(x - 2, 6, 26, 26, UI_ACCENT);
    }
    for (int i = 0; i < 4; i++) {
        int x = 330 + i * 30;
        gfx_fill_rect(x, 6, 26, 26, sizes[i] == brush ? UI_SELECT : UI_FACE);
        gfx_rect(x, 6, 26, 26, sizes[i] == brush ? UI_ACCENT : UI_SHADOW);
        gfx_fill_circle(x + 13, 19, sizes[i] < 2 ? 1 : sizes[i], UI_TEXT);
    }
    ui_button(W - 76, 7, 68, 26, "Clear", false);
}

static void show_canvas(int x, int y, int w, int h) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H - BAR_H) h = H - BAR_H - y;
    if (w <= 0 || h <= 0) return;
    for (int j = 0; j < h; j++) memcpy(gfx_buf + (BAR_H + y + j) * W + x, canvas + (y + j) * W + x, w * 4);
    win_update_rect(win, x, BAR_H + y, w, h);
}

int main(void) {
    win = win_create("Paint", W, H);
    if (!win) { dprintf(STDERR, "draw: the desktop is not running\n"); return 1; }
    for (int i = 0; i < W * (H - BAR_H); i++) canvas[i] = C_WHITE;
    draw_bar();
    show_canvas(0, 0, W, H - BAR_H);
    win_update(win);
    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_DOWN && e.y < BAR_H) {
            for (int i = 0; i < NPAL; i++) if (ui_inside(e.x, e.y, 8 + i * 26, 8, 22, 22)) cur_col = i;
            for (int i = 0; i < 4; i++) if (ui_inside(e.x, e.y, 330 + i * 30, 6, 26, 26)) brush = sizes[i];
            if (ui_inside(e.x, e.y, W - 76, 7, 68, 26)) {
                for (int i = 0; i < W * (H - BAR_H); i++) canvas[i] = C_WHITE;
                show_canvas(0, 0, W, H - BAR_H);
            }
            draw_bar();
            win_update_rect(win, 0, 0, W, BAR_H);
            continue;
        }
        if ((e.type == EV_MOUSE_DOWN || e.type == EV_MOUSE_MOVE) && e.buttons) {
            int cy = e.y - BAR_H;
            color_t c = (e.buttons & MOUSE_RIGHT) ? C_WHITE : palette[cur_col];   /* правая кнопка — ластик */
            int r = (e.buttons & MOUSE_RIGHT) ? brush * 3 : brush;
            if (e.type == EV_MOUSE_DOWN || last_x < 0) { last_x = e.x; last_y = cy; }
            canvas_line(last_x, last_y, e.x, cy, r, c);
            int x0 = last_x < e.x ? last_x : e.x, y0 = last_y < cy ? last_y : cy;
            int x1 = last_x > e.x ? last_x : e.x, y1 = last_y > cy ? last_y : cy;
            show_canvas(x0 - r - 1, y0 - r - 1, x1 - x0 + 2 * r + 3, y1 - y0 + 2 * r + 3);
            last_x = e.x; last_y = cy;
        }
        if (e.type == EV_MOUSE_UP) last_x = -1;
    }
    win_destroy(win);
    return 0;
}
