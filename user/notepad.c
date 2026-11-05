/* notepad [файл] — текстовый редактор в окне.
 *
 *   стрелки, Home/End, PgUp/PgDn — двигаться; мышь — поставить курсор
 *   Ctrl+S — сохранить, Ctrl+O — открыть (имя в поле сверху), Ctrl+N — новый
 *   Tab — 4 пробела */
#include "win.h"

#define W 640
#define H 440
#define BAR_H 32
#define STATUS_H 22
#define TEXT_X 6
#define TEXT_Y (BAR_H + 4)
#define VIS_ROWS ((H - BAR_H - STATUS_H - 8) / 16)
#define VIS_COLS ((W - 2 * TEXT_X - 12) / 8)
#define MAX_LINES 4000

static window *win;
static char *lines[MAX_LINES];
static int   lens[MAX_LINES], caps[MAX_LINES];
static int   n_lines;
static int   cur_r, cur_c, top, left;
static bool  modified;
static char  path[96];
static bool  editing_name;                  /* курсор в поле имени файла */
static int   name_len;
static char  status_msg[64];
static int   pressed_btn = -1;

static const char *btn_labels[] = { "New", "Open", "Save" };
#define BTN_X(i) (6 + (i) * 64)
#define NAME_X 206

/* ------------------------------------------------------------ текст */

static void ensure_cap(int r, int need) {
    if (caps[r] >= need) return;
    int cap = caps[r] ? caps[r] : 16;
    while (cap < need) cap *= 2;
    char *p = malloc(cap);
    if (lines[r]) memcpy(p, lines[r], lens[r]);
    free(lines[r]);
    lines[r] = p;
    caps[r] = cap;
}

static void insert_line(int at, const char *s, int n) {
    if (n_lines >= MAX_LINES) return;
    for (int i = n_lines; i > at; i--) { lines[i] = lines[i - 1]; lens[i] = lens[i - 1]; caps[i] = caps[i - 1]; }
    lines[at] = NULL; lens[at] = 0; caps[at] = 0;
    ensure_cap(at, n + 1);
    memcpy(lines[at], s, n);
    lens[at] = n;
    n_lines++;
}

static void delete_line(int at) {
    free(lines[at]);
    for (int i = at; i < n_lines - 1; i++) { lines[i] = lines[i + 1]; lens[i] = lens[i + 1]; caps[i] = caps[i + 1]; }
    n_lines--;
    lines[n_lines] = NULL; lens[n_lines] = 0; caps[n_lines] = 0;
}

static void clear_text(void) {
    while (n_lines) delete_line(n_lines - 1);
    insert_line(0, "", 0);
    cur_r = cur_c = top = left = 0;
}

static void set_title(void) {
    char t[64];
    const char *b = path[0] ? strrchr(path, '/') : NULL;
    snprintf(t, sizeof(t), "%s%s - Notepad", modified ? "*" : "", path[0] ? (b ? b + 1 : path) : "Untitled");
    win_set_title(win, t);
}

static void load(const char *p) {
    int fd = open(p, O_READ);
    if (fd < 0) { snprintf(status_msg, sizeof(status_msg), "cannot open %s", p); return; }
    while (n_lines) delete_line(n_lines - 1);
    char buf[512], cur[1024];
    int cl = 0, n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        for (int i = 0; i < n; i++) {
            if (buf[i] == '\n') { insert_line(n_lines, cur, cl); cl = 0; }
            else if (buf[i] == '\t') { for (int k = 0; k < 4 && cl < 1023; k++) cur[cl++] = ' '; }
            else if (buf[i] != '\r' && cl < 1023) cur[cl++] = buf[i];
        }
    insert_line(n_lines, cur, cl);
    close(fd);
    cur_r = cur_c = top = left = 0;
    modified = false;
    snprintf(status_msg, sizeof(status_msg), "opened, %d lines", n_lines);
}

static void save(void) {
    if (!path[0]) { editing_name = true; strcpy(status_msg, "type a file name, then Enter"); return; }
    int fd = open(path, O_WRITE);
    if (fd < 0) { snprintf(status_msg, sizeof(status_msg), "cannot save: %s", strerror(-fd)); return; }
    int total = 0;
    for (int i = 0; i < n_lines; i++) {
        if (lens[i]) write(fd, lines[i], lens[i]);
        if (i < n_lines - 1 || lens[i]) write(fd, "\n", 1);
        total += lens[i] + 1;
    }
    close(fd);
    modified = false;
    snprintf(status_msg, sizeof(status_msg), "saved %d bytes", total);
}

