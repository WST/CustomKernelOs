/* Measure owner wakeups while a frame is intentionally unfinished. */
#include "gfx.h"
int main(int argc, char **argv) {
    (void)argv;
    if (gfx_open()<0 || gfx_set_mode(800,600)<0) { printf("LATENCY FAIL open\n"); return 1; }
    gfx_check_mode(); gfx_clear(RGB(30,40,55)); gfx_flip();
    struct gfx_info2 gi; gfx_info2(&gi);
    if (gi.caps & GFX_CAP_CURSOR) {
        if (gfx_hw_cursor(1)<0) { printf("LATENCY FAIL hardware cursor\n"); return 1; }
        gfx_info2(&gi);
        if (!(gi.caps & GFX_CAP_CURSOR_ON)) { printf("LATENCY FAIL cursor state\n"); return 1; }
        printf("LATENCY HARDWARE CURSOR\n");
    }
    color_t *before=NULL,*after=NULL;
    if (gi.caps & GFX_CAP_COMPOSITOR) {
        before=malloc(gfx_w*gfx_h*4); after=malloc(gfx_w*gfx_h*4);
        if (!before || !after || gfx_readback(before)<0) { printf("LATENCY FAIL readback\n"); return 1; }
    }
    gfx_frame_begin();
    if (before) { gfx_clear(RGB(190,25,80)); gfx_flip(); }
    struct mouse m; mouse_get(&m); unsigned previous=m.events;
    printf("LATENCY READY %d %d\n",m.x,m.y);
    for (;;) {
        struct msg message;
        msg_recv(&message,argc > 1 ? 1000 : 10);
        if (readkey()=='q') break;
        mouse_get(&m);
        if (m.events != previous) { previous=m.events; printf("LATENCY SAMPLE %d %d\n",m.x,m.y); }
    }
    if (before) {
        if (gfx_readback(after)<0 || memcmp(before,after,gfx_w*gfx_h*4)) {
            printf("LATENCY FAIL unfinished frame became visible\n"); return 1;
        }
        free(before); free(after);
    }
    gfx_frame_end(); gfx_close(); printf("LATENCY PASS\n"); return 0;
}
