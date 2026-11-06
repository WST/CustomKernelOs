/* battle — «Морской бой» против компьютера.
 * Сначала расставь флот на левом поле: левая кнопка — поставить корабль (щелчок по
 * кораблю — убрать), правая кнопка или R — повернуть, Random — расставить случайно.
 * Start — в бой: стреляй по правому полю. Попал — стреляешь ещё раз, промахнулся —
 * ходит компьютер. Корабли не касаются друг друга даже углами. N — новая игра. */
#include "win.h"

#define N 10
#define SHIPS 10
#define CELL 26
#define LAB 20                                  /* место под подписи A..J и 1..10 */
#define TOP 48                                  /* шапка с кнопками */
#define BY (TOP + 40)                           /* верх обоих полей */
#define BX0 (12 + LAB)
#define BX1 (BX0 + N * CELL + 36 + LAB)
#define W (BX1 + N * CELL + 12)
#define H (BY + N * CELL + 40)
#define AI_DELAY 600

enum { PLACE, PLAYER, AI, WON, LOST };
enum { MISS, HIT, SUNK };

struct ship { int r, c, len; bool horiz; int hits; };

static window *win;
static int  cell[2][N][N];                      /* 0 — вода, иначе номер корабля + 1; [0] игрок, [1] компьютер */
static char shot[2][N][N];                      /* 0 — не стреляли, 1 — промах, 2 — попадание */
static struct ship fleet[2][SHIPS];
static bool placed[SHIPS];                      /* какие корабли игрок уже поставил */
static const int sizes[SHIPS] = { 4, 3, 3, 2, 2, 2, 1, 1, 1, 1 };
static int  state;
static bool horiz = true;                       /* как ставим следующий корабль */
static int  hover_b = -1, hover_r, hover_c;     /* клетка под мышью */
static int  last_r = -1, last_c = -1;           /* последний выстрел компьютера */
static unsigned ai_due;
static const char *status = "";               /* строка внизу окна */

/* что знает компьютер: клетки, где корабля быть не может, раненый корабль, оставшиеся корабли */
static bool ai_no[N][N];
static int  wound_r[4], wound_c[4], n_wound;
static int  ai_left[5];                         /* сколько кораблей каждой длины ещё не потоплено */

static bool inside(int r, int c) { return r >= 0 && r < N && c >= 0 && c < N; }

/* ------------------------------------------------------------ флот */

static bool can_place(int side, int r, int c, int len, bool hz) {
    for (int k = 0; k < len; k++) {
        int rr = r + (hz ? 0 : k), cc = c + (hz ? k : 0);
        if (!inside(rr, cc)) return false;
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                if (inside(rr + dr, cc + dc) && cell[side][rr + dr][cc + dc]) return false;
    }
    return true;
}

static void place(int side, int i, int r, int c, bool hz) {
    struct ship *s = &fleet[side][i];
    s->r = r; s->c = c; s->len = sizes[i]; s->horiz = hz; s->hits = 0;
    for (int k = 0; k < s->len; k++) cell[side][r + (hz ? 0 : k)][c + (hz ? k : 0)] = i + 1;
}

static void remove_ship(int side, int i) {
    struct ship *s = &fleet[side][i];
    for (int k = 0; k < s->len; k++) cell[side][s->r + (s->horiz ? 0 : k)][s->c + (s->horiz ? k : 0)] = 0;
}

static void random_fleet(int side) {
restart:
    memset(cell[side], 0, sizeof(cell[side]));
    for (int i = 0; i < SHIPS; i++) {
        int tries = 0;
        for (;;) {
            if (++tries > 1000) goto restart;
            bool hz = rand() % 2;
            int r = rand() % N, c = rand() % N;
            if (can_place(side, r, c, sizes[i], hz)) { place(side, i, r, c, hz); break; }
        }
    }
}

