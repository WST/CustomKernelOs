/* about — окно «О системе» */
#include "win.h"

#define W 420
#define H 300

int main(void) {
    window *win = win_create("About MyOS", W, H);
    if (!win) { dprintf(STDERR, "about: the desktop is not running\n"); return 1; }
    gfx_gradient(0, 0, W, 96, RGB(119, 33, 111), RGB(233, 84, 32));    /* цвета Ubuntu */
    text_draw(FONT_UI_BOLD, 44, 24, 18, "MyOS", C_WHITE);
    text_draw(FONT_UI, 15, 160, 44, "version 0.18", RGB(255, 228, 214));
    gfx_fill_rect(0, 96, W, H - 96, UI_FACE);
    static const char *lines[] = {
        "An operating system written from scratch",
        "in C and x86 assembly \xE2\x80\x94 by Daniil.",
        "",
        "\xE2\x80\xA2 own boot loader, protected mode, paging",
        "\xE2\x80\xA2 preemptive multitasking, user mode, fork/exec",
        "\xE2\x80\xA2 MyFS file system, ATA disk driver",
        "\xE2\x80\xA2 GPU, windows, smooth vector graphics and fonts",
        "\xE2\x80\xA2 TCP/IP, HTTPS, a browser with JavaScript and CSS",
        "\xE2\x80\xA2 a C compiler that compiles itself",
    };
    for (int i = 0; i < 9; i++) ui_text(20, 106 + i * 18, lines[i], UI_TEXT);
    unsigned s = ticks() / 1000;
    char up[48];
    snprintf(up, sizeof(up), "uptime %u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    ui_text(20, H - 32, up, UI_DARK);
    ui_button_accent(W - 100, H - 40, 86, 30, "OK", false);
    win_update(win);
    struct event e;
    while (win_event(&e, -1)) {
        if (e.type == EV_CLOSE) break;
        if (e.type == EV_KEY && (e.key == '\n' || e.key == 27)) break;
        if (e.type == EV_MOUSE_UP && ui_inside(e.x, e.y, W - 100, H - 40, 86, 30)) break;
    }
    win_destroy(win);
    return 0;
}
