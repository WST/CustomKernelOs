/* mines — «Сапёр». Левая кнопка — открыть клетку, правая — флажок.
 * Цифра — сколько мин вокруг. Открыл все клетки без мин — победа. Смайлик — новая игра. */
#include "win.h"

#define N 9
#define MINES 10
#define CELL 26
#define TOP 44
#define W (N * CELL + 16)
#define H (TOP + N * CELL + 8)

static window *win;
static bool mine[N][N], open_[N][N], flag[N][N];
static int  around[N][N];
static int  state;                         /* 0 игра, 1 проиграл, 2 выиграл */
static bool started;
static unsigned start_tick, elapsed;
static int  boom_r = -1, boom_c = -1;

static const color_t num_col[9] = {
    0, RGB(52, 101, 164), RGB(78, 154, 6), RGB(204, 0, 0), RGB(117, 80, 123),
    RGB(206, 92, 0), RGB(6, 152, 154), RGB(46, 52, 54), RGB(136, 138, 133),
};

static void new_game(void) {
    memset(mine, 0, sizeof(mine));
    memset(open_, 0, sizeof(open_));
    memset(flag, 0, sizeof(flag));
    state = 0; started = false; elapsed = 0;
    boom_r = boom_c = -1;
}

/* мины раскладываем после первого клика — чтобы первый ход не взорвался */
static void place_mines(int sr, int sc) {
    srand(ticks());
    int placed = 0;
    while (placed < MINES) {
        int r = rand() % N, c = rand() % N;
        if (mine[r][c] || (r >= sr - 1 && r <= sr + 1 && c >= sc - 1 && c <= sc + 1)) continue;
        mine[r][c] = true;
        placed++;
    }
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            int n = 0;
            for (int dr = -1; dr <= 1; dr++)
                for (int dc = -1; dc <= 1; dc++) {
                    int rr = r + dr, cc = c + dc;
                    if (rr >= 0 && rr < N && cc >= 0 && cc < N && mine[rr][cc]) n++;
                }
            around[r][c] = n;
        }
}

static void reveal(int r, int c) {
    if (r < 0 || r >= N || c < 0 || c >= N || open_[r][c] || flag[r][c]) return;
    open_[r][c] = true;
    if (around[r][c] == 0 && !mine[r][c])
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++) reveal(r + dr, c + dc);
}

static void check_win(void) {
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++)
            if (!mine[r][c] && !open_[r][c]) return;
    state = 2;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (mine[r][c]) flag[r][c] = true;
}

static void draw_mine(int x, int y) {
    int cx = (x + CELL / 2) * 16, cy = (y + CELL / 2) * 16;
    gfx_line16(cx - 136, cy, cx + 136, cy, 28, RGB(46, 52, 54), 255);
    gfx_line16(cx, cy - 136, cx, cy + 136, 28, RGB(46, 52, 54), 255);
    gfx_line16(cx - 96, cy - 96, cx + 96, cy + 96, 28, RGB(46, 52, 54), 255);
    gfx_line16(cx + 96, cy - 96, cx - 96, cy + 96, 28, RGB(46, 52, 54), 255);
    gfx_disc16(cx, cy, 96, RGB(46, 52, 54), 255);
    gfx_disc16(cx - 32, cy - 32, 28, C_WHITE, 220);
}

static void draw_flag(int x, int y) {
    gfx_round_rect(x + 12, y + 6, 2, 15, 1, RGB(46, 52, 54));
    gfx_path p;
    path_begin(&p, x, y, 1, 1);
    path_move(&p, 14, 6); path_line(&p, 21, 9); path_line(&p, 14, 13); path_close(&p);
    path_fill(&p, UI_ACCENT, 255);
    path_free(&p);
    gfx_round_rect(x + 8, y + 20, 10, 2, 1, RGB(46, 52, 54));
}

/* табло: число на тёмной плашке */
static void counter(int x, const char *s) {
    gfx_round_rect(x, 8, 48, 28, 6, RGB(46, 52, 54));
    int tw = text_width(FONT_UI_BOLD, 16, s);
    text_draw(FONT_UI_BOLD, 16, x + (48 - tw) / 2, 8 + (28 - text_line_h(FONT_UI_BOLD, 16)) / 2, s, RGB(252, 233, 79));
}

