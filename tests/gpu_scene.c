/* Exercise the actual desktop compositor against its software reference. */
#define main desktop_program_main
#include "../user/desktop.c"
#undef main

static void check(int condition, const char *reason) {
    if (!condition) { gfx_close(); printf("GPUSCENE FAIL %s\n", reason); exit(1); }
}

static void compare_frame(const char *name,bool repair) {
    compose(mkrect(0, 0, SW, SH));
    color_t *reference = malloc(SW * SH * 4), *actual = malloc(SW * SH * 4);
    check(reference && actual, "pixel allocations");
    memcpy(reference, back, SW * SH * 4);
    if(repair) {
        check(gfx_frame_begin() == 0, "frame begin");
        check(gpu_compose(mkrect(0, 0, SW, SH)), "compose");
        check(gfx_frame_end() == 0,"frame end");
    }
    check(gfx_readback(actual) == 0, "readback");
    int different = 0, worst = 0;
    for (int i = 0; i < SW * SH; i++) {
        int delta = 0;
        for (int shift = 0; shift < 24; shift += 8) {
            int d = iabs((int)((reference[i] >> shift) & 255) - (int)((actual[i] >> shift) & 255));
            if (d > delta) delta = d;
        }
        if (delta > 4) different++;
        if (delta > worst) worst = delta;
    }
    printf("GPUSCENE %s differences %d max %d\n", name, different, worst);
    free(reference); free(actual);
    check(different == 0, "software/GPU pixels disagree");
}
static void compare(const char *name) { compare_frame(name,true); }
static void effect_run(const char *name) {
    int frames=0;unsigned began=ticks();
    while(motions_active() && ticks()-began<2000) {
        int wait=motion_wait();if(wait>0)sleep_ms(wait);
        gfx_frame_begin();motions_tick();flush();gfx_frame_end();frames++;
    }
    check(!motions_active() && frames>=3,"animation cadence/completion");
    printf("GPUSCENE EFFECT %s frames %d\n",name,frames);
    compare_frame(name,false);
}

int main(int argc,char **argv) {
    (void)argv;
    check(gfx_open() == 0, "open");
    check(gfx_set_mode(argc>1?2560:1024, argc>1?1600:768) == 0, "mode"); gfx_check_mode();
    SW = gfx_w; SH = gfx_h; back = gfx_buf;
    gpu_reset(); check(gpu_scene, "GPU compositor unavailable");
    wallpaper = malloc(SW * SH * 4); check(wallpaper != NULL, "wallpaper");
    for (int y = 0; y < SH; y++) for (int x = 0; x < SW; x++)
        wallpaper[y * SW + x] = RGB(35 + x * 30 / SW, 28 + y * 40 / SH, 52);
    load_apps(); dock_build(); update_clock();
    mouse_x = 800; mouse_y = 650; focus = 1; nz = 2;
    for (int k = 0; k < 2; k++) {
        struct win *w = &wins[k];
        w->used = true; w->w = w->bw = 400; w->h = w->bh = 240;
        w->x = 130 + k * 250; w->y = 120 + k * 140;
        w->resizable = true; w->app = -1; w->gpu_dirty = true;
        strcpy(w->title, k ? "GPU alpha and clipping" : "Retained window texture");
        w->px = malloc(w->bw * w->bh * 4); check(w->px != NULL, "window buffer");
        for (int y = 0; y < w->bh; y++) for (int x = 0; x < w->bw; x++)
            w->px[y * w->bw + x] = k ? RGB(230, 120 + y / 3, 45) : RGB(45, 80 + x / 4, 160);
        zorder[k] = k;
    }
    compare("initial");
    printf("SMOOTHBENCH DRAG BEGIN\n");
    unsigned drag_begin=ticks();
    for(int frame=0;frame<100;frame++) {
        gfx_frame_begin();move_window(&wins[1],380+(frame&1)*16,260+(frame&1)*8);
        flush();gfx_frame_end();
    }
    printf("SMOOTHBENCH DRAG END %u ms\n",ticks()-drag_begin);
    wins[1].x = -35; wins[1].y = 190;
    compare("clipped move");
    struct win *w = &wins[0];
    for (int y = 17; y < 25; y++) for (int x = 23; x < 39; x++) w->px[y * w->bw + x] = RGB(220, 10, 90);
    w->gpu_dirty = true; w->gpu_damage = mkrect(23, 17, 16, 8);
    compare("partial update");
    wins[1].minimized = true;
    compare("minimize");
    hover_top = 1; kb_lang = 1; hover_dock = n_dock; mouse_x = 600;
    compare("shell state update");
    sysmenu_open = true; hover_sm = 1;
    compare("menu update");
    sysmenu_open = false; hover_dock = -1;
    compare("menu hidden");
    gfx_cursor_shape(GFX_CURSOR_RESIZE_H);
    compare("cursor shape update");
    w->w = 300;
    compare("smaller window with retained buffer");
    for (int y=17; y<25; y++) for (int x=350; x<366; x++) w->px[y*w->bw+x] = RGB(10,220,90);
    struct msg update = {0}; update.type=WM_UPDATE; update.from=w->owner;
    update.a=0; update.b=350; update.c=17; update.d=(8<<16)|16;
    handle_msg(&update);
    compare("damage outside current window");
    w->w = 400;
    compare("retained buffer exposed again");
    printf("GPUSCENE SOFTWARE BEGIN\n");
    unsigned begin = ticks();
    for (int i = 0; i < 100; i++) { compose(mkrect(0,0,SW,SH)); gfx_frame_begin(); gfx_flip(); gfx_frame_end(); }
    unsigned cpu_ms = ticks() - begin;
    printf("GPUSCENE SOFTWARE END\n");
    printf("GPUSCENE GPU BEGIN\n");
    begin = ticks();
    for (int i = 0; i < 100; i++) { gfx_frame_begin(); check(gpu_compose(mkrect(0,0,SW,SH)), "benchmark compose"); gfx_frame_end(); }
    unsigned gpu_ms = ticks() - begin;
    printf("GPUSCENE GPU END\n");
    printf("GPUSCENE BENCH 100 frames software %u ms GPU %u ms\n", cpu_ms, gpu_ms);
    effects_enabled=true;
    struct rect to=window_rect(w),from=to;
    from.x+=8;from.y+=8;from.w-=16;from.h-=16;
    motion_start(w,from,to,0,255,150);effect_run("animated open");
    minimize(0);check(wins[0].motion,"minimize animation started");effect_run("animated minimize");
    restore(0);effect_run("animated restore");
    maximize(0,true);effect_run("animated maximize");
    maximize(0,false);effect_run("animated unmaximize");
    remove_window(0);check(wins[0].used && wins[0].closing,"close defers resource release");
    effect_run("animated close");check(!wins[0].used,"close releases window");
    for (int k = 0; k < 2; k++) free(wins[k].px);
    free(wallpaper); gfx_close(); printf("GPUSCENE PASS\n");
    return 0;
}
