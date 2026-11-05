/* terminal — окно с оболочкой sh на псевдотерминале.
 *
 * Как устроено: ядро даёт пару pty (pty_open): ведомую сторону получает sh как
 * stdin/stdout/stderr — для него это обычная консоль (строка с эхом, сырой
 * режим, termsize, Ctrl+C), а ведущая — у нас. Нажатые клавиши пишем в неё
 * байтами, как xterm (стрелка вверх = ESC [ A, русская буква — UTF-8), а всё,
 * что программы печатают, читаем оттуда и рисуем, понимая escape-
 * последовательности ANSI (цвета, курсор, очистка, область прокрутки). Поэтому
 * полноэкранные программы (edit, top, snake) работают прямо в окне.
 *
 * Чтение из pty блокирует — его делает процесс-помощник и пересылает нам
 * сообщениями (они приходят тем же путём, что и события окна).
 *
 * Окно можно растягивать и разворачивать: число строк и столбцов меняется,
 * и мы сообщаем новый размер ядру (ttyctl) — программы узнают его из termsize. */
#include "win.h"

#define CW 8                                /* клетка: Ubuntu Mono 8x16 */
#define CH 16
#define PAD 4
#define MSG_DATA 1000
#define MSG_EOF  1001

struct cell { int ch; color_t fg, bg; };

static window *win;
static struct cell *grid;                   /* rows * cols */
static bool *row_dirty;
static int  cols, rows;
static int  cx, cy;                         /* курсор */
static int  saved_cx, saved_cy;
static bool cur_visible = true, focused = true;
static int  last_cur_row = -1;
static color_t cur_fg, cur_bg;
static bool bold, inverse;
static int  top_m, bot_m;                   /* область прокрутки (строки, включительно) */
static int  master = -1, sh_pid, reader_pid;

/* палитра Tango, как в терминале Ubuntu; фон — фирменный баклажановый */
static const color_t ansi[8] = {
    RGB(46, 52, 54), RGB(204, 0, 0), RGB(78, 154, 6), RGB(196, 160, 0),
    RGB(52, 101, 164), RGB(117, 80, 123), RGB(6, 152, 154), RGB(211, 215, 207),
};
static const color_t ansi_bright[8] = {
    RGB(85, 87, 83), RGB(239, 41, 41), RGB(138, 226, 52), RGB(252, 233, 79),
    RGB(114, 159, 207), RGB(173, 127, 168), RGB(52, 226, 226), RGB(238, 238, 236),
};
#define DEF_FG RGB(238, 238, 236)
#define DEF_BG RGB(48, 10, 36)

static struct cell *at(int r, int c) { return &grid[r * cols + c]; }
static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

/* ------------------------------------------------------------ экран */

static void clear_cells(int r, int c0, int c1) {
    for (int c = c0; c < c1; c++) { struct cell *p = at(r, c); p->ch = ' '; p->fg = cur_fg; p->bg = cur_bg; }
    row_dirty[r] = true;
}

static void clear_row(int r) { clear_cells(r, 0, cols); }

static void scroll_region(int top, int bot, int n) {       /* n > 0 — вверх, n < 0 — вниз */
    if (n > 0) {
        for (int r = top; r <= bot; r++) {
            if (r + n <= bot) memcpy(at(r, 0), at(r + n, 0), cols * sizeof(struct cell));
            else clear_row(r);
            row_dirty[r] = true;
        }
    } else if (n < 0) {
        n = -n;
        for (int r = bot; r >= top; r--) {
            if (r - n >= top) memcpy(at(r, 0), at(r - n, 0), cols * sizeof(struct cell));
            else clear_row(r);
            row_dirty[r] = true;
        }
    }
}

static bool wrap_pending;                   /* напечатали в последний столбец */

static void newline(void) {
    row_dirty[cy] = true;
    cx = 0;
    if (cy == bot_m) scroll_region(top_m, bot_m, 1);
    else if (cy < rows - 1) cy++;
}

