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
    0, RGB(0, 0, 255), RGB(0, 128, 0), RGB(255, 0, 0), RGB(0, 0, 128),
    RGB(128, 0, 0), RGB(0, 128, 128), RGB(0, 0, 0), RGB(128, 128, 128),
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

static void raised(int x, int y, int w, int h) {
    gfx_fill_rect(x, y, w, h, RGB(192, 192, 192));
    gfx_fill_rect(x, y, w, 2, C_WHITE);
    gfx_fill_rect(x, y, 2, h, C_WHITE);
    gfx_fill_rect(x, y + h - 2, w, 2, RGB(128, 128, 128));
    gfx_fill_rect(x + w - 2, y, 2, h, RGB(128, 128, 128));
}

static void draw_mine(int x, int y) {
    gfx_fill_circle(x + CELL / 2, y + CELL / 2, 6, C_BLACK);
    gfx_fill_rect(x + 4, y + CELL / 2, CELL - 8, 1, C_BLACK);
    gfx_fill_rect(x + CELL / 2, y + 4, 1, CELL - 8, C_BLACK);
    gfx_fill_rect(x + CELL / 2 - 3, y + CELL / 2 - 3, 2, 2, C_WHITE);
}

static void draw(void) {
    gfx_clear(RGB(192, 192, 192));
    /* шапка: мины, смайлик, время */
    int left = MINES;
    for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (flag[r][c]) left--;
    char s[8];
    gfx_fill_rect(8, 8, 40, 28, C_BLACK);
    snprintf(s, sizeof(s), "%03d", left < 0 ? 0 : left);
    gfx_text(12, 14, s, RGB(255, 40, 40), TRANSPARENT);
    gfx_fill_rect(W - 48, 8, 40, 28, C_BLACK);
    snprintf(s, sizeof(s), "%03u", elapsed > 999 ? 999 : elapsed);
    gfx_text(W - 44, 14, s, RGB(255, 40, 40), TRANSPARENT);
    int sx = W / 2 - 14;
    raised(sx, 6, 30, 30);
    gfx_fill_circle(sx + 15, 21, 10, C_YELLOW);
    gfx_circle(sx + 15, 21, 10, C_BLACK);
    if (state == 1) {                                          /* :( */
        gfx_line(sx + 10, 16, sx + 13, 19, C_BLACK); gfx_line(sx + 13, 16, sx + 10, 19, C_BLACK);
        gfx_line(sx + 17, 16, sx + 20, 19, C_BLACK); gfx_line(sx + 20, 16, sx + 17, 19, C_BLACK);
        gfx_line(sx + 10, 27, sx + 20, 27, C_BLACK);
    } else {
        if (state == 2) gfx_fill_rect(sx + 8, 16, 14, 3, C_BLACK);   /* очки победителя */
        else { gfx_fill_rect(sx + 10, 17, 2, 2, C_BLACK); gfx_fill_rect(sx + 18, 17, 2, 2, C_BLACK); }
        gfx_line(sx + 10, 25, sx + 15, 28, C_BLACK); gfx_line(sx + 15, 28, sx + 20, 25, C_BLACK);
    }
    /* поле */
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) {
            int x = 8 + c * CELL, y = TOP + r * CELL;
            if (open_[r][c] || (state == 1 && mine[r][c])) {
                gfx_fill_rect(x, y, CELL, CELL, (r == boom_r && c == boom_c) ? RGB(255, 0, 0) : RGB(192, 192, 192));
                gfx_rect(x, y, CELL + 1, CELL + 1, RGB(128, 128, 128));
                if (mine[r][c]) draw_mine(x, y);
                else if (around[r][c]) {
                    char d[2] = { (char)('0' + around[r][c]), 0 };
                    gfx_text(x + 9, y + 5, d, num_col[around[r][c]], TRANSPARENT);
                    gfx_text(x + 10, y + 5, d, num_col[around[r][c]], TRANSPARENT);
                }
            } else {
                raised(x, y, CELL, CELL);
                if (flag[r][c]) {
                    gfx_fill_rect(x + 12, y + 6, 2, 14, C_BLACK);
                    gfx_fill_rect(x + 7, y + 6, 6, 6, RGB(255, 0, 0));
                    gfx_fill_rect(x + 8, y + 19, 10, 2, C_BLACK);
                }
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
