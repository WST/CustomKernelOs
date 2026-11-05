/* hellogfx — самая маленькая графическая программа (пример из README) */
#include "gfx.h"
int main(void) {
    if (gfx_open() < 0) return 1;
    gfx_clear(RGB(0, 0, 60));
    gfx_fill_circle(512, 384, 100, C_YELLOW);
    gfx_text_scaled(380, 520, "Hello, MyOS!", C_WHITE, 3);
    gfx_flip();
    while (readkey() < 0) sleep_ms(20);     /* любая клавиша */
    gfx_close();
    return 0;
}