static void put_cp(int ch) {
    if (wrap_pending) { newline(); wrap_pending = false; }
    struct cell *p = at(cy, cx);
    p->ch = ch;
    p->fg = inverse ? cur_bg : cur_fg;
    p->bg = inverse ? cur_fg : cur_bg;
    row_dirty[cy] = true;
    if (cx == cols - 1) wrap_pending = true;
    else cx++;
}

static void control(unsigned char c) {
    switch (c) {
    case '\n': newline(); wrap_pending = false; break;     /* как консоль MyOS: \n — ещё и в начало строки */
    case '\r': cx = 0; wrap_pending = false; break;
    case '\b': if (cx > 0) cx--; wrap_pending = false; row_dirty[cy] = true; break;
    case '\t': do put_cp(' '); while (cx % 8 && !wrap_pending); break;
    case '\f':                                              /* clear */
        for (int r = 0; r < rows; r++) clear_row(r);
        cx = cy = 0; wrap_pending = false;
        break;
    }
}

/* ------------------------------------------------------------ ESC [ ... */

static int esc_state;                       /* 0 текст, 1 после ESC, 2 внутри ESC[ */
static int esc_params[16], esc_n;
static bool esc_private;
static int utf_need, utf_cp;                /* недособранная буква UTF-8 */

static void apply_sgr(void) {
    if (esc_n == 0) esc_params[esc_n++] = 0;
    for (int i = 0; i < esc_n; i++) {
        int p = esc_params[i];
        if (p == 0) { cur_fg = DEF_FG; cur_bg = DEF_BG; bold = inverse = false; }
        else if (p == 1) bold = true;
        else if (p == 22) bold = false;
        else if (p == 7) inverse = true;
        else if (p == 27) inverse = false;
        else if (p >= 30 && p <= 37) cur_fg = bold ? ansi_bright[p - 30] : ansi[p - 30];
        else if (p >= 90 && p <= 97) cur_fg = ansi_bright[p - 90];
        else if (p >= 40 && p <= 47) cur_bg = ansi[p - 40];
        else if (p >= 100 && p <= 107) cur_bg = ansi_bright[p - 100];
        else if (p == 39) cur_fg = DEF_FG;
        else if (p == 49) cur_bg = DEF_BG;
    }
}