static int alive(int side) {
    int n = 0;
    for (int i = 0; i < SHIPS; i++) if (fleet[side][i].hits < fleet[side][i].len) n++;
    return n;
}

/* выстрел по полю side; вокруг потопленного корабля сразу ставим промахи */
static int fire(int side, int r, int c) {
    int id = cell[side][r][c];
    if (!id) { shot[side][r][c] = 1; return MISS; }
    shot[side][r][c] = 2;
    struct ship *s = &fleet[side][id - 1];
    if (++s->hits < s->len) return HIT;
    for (int k = 0; k < s->len; k++) {
        int rr = s->r + (s->horiz ? 0 : k), cc = s->c + (s->horiz ? k : 0);
        for (int dr = -1; dr <= 1; dr++)
            for (int dc = -1; dc <= 1; dc++)
                if (inside(rr + dr, cc + dc) && !shot[side][rr + dr][cc + dc]) shot[side][rr + dr][cc + dc] = 1;
    }
    return SUNK;
}

static bool sunk_at(int side, int r, int c) {
    int id = cell[side][r][c];
    return id && fleet[side][id - 1].hits == fleet[side][id - 1].len;
}

static int next_ship(void) {
    for (int i = 0; i < SHIPS; i++) if (!placed[i]) return i;
    return -1;
}

/* ------------------------------------------------------------ компьютер */

static bool ai_free(int r, int c) { return inside(r, c) && !shot[0][r][c] && !ai_no[r][c]; }

/* влезает ли через (r, c) хоть один корабль длины len — по горизонтали или вертикали */
static bool ai_fits(int r, int c, int len) {
    int a = c, b = c;
    while (ai_free(r, a - 1)) a--;
    while (ai_free(r, b + 1)) b++;
    if (b - a + 1 >= len) return true;
    a = b = r;
    while (ai_free(a - 1, c)) a--;
    while (ai_free(b + 1, c)) b++;
    return b - a + 1 >= len;
}

static void ai_reset(void) {
    memset(ai_no, 0, sizeof(ai_no));
    n_wound = 0;
    for (int l = 0; l < 5; l++) ai_left[l] = 0;
    for (int i = 0; i < SHIPS; i++) ai_left[sizes[i]]++;
}

static int ai_shot(void) {
    static int cr[N * N], cc[N * N];
    int n = 0;
    /* добивание: рядом с раненым кораблём */
    if (n_wound == 1) {
        static const int d[4][2] = { { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 } };
        for (int k = 0; k < 4; k++) {
            int r = wound_r[0] + d[k][0], c = wound_c[0] + d[k][1];
            if (ai_free(r, c)) { cr[n] = r; cc[n] = c; n++; }
        }
    } else if (n_wound > 1) {
        bool hz = wound_r[0] == wound_r[1];
        int lo = 99, hi = -1;
        for (int k = 0; k < n_wound; k++) {
            int v = hz ? wound_c[k] : wound_r[k];
            if (v < lo) lo = v;
            if (v > hi) hi = v;
        }
        int r0 = hz ? wound_r[0] : lo - 1, c0 = hz ? lo - 1 : wound_c[0];
        int r1 = hz ? wound_r[0] : hi + 1, c1 = hz ? hi + 1 : wound_c[0];
        if (ai_free(r0, c0)) { cr[n] = r0; cc[n] = c0; n++; }
        if (ai_free(r1, c1)) { cr[n] = r1; cc[n] = c1; n++; }
    }
    /* охота: шахматный порядок по самому короткому оставшемуся кораблю,
     * и только там, куда он вообще влезает */
    if (!n) {
        int minlen = 1;
        for (int l = 4; l >= 1; l--) if (ai_left[l]) minlen = l;
        for (int pass = 0; pass < 3 && !n; pass++)
            for (int r = 0; r < N; r++)
                for (int c = 0; c < N; c++) {
                    if (!ai_free(r, c)) continue;
                    if (pass < 2 && !ai_fits(r, c, minlen)) continue;
                    if (pass == 0 && (r + c) % minlen) continue;
                    cr[n] = r; cc[n] = c; n++;
                }
    }
    if (!n)                                             /* на всякий случай: любая необстрелянная */
        for (int r = 0; r < N; r++)
            for (int c = 0; c < N; c++)
                if (!shot[0][r][c]) { cr[n] = r; cc[n] = c; n++; }
    int k = rand() % n, r = cr[k], c = cc[k];
    int res = fire(0, r, c);
    last_r = r; last_c = c;
    if (res != MISS) {
        if (n_wound < 4) { wound_r[n_wound] = r; wound_c[n_wound] = c; n_wound++; }
        /* корабли не касаются углами — по диагонали от попадания пусто */
        for (int dr = -1; dr <= 1; dr += 2)
            for (int dc = -1; dc <= 1; dc += 2)
                if (inside(r + dr, c + dc)) ai_no[r + dr][c + dc] = true;
        if (res == SUNK) { ai_left[n_wound]--; n_wound = 0; }
    }
    return res;
}