/* ------------------------------------------------------------ рисование */

static void draw(void) {
    gfx_fill_rect(0, 0, W, BAR_H, UI_FACE);
    gfx_fill_rect(0, BAR_H - 1, W, 1, UI_SHADOW);
    for (int i = 0; i < 3; i++) ui_button(BTN_X(i), 4, 58, 24, btn_labels[i], pressed_btn == i);
    ui_sunken(NAME_X, 4, W - NAME_X - 6, 24, editing_name ? C_WHITE : RGB(248, 248, 248));
    if (!path[0] && !editing_name) gfx_text(NAME_X + 6, 8, "(file name)", UI_SHADOW, TRANSPARENT);
    else gfx_text(NAME_X + 6, 8, path, UI_TEXT, TRANSPARENT);
    if (editing_name) gfx_fill_rect(NAME_X + 6 + name_len * 8, 8, 2, 16, UI_TEXT);

    int th = H - BAR_H - STATUS_H;
    gfx_fill_rect(0, BAR_H, W, th, C_WHITE);
    for (int r = 0; r < VIS_ROWS && top + r < n_lines; r++) {
        int li = top + r;
        char buf[VIS_COLS + 1];
        int n = 0;
        for (int c = left; c < lens[li] && n < VIS_COLS; c++) buf[n++] = lines[li][c];
        buf[n] = 0;
        gfx_text(TEXT_X, TEXT_Y + r * 16, buf, UI_TEXT, TRANSPARENT);
    }
    if (!editing_name && cur_r >= top && cur_r < top + VIS_ROWS && cur_c >= left)
        gfx_fill_rect(TEXT_X + (cur_c - left) * 8, TEXT_Y + (cur_r - top) * 16, 2, 16, UI_TEXT);
    /* полоса прокрутки */
    int sb_x = W - 12;
    gfx_fill_rect(sb_x, BAR_H, 12, th, RGB(235, 235, 235));
    if (n_lines > VIS_ROWS) {
        int kh = th * VIS_ROWS / n_lines;
        if (kh < 20) kh = 20;
        int ky = BAR_H + (th - kh) * top / (n_lines - VIS_ROWS);
        gfx_fill_rect(sb_x + 2, ky, 8, kh, RGB(190, 190, 190));
    }
    gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, UI_FACE);
    gfx_fill_rect(0, H - STATUS_H, W, 1, UI_SHADOW);
    char st[96];
    snprintf(st, sizeof(st), "Ln %d, Col %d   %d lines   %s", cur_r + 1, cur_c + 1, n_lines, status_msg);
    gfx_text(8, H - STATUS_H + 3, st, UI_DARK, TRANSPARENT);
    win_update(win);
}

static void keep_visible(void) {
    if (cur_r < top) top = cur_r;
    if (cur_r >= top + VIS_ROWS) top = cur_r - VIS_ROWS + 1;
    if (cur_c < left) left = cur_c;
    if (cur_c >= left + VIS_COLS) left = cur_c - VIS_COLS + 1;
}

static void changed(void) {
    if (!modified) { modified = true; set_title(); }
    status_msg[0] = 0;
}

/* ------------------------------------------------------------ клавиши */

static void name_key(int k) {
    if (k == '\n' || k == '\r') {
        editing_name = false;
        if (path[0]) {
            struct dirent_info st;
            if (stat(path, &st) == 0 && st.type == 1 && !modified) load(path);   /* существующий файл — открыть */
            else save();
            set_title();
        }
    } else if (k == 27) {
        editing_name = false;
    } else if ((k == 8 || k == 127) && name_len) {
        path[--name_len] = 0;
    } else if (k >= 32 && k < 127 && name_len < (int)sizeof(path) - 1) {
        path[name_len++] = k;
        path[name_len] = 0;
    }
}