static void esc_final(char c) {
    int p0 = esc_n > 0 ? esc_params[0] : 0, n = p0 ? p0 : 1;
    row_dirty[cy] = true;
    wrap_pending = false;
    switch (c) {
    case 'm': apply_sgr(); break;
    case 'J':
        if (p0 == 0) { clear_cells(cy, cx, cols); for (int r = cy + 1; r < rows; r++) clear_row(r); }
        else if (p0 == 1) { clear_cells(cy, 0, cx + 1); for (int r = 0; r < cy; r++) clear_row(r); }
        else for (int r = 0; r < rows; r++) clear_row(r);
        break;
    case 'K':
        if (p0 == 0) clear_cells(cy, cx, cols);
        else if (p0 == 1) clear_cells(cy, 0, cx + 1);
        else clear_row(cy);
        break;
    case 'H': case 'f':
        cy = clampi((esc_n > 0 && esc_params[0] ? esc_params[0] : 1) - 1, 0, rows - 1);
        cx = clampi((esc_n > 1 && esc_params[1] ? esc_params[1] : 1) - 1, 0, cols - 1);
        break;
    case 'A': cy = clampi(cy - n, 0, rows - 1); break;
    case 'B': cy = clampi(cy + n, 0, rows - 1); break;
    case 'C': cx = clampi(cx + n, 0, cols - 1); break;
    case 'D': cx = clampi(cx - n, 0, cols - 1); break;
    case 'G': cx = clampi(n - 1, 0, cols - 1); break;
    case 'd': cy = clampi(n - 1, 0, rows - 1); break;
    case 'L': if (cy >= top_m && cy <= bot_m) scroll_region(cy, bot_m, -n); break;   /* вставить строки */
    case 'M': if (cy >= top_m && cy <= bot_m) scroll_region(cy, bot_m, n); break;    /* удалить строки */
    case 'S': scroll_region(top_m, bot_m, n); break;
    case 'T': scroll_region(top_m, bot_m, -n); break;
    case '@':                                               /* вставить пробелы */
        for (int c2 = cols - 1; c2 >= cx + n; c2--) *at(cy, c2) = *at(cy, c2 - n);
        clear_cells(cy, cx, cx + n < cols ? cx + n : cols);
        break;
    case 'P':                                               /* удалить символы */
        for (int c2 = cx; c2 < cols; c2++) {
            if (c2 + n < cols) *at(cy, c2) = *at(cy, c2 + n);
            else clear_cells(cy, c2, c2 + 1);
        }
        break;
    case 'X': clear_cells(cy, cx, cx + n < cols ? cx + n : cols); break;
    case 'r':
        top_m = clampi((esc_n > 0 && esc_params[0] ? esc_params[0] : 1) - 1, 0, rows - 1);
        bot_m = clampi((esc_n > 1 && esc_params[1] ? esc_params[1] : rows) - 1, 0, rows - 1);
        if (bot_m <= top_m) { top_m = 0; bot_m = rows - 1; }
        cx = cy = 0;
        break;
    case 's': saved_cx = cx; saved_cy = cy; break;
    case 'u': cx = clampi(saved_cx, 0, cols - 1); cy = clampi(saved_cy, 0, rows - 1); break;
    case 'h': case 'l':
        if (esc_private && p0 == 25) cur_visible = c == 'h';
        break;
    }
    row_dirty[cy] = true;
}

static void term_write(const char *s, int n) {
    for (int i = 0; i < n; i++) {
        unsigned char c = s[i];
        if (esc_state == 1) {
            if (c == '[') { esc_state = 2; esc_n = 0; esc_private = false; memset(esc_params, 0, sizeof(esc_params)); }
            else {
                if (c == '7') { saved_cx = cx; saved_cy = cy; }
                else if (c == '8') { cx = clampi(saved_cx, 0, cols - 1); cy = clampi(saved_cy, 0, rows - 1); row_dirty[cy] = true; }
                esc_state = 0;
            }
            continue;
        }
        if (esc_state == 2) {
            if (c >= '0' && c <= '9') {
                if (esc_n == 0) esc_n = 1;
                esc_params[esc_n - 1] = esc_params[esc_n - 1] * 10 + (c - '0');
            } else if (c == ';') {
                if (esc_n == 0) esc_n = 1;
                if (esc_n < 16) esc_params[esc_n++] = 0;
            } else if (c == '?' || c == '>' || c == '=') {
                esc_private = true;
            } else if (c >= 0x40 && c <= 0x7E) {
                esc_final(c);
                esc_state = 0;
            } else if (c < 32) control(c);
            continue;
        }
        /* обычный текст: UTF-8 собираем в букву; одиночный байт >= 0x80 — символ CP437 */
        if (utf_need) {
            if ((c & 0xC0) == 0x80) {
                utf_cp = utf_cp << 6 | (c & 0x3F);
                if (--utf_need == 0) put_cp(utf_cp);
                continue;
            }
            utf_need = 0;
        }
        if (c == 27) { esc_state = 1; continue; }
        if (c < 32) { control(c); continue; }
        if (c == 127) continue;
        if (c >= 0xC0 && c < 0xF0) { utf_need = c >= 0xE0 ? 2 : 1; utf_cp = c & (c >= 0xE0 ? 0x0F : 0x1F); continue; }
        if (c >= 0x80) { put_cp(0x10000 | c); continue; }
        put_cp(c);
    }
}