/* ------------------------------------------------------------ игра */

static void new_game(void) {
    memset(cell, 0, sizeof(cell));
    memset(shot, 0, sizeof(shot));
    memset(fleet, 0, sizeof(fleet));
    memset(placed, 0, sizeof(placed));
    ai_reset();
    state = PLACE; horiz = true;
    last_r = last_c = -1;
    srand(ticks());
}

static void update_status(void) {
    static char buf[80];
    int i = next_ship();
    if (state == WON) status = "You won! Press New game or N to play again";
    else if (state == LOST) status = "You lost. Press New game or N to play again";
    else if (state != PLACE) return;
    else if (i < 0) status = "All ships are placed - press Start";
    else {
        snprintf(buf, sizeof(buf), "Place a %d-cell ship: click your board, right click or R to rotate", sizes[i]);
        status = buf;
    }
}

static void start_battle(void) {
    if (next_ship() >= 0) return;
    random_fleet(1);
    state = PLAYER;
    status = "Your turn: fire at the enemy board";
}

static void player_fire(int r, int c) {
    if (shot[1][r][c]) return;
    int res = fire(1, r, c);
    if (!alive(1)) { state = WON; update_status(); return; }
    if (res == MISS) { state = AI; ai_due = ticks() + AI_DELAY; status = "Miss. Computer is aiming..."; }
    else status = res == SUNK ? "Sunk! Fire again" : "Hit! Fire again";
}

static void ai_turn(void) {
    int res = ai_shot();
    if (!alive(0)) { state = LOST; update_status(); return; }
    if (res == MISS) { state = PLAYER; status = "Computer missed. Your turn"; }
    else { ai_due = ticks() + AI_DELAY; status = res == SUNK ? "Computer sank your ship!" : "Computer hit your ship!"; }
}

/* ------------------------------------------------------------ рисование */

static int bx(int b) { return b ? BX1 : BX0; }

static void tile(int x, int y, color_t low, color_t high) {
    gfx_round_rect(x + 1, y + 1, CELL - 2, CELL - 2, 4, low);
    gfx_round_rect(x + 1, y + 1, CELL - 2, CELL - 4, 4, high);
}

static void flat(int x, int y, color_t c) { gfx_round_rect(x + 1, y + 1, CELL - 2, CELL - 2, 4, c); }

static void cross(int x, int y, color_t c) {
    int cx = (x + CELL / 2) * 16, cy = (y + CELL / 2) * 16;
    gfx_line16(cx - 96, cy - 96, cx + 96, cy + 96, 40, c, 255);
    gfx_line16(cx + 96, cy - 96, cx - 96, cy + 96, 40, c, 255);
}

