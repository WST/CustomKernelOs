/* files [папка] — проводник: папки и файлы диска MyFS.
 * Двойной клик: папка — открыть, программа из /bin — запустить,
 * остальное — открыть в Блокноте. Backspace или «Up» — на уровень выше. */
#include "win.h"

static int W = 560, H = 420;
#define BAR_H 34
#define ROW_H 22
#define LIST_Y (BAR_H + 22)
#define STATUS_H 22
#define VIS ((H - LIST_Y - STATUS_H) / ROW_H)

static window *win;
static char cwd[128] = "/";
static struct dirent_info ents[256];
static int n_ents, sel = -1, top;
static unsigned last_click;
static int last_idx = -1;
static char status[80];

static int cmp_ent(struct dirent_info *a, struct dirent_info *b) {
    if (a->type != b->type) return a->type == 2 ? -1 : 1;     /* папки сначала */
    return strcmp(a->name, b->name);
}

static void read_dir(void) {
    n_ents = 0;
    struct dirent_info e;
    for (int i = 0; n_ents < 256 && dirread(cwd, i, &e) == 1; i++) {
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        int j = n_ents++;
        while (j > 0 && cmp_ent(&e, &ents[j - 1]) < 0) { ents[j] = ents[j - 1]; j--; }
        ents[j] = e;
    }
    sel = -1; top = 0;
    snprintf(status, sizeof(status), "%d items", n_ents);
    char t[64];
    snprintf(t, sizeof(t), "%s - Files", cwd);
    win_set_title(win, t);
}

static void full_path(char *out, int n, const char *name) {
    if (!strcmp(cwd, "/")) snprintf(out, n, "/%s", name);
    else snprintf(out, n, "%s/%s", cwd, name);
}

static void go_up(void) {
    if (!strcmp(cwd, "/")) return;
    char *s = strrchr(cwd, '/');
    if (s == cwd) cwd[1] = 0; else *s = 0;
    read_dir();
}

/* маленькие значки 16x16 — сглаженные, как в Yaru */
static void icon_folder(int x, int y) {
    gfx_round_rect(x + 1, y + 2, 7, 4, 1, RGB(196, 80, 38));
    gfx_round_rect(x + 1, y + 3, 14, 11, 2, RGB(196, 80, 38));
    gfx_round_rect(x + 1, y + 5, 14, 9, 2, RGB(240, 120, 64));
}
static void icon_file(int x, int y, bool program) {
    if (program) {
        gfx_round_rect(x + 1, y + 2, 14, 12, 2, RGB(60, 60, 60));
        gfx_round_rect(x + 2, y + 4, 12, 9, 1, RGB(48, 10, 36));
        gfx_line16((x + 4) * 16, (y + 6) * 16, (x + 6) * 16 + 8, (y + 8) * 16 + 8, 20, C_WHITE, 255);
        gfx_line16((x + 6) * 16 + 8, (y + 8) * 16 + 8, (x + 4) * 16, (y + 11) * 16, 20, C_WHITE, 255);
    } else {
        gfx_round_frame(x + 2, y + 1, 12, 15, 2, RGB(170, 170, 170), C_WHITE);
        for (int i = 0; i < 4; i++) gfx_fill_rect(x + 5, y + 5 + i * 2 + i, 6, 1, RGB(150, 150, 150));
    }
}

static bool is_program(const char *name) {
    if (!strncmp(cwd, "/bin", 4)) return true;
    return !strchr(name, '.');                    /* без расширения — скорее всего программа */
}