/* ------------------------------------------------------------ рисование */

static void draw_cell(int r, int c, bool cursor) {
    struct cell *p = at(r, c);
    int x = PAD + c * CW, y = PAD + r * CH;
    color_t fg = p->fg, bg = p->bg;
    if (cursor && focused) { color_t t = fg; fg = bg; bg = t; if (fg == bg) { fg = DEF_BG; bg = DEF_FG; } }
    char s[4];
    int n;
    if (p->ch & 0x10000) { s[0] = (char)(p->ch & 0xFF); n = 1; }
    else n = utf8_put(s, p->ch);
    s[n] = 0;
    gfx_text(x, y, s, fg, bg);
    if (cursor && !focused) gfx_rect(x, y, CW, CH, DEF_FG);    /* в неактивном окне — рамкой */
}

static void draw_rows(void) {
    if (last_cur_row >= 0 && last_cur_row < rows) row_dirty[last_cur_row] = true;
    if (cur_visible) row_dirty[cy] = true;
    int first = -1, last = -1;
    for (int r = 0; r < rows; r++) {
        if (!row_dirty[r]) continue;
        row_dirty[r] = false;
        if (first < 0) first = r;
        last = r;
        for (int c = 0; c < cols; c++) draw_cell(r, c, cur_visible && r == cy && c == cx);
    }
    last_cur_row = cur_visible ? cy : -1;
    if (first >= 0) win_update_rect(win, 0, PAD + first * CH, win->w, (last - first + 1) * CH);
}

/* окно нового размера: сетка заново, содержимое — сколько влезет (курсор остаётся виден) */
static void set_grid(int nc, int nr) {
    if (nc < 10) nc = 10;
    if (nr < 3) nr = 3;
    struct cell *g = malloc(nc * nr * sizeof(struct cell));
    bool *d = malloc(nr);
    if (!g || !d) { free(g); free(d); return; }
    int shift = 0;                                          /* сколько верхних строк уйдёт */
    if (grid && cy >= nr) shift = cy - nr + 1;
    for (int r = 0; r < nr; r++) {
        d[r] = true;
        for (int c = 0; c < nc; c++) {
            struct cell *p = &g[r * nc + c];
            int orow = r + shift;
            if (grid && orow < rows && c < cols) *p = grid[orow * cols + c];
            else { p->ch = ' '; p->fg = DEF_FG; p->bg = DEF_BG; }
        }
    }
    free(grid); free(row_dirty);
    grid = g; row_dirty = d;
    cols = nc; rows = nr;
    cy -= shift;
    cx = clampi(cx, 0, cols - 1);
    cy = clampi(cy, 0, rows - 1);
    top_m = 0; bot_m = rows - 1;
    wrap_pending = false;
    last_cur_row = -1;
    if (master >= 0) ttyctl(master, TTYCTL_SETSIZE, cols << 16 | rows);
}

static void redraw_all(void) {
    gfx_clear(DEF_BG);
    for (int r = 0; r < rows; r++) row_dirty[r] = true;
    draw_rows();
    win_update(win);
}

/* ------------------------------------------------------------ клавиши -> байты в pty */

static void send_bytes(const char *s, int n) { if (master >= 0) write(master, s, n); }
static void send_str(const char *s) { send_bytes(s, strlen(s)); }

static void on_key(int k) {
    switch (k) {
    case KEY_UP:    send_str("\x1b[A"); return;
    case KEY_DOWN:  send_str("\x1b[B"); return;
    case KEY_RIGHT: send_str("\x1b[C"); return;
    case KEY_LEFT:  send_str("\x1b[D"); return;
    case KEY_HOME:  send_str("\x1b[H"); return;
    case KEY_END:   send_str("\x1b[F"); return;
    case KEY_PGUP:  send_str("\x1b[5~"); return;
    case KEY_PGDN:  send_str("\x1b[6~"); return;
    case KEY_DEL:   send_str("\x1b[3~"); return;
    case KEY_INS:   send_str("\x1b[2~"); return;
    }
    if (k == '\n') k = '\r';                                /* Enter, как у настоящего терминала */
    char u[4];
    if (k > 0 && k < 0x10000) send_bytes(u, utf8_put(u, k));
}

