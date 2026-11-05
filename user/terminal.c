/* terminal — окно с оболочкой sh.
 *
 * Как устроено: запускаем «sh -t», его ввод и вывод — каналы (pipe).
 * Строку, которую печатает пользователь, редактируем сами и по Enter
 * отправляем в sh. А чтобы не ждать вывод sh (read из канала блокирует),
 * его читает отдельный процесс-помощник и пересылает нам сообщениями —
 * они приходят тем же путём, что и события окна.
 *
 * Полноэкранные программы (edit, snake, top, mandel) здесь не работают —
 * им нужна настоящая консоль: запускай их, выйдя из рабочего стола. */
#include "win.h"

#define COLS 80
#define ROWS 25
#define PAD 4
#define MSG_DATA 1000
#define MSG_EOF  1001

static window *win;
static char  ch[ROWS][COLS];
static color_t fgc[ROWS][COLS], bgc[ROWS][COLS];
static bool  row_dirty[ROWS];
static int   cx, cy;                        /* курсор */
static color_t cur_fg, cur_bg;
static bool  bold;
static int   to_sh = -1, sh_pid, reader_pid;

static const color_t ansi[8] = {
    RGB(12, 12, 12), RGB(197, 15, 31), RGB(19, 161, 14), RGB(193, 156, 0),
    RGB(59, 120, 255), RGB(136, 23, 152), RGB(58, 150, 221), RGB(204, 204, 204),
};
static const color_t ansi_bright[8] = {
    RGB(118, 118, 118), RGB(231, 72, 86), RGB(22, 198, 12), RGB(249, 241, 165),
    RGB(59, 120, 255), RGB(180, 0, 158), RGB(97, 214, 214), RGB(242, 242, 242),
};
#define DEF_FG RGB(204, 204, 204)
#define DEF_BG RGB(12, 12, 20)

/* ------------------------------------------------------------ экран */

static void clear_row(int r) {
    for (int c = 0; c < COLS; c++) { ch[r][c] = ' '; fgc[r][c] = DEF_FG; bgc[r][c] = DEF_BG; }
    row_dirty[r] = true;
}

static void scroll_up(void) {
    for (int r = 0; r < ROWS - 1; r++) {
        memcpy(ch[r], ch[r + 1], COLS);
        memcpy(fgc[r], fgc[r + 1], sizeof(fgc[r]));
        memcpy(bgc[r], bgc[r + 1], sizeof(bgc[r]));
        row_dirty[r] = true;
    }
    clear_row(ROWS - 1);
}

static void newline(void) {
    row_dirty[cy] = true;
    cx = 0;
    if (++cy == ROWS) { scroll_up(); cy = ROWS - 1; }
}

static void put_char(char c) {
    if (c == '\n') { newline(); return; }
    if (c == '\r') { cx = 0; return; }
    if (c == '\b') { if (cx > 0) cx--; row_dirty[cy] = true; return; }
    if (c == '\t') { do put_char(' '); while (cx % 8); return; }
    if (c == '\f') { for (int r = 0; r < ROWS; r++) clear_row(r); cx = cy = 0; return; }   /* clear */
    if ((unsigned char)c < 32) return;
    if (cx == COLS) newline();
    ch[cy][cx] = c;
    fgc[cy][cx] = cur_fg;
    bgc[cy][cx] = cur_bg;
    row_dirty[cy] = true;
    cx++;
}

/* ESC [ ... — цвета, очистка экрана, курсор */
static int esc_state;                       /* 0 обычный текст, 1 после ESC, 2 внутри [ ... */
static int esc_params[8], esc_n;

