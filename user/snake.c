/* snake — змейка. Стрелки или WASD, P — пауза, Q — выход.
 * Терминал в сыром неблокирующем режиме: read() не ждёт клавишу, поэтому
 * игра идёт сама, а мы раз в такт просто проверяем, что нажато. */
#include "myos.h"

#define X0 1
#define Y0 2
#define MAXW 128                       /* поле подстраивается под терминал (80x25 или 128x48) */
#define MAXH 48
#define MAXLEN (MAXW * MAXH)

static int W, H;                       /* поле внутри рамки */
static unsigned char body_x[MAXLEN], body_y[MAXLEN];
static int head, len;                  /* кольцевой буфер: body[head] — голова */
static unsigned char board[MAXH][MAXW];  /* 1 — занято змейкой */
static int dx, dy, food_x, food_y, score, best;
static char outbuf[4096];
static int olen;

static void flush(void) { write(STDOUT, outbuf, olen); olen = 0; }
static void out(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(outbuf + olen, sizeof(outbuf) - olen, fmt, ap);
    va_end(ap);
    olen += n;
    if (olen > (int)sizeof(outbuf) - 200) flush();
}
static void cell(int x, int y, const char *color, char c) {
    out("\x1b[%d;%dH%s%c" ANSI_RESET, Y0 + y + 1, X0 + x + 1, color, c);
}

static void load_best(void) {
    int fd = open("/snake.hi", O_READ);
    if (fd < 0) return;
    char b[16] = {0};
    read(fd, b, sizeof(b) - 1);
    close(fd);
    best = atoi(b);
}

static void save_best(void) {
    int fd = open("/snake.hi", O_WRITE);
    if (fd < 0) return;
    char b[16];
    int n = snprintf(b, sizeof(b), "%d\n", best);
    write(fd, b, n);
    close(fd);
}

static void status(void) {
    out("\x1b[1;1H" ANSI_REVERSE " SNAKE " ANSI_RESET "  score " ANSI_YELLOW ANSI_BOLD "%d" ANSI_RESET
        "   best " ANSI_GREEN "%d" ANSI_RESET "   length %d\x1b[K", score, best, len);
}

static void place_food(void) {
    do { food_x = rand() % W; food_y = rand() % H; } while (board[food_y][food_x]);
    cell(food_x, food_y, ANSI_RED ANSI_BOLD, '*');
}

static void new_game(void) {
    memset(board, 0, sizeof(board));
    out(ANSI_CLEAR ANSI_HIDE);
    /* рамка */
    out("\x1b[%d;1H" ANSI_CYAN "+", Y0);
    for (int i = 0; i < W; i++) out("-");
    out("+");
    for (int y = 0; y < H; y++) out("\x1b[%d;1H|\x1b[%d;%dH|", Y0 + y + 1, Y0 + y + 1, SCREEN_W);
    out("\x1b[%d;1H+", Y0 + H + 1);
    for (int i = 0; i < W; i++) out("-");
    out("+" ANSI_RESET);
    out("\x1b[%d;1H arrows/WASD move   P pause   Q quit", SCREEN_H);   /* < 80 символов */

    len = 4; head = len - 1; score = 0; dx = 1; dy = 0;
    for (int i = 0; i < len; i++) {
        body_x[i] = W / 2 - len + i; body_y[i] = H / 2;
        board[body_y[i]][body_x[i]] = 1;
        cell(body_x[i], body_y[i], ANSI_GREEN, i == head ? '@' : 'o');
    }
    place_food();
    status();
    flush();
}

/* -> false, если игрок нажал Q */
static bool game_over(void) {
    if (score > best) { best = score; save_best(); }
    static const char *box[5] = {
        "                          ",
        "   G A M E   O V E R      ",
        NULL,
        "   R - again    Q - quit  ",
        "                          ",
    };
    int bx = (W - 26) / 2 + X0 + 1, by = (H - 5) / 2 + Y0 + 1;
    for (int i = 0; i < 5; i++) {
        out("\x1b[%d;%dH" ANSI_REVERSE ANSI_RED, by + i, bx);
        if (box[i]) out("%s", box[i]);
        else out("   score %-4d best %-4d   ", score, best);
        out(ANSI_RESET);
    }
    status();
    flush();
    ttymode(TTY_RAW);                     /* теперь можно и подождать клавишу */
    for (;;) {
        int k = readkey();
        if (k == 'q' || k == 'Q') return false;
        if (k == 'r' || k == 'R') { ttymode(TTY_RAW | TTY_NONBLOCK); return true; }
    }
}

int main(void) {
    W = SCREEN_W - 2;
    H = SCREEN_H - 4;
    if (W > MAXW) W = MAXW;
    if (H > MAXH) H = MAXH;
    srand(ticks() * 2654435761u);
    load_best();
    ttymode(TTY_RAW | TTY_NONBLOCK);
    new_game();
    bool paused = false;
    for (;;) {
        /* клавиши: применяем первый допустимый поворот за такт */
        int ndx = dx, ndy = dy, k;
        while ((k = readkey()) >= 0) {
            if (k == 'q' || k == 'Q') goto quit;
            if (k == 'p' || k == 'P') paused = !paused;
            int tx = 0, ty = 0;
            if (k == KEY_UP || k == 'w' || k == 'W')    ty = -1;
            if (k == KEY_DOWN || k == 's' || k == 'S')  ty = 1;
            if (k == KEY_LEFT || k == 'a' || k == 'A')  tx = -1;
            if (k == KEY_RIGHT || k == 'd' || k == 'D') tx = 1;
            if ((tx || ty) && ndx == dx && ndy == dy && !(tx == -dx && ty == -dy)) { ndx = tx; ndy = ty; }
        }
        if (paused) { sleep_ms(50); continue; }
        dx = ndx; dy = ndy;

        int nx = body_x[head] + dx, ny = body_y[head] + dy;
        int tail = (head - len + 1 + MAXLEN) % MAXLEN;
        bool eat = nx == food_x && ny == food_y;
        /* хвост уйдёт с клетки в этом же такте — в неё можно наступить */
        bool hit = nx < 0 || ny < 0 || nx >= W || ny >= H ||
                   (board[ny][nx] && !(!eat && nx == body_x[tail] && ny == body_y[tail]));
        if (hit) {
            if (!game_over()) goto quit;
            new_game();
            continue;
        }
        if (!eat) {                                   /* хвост сдвигается */
            board[body_y[tail]][body_x[tail]] = 0;
            cell(body_x[tail], body_y[tail], "", ' ');
        } else {
            len++;
            score += 10;
        }
        cell(body_x[head], body_y[head], ANSI_GREEN, 'o');
        head = (head + 1) % MAXLEN;
        body_x[head] = nx; body_y[head] = ny;
        board[ny][nx] = 1;
        cell(nx, ny, ANSI_GREEN ANSI_BOLD, '@');
        if (eat) { place_food(); status(); }
        out("\x1b[%d;1H", SCREEN_H);                  /* курсор прячем в угол */
        flush();

        int delay = 110 - score / 5;                  /* чем длиннее — тем быстрее */
        sleep_ms(delay < 45 ? 45 : delay);
    }
quit:
    out(ANSI_RESET ANSI_CLEAR ANSI_SHOW);
    flush();
    ttymode(0);
    printf("Bye! Score %d, best %d\n", score, best);
    return 0;
}