static void draw(void) {
    gfx_clear(UI_FACE);
    /* шапка: мины, смайлик, время */
    int left = MINES;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (flag[r][c]) left--;
    char s[8];
    snprintf(s, sizeof(s), "%03d", left < 0 ? 0 : left);
    counter(8, s);
    snprintf(s, sizeof(s), "%03u", elapsed > 999 ? 999 : elapsed);
    counter(W - 56, s);
    int sx = W / 2 - 14;
    gfx_round_frame(sx - 1, 6, 32, 32, 8, UI_SHADOW, RGB(246, 245, 244));
    int fx = (sx + 15) * 16, fy = 22 * 16;
    gfx_disc16(fx, fy, 160, RGB(250, 205, 40), 255);
    if (state == 1) {                                          /* :( */
        gfx_line16(fx - 80, fy - 64, fx - 32, fy - 16, 20, RGB(46, 52, 54), 255);
        gfx_line16(fx - 32, fy - 64, fx - 80, fy - 16, 20, RGB(46, 52, 54), 255);
        gfx_line16(fx + 32, fy - 64, fx + 80, fy - 16, 20, RGB(46, 52, 54), 255);
        gfx_line16(fx + 80, fy - 64, fx + 32, fy - 16, 20, RGB(46, 52, 54), 255);
        gfx_line16(fx - 64, fy + 80, fx + 64, fy + 80, 22, RGB(46, 52, 54), 255);
    } else {
        if (state == 2) gfx_round_rect(sx + 7, 17, 17, 4, 2, RGB(46, 52, 54));    /* очки победителя */
        else { gfx_disc16(fx - 56, fy - 40, 22, RGB(46, 52, 54), 255); gfx_disc16(fx + 56, fy - 40, 22, RGB(46, 52, 54), 255); }
        gfx_line16(fx - 72, fy + 48, fx, fy + 88, 22, RGB(46, 52, 54), 255);
        gfx_line16(fx, fy + 88, fx + 72, fy + 48, 22, RGB(46, 52, 54), 255);
    }
    /* поле: закрытые клетки — выпуклые плитки, открытые — плоские */
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            int x = 8 + c * CELL, y = TOP + r * CELL;
            if (open_[r][c] || (state == 1 && mine[r][c])) {
                bool boom = r == boom_r && c == boom_c;
                gfx_round_rect(x + 1, y + 1, CELL - 2, CELL - 2, 4, boom ? RGB(239, 41, 41) : RGB(236, 236, 234));
                if (mine[r][c]) draw_mine(x, y);
                else if (around[r][c]) {
                    char d[2] = { (char)('0' + around[r][c]), 0 };
                    int tw = text_width(FONT_UI_BOLD, 16, d);
                    text_draw(FONT_UI_BOLD, 16, x + (CELL - tw) / 2, y + (CELL - text_line_h(FONT_UI_BOLD, 16)) / 2,
                              d, num_col[around[r][c]]);
                }
            } else {
                gfx_round_rect(x + 1, y + 1, CELL - 2, CELL - 2, 4, RGB(186, 189, 182));
                gfx_round_rect(x + 1, y + 1, CELL - 2, CELL - 4, 4, RGB(211, 215, 207));
                if (flag[r][c]) draw_flag(x, y);
            }
        }
    win_update(win);
}

int main(void) {
    win = win_create("Minesweeper", W, H);
    if (!win) { dprintf(STDERR, "mines: the desktop is not running\n"); return 1; }
    new_game();
    draw();
    struct event e;
    while (win_event(&e, started && state == 0 ? 1000 : -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_TIMEOUT || (started && state == 0 && (ticks() - start_tick) / 1000 != elapsed)) {
            elapsed = (ticks() - start_tick) / 1000;
            if (e.type == EV_TIMEOUT) { draw(); continue; }
        }
        if (e.type == EV_KEY && (e.key == 'n' || e.key == 'N' || e.key == ' ')) { new_game(); draw(); continue; }
        if (e.type != EV_MOUSE_DOWN) continue;
        if (ui_inside(e.x, e.y, W / 2 - 14, 6, 30, 30)) { new_game(); draw(); continue; }
        int c = (e.x - 8) / CELL, r = (e.y - TOP) / CELL;
        if (state != 0 || e.y < TOP || e.x < 8 || r >= N || c >= N) continue;
        if (e.buttons & MOUSE_RIGHT) {
            if (!open_[r][c]) flag[r][c] = !flag[r][c];
        } else if (!flag[r][c]) {
            if (!started) { place_mines(r, c); started = true; start_tick = ticks(); }
            if (mine[r][c]) { state = 1; boom_r = r; boom_c = c; }
            else { reveal(r, c); check_win(); }
        }
        draw();
    }
    win_destroy(win);
    return 0;
}
