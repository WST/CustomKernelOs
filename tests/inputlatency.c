/* Measure owner wakeups with an unfinished frame or continuous GPU presents. */
#include "gfx.h"
int main(int argc, char **argv) {
    bool render=argc>1 && !strcmp(argv[1],"render");
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
    if (!render && (gi.caps & GFX_CAP_COMPOSITOR)) {
        before=malloc(gfx_w*gfx_h*4); after=malloc(gfx_w*gfx_h*4);
        if (!before || !after || gfx_readback(before)<0) { printf("LATENCY FAIL readback\n"); return 1; }
    }
    if(!render)gfx_frame_begin();
    if (before && !render) { gfx_clear(RGB(190,25,80)); gfx_flip(); }
    color_t tile[32*32];
    if(render) {
        for(int i=0;i<32*32;i++)tile[i]=RGB(210,70,100);
        if(!(gi.caps&GFX_CAP_COMPOSITOR) || gfx_surface_upload(0,tile,32,32,false)<0) {
            printf("LATENCY FAIL render surface\n");return 1;
        }
        printf("LATENCY RENDER LOAD\n");
    }
    struct mouse m; mouse_get(&m); unsigned previous=m.events;
    printf("LATENCY READY %d %d\n",m.x,m.y);
    for (;;) {
        if(render) {
            if(gfx_frame_begin()<0 || gfx_surface_blit(0,0,0,40,40,32,32)<0 || gfx_frame_end()<0) {
                printf("LATENCY FAIL render frame\n");return 1;
            }
        }
        struct msg message;
        msg_recv(&message,render?0:argc > 1 ? 1000 : 10);
        if (readkey()=='q') break;
        mouse_get(&m);
        if (m.events != previous) { previous=m.events; printf("LATENCY SAMPLE %d %d\n",m.x,m.y); }
    }
    if (before && !render) {
        if (gfx_readback(after)<0 || memcmp(before,after,gfx_w*gfx_h*4)) {
            printf("LATENCY FAIL unfinished frame became visible\n"); return 1;
        }
    }
    free(before);free(after);
    if(!render)gfx_frame_end();
    gfx_close(); printf("LATENCY PASS\n"); return 0;
}
