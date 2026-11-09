/* edit [file] — полноэкранный текстовый редактор в духе nano.
 *
 *   стрелки, Home/End, PgUp/PgDn   перемещение
 *   Ctrl+S  сохранить              Ctrl+Q  выйти
 *   Ctrl+K  вырезать строку        Ctrl+U  вставить вырезанное
 *   Ctrl+F  найти (Ctrl+F ещё раз — следующее)
 *   Ctrl+G  перейти к строке
 *
 * Как устроено: терминал в сыром режиме (клавиши приходят сразу), экран
 * рисуется escape-последовательностями ANSI. Текст — массив строк в куче
 * (malloc/realloc поверх sbrk). */
#include "myos.h"

#define TEXT_ROWS (SCREEN_H - 2)        /* 23 строки текста + строка статуса + строка подсказок */
#define TAB_WIDTH 4

struct line { char *s; int len, cap; };

static struct line *L;                  /* строки файла */
static int nl, lcap;
static int cx, cy;                      /* курсор: столбец и строка в файле */
static int rowoff, coloff;              /* что видно на экране */
static bool dirty;
static char filename[64];
static char msg[80];
static bool quit_armed;
static char search[40];

static struct line *cut;                /* буфер вырезанных строк */
static int ncut, cutcap;
static bool last_was_cut;

/* ------------------------------------------------------------ строки */

static void line_reserve(struct line *l, int need) {
    if (need <= l->cap) return;
    int cap = l->cap ? l->cap * 2 : 16;
    while (cap < need) cap *= 2;
    l->s = realloc(l->s, cap);
    l->cap = cap;
}

static void insert_line(int at, const char *s, int len) {
    if (nl == lcap) {
        lcap = lcap ? lcap * 2 : 64;
        L = realloc(L, lcap * sizeof(struct line));
    }
    for (int i = nl; i > at; i--) L[i] = L[i - 1];
    L[at].s = NULL; L[at].len = L[at].cap = 0;
    line_reserve(&L[at], len + 1);
    memcpy(L[at].s, s, len);
    L[at].len = len;
    nl++;
}

static void delete_line(int at) {
    free(L[at].s);
    for (int i = at; i < nl - 1; i++) L[i] = L[i + 1];
    nl--;
    if (nl == 0) insert_line(0, "", 0);
}

static void insert_char(struct line *l, int at, char c) {
    line_reserve(l, l->len + 2);
    for (int i = l->len; i > at; i--) l->s[i] = l->s[i - 1];
    l->s[at] = c;
    l->len++;
}

static void append(struct line *l, const char *s, int n) {
    line_reserve(l, l->len + n + 1);
    memcpy(l->s + l->len, s, n);
    l->len += n;
}

/* ------------------------------------------------------------ файл */

static void load(void) {
    int fd = open(filename, O_READ);
    if (fd < 0) {
        insert_line(0, "", 0);
        snprintf(msg, sizeof(msg), "New file");
        return;
    }
    char buf[512], cur[1024];
    int n, clen = 0, bytes = 0;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            char c = buf[i];
            bytes++;
            if (c == '\n') { insert_line(nl, cur, clen); clen = 0; continue; }
            if (c == '\r') continue;
            if (c == '\t') { for (int k = 0; k < TAB_WIDTH && clen < (int)sizeof(cur); k++) cur[clen++] = ' '; continue; }
            if (clen < (int)sizeof(cur)) cur[clen++] = c;
        }
    }
    close(fd);
    if (clen || nl == 0) insert_line(nl, cur, clen);
    snprintf(msg, sizeof(msg), "Read %d lines, %d bytes", nl, bytes);
}

static void save(void) {
    int fd = open(filename, O_WRITE);
    if (fd < 0) { snprintf(msg, sizeof(msg), "Cannot save: %s", strerror(errno)); return; }
    int total = 0;
    for (int i = 0; i < nl; i++) {
        int r = write(fd, L[i].s, L[i].len);
        int r2 = write(fd, "\n", 1);
        if (r < 0 || r2 < 0) {
            close(fd);
            snprintf(msg, sizeof(msg), "Save FAILED: %s", strerror(errno));
            return;
        }
        total += L[i].len + 1;
    }
    close(fd);
    dirty = false;
    snprintf(msg, sizeof(msg), "Saved %s: %d lines, %d bytes", filename, nl, total);
}

/* ------------------------------------------------------------ экран */

static char out[8192];
static int  olen;