static void draw_cell(int b, int r, int c) {
    int x = bx(b) + c * CELL, y = BY + r * CELL;
    bool show = cell[b][r][c] && (b == 0 || state == LOST);
    if (shot[b][r][c] == 2) {
        if (sunk_at(b, r, c)) { flat(x, y, RGB(164, 0, 0)); cross(x, y, RGB(255, 220, 220)); }
        else { flat(x, y, RGB(252, 175, 62)); cross(x, y, RGB(204, 0, 0)); }
    } else if (shot[b][r][c] == 1) {
        flat(x, y, RGB(226, 236, 244));
        gfx_disc16((x + CELL / 2) * 16, (y + CELL / 2) * 16, 48, RGB(46, 52, 54), 255);
    } else if (show) tile(x, y, RGB(85, 87, 83), RGB(124, 128, 122));
    else tile(x, y, RGB(158, 192, 222), RGB(190, 216, 238));
    if (b == 0 && r == last_r && c == last_c && state != PLACE)
        gfx_round_rect_a(x + 1, y + 1, CELL - 2, CELL - 2, 4, UI_ACCENT, 70);
    if (b == 1 && state == PLAYER && hover_b == 1 && r == hover_r && c == hover_c && !shot[1][r][c])
        gfx_round_rect_a(x + 1, y + 1, CELL - 2, CELL - 2, 4, UI_ACCENT, 120);
}

static void draw_board(int b, const char *title) {
    int x0 = bx(b);
    text_draw(FONT_UI_BOLD, 14, x0, TOP + 2, title, UI_TEXT);
    for (int i = 0; i < N; i++) {
        char s[4] = { (char)('A' + i), 0 };
        int tw = text_width(FONT_UI, 12, s);
        text_draw(FONT_UI, 12, x0 + i * CELL + (CELL - tw) / 2, BY - 18, s, UI_DARK);
        snprintf(s, sizeof(s), "%d", i + 1);
        tw = text_width(FONT_UI, 12, s);
        text_draw(FONT_UI, 12, x0 - 4 - tw, BY + i * CELL + (CELL - text_line_h(FONT_UI, 12)) / 2, s, UI_DARK);
    }
    for (int r = 0; r < N; r++)
        for (int c = 0; c < N; c++) draw_cell(b, r, c);
}

/* «призрак» корабля, который сейчас ставим */
static void draw_ghost(void) {
    int i = next_ship();
    if (state != PLACE || i < 0 || hover_b != 0) return;
    bool ok = can_place(0, hover_r, hover_c, sizes[i], horiz);
    for (int k = 0; k < sizes[i]; k++) {
        int r = hover_r + (horiz ? 0 : k), c = hover_c + (horiz ? k : 0);
        if (!inside(r, c)) break;
        gfx_round_rect_a(BX0 + c * CELL + 1, BY + r * CELL + 1, CELL - 2, CELL - 2, 4,
                         ok ? RGB(78, 154, 6) : RGB(239, 41, 41), 150);
    }
}

/* табло: число на тёмной плашке и подпись слева */
static void counter(int x, const char *label, int v) {
    char s[8];
    snprintf(s, sizeof(s), "%d", v);
    int lw = text_width(FONT_UI, 13, label);
    text_draw(FONT_UI, 13, x - lw - 6, 10 + (28 - text_line_h(FONT_UI, 13)) / 2, label, UI_TEXT);
    gfx_round_rect(x, 10, 40, 28, 6, RGB(46, 52, 54));
    int tw = text_width(FONT_UI_BOLD, 16, s);
    text_draw(FONT_UI_BOLD, 16, x + (40 - tw) / 2, 10 + (28 - text_line_h(FONT_UI_BOLD, 16)) / 2, s, RGB(252, 233, 79));
}