static void draw(void) {
    gfx_fill_rect(0, 0, W, BAR_H, UI_FACE);
    ui_button(6, 4, 50, 26, "Up", false);
    ui_sunken(62, 4, W - 68, 26, C_WHITE);
    ui_text(72, 4 + (26 - text_line_h(FONT_UI, UI_FONT_SIZE)) / 2, cwd, UI_TEXT);
    gfx_fill_rect(0, BAR_H, W, H - BAR_H, C_WHITE);
    gfx_fill_rect(0, BAR_H, W, 22, RGB(250, 250, 250));
    text_draw(FONT_UI_MEDIUM, 13, 34, BAR_H + 3, "Name", UI_DARK);
    text_draw(FONT_UI_MEDIUM, 13, W - 120, BAR_H + 3, "Size", UI_DARK);
    gfx_fill_rect(0, LIST_Y - 1, W, 1, RGB(230, 230, 230));
    for (int i = 0; i < VIS && top + i < n_ents; i++) {
        struct dirent_info *e = &ents[top + i];
        int y = LIST_Y + i * ROW_H;
        bool s_ = top + i == sel;
        if (s_) gfx_round_rect(4, y + 1, W - 20, ROW_H - 2, 5, UI_ACCENT);
        if (e->type == 2) icon_folder(10, y + 3); else icon_file(10, y + 3, is_program(e->name));
        int ty = y + (ROW_H - text_line_h(FONT_UI, UI_FONT_SIZE)) / 2;
        ui_text(34, ty, e->name, s_ ? C_WHITE : UI_TEXT);
        char sz[24];
        if (e->type == 2) strcpy(sz, "folder");
        else if (e->size < 10240) snprintf(sz, sizeof(sz), "%u B", e->size);
        else snprintf(sz, sizeof(sz), "%u KB", e->size / 1024);
        ui_text(W - 120, ty, sz, s_ ? C_WHITE : UI_DARK);
    }
    if (n_ents > VIS) {                                       /* полоса прокрутки */
        int th = H - LIST_Y - STATUS_H, kh = th * VIS / n_ents;
        int ky = LIST_Y + (th - kh) * top / (n_ents - VIS);
        gfx_round_rect(W - 10, ky + 2, 6, kh - 4, 3, RGB(170, 170, 170));
    }
    gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, UI_FACE);
    gfx_fill_rect(0, H - STATUS_H, W, 1, RGB(230, 230, 230));
    text_draw(FONT_UI, 13, 8, H - STATUS_H + (STATUS_H - text_line_h(FONT_UI, 13)) / 2, status, UI_DARK);
    win_update(win);
}

static void launch(const char *prog, const char *arg) {
    int pid = fork();
    if (pid == 0) {
        int p[2];                                             /* ввод — пустой канал: клавиатура только у стола */
        if (pipe(p) == 0) { close(p[1]); dup2(p[0], 0); close(p[0]); }
        char *argv[3] = { (char *)prog, (char *)arg, NULL };
        exec(prog, argv);
        exit(127);
    }
}

static void open_entry(int i) {
    struct dirent_info *e = &ents[i];
    char p[160];
    full_path(p, sizeof(p), e->name);
    if (e->type == 2) { strcpy(cwd, p); read_dir(); return; }
    if (is_program(e->name)) {
        launch(p, NULL);
        snprintf(status, sizeof(status), "started %s (console programs show nothing here)", e->name);
    } else {
        launch("/bin/notepad", p);
        snprintf(status, sizeof(status), "opened %s in Notepad", e->name);
    }
}

static void scroll_to_sel(void) {
    if (sel < top) top = sel;
    if (sel >= top + VIS) top = sel - VIS + 1;
    if (top < 0) top = 0;
}

int main(int argc, char **argv) {
    win = win_create("Files", W, H);
    if (!win) { dprintf(STDERR, "files: the desktop is not running\n"); return 1; }
    win_set_resizable(win, 360, 220);
    if (argc > 1) snprintf(cwd, sizeof(cwd), "%s", argv[1]);
    read_dir();
    draw();
    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_DOWN) {
            if (ui_inside(e.x, e.y, 6, 4, 50, 26)) go_up();
            else if (e.y >= LIST_Y && e.y < H - STATUS_H) {
                if (e.x >= W - 12 && n_ents > VIS) {           /* клик по полосе — страница вверх/вниз */
                    top += e.y < (LIST_Y + H - STATUS_H) / 2 ? -VIS : VIS;
                    if (top > n_ents - VIS) top = n_ents - VIS;
                    if (top < 0) top = 0;
                } else {
                    int i = top + (e.y - LIST_Y) / ROW_H;
                    if (i < n_ents) {
                        if (i == last_idx && ticks() - last_click < 500) { open_entry(i); last_idx = -1; }
                        else { sel = i; last_idx = i; last_click = ticks(); }
                    }
                }
            }
        } else if (e.type == EV_RESIZE) {
            W = e.x; H = e.y;
            if (sel >= 0) scroll_to_sel();
        } else if (e.type == EV_KEY) {
            if (e.key == KEY_DOWN && sel < n_ents - 1) sel++;
            else if (e.key == KEY_UP && sel > 0) sel--;
            else if (e.key == KEY_PGDN) { sel += VIS; if (sel >= n_ents) sel = n_ents - 1; }
            else if (e.key == KEY_PGUP) { sel -= VIS; if (sel < 0) sel = 0; }
            else if ((e.key == '\n' || e.key == '\r') && sel >= 0) open_entry(sel);
            else if (e.key == 8 || e.key == 127) go_up();
            else if (e.key == CTRL('r')) read_dir();
            if (sel >= 0) scroll_to_sel();
        } else continue;
        draw();
    }
    win_destroy(win);
    return 0;
}