static void text_key(int k) {
    if (k == CTRL('s')) { save(); set_title(); return; }
    if (k == CTRL('o')) { editing_name = true; name_len = strlen(path); strcpy(status_msg, "file name, then Enter"); return; }
    if (k == CTRL('n')) { clear_text(); path[0] = 0; modified = false; set_title(); return; }
    switch (k) {
    case KEY_UP:    if (cur_r > 0) cur_r--; break;
    case KEY_DOWN:  if (cur_r < n_lines - 1) cur_r++; break;
    case KEY_LEFT:  if (cur_c > 0) cur_c--; else if (cur_r > 0) { cur_r--; cur_c = lens[cur_r]; } break;
    case KEY_RIGHT: if (cur_c < lens[cur_r]) cur_c++; else if (cur_r < n_lines - 1) { cur_r++; cur_c = 0; } break;
    case KEY_HOME:  cur_c = 0; break;
    case KEY_END:   cur_c = lens[cur_r]; break;
    case KEY_PGUP:  cur_r -= VIS_ROWS; if (cur_r < 0) cur_r = 0; break;
    case KEY_PGDN:  cur_r += VIS_ROWS; if (cur_r >= n_lines) cur_r = n_lines - 1; break;
    case '\n': case '\r':
        insert_line(cur_r + 1, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
        lens[cur_r] = cur_c;
        cur_r++; cur_c = 0;
        changed();
        break;
    case 8: case 127:
        if (cur_c > 0) {
            memmove(lines[cur_r] + cur_c - 1, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
            lens[cur_r]--; cur_c--;
        } else if (cur_r > 0) {
            int pl = lens[cur_r - 1];
            ensure_cap(cur_r - 1, pl + lens[cur_r] + 1);
            memcpy(lines[cur_r - 1] + pl, lines[cur_r], lens[cur_r]);
            lens[cur_r - 1] += lens[cur_r];
            delete_line(cur_r);
            cur_r--; cur_c = pl;
        }
        changed();
        break;
    case KEY_DEL:
        if (cur_c < lens[cur_r]) {
            memmove(lines[cur_r] + cur_c, lines[cur_r] + cur_c + 1, lens[cur_r] - cur_c - 1);
            lens[cur_r]--;
        } else if (cur_r < n_lines - 1) {
            ensure_cap(cur_r, lens[cur_r] + lens[cur_r + 1] + 1);
            memcpy(lines[cur_r] + lens[cur_r], lines[cur_r + 1], lens[cur_r + 1]);
            lens[cur_r] += lens[cur_r + 1];
            delete_line(cur_r + 1);
        }
        changed();
        break;
    case '\t':
        for (int i = 0; i < 4; i++) text_key(' ');
        return;
    default:
        if (k < 32 || k >= 127) return;
        ensure_cap(cur_r, lens[cur_r] + 2);
        memmove(lines[cur_r] + cur_c + 1, lines[cur_r] + cur_c, lens[cur_r] - cur_c);
        lines[cur_r][cur_c++] = k;
        lens[cur_r]++;
        changed();
    }
    if (cur_c > lens[cur_r]) cur_c = lens[cur_r];
}

static void click(int x, int y) {
    for (int i = 0; i < 3; i++)
        if (ui_inside(x, y, BTN_X(i), 4, 58, 24)) {
            pressed_btn = i;
            if (i == 0) text_key(CTRL('n'));
            if (i == 1) text_key(CTRL('o'));
            if (i == 2) { save(); set_title(); }
            return;
        }
    if (ui_inside(x, y, NAME_X, 4, W - NAME_X - 6, 24)) { editing_name = true; name_len = strlen(path); return; }
    if (y >= BAR_H && y < H - STATUS_H && x < W - 12) {
        editing_name = false;
        int r = top + (y - TEXT_Y) / 16;
        if (r < 0) r = 0;
        if (r >= n_lines) r = n_lines - 1;
        int c = left + (x - TEXT_X + 4) / 8;
        if (c < 0) c = 0;
        if (c > lens[r]) c = lens[r];
        cur_r = r; cur_c = c;
    }
}

int main(int argc, char **argv) {
    win = win_create("Notepad", W, H);
    if (!win) { dprintf(STDERR, "notepad: the desktop is not running\n"); return 1; }
    clear_text();
    if (argc > 1) {
        snprintf(path, sizeof(path), "%s", argv[1]);
        name_len = strlen(path);
        struct dirent_info st;
        if (stat(path, &st) == 0) load(path);
        else strcpy(status_msg, "new file");
    }
    set_title();
    draw();
    struct event e;
    bool close_armed = false;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) {
            if (!modified || close_armed) break;
            close_armed = true;                                   /* как в Windows: не терять текст молча */
            strcpy(status_msg, "not saved! Ctrl+S to save, X again to discard");
            draw();
            continue;
        }
        if (e.type == EV_KEY) { if (editing_name) name_key(e.key); else text_key(e.key); keep_visible(); }
        else if (e.type == EV_MOUSE_DOWN) { click(e.x, e.y); keep_visible(); }
        else if (e.type == EV_MOUSE_UP) pressed_btn = -1;
        else continue;
        draw();
    }
    win_destroy(win);
    return 0;
}
