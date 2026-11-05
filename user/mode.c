/* mode — видеокарта и разрешение экрана.
 *
 *   mode               что за карта, какой режим, какие разрешения есть
 *   mode 1280x720      сменить разрешение и запомнить в /etc/display
 *   mode cursor hw|sw  аппаратный или программный курсор на рабочем столе */
#include "gfx.h"
#include "win.h"

static void show(void) {
    struct gfx_info2 gi;
    if (gfx_info2(&gi) < 0) { printf("mode: no graphics (VGA text mode)\n"); return; }
    printf("Adapter:    %s, %u KB video memory\n", gi.driver, gi.vram_kb);
    printf("Resolution: %ux%u, %u bpp, %u bytes per line\n", gi.width, gi.height, gi.bpp, gi.pitch);
    printf("GPU 2D:     %s\n", (gi.caps & GFX_CAP_GPU_ON) ? "on (rect copy and fill on the card)" :
                            (gi.caps & GFX_CAP_GPU) ? "supported, not active yet" : "no (CPU)");
    printf("HW cursor:  %s\n", !(gi.caps & GFX_CAP_CURSOR) ? "not supported" : gi.hw_cursor ? "on" : "off");
    struct gfx_mode m[32];
    int n = gfx_modes(m, 32);
    printf("Modes:     ");
    for (int i = 0; i < n; i++)
        if (m[i].ok) printf(" %s%dx%d%s", (m[i].w == (int)gi.width && m[i].h == (int)gi.height) ? "[" : "",
                            m[i].w, m[i].h, (m[i].w == (int)gi.width && m[i].h == (int)gi.height) ? "]" : "");
    printf("\nUsage: mode WIDTHxHEIGHT | mode cursor hw|sw\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { show(); return 0; }
    struct gfx_info2 gi;
    if (gfx_info2(&gi) < 0) { dprintf(STDERR, "mode: no graphics\n"); return 1; }
    int cw = gi.width, ch = gi.height, hw = 0;
    gfx_load_config(&cw, &ch, &hw);
    if (!strcmp(argv[1], "cursor") && argc > 2) {
        hw = !strcmp(argv[2], "hw") || !strcmp(argv[2], "on");
        gfx_save_config(gi.width, gi.height, hw);
        int d = win_desktop_pid();
        if (d) {
            struct msg m;
            memset(&m, 0, sizeof(m));
            m.type = WM_DISPLAY;
            m.a = hw;
            msg_send(d, &m);
        }
        printf("cursor: %s\n", hw ? "hardware" : "software");
        return 0;
    }
    int w = atoi(argv[1]);
    char *x = strchr(argv[1], 'x');
    int h = x ? atoi(x + 1) : 0;
    if (w <= 0 || h <= 0) { dprintf(STDERR, "mode: expected WIDTHxHEIGHT, e.g. 1280x720\n"); return 1; }
    if (gfx_set_mode(w, h) < 0) {
        dprintf(STDERR, "mode: %dx%d is not supported (width must be a multiple of 8, max %ux%u)\n",
                w, h, gi.max_w, gi.max_h);
        return 1;
    }
    gfx_save_config(w, h, hw);
    printf("Switched to %dx%d (saved to /etc/display)\n", w, h);
    return 0;
}