/* ------------------------------------------------------------ запуск sh */

static bool start_shell(void) {
    int p[2];
    if (pty_open(p) < 0) return false;
    master = p[0];
    int slave = p[1];
    ttyctl(master, TTYCTL_SETSIZE, cols << 16 | rows);
    int me = getpid();
    sh_pid = fork();
    if (sh_pid == 0) {
        dup2(slave, 0);
        dup2(slave, 1);
        dup2(slave, 2);
        close(slave); close(master);
        setpgid(0, 0);
        char *argv[] = { "sh", NULL };
        exec("/bin/sh", argv);
        exit(127);
    }
    reader_pid = fork();
    if (reader_pid == 0) {                                  /* помощник: pty -> сообщения */
        close(slave);
        struct msg m;
        for (;;) {
            memset(&m, 0, sizeof(m));
            int n = read(master, m.data, sizeof(m.data));
            m.type = n > 0 ? MSG_DATA : MSG_EOF;
            m.a = n;
            while (msg_send(me, &m) == -EAGAIN) sleep_ms(5);   /* ящик полон — подождём */
            if (n <= 0) exit(0);
        }
    }
    close(slave);                                           /* у нас — только ведущая сторона */
    return sh_pid > 0 && reader_pid > 0;
}

/* sh и всё, что он запустил (дети, внуки) */
static void kill_tree(int root) {
    struct proc_info pi[64];
    int n = ps(pi, 64);
    int list[64], nl = 0;
    list[nl++] = root;
    for (int i = 0; i < nl; i++)
        for (int j = 0; j < n && nl < 64; j++)
            if ((int)pi[j].ppid == list[i] && (int)pi[j].pid != reader_pid) list[nl++] = pi[j].pid;
    for (int i = nl - 1; i >= 0; i--) kill(list[i]);
}

static bool handle(struct event *e) {
    switch (e->type) {
    case EV_CLOSE: return false;
    case EV_KEY: on_key(e->key); break;
    case EV_FOCUS: focused = e->key != 0; row_dirty[cy] = true; break;
    case EV_RESIZE:
        set_grid((e->x - 2 * PAD) / CW, (e->y - 2 * PAD) / CH);
        redraw_all();
        break;
    case EV_MESSAGE:
        if (e->msg.type == MSG_DATA) term_write(e->msg.data, e->msg.a);
        else if (e->msg.type == MSG_EOF) return false;      /* sh завершился (exit) */
        break;
    }
    return true;
}

int main(void) {
    cols = 80; rows = 25;
    win = win_create("Terminal", cols * CW + 2 * PAD, rows * CH + 2 * PAD);
    if (!win) { dprintf(STDERR, "terminal: the desktop is not running\n"); return 1; }
    win_set_resizable(win, 20 * CW + 2 * PAD, 5 * CH + 2 * PAD);
    cur_fg = DEF_FG; cur_bg = DEF_BG;
    set_grid(cols, rows);
    if (!start_shell()) term_write("cannot start sh\n", 16);
    redraw_all();

    struct event e;
    bool run = true;
    while (run && win_event(&e, -1)) {
        run = handle(&e);
        /* всё, что уже пришло (поток вывода), — разобрать и нарисовать одним кадром */
        for (int i = 0; run && i < 200; i++) {
            if (!win_event(&e, 0)) { run = false; break; }
            if (e.type == EV_TIMEOUT) break;
            run = handle(&e);
        }
        draw_rows();
    }
    kill_tree(sh_pid);
    kill(reader_pid);
    if (master >= 0) close(master);
    win_destroy(win);
    return 0;
}
