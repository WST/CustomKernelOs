/* about — окно «О системе» */
#include "win.h"

#define W 420
#define H 300

int main(void) {
    window *win = win_create("About MyOS", W, H);
    if (!win) { dprintf(STDERR, "about: the desktop is not running\n"); return 1; }
    for (int y = 0; y < 90; y++) gfx_fill_rect(0, y, W, 1, RGB(10 + y / 3, 60 + y, 140 + y));
    gfx_text_scaled(24, 18, "MyOS", C_WHITE, 4);
    gfx_text(170, 40, "version 0.14", RGB(220, 235, 255), TRANSPARENT);
    gfx_fill_rect(0, 90, W, H - 90, C_WHITE);
    static const char *lines[] = {
        "An operating system written from scratch",
        "in C and x86 assembly - by Daniil.",
        "",
        "* own boot loader, protected mode, paging",
        "* preemptive multitasking, user mode, fork/exec",
        "* MyFS file system, ATA disk driver",
        "* graphics, mouse, windows, this desktop",
        "* TCP/IP network stack, web server and browser",
        "* a C compiler that compiles itself",
    };
    for (int i = 0; i < 9; i++) gfx_text(20, 102 + i * 18, lines[i], UI_TEXT, TRANSPARENT);
    unsigned s = ticks() / 1000;
    char up[48];
    snprintf(up, sizeof(up), "uptime %u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    gfx_text(20, H - 32, up, UI_DARK, TRANSPARENT);
    ui_button(W - 100, H - 38, 86, 28, "OK", false);
    win_update(win);
    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_KEY && (e.key == '\n' || e.key == 27)) break;
        if (e.type == EV_MOUSE_UP && ui_inside(e.x, e.y, W - 100, H - 38, 86, 28)) break;
    }
    win_destroy(win);
    return 0;
}