static void apply_sgr(void) {
    if (esc_n == 0) esc_params[esc_n++] = 0;
    for (int i = 0; i < esc_n; i++) {
        int p = esc_params[i];
        if (p == 0) { cur_fg = DEF_FG; cur_bg = DEF_BG; bold = false; }
        else if (p == 1) bold = true;
        else if (p == 7) { color_t t = cur_fg; cur_fg = cur_bg; cur_bg = t; }
        else if (p >= 30 && p <= 37) cur_fg = bold ? ansi_bright[p - 30] : ansi[p - 30];
        else if (p >= 90 && p <= 97) cur_fg = ansi_bright[p - 90];
        else if (p >= 40 && p <= 47) cur_bg = ansi[p - 40];
        else if (p == 39) cur_fg = DEF_FG;
        else if (p == 49) cur_bg = DEF_BG;
    }
}

static void esc_final(char c) {
    int p0 = esc_n > 0 ? esc_params[0] : 0;
    switch (c) {
    case 'm': apply_sgr(); break;
    case 'J': for (int r = 0; r < ROWS; r++) clear_row(r); cx = cy = 0; break;
    case 'K': for (int c2 = cx; c2 < COLS; c2++) { ch[cy][c2] = ' '; bgc[cy][c2] = DEF_BG; } row_dirty[cy] = true; break;
    case 'H': {
        int r = esc_n > 0 ? esc_params[0] - 1 : 0, c2 = esc_n > 1 ? esc_params[1] - 1 : 0;
        row_dirty[cy] = true;
        cy = r < 0 ? 0 : r >= ROWS ? ROWS - 1 : r;
        cx = c2 < 0 ? 0 : c2 >= COLS ? COLS - 1 : c2;
        row_dirty[cy] = true;
        break;
    }
    case 'A': cy -= p0 ? p0 : 1; if (cy < 0) cy = 0; break;
    case 'B': cy += p0 ? p0 : 1; if (cy >= ROWS) cy = ROWS - 1; break;
    case 'C': cx += p0 ? p0 : 1; if (cx >= COLS) cx = COLS - 1; break;
    case 'D': cx -= p0 ? p0 : 1; if (cx < 0) cx = 0; break;
    }
    row_dirty[cy] = true;
}

static void term_write(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (esc_state == 0) {
            if (c == 27) { esc_state = 1; continue; }
            put_char(c);
        } else if (esc_state == 1) {
            if (c == '[') { esc_state = 2; esc_n = 0; esc_params[0] = 0; }
            else esc_state = 0;
        } else {
            if (c >= '0' && c <= '9') {
                if (esc_n == 0) esc_n = 1;
                esc_params[esc_n - 1] = esc_params[esc_n - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (esc_n == 0) esc_n = 1;
                if (esc_n < 8) esc_params[esc_n++] = 0;
            } else if (c == '?') {
            } else {
                esc_final(c);
                esc_state = 0;
            }
        }
    }
}

static void draw_rows(void) {
    int first = -1, last = -1;
    for (int r = 0; r < ROWS; r++) {
        if (!row_dirty[r]) continue;
        row_dirty[r] = false;
        if (first < 0) first = r;
        last = r;
        int y = PAD + r * 16;
        for (int c = 0; c < COLS; c++) {
            char s[2] = { ch[r][c], 0 };
            gfx_text(PAD + c * 8, y, s, fgc[r][c], bgc[r][c]);
        }
        if (r == cy && cx < COLS) gfx_fill_rect(PAD + cx * 8, y + 13, 8, 3, RGB(230, 230, 230));
    }
    if (first >= 0) win_update_rect(win, 0, PAD + first * 16, win->w, (last - first + 1) * 16);
}

/* ------------------------------------------------------------ строка ввода */

static char line[256];
static int  line_len;
static char history[16][256];
static int  n_hist, hist_pos;

static void erase_typed(void) {
    for (int i = 0; i < line_len; i++) {
        if (cx == 0 && cy > 0) { cy--; cx = COLS; }
        cx--;
        ch[cy][cx] = ' ';
        row_dirty[cy] = true;
    }
}

static void set_line(const char *s) {
    erase_typed();
    line_len = 0;
    for (; *s && line_len < 255; s++) { line[line_len++] = *s; put_char(*s); }
}

