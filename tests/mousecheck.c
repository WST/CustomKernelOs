/* Guest-side state reader for the QMP pointer integration test. */
#include "gfx.h"
int main(void) {
    if (gfx_open() < 0 || gfx_set_mode(800, 600) < 0) { printf("MOUSECHECK FAIL open/mode\n"); return 1; }
    gfx_check_mode(); gfx_clear(RGB(30,40,55)); gfx_flip();
    printf("MOUSECHECK READY\n");
    for (;;) {
        int k = readkey();
        if (k == 'q') break;
        if (k == 'r') {
            if (gfx_set_mode(1280,720) < 0) { printf("MOUSECHECK FAIL resize\n"); break; }
            gfx_check_mode(); printf("MOUSECHECK RESIZED\n");
        }
        if (k == 'n') {
            struct mouse m; mouse_get(&m);
            printf("MOUSECHECK STATE %d %d %u %u\n", m.x,m.y,m.buttons,m.wheel);
        }
        sleep_ms(5);
    }
    gfx_close(); printf("MOUSECHECK PASS\n"); return 0;
}