static void put(const char *s, int n) {
    if (olen + n > (int)sizeof(out)) { write(STDOUT, out, olen); olen = 0; }
    memcpy(out + olen, s, n);
    olen += n;
}
static void puts_(const char *s) { put(s, strlen(s)); }
static void putf(const char *fmt, ...) {
    char tmp[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    put(tmp, n < (int)sizeof(tmp) ? n : (int)sizeof(tmp) - 1);
}

static void scroll(void) {
    if (cy < rowoff) rowoff = cy;
    if (cy >= rowoff + TEXT_ROWS) rowoff = cy - TEXT_ROWS + 1;
    if (cx < coloff) coloff = cx;
    if (cx >= coloff + SCREEN_W) coloff = cx - SCREEN_W + 1;
}

static void draw(void) {
    scroll();
    olen = 0;
    puts_(ANSI_HIDE);
    for (int y = 0; y < TEXT_ROWS; y++) {
        int fr = rowoff + y;
        putf("\x1b[%d;1H", y + 1);
        if (fr < nl) {
            int len = L[fr].len - coloff;
            if (len > SCREEN_W) len = SCREEN_W;
            for (int i = 0; i < len; i++) {
                char c = L[fr].s[coloff + i];
                put(c >= 32 && c < 127 ? &c : "?", 1);
            }
            if (len < SCREEN_W) puts_("\x1b[K");
        } else {
            puts_(ANSI_BLUE "~" ANSI_RESET "\x1b[K");
        }
    }
    /* строка статуса (инверсная). Не длиннее 79 символов: 80-й перевёл бы строку */
    char status[80], right[40];
    int ls = snprintf(status, sizeof(status), " edit: %.30s%s", filename, dirty ? " [modified]" : "");
    int rs = snprintf(right, sizeof(right), "line %d/%d, col %d ", cy + 1, nl, cx + 1);
    putf("\x1b[%d;1H" ANSI_REVERSE "%s", TEXT_ROWS + 1, status);
    for (int i = ls; i < SCREEN_W - 1 - rs; i++) put(" ", 1);
    puts_(right);
    puts_(ANSI_RESET);
    /* нижняя строка: сообщение или подсказка */
    putf("\x1b[%d;1H\x1b[K", TEXT_ROWS + 2);
    if (msg[0]) putf(ANSI_YELLOW "%.78s" ANSI_RESET, msg);
    else puts_(ANSI_CYAN "^S" ANSI_RESET " save  " ANSI_CYAN "^Q" ANSI_RESET " quit  "
               ANSI_CYAN "^K" ANSI_RESET " cut  " ANSI_CYAN "^U" ANSI_RESET " paste  "
               ANSI_CYAN "^F" ANSI_RESET " find  " ANSI_CYAN "^G" ANSI_RESET " go to line");
    putf("\x1b[%d;%dH" ANSI_SHOW, cy - rowoff + 1, cx - coloff + 1);
    write(STDOUT, out, olen);
}

/* однострочный ввод в нижней строке; -> false, если Esc */
static bool prompt(const char *q, char *buf, int max) {
    int len = strlen(buf);
    for (;;) {
        olen = 0;
        putf("\x1b[%d;1H\x1b[K" ANSI_YELLOW "%s" ANSI_RESET "%s", TEXT_ROWS + 2, q, buf);
        write(STDOUT, out, olen);
        int k = readkey();
        if (k == KEY_RESIZE) { draw(); continue; }
        if (k == '\r') return true;
        if (k == 27 || k == CTRL('c')) return false;
        if ((k == '\b' || k == 127) && len) buf[--len] = 0;
        else if (k >= 32 && k < 127 && len < max - 1) { buf[len++] = k; buf[len] = 0; }
    }
}

/* ------------------------------------------------------------ команды */

static void clamp_cx(void) { if (cx > L[cy].len) cx = L[cy].len; }

static void find_next(void) {
    int m = strlen(search);
    if (!m) return;
    for (int step = 0; step <= nl; step++) {
        int r = (cy + step) % nl;
        int from = step == 0 ? cx + 1 : 0;
        for (int i = from; i + m <= L[r].len; i++)
            if (!memcmp(L[r].s + i, search, m)) {
                cy = r; cx = i;
                snprintf(msg, sizeof(msg), "Found \"%s\" (^F again for next)", search);
                return;
            }
    }
    snprintf(msg, sizeof(msg), "\"%s\" not found", search);
}

static void cut_line(void) {
    if (!last_was_cut) {                 /* новая серия вырезаний — буфер с нуля */
        for (int i = 0; i < ncut; i++) free(cut[i].s);
        ncut = 0;
    }
    if (ncut == cutcap) { cutcap = cutcap ? cutcap * 2 : 16; cut = realloc(cut, cutcap * sizeof(struct line)); }
    cut[ncut] = L[cy];                   /* забираем строку целиком (без копирования) */
    ncut++;
    for (int i = cy; i < nl - 1; i++) L[i] = L[i + 1];
    nl--;
    if (nl == 0) insert_line(0, "", 0);
    if (cy >= nl) cy = nl - 1;
    cx = 0;
    dirty = true;
    snprintf(msg, sizeof(msg), "Cut %d line%s (^U to paste)", ncut, ncut > 1 ? "s" : "");
}

static void paste(void) {
    for (int i = 0; i < ncut; i++) insert_line(cy++, cut[i].s, cut[i].len);
    cx = 0;
    if (ncut) dirty = true;
}

static bool handle(int k) {
    struct line *l = &L[cy];
    bool was_cut = last_was_cut;
    last_was_cut = false;
    if (k != CTRL('q')) quit_armed = false;

    switch (k) {
    case CTRL('q'):
        if (dirty && !quit_armed) {
            quit_armed = true;
            snprintf(msg, sizeof(msg), "Unsaved changes! ^Q again to quit without saving, ^S to save");
            return true;
        }
        return false;
    case CTRL('s'): save(); break;
    case CTRL('k'): last_was_cut = was_cut; cut_line(); last_was_cut = true; break;
    case CTRL('u'): paste(); break;
    case CTRL('f'):
        if (prompt("Find: ", search, sizeof(search))) find_next();
        break;
    case CTRL('g'): {
        char num[12] = "";
        if (prompt("Go to line: ", num, sizeof(num)) && atoi(num) > 0) {
            cy = atoi(num) - 1;
            if (cy >= nl) cy = nl - 1;
            cx = 0;
        }
        break;
    }
    case KEY_UP:    if (cy > 0) { cy--; clamp_cx(); } break;
    case KEY_DOWN:  if (cy < nl - 1) { cy++; clamp_cx(); } break;
    case KEY_LEFT:
        if (cx > 0) cx--;
        else if (cy > 0) { cy--; cx = L[cy].len; }
        break;
    case KEY_RIGHT:
        if (cx < l->len) cx++;
        else if (cy < nl - 1) { cy++; cx = 0; }
        break;
    case KEY_HOME: cx = 0; break;
    case KEY_END:  cx = l->len; break;
    case KEY_PGUP: cy = cy > TEXT_ROWS ? cy - TEXT_ROWS : 0; clamp_cx(); break;
    case KEY_PGDN: cy = cy + TEXT_ROWS < nl ? cy + TEXT_ROWS : nl - 1; clamp_cx(); break;
    case '\r': {                                   /* разрезать строку */
        insert_line(cy + 1, l->s + cx, l->len - cx);
        L[cy].len = cx;
        cy++; cx = 0;
        dirty = true;
        break;
    }
    case '\b': case 127:
        if (cx > 0) {
            for (int i = cx - 1; i < l->len - 1; i++) l->s[i] = l->s[i + 1];
            l->len--; cx--;
            dirty = true;
        } else if (cy > 0) {                       /* склеить с предыдущей */
            cx = L[cy - 1].len;
            append(&L[cy - 1], l->s, l->len);
            delete_line(cy);
            cy--;
            dirty = true;
        }
        break;
    case KEY_DEL:
        if (cx < l->len) {
            for (int i = cx; i < l->len - 1; i++) l->s[i] = l->s[i + 1];
            l->len--;
            dirty = true;
        } else if (cy < nl - 1) {
            append(l, L[cy + 1].s, L[cy + 1].len);
            delete_line(cy + 1);
            dirty = true;
        }
        break;
    case '\t':
        for (int i = 0; i < TAB_WIDTH; i++) insert_char(l, cx++, ' ');
        dirty = true;
        break;
    default:
        if (k >= 32 && k < 127) { insert_char(l, cx++, (char)k); dirty = true; }
        break;
    }
    return true;
}

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: edit <file>\n"); return 2; }
    strncpy(filename, argv[1], sizeof(filename) - 1);
    load();
    ttymode(TTY_RAW | TTY_NOSIG);       /* Ctrl+C не должен убить редактор с несохранённым текстом */
    write(STDOUT, ANSI_CLEAR, strlen(ANSI_CLEAR));
    for (;;) {
        draw();
        int k = readkey();
        if (k == KEY_RESIZE) { write(STDOUT, ANSI_CLEAR, strlen(ANSI_CLEAR)); continue; }
        char keep = msg[0];
        msg[0] = 0;                     /* сообщение живёт до следующей клавиши */
        (void)keep;
        if (!handle(k)) break;
    }
    write(STDOUT, ANSI_RESET ANSI_CLEAR ANSI_SHOW, strlen(ANSI_RESET ANSI_CLEAR ANSI_SHOW));
    ttymode(0);
    return 0;
}