/* Ctrl+C: остановить то, что запустил sh (его детей), сам sh не трогаем */
static void interrupt_children(void) {
    struct proc_info pi[64];
    int n = ps(pi, 64);
    for (int i = 0; i < n; i++)
        if ((int)pi[i].ppid == sh_pid) kill(pi[i].pid);
    term_write("^C\n", 3);
}

static void on_key(int k) {
    if (k == '\n' || k == '\r') {
        line[line_len] = '\n';
        put_char('\n');
        if (to_sh >= 0) write(to_sh, line, line_len + 1);
        line[line_len] = 0;
        if (line_len && (n_hist == 0 || strcmp(history[(n_hist - 1) % 16], line))) {
            strcpy(history[n_hist % 16], line);
            n_hist++;
        }
        hist_pos = n_hist;
        line_len = 0;
    } else if (k == 8 || k == 127) {
        if (line_len) { line_len--; put_char('\b'); ch[cy][cx] = ' '; row_dirty[cy] = true; }
    } else if (k == CTRL('c')) {
        line_len = 0;
        interrupt_children();
    } else if (k == CTRL('l')) {
        for (int r = 0; r < ROWS; r++) clear_row(r);
        cx = cy = 0;
        if (to_sh >= 0) write(to_sh, "\n", 1);                   /* sh напечатает приглашение заново */
    } else if (k == CTRL('d')) {
        if (to_sh >= 0) { close(to_sh); to_sh = -1; }
    } else if (k == KEY_UP) {
        if (hist_pos > 0 && hist_pos > n_hist - 16) { hist_pos--; set_line(history[hist_pos % 16]); }
    } else if (k == KEY_DOWN) {
        if (hist_pos < n_hist) { hist_pos++; set_line(hist_pos == n_hist ? "" : history[hist_pos % 16]); }
    } else if (k >= 32 && k < 127 && line_len < 254) {
        line[line_len++] = k;
        put_char(k);
    }
}

/* ------------------------------------------------------------ запуск sh */

static bool start_shell(void) {
    int in[2], out[2];
    if (pipe(in) < 0 || pipe(out) < 0) return false;
    int me = getpid();
    sh_pid = fork();
    if (sh_pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        char *argv[] = { "sh", "-t", NULL };
        exec("/bin/sh", argv);
        exit(127);
    }
    reader_pid = fork();
    if (reader_pid == 0) {                                     /* помощник: канал -> сообщения */
        close(in[0]); close(in[1]); close(out[1]);
        struct msg m;
        for (;;) {
            memset(&m, 0, sizeof(m));
            int n = read(out[0], m.data, sizeof(m.data));
            m.type = n > 0 ? MSG_DATA : MSG_EOF;
            m.a = n;
            while (msg_send(me, &m) == -EAGAIN) sleep_ms(5);  /* ящик полон — подождём */
            if (n <= 0) exit(0);
        }
    }
    close(in[0]); close(out[0]); close(out[1]);
    to_sh = in[1];
    return sh_pid > 0 && reader_pid > 0;
}

int main(void) {
    win = win_create("Terminal", COLS * 8 + 2 * PAD, ROWS * 16 + 2 * PAD);
    if (!win) { dprintf(STDERR, "terminal: the desktop is not running\n"); return 1; }
    cur_fg = DEF_FG; cur_bg = DEF_BG;
    gfx_clear(DEF_BG);
    for (int r = 0; r < ROWS; r++) clear_row(r);
    if (!start_shell()) term_write("cannot start sh\n", 16);
    draw_rows();
    win_update(win);

    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_KEY) on_key(e.key);
        if (e.type == EV_MESSAGE) {
            if (e.msg.type == MSG_DATA) term_write(e.msg.data, e.msg.a);
            else if (e.msg.type == MSG_EOF) break;               /* sh завершился (exit) */
        }
        draw_rows();
    }
    if (to_sh >= 0) close(to_sh);
    interrupt_children();
    kill(sh_pid);
    kill(reader_pid);
    win_destroy(win);
    return 0;
}
