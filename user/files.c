/* files [папка] — проводник: папки и файлы диска MyFS.
 * Двойной клик: папка — открыть, программа из /bin — запустить,
 * остальное — открыть в Блокноте. Backspace или «Up» — на уровень выше. */
#include "win.h"

#define W 560
#define H 420
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
    for (int i = 0; n_ents < 256 && readdir(cwd, i, &e) == 1; i++) {
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

/* маленькие значки 16x16 */
static void icon_folder(int x, int y) {
    gfx_fill_rect(x + 1, y + 3, 6, 2, RGB(220, 160, 30));
    gfx_fill_rect(x + 1, y + 5, 14, 9, RGB(255, 210, 80));
    gfx_rect(x + 1, y + 5, 14, 9, RGB(220, 160, 30));
}
static void icon_file(int x, int y, bool program) {
    if (program) {
        gfx_fill_rect(x + 1, y + 2, 14, 12, RGB(25, 25, 30));
        gfx_fill_rect(x + 1, y + 2, 14, 3, RGB(110, 110, 120));
        gfx_text(x + 3, y + 1, ">", RGB(80, 220, 100), TRANSPARENT);
    } else {
        gfx_fill_rect(x + 3, y + 1, 10, 14, C_WHITE);
        gfx_rect(x + 3, y + 1, 10, 14, RGB(120, 120, 130));
        for (int i = 0; i < 4; i++) gfx_fill_rect(x + 5, y + 4 + i * 3, 6, 1, RGB(70, 150, 240));
    }
}

static bool is_program(const char *name) {
    if (!strncmp(cwd, "/bin", 4)) return true;
    return !strchr(name, '.');                    /* без расширения — скорее всего программа */
}

static void draw(void) {
    gfx_fill_rect(0, 0, W, BAR_H, UI_FACE);
    ui_button(6, 5, 50, 24, "Up", false);
    ui_sunken(62, 5, W - 68, 24, C_WHITE);
    gfx_text(70, 9, cwd, UI_TEXT, TRANSPARENT);
    gfx_fill_rect(0, BAR_H, W, H - BAR_H, C_WHITE);
    gfx_fill_rect(0, BAR_H, W, 22, RGB(245, 245, 245));
    gfx_text(34, BAR_H + 3, "Name", UI_DARK, TRANSPARENT);
    gfx_text(W - 120, BAR_H + 3, "Size", UI_DARK, TRANSPARENT);
    gfx_fill_rect(0, LIST_Y - 1, W, 1, RGB(220, 220, 220));
    for (int i = 0; i < VIS && top + i < n_ents; i++) {
        struct dirent_info *e = &ents[top + i];
        int y = LIST_Y + i * ROW_H;
        if (top + i == sel) gfx_fill_rect(2, y, W - 16, ROW_H, UI_SELECT);
        if (e->type == 2) icon_folder(10, y + 3); else icon_file(10, y + 3, is_program(e->name));
        gfx_text(34, y + 3, e->name, UI_TEXT, TRANSPARENT);
        char sz[24];
        if (e->type == 2) strcpy(sz, "folder");
        else if (e->size < 10240) snprintf(sz, sizeof(sz), "%u B", e->size);
        else snprintf(sz, sizeof(sz), "%u KB", e->size / 1024);
        gfx_text(W - 120, y + 3, sz, UI_DARK, TRANSPARENT);
    }
    if (n_ents > VIS) {                                       /* полоса прокрутки */
        int th = H - LIST_Y - STATUS_H, kh = th * VIS / n_ents;
        int ky = LIST_Y + (th - kh) * top / (n_ents - VIS);
        gfx_fill_rect(W - 12, LIST_Y, 12, th, RGB(235, 235, 235));
        gfx_fill_rect(W - 10, ky, 8, kh, RGB(190, 190, 190));
    }
    gfx_fill_rect(0, H - STATUS_H, W, STATUS_H, UI_FACE);
    gfx_text(8, H - STATUS_H + 3, status, UI_DARK, TRANSPARENT);
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
    if (argc > 1) snprintf(cwd, sizeof(cwd), "%s", argv[1]);
    read_dir();
    draw();
    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_DOWN) {
            if (ui_inside(e.x, e.y, 6, 5, 50, 24)) go_up();
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
