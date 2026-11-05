/* hellowin — самая простая программа с окном. Образец для своих программ:
 *   cp /src/hellowin.c my.c ; edit my.c ; cc my.c ; ./my
 * Кнопка считает нажатия, клавиши печатаются в окне. */
#include "win.h"

int main(void) {
    window *w = win_create("Hello, window!", 320, 180);
    if (!w) { printf("start the desktop first: desktop\n"); return 1; }
    int clicks = 0;
    char last_key[32] = "-";
    struct event e;
    for (;;) {
        /* нарисовать всё окно */
        gfx_clear(C_WHITE);
        gfx_text_scaled(20, 16, "Hello!", UI_ACCENT, 3);
        char s[48];
        snprintf(s, sizeof(s), "clicks: %d", clicks);
        gfx_text(20, 80, s, UI_TEXT, TRANSPARENT);
        snprintf(s, sizeof(s), "last key: %s", last_key);
        gfx_text(20, 100, s, UI_TEXT, TRANSPARENT);
        ui_button(200, 130, 100, 30, "Click me", false);
        win_update(w);

        if (!win_event(&e, -1) || e.type == EV_CLOSE) break;   /* ждём, что сделает пользователь */
        if (e.type == EV_MOUSE_DOWN && ui_inside(e.x, e.y, 200, 130, 100, 30)) clicks++;
        if (e.type == EV_KEY && e.key >= 32 && e.key < 127) snprintf(last_key, sizeof(last_key), "'%c'", e.key);
    }
    win_destroy(w);
    return 0;
}
