/* taskmgr — диспетчер задач: процессы, загрузка процессора, память.
 * Выдели процесс и нажми «End task» (или Delete), чтобы его завершить. */
#include "win.h"

static int W = 520, H = 400;
#define ROW_H 20
#define LIST_Y 82
#define GRAPH_H 0
#define BOTTOM_H 44
#define VIS ((H - LIST_Y - BOTTOM_H) / ROW_H)

static window *win;
static struct proc_info procs[64];
static int n_procs, sel_pid = -1, top;
static int cpu_hist[60], n_hist;
static char status[64];
static bool btn_pressed;

static const char *states[] = { "ready", "running", "sleeping", "waiting", "zombie" };

static void refresh(void) {
    n_procs = ps(procs, 64);
    if (n_procs < 0) n_procs = 0;
    int busy = 0;
    for (int i = 0; i < n_procs; i++)
        if (strcmp(procs[i].name, "idle")) busy += procs[i].cpu_percent;
    if (busy > 100) busy = 100;
    if (n_hist == 60) { memmove(cpu_hist, cpu_hist + 1, sizeof(int) * 59); n_hist--; }
    cpu_hist[n_hist++] = busy;
}

static void draw(void) {
    gfx_clear(C_WHITE);
    /* график загрузки процессора за минуту */
    int gx = 8, gy = 6, gw = W - 290 > 240 ? W - 290 : 240, gh = 44;
    gfx_round_frame(gx, gy, gw, gh, 6, RGB(230, 230, 230), RGB(252, 252, 252));
    if (n_hist > 1) {                                      /* залитая область под линией */
        gfx_path p;
        path_begin(&p, 0, 0, 1, 16);
        path_move(&p, (gx + 1) * 16, (gy + gh - 1) * 16);
        for (int i = 0; i < n_hist; i++)
            path_line(&p, (gx + 1 + i * (gw - 2) / 60) * 16, (gy + gh - 2) * 16 - cpu_hist[i] * (gh - 4) * 16 / 100);
        path_line(&p, (gx + 1 + (n_hist - 1) * (gw - 2) / 60) * 16, (gy + gh - 1) * 16);
        path_close(&p);
        path_fill(&p, UI_ACCENT, 50);
        path_free(&p);
    }
    for (int i = 1; i < n_hist; i++) {
        int x0 = gx + 1 + (i - 1) * (gw - 2) / 60, x1 = gx + 1 + i * (gw - 2) / 60;
        int y0 = (gy + gh - 2) * 16 - cpu_hist[i - 1] * (gh - 4) * 16 / 100, y1 = (gy + gh - 2) * 16 - cpu_hist[i] * (gh - 4) * 16 / 100;
        gfx_line16(x0 * 16, y0, x1 * 16, y1, 24, UI_ACCENT, 255);
    }
    unsigned mem = 0;
    for (int i = 0; i < n_procs; i++) mem += procs[i].mem_kb;
    char s[64];
    snprintf(s, sizeof(s), "CPU %d%%", n_hist ? cpu_hist[n_hist - 1] : 0);
    ui_text(gx + gw + 20, 10, s, UI_TEXT);
    snprintf(s, sizeof(s), "%d processes, %u KB", n_procs, mem);
    ui_text(gx + gw + 20, 30, s, UI_DARK);
    /* таблица */
    gfx_fill_rect(0, LIST_Y - 22, W, 22, RGB(245, 245, 245));
    ui_text(10, LIST_Y - 19, "PID", UI_DARK);
    ui_text(60, LIST_Y - 19, "Name", UI_DARK);
    ui_text(200, LIST_Y - 19, "State", UI_DARK);
    ui_text(300, LIST_Y - 19, "CPU", UI_DARK);
    ui_text(360, LIST_Y - 19, "Memory", UI_DARK);
    ui_text(450, LIST_Y - 19, "Ring", UI_DARK);
    for (int i = 0; i < VIS && top + i < n_procs; i++) {
        struct proc_info *p = &procs[top + i];
        int y = LIST_Y + i * ROW_H;
        if ((int)p->pid == sel_pid) gfx_round_rect(4, y, W - 8, ROW_H, 5, UI_SELECT);
        snprintf(s, sizeof(s), "%u", p->pid);
        ui_text(10, y + 2, s, UI_TEXT);
        ui_text(60, y + 2, p->name, UI_TEXT);
        ui_text(200, y + 2, p->state < 5 ? states[p->state] : "?", UI_TEXT);
        snprintf(s, sizeof(s), "%u%%", p->cpu_percent);
        ui_text(300, y + 2, s, UI_TEXT);
        snprintf(s, sizeof(s), "%u KB", p->mem_kb);
        ui_text(360, y + 2, s, UI_TEXT);
        ui_text(450, y + 2, p->ring ? "user" : "kernel", p->ring ? UI_TEXT : UI_SHADOW);
    }
    gfx_fill_rect(0, H - BOTTOM_H, W, BOTTOM_H, UI_FACE);
    ui_button(W - 118, H - BOTTOM_H + 9, 108, 26, "End task", btn_pressed);
    ui_text(10, H - BOTTOM_H + 14, status, UI_DARK);
    win_update(win);
}

static void end_task(void) {
    if (sel_pid < 0) { strcpy(status, "select a process first"); return; }
    for (int i = 0; i < n_procs; i++)
        if ((int)procs[i].pid == sel_pid && !procs[i].ring) { strcpy(status, "kernel threads can't be stopped"); return; }
    if (sel_pid == getpid()) { strcpy(status, "that's me :)"); return; }
    int r = kill(sel_pid);
    if (r < 0) snprintf(status, sizeof(status), "kill %d: %s", sel_pid, strerror(-r));
    else snprintf(status, sizeof(status), "process %d stopped", sel_pid);
    sel_pid = -1;
}

int main(void) {
    win = win_create("Task Manager", W, H);
    if (!win) { dprintf(STDERR, "taskmgr: the desktop is not running\n"); return 1; }
    win_set_resizable(win, 520, 240);
    refresh();
    draw();
    struct event e;
    unsigned last = ticks();
    while (win_event(&e, 1000)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_MOUSE_DOWN) {
            if (ui_inside(e.x, e.y, W - 118, H - BOTTOM_H + 9, 108, 26)) { btn_pressed = true; end_task(); }
            else if (e.y >= LIST_Y && e.y < H - BOTTOM_H) {
                int i = top + (e.y - LIST_Y) / ROW_H;
                if (i < n_procs) sel_pid = procs[i].pid;
            }
        } else if (e.type == EV_MOUSE_UP) {
            btn_pressed = false;
        } else if (e.type == EV_RESIZE) {
            W = e.x; H = e.y;
        } else if (e.type == EV_KEY) {
            if (e.key == KEY_DEL) end_task();
            else if (e.key == KEY_PGDN && top + VIS < n_procs) top += VIS;
            else if (e.key == KEY_PGUP) { top -= VIS; if (top < 0) top = 0; }
        }
        if (ticks() - last >= 1000) { last = ticks(); refresh(); }
        draw();
    }
    win_destroy(win);
    return 0;
}
