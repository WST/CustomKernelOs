/* ncurses.c — экран nano в памяти, в терминал уходят только изменившиеся куски.
 * libc tcsetattr не ставит TTY_NONBLOCK и TTY_RESIZE_EVENT: без них wgetch
 * заблокируется и не заметит смену размера. Режим ставится через ttymode. */
#include "myos.h"
#undef KEY_RESIZE
#include "curses.h"
#include "string.h"
#include "stdarg.h"

WINDOW *stdscr, *curscr;
int LINES, COLS;
static struct cell *scene, *shown;
static int saved_mode, have_mode, saved_oflags, have_oflags;
static bool ended = true, initialized;
static int cursor_y, cursor_x, cursor_visible = 1, half_timeout = -1, pushed = -1;
static char output[8192];
static unsigned output_size;
static void (*resize_hook)(void);

void ncurses_set_resize_hook(void (*fn)(void)) { resize_hook = fn; }

static int utf8_get(unsigned *wc, const char *s, unsigned n) {
    unsigned c, len, min, i;
    if (!s || !n) return 0;
    c = (unsigned char)s[0];
    if (c < 128) { *wc = c; return c ? 1 : 0; }
    if (c >= 0xc2 && c <= 0xdf) { len = 2; c &= 31; min = 0x80; }
    else if (c >= 0xe0 && c <= 0xef) { len = 3; c &= 15; min = 0x800; }
    else if (c >= 0xf0 && c <= 0xf4) { len = 4; c &= 7; min = 0x10000; }
    else return -1;
    if (n < len) return -1;
    for (i = 1; i < len; i++) {
        unsigned ch = (unsigned char)s[i];
        if ((ch & 0xc0) != 0x80) return -1;
        c = (c << 6) | (ch & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return -1;
    *wc = c;
    return (int)len;
}

static int nc_utf8_put(char *s, unsigned c) {
    if (c < 128) { s[0] = (char)c; return 1; }
    if (c < 0x800) { s[0] = (char)(0xc0 | (c >> 6)); s[1] = (char)(0x80 | (c & 63)); return 2; }
    if (c >= 0xd800 && c <= 0xdfff) return -1;
    if (c < 0x10000) {
        s[0] = (char)(0xe0 | (c >> 12));
        s[1] = (char)(0x80 | ((c >> 6) & 63));
        s[2] = (char)(0x80 | (c & 63));
        return 3;
    }
    if (c > 0x10ffff) return -1;
    s[0] = (char)(0xf0 | (c >> 18));
    s[1] = (char)(0x80 | ((c >> 12) & 63));
    s[2] = (char)(0x80 | ((c >> 6) & 63));
    s[3] = (char)(0x80 | (c & 63));
    return 4;
}

static int utf8_width(unsigned c) {
    if (!c) return 0;
    if (c < 32 || (c >= 127 && c < 160)) return -1;
    if ((c >= 0x300 && c <= 0x36f) || (c >= 0x1ab0 && c <= 0x1aff) || (c >= 0xfe00 && c <= 0xfe0f)) return 0;
    if (c >= 0x1100 && ((c <= 0x115f) || (c >= 0x2e80 && c <= 0xa4cf) || (c >= 0xac00 && c <= 0xd7a3) ||
                       (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff01 && c <= 0xff60) || (c >= 0x1f300 && c <= 0x1faff)))
        return 2;
    return 1;
}

static void emit_flush(void) {
    unsigned done = 0;
    while (done < output_size) {
        int n = write(1, output + done, output_size - done);
        if (n <= 0) break;
        done += (unsigned)n;
    }
    output_size = 0;
}

static void emit(const char *s, unsigned n) {
    while (n) {
        unsigned room = (unsigned)sizeof output - output_size, k = n < room ? n : room;
        if (!room) { emit_flush(); continue; }
        memcpy(output + output_size, s, k);
        output_size += k;
        s += k;
        n -= k;
    }
}

static void text(const char *s) { emit(s, (unsigned)strlen(s)); }

static void position(int y, int x) {
    char seq[48];
    int n = snprintf(seq, sizeof seq, "\033[%d;%dH", y + 1, x + 1);
    if (n > 0) emit(seq, (unsigned)n);
}

static void dimensions(int *h, int *w) {
    int value = ttyctl(0, TTYCTL_GETSIZE, 0);
    *w = value < 0 ? term_cols() : (value >> 16) & 65535;
    *h = value < 0 ? term_rows() : value & 65535;
    if (!*w) *w = 80;
    if (!*h) *h = 24;
    if (*w < 2) *w = 2;
    if (*h < 2) *h = 2;
    if (*w > 1000) *w = 1000;
    if (*h > 1000) *h = 1000;
}

static void cell_set(struct cell *cell, unsigned ch, unsigned attr) { cell->ch = ch; cell->attr = attr; }

static void blank(struct cell *cells, int count, unsigned attr) {
    int i;
    for (i = 0; i < count; i++) { cells[i].ch = ' '; cells[i].attr = attr; }
}

static int screen_size(void) {
    int h, w;
    struct cell *next, *old;
    dimensions(&h, &w);
    if (scene && h == LINES && w == COLS) return 0;
    next = malloc((size_t)h * (size_t)w * sizeof *next);
    old = malloc((size_t)h * (size_t)w * sizeof *old);
    if (!next || !old) { free(next); free(old); return ERR; }
    free(scene);
    free(shown);
    scene = next;
    shown = old;
    LINES = h;
    COLS = w;
    blank(scene, h * w, 0);
    memset(shown, 255, (size_t)h * (size_t)w * sizeof *shown);
    if (stdscr) {
        free(stdscr->cells);
        stdscr->cells = calloc((size_t)h * (size_t)w, sizeof *scene);
        stdscr->h = h;
        stdscr->w = w;
        stdscr->cx = stdscr->cy = 0;
        if (!stdscr->cells) return ERR;
        blank(stdscr->cells, h * w, 0);
    }
    return 0;
}

static void enter_raw(void) {
    /* Повторный raw не должен затирать режим, который был до initscr. */
    int prev = ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG | TTY_RESIZE_EVENT);
    if (!have_mode) {
        saved_mode = prev;
        saved_oflags = ttyctl(0, TTYCTL_GETOFLAGS, 0);
        have_oflags = saved_oflags >= 0;
        have_mode = 1;
    }
}

static void leave_raw(void) {
    if (have_mode) ttymode(saved_mode);
    if (have_oflags) ttyctl(0, TTYCTL_SETOFLAGS, saved_oflags);
    have_mode = 0;
}

WINDOW *newwin(int h, int w, int y, int x) {
    WINDOW *win;
    if (!h) h = LINES - y;
    if (!w) w = COLS - x;
    if (h < 1 || w < 1 || h > 1000 || w > 1000) return NULL;
    win = calloc(1, sizeof *win);
    if (!win) return NULL;
    win->cells = malloc((size_t)h * (size_t)w * sizeof *win->cells);
    if (!win->cells) { free(win); return NULL; }
    win->h = h;
    win->w = w;
    win->y = y;
    win->x = x;
    win->delay = -1;
    blank(win->cells, h * w, 0);
    return win;
}

WINDOW *initscr(void) {
    if (!initialized) { enter_raw(); initialized = true; }
    if (screen_size() < 0) return NULL;
    if (!stdscr) stdscr = newwin(LINES, COLS, 0, 0);
    curscr = stdscr;
    if (!stdscr) return NULL;
    ended = false;
    text("\033[?1049h\033[2J\033[H");
    emit_flush();
    return stdscr;
}

int endwin(void) {
    if (!initialized) return OK;
    if (!ended) {
        text("\033[0m\033[?25h\033[?1049l");
        emit_flush();
        leave_raw();
        ended = true;
    }
    return OK;
}

int isendwin(void) { return ended; }

int delwin(WINDOW *w) {
    if (!w || w == stdscr) return ERR;
    free(w->cells);
    free(w);
    return OK;
}

int wmove(WINDOW *w, int y, int x) {
    if (!w || y < 0 || x < 0 || y >= w->h || x >= w->w) return ERR;
    w->cy = y;
    w->cx = x;
    return OK;
}

int waddch(WINDOW *w, chtype value) {
    unsigned c, attr;
    int width;
    if (!w) return ERR;
    c = value & A_CHARTEXT;
    attr = w->attr | (value & ~A_CHARTEXT);
    if (c == '\r') { w->cx = 0; return OK; }
    if (c == '\n') { wclrtoeol(w); w->cx = 0; if (w->cy < w->h - 1) w->cy++; return OK; }
    if (c == '\t') { int n = 8 - w->cx % 8; while (n--) waddch(w, ' '); return OK; }
    if (w->cx >= w->w) { w->cx = 0; w->cy++; }
    if (w->cy >= w->h) { w->cy = w->h - 1; w->cx = w->w - 1; return ERR; }
    cell_set(&w->cells[w->cy * w->w + w->cx], c, attr);
    width = utf8_width(c);
    if (width < 1) width = 1;
    w->cx++;
    if (width == 2 && w->cx < w->w) cell_set(&w->cells[w->cy * w->w + w->cx++], 0, attr);
    return OK;
}

int waddnstr(WINDOW *w, const char *s, int n) {
    int off;
    if (!s) return ERR;
    if (n < 0) n = (int)strlen(s);
    for (off = 0; off < n && s[off];) {
        unsigned c;
        int k = utf8_get(&c, s + off, (unsigned)(n - off));
        if (k <= 0) { c = '?'; k = 1; }
        waddch(w, c);
        off += k;
    }
    return OK;
}

int waddstr(WINDOW *w, const char *s) { return waddnstr(w, s, -1); }
int mvwaddch(WINDOW *w, int y, int x, chtype c) { return wmove(w, y, x) < 0 ? ERR : waddch(w, c); }
int mvwaddnstr(WINDOW *w, int y, int x, const char *s, int n) { return wmove(w, y, x) < 0 ? ERR : waddnstr(w, s, n); }
int mvwaddstr(WINDOW *w, int y, int x, const char *s) { return mvwaddnstr(w, y, x, s, -1); }

static int formatted(WINDOW *w, const char *fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return waddstr(w, buf);
}

int wprintw(WINDOW *w, const char *fmt, ...) {
    va_list ap;
    int r;
    va_start(ap, fmt);
    r = formatted(w, fmt, ap);
    va_end(ap);
    return r;
}

int mvwprintw(WINDOW *w, int y, int x, const char *fmt, ...) {
    va_list ap;
    int r;
    if (wmove(w, y, x) < 0) return ERR;
    va_start(ap, fmt);
    r = formatted(w, fmt, ap);
    va_end(ap);
    return r;
}

int wnoutrefresh(WINDOW *w) {
    int y, x;
    if (!w || !scene) return ERR;
    for (y = 0; y < w->h; y++) {
        int row = w->y + y;
        if (row < 0 || row >= LINES) continue;
        for (x = 0; x < w->w; x++) {
            int col = w->x + x;
            if (col >= 0 && col < COLS) scene[row * COLS + col] = w->cells[y * w->w + x];
        }
    }
    if (!w->leave) {
        cursor_y = w->y + w->cy;
        cursor_x = w->x + w->cx;
        if (cursor_x >= COLS) cursor_x = COLS - 1;
    }
    return OK;
}

int doupdate(void) {
    unsigned attr = ~0u;
    int y;
    if (ended) {
        if (screen_size() < 0) return ERR;
        ended = false;
        text("\033[?1049h\033[2J");
        raw();
    }
    for (y = 0; y < LINES; y++) {
        int start = 0;
        while (start < COLS) {
            int end, x;
            while (start < COLS && !memcmp(scene + y * COLS + start, shown + y * COLS + start, sizeof *scene)) start++;
            if (start == COLS) break;
            end = COLS - 1;
            while (end > start && !memcmp(scene + y * COLS + end, shown + y * COLS + end, sizeof *scene)) end--;
            text("\033[?25l");
            position(y, start);
            for (x = start; x <= end; x++) {
                struct cell cell = scene[y * COLS + x];
                if (attr != cell.attr) {
                    text("\033[0m");
                    if (cell.attr & A_REVERSE) text("\033[7m");
                    if (cell.attr & A_BOLD) text("\033[1m");
                    if (cell.attr & A_ITALIC) text("\033[3m");
                    attr = cell.attr;
                }
                if (cell.ch) {
                    char bytes[4];
                    int n = nc_utf8_put(bytes, cell.ch);
                    if (n < 1) { bytes[0] = '?'; n = 1; }
                    emit(bytes, (unsigned)n);
                }
                shown[y * COLS + x] = cell;
            }
            start = end + 1;
        }
    }
    text("\033[0m");
    position(cursor_y < LINES ? cursor_y : LINES - 1, cursor_x < COLS ? cursor_x : COLS - 1);
    text(cursor_visible ? "\033[?25h" : "\033[?25l");
    emit_flush();
    return OK;
}

int wrefresh(WINDOW *w) {
    if (w == curscr) {
        if (ended && screen_size() < 0) return ERR;
        memset(shown, 255, (size_t)LINES * (size_t)COLS * sizeof *shown);
    } else wnoutrefresh(w);
    return doupdate();
}

int refresh(void) { return wrefresh(stdscr); }

int wclrtoeol(WINDOW *w) {
    int x;
    if (!w || w->cy < 0 || w->cy >= w->h) return ERR;
    for (x = w->cx; x < w->w; x++) if (x >= 0) cell_set(&w->cells[w->cy * w->w + x], ' ', w->attr);
    return OK;
}

int werase(WINDOW *w) {
    if (!w) return ERR;
    blank(w->cells, w->h * w->w, w->attr);
    w->cx = w->cy = 0;
    return OK;
}

int wattroff(WINDOW *w, int attr) { w->attr &= ~(unsigned)attr; return OK; }
int wattron(WINDOW *w, int attr) { w->attr |= (unsigned)attr; return OK; }
int wattrset(WINDOW *w, int attr) { w->attr = (unsigned)attr; return OK; }

int wredrawln(WINDOW *w, int first, int count) {
    int y;
    for (y = first; y < first + count; y++)
        if (y + w->y >= 0 && y + w->y < LINES) memset(shown + (y + w->y) * COLS, 255, (size_t)COLS * sizeof *shown);
    return OK;
}

int wscrl(WINDOW *w, int lines) {
    if (!w) return ERR;
    if (lines >= w->h || lines <= -w->h) return werase(w);
    if (lines > 0) {
        memmove(w->cells, w->cells + lines * w->w, (size_t)(w->h - lines) * (size_t)w->w * sizeof *scene);
        blank(w->cells + (w->h - lines) * w->w, lines * w->w, w->attr);
    } else if (lines < 0) {
        lines = -lines;
        memmove(w->cells + lines * w->w, w->cells, (size_t)(w->h - lines) * (size_t)w->w * sizeof *scene);
        blank(w->cells, lines * w->w, w->attr);
    }
    return OK;
}

int raw(void) { half_timeout = -1; enter_raw(); return OK; }
int cbreak(void) { return raw(); }
int noecho(void) { return raw(); }
int nonl(void) { return OK; }
int keypad(WINDOW *w, int on) { w->keys = on; return OK; }
int meta(WINDOW *w, int on) { (void)w; (void)on; return OK; }
int nodelay(WINDOW *w, int on) { w->delay = on ? 0 : -1; return OK; }
void wtimeout(WINDOW *w, int ms) { w->delay = ms; }
int halfdelay(int tenths) { half_timeout = tenths * 100; return OK; }

int wgetch(WINDOW *w) {
    unsigned start;
    int delay;
    if (pushed >= 0) { int c = pushed; pushed = -1; return c; }
    start = ticks();
    delay = w->delay >= 0 ? w->delay : half_timeout;
    for (;;) {
        int h, cols, n;
        unsigned char c;
        dimensions(&h, &cols);
        if (h != LINES || cols != COLS) {
            if (resize_hook) resize_hook();
            return ERR;
        }
        n = read(0, &c, 1);
        if (n == 1) return c;
        if (n < 0 && errno != EAGAIN && errno != EINTR) return ERR;
        if (delay == 0 || (delay > 0 && ticks() - start >= (unsigned)delay)) return ERR;
        sleep_ms(5);
    }
}

int ungetch(int c) { pushed = c; return OK; }
int napms(int ms) { if (ms > 0) sleep_ms((unsigned)ms); return OK; }
int curs_set(int value) {
    int old = cursor_visible;
    cursor_visible = value != 0;
    if (initialized && !ended) { text(cursor_visible ? "\033[?25h" : "\033[?25l"); emit_flush(); }
    return old;
}
int beep(void) { text("\a"); emit_flush(); return OK; }
int leaveok(WINDOW *w, int on) { w->leave = on; return OK; }
int clearok(WINDOW *w, int on) { (void)w; if (on && shown) memset(shown, 255, (size_t)LINES * (size_t)COLS * sizeof *shown); return OK; }
int typeahead(int fd) { (void)fd; return OK; }
int intrflush(WINDOW *w, int on) { (void)w; (void)on; return OK; }
int scrollok(WINDOW *w, int on) { (void)w; (void)on; return OK; }
int idlok(WINDOW *w, int on) { (void)w; (void)on; return OK; }
int notimeout(WINDOW *w, int on) { (void)w; (void)on; return OK; }
int has_colors(void) { return FALSE; }
int start_color(void) { return ERR; }
int init_pair(short a, short b, short c) { (void)a; (void)b; (void)c; return ERR; }
int use_default_colors(void) { return OK; }

char *tigetstr(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "clear")) return "\033[2J";
    if (!strcmp(name, "el")) return "\033[K";
    if (!strcmp(name, "ed")) return "\033[J";
    if (!strcmp(name, "cup")) return "\033[%i%p1%d;%p2%dH";
    if (!strcmp(name, "civis")) return "\033[?25l";
    if (!strcmp(name, "cnorm")) return "\033[?25h";
    if (!strcmp(name, "smcup")) return "\033[?1049h";
    if (!strcmp(name, "rmcup")) return "\033[?1049l";
    if (!strcmp(name, "bold")) return "\033[1m";
    if (!strcmp(name, "rev") || !strcmp(name, "smso")) return "\033[7m";
    if (!strcmp(name, "rmso")) return "\033[27m";
    if (!strcmp(name, "sitm")) return "\033[3m";
    if (!strcmp(name, "ritm")) return "\033[23m";
    if (!strcmp(name, "smul")) return "\033[4m";
    if (!strcmp(name, "rmul")) return "\033[24m";
    if (!strcmp(name, "sgr0")) return "\033[0m";
    if (!strcmp(name, "home")) return "\033[H";
    return NULL;
}

int key_defined(const char *s) { (void)s; return 0; }
int define_key(const char *s, int n) { (void)s; (void)n; return OK; }
int use_extended_names(int yes) { return yes; }
int set_escdelay(int ms) { (void)ms; return OK; }