static void draw(void) {
    gfx_clear(UI_FACE);
    if (state == PLACE) {
        ui_button(12, 10, 80, 28, "Random", false);
        ui_button(100, 10, 70, 28, "Clear", false);
        if (next_ship() < 0) ui_button_accent(178, 10, 80, 28, "Start", false);
        else {
            gfx_round_frame(178, 10, 80, 28, 6, UI_SHADOW, RGB(236, 236, 234));
            ui_text_center(178, 10, 80, 28, "Start", UI_DARK);
        }
    } else {
        ui_button(12, 10, 100, 28, "New game", false);
        counter(W - 52 - 110, "You", alive(0));
        counter(W - 52, "Enemy", alive(1));
    }
    draw_board(0, "Your fleet");
    draw_board(1, "Enemy waters");
    draw_ghost();
    color_t sc = state == WON ? RGB(78, 154, 6) : state == LOST ? RGB(204, 0, 0) : UI_TEXT;
    int tw = text_width(FONT_UI_MEDIUM, 14, status);
    text_draw(FONT_UI_MEDIUM, 14, (W - tw) / 2, BY + N * CELL + 12, status, sc);
    win_update(win);
}

/* клетка поля под точкой окна: -> номер поля или -1 */
static int hit_board(int x, int y, int *r, int *c) {
    for (int b = 0; b < 2; b++)
        if (ui_inside(x, y, bx(b), BY, N * CELL, N * CELL)) {
            *r = (y - BY) / CELL; *c = (x - bx(b)) / CELL;
            return b;
        }
    return -1;
}

static void place_click(int r, int c) {
    int id = cell[0][r][c];
    if (id) { remove_ship(0, id - 1); placed[id - 1] = false; return; }
    int i = next_ship();
    if (i >= 0 && can_place(0, r, c, sizes[i], horiz)) { place(0, i, r, c, horiz); placed[i] = true; }
}

static void on_click(int x, int y, unsigned buttons) {
    int r, c, b = hit_board(x, y, &r, &c);
    if (state == PLACE) {
        if (buttons & MOUSE_RIGHT) { horiz = !horiz; return; }
        if (ui_inside(x, y, 12, 10, 80, 28)) {
            random_fleet(0);
            for (int i = 0; i < SHIPS; i++) placed[i] = true;
        } else if (ui_inside(x, y, 100, 10, 70, 28)) {
            memset(cell[0], 0, sizeof(cell[0]));
            memset(placed, 0, sizeof(placed));
        } else if (ui_inside(x, y, 178, 10, 80, 28)) {
            start_battle();
            return;
        } else if (b == 0) place_click(r, c);
        update_status();
        return;
    }
    if (ui_inside(x, y, 12, 10, 100, 28)) { new_game(); update_status(); return; }
    if (state == PLAYER && b == 1 && (buttons & MOUSE_LEFT)) player_fire(r, c);
}

int main(void) {
    win = win_create("Battleship", W, H);
    if (!win) { dprintf(STDERR, "battle: the desktop is not running\n"); return 1; }
    new_game();
    update_status();
    draw();
    struct event e;
    for (;;) {
        int timeout = -1;
        if (state == AI) {
            int d = (int)(ai_due - ticks());
            timeout = d < 1 ? 1 : d;
        }
        if (!win_event(&e, timeout)) break;
        if (e.type == EV_CLOSE) break;
        bool redraw = false;
        if (e.type == EV_KEY) {
            if (e.key == 'n' || e.key == 'N') { new_game(); update_status(); redraw = true; }
            else if ((e.key == 'r' || e.key == 'R') && state == PLACE) { horiz = !horiz; redraw = true; }
        } else if (e.type == EV_MOUSE_MOVE) {
            int r = 0, c = 0, b = hit_board(e.x, e.y, &r, &c);
            if (b != hover_b || r != hover_r || c != hover_c) {
                hover_b = b; hover_r = r; hover_c = c;
                redraw = true;
            }
        } else if (e.type == EV_MOUSE_DOWN) {
            on_click(e.x, e.y, e.buttons);
            redraw = true;
        }
        if (state == AI && (int)(ticks() - ai_due) >= 0) { ai_turn(); redraw = true; }
        if (redraw) draw();
    }
    win_destroy(win);
    return 0;
}
