/* display — «Экран»: видеокарта, разрешение, аппаратный курсор.
 *
 * Щелчок по разрешению меняет его сразу для всей системы (ядро перестраивает
 * консоль и мышь, рабочий стол — себя) и запоминает в /etc/display, откуда
 * его прочитает ядро при следующей загрузке. */
#include "win.h"

#define W 452
#define H 340
#define COLS 4
#define BW 102
#define BH 30
#define GX 14
#define GY 128

static window *win;
static struct gfx_mode modes[32];
static int nmodes;
static int hw_cursor;
static char status[64];

static void mode_cell(int i, int *x, int *y) {
    *x = GX + (i % COLS) * (BW + 4);
    *y = GY + (i / COLS) * (BH + 4);
}

static void checkbox(int x, int y, bool on, bool enabled, const char *label) {
    ui_sunken(x, y, 16, 16, enabled ? C_WHITE : UI_FACE);
    if (on) {
        color_t c = enabled ? UI_ACCENT : UI_SHADOW;
        gfx_thick_line(x + 3, y + 8, x + 6, y + 12, 1, c);
        gfx_thick_line(x + 6, y + 12, x + 12, y + 4, 1, c);
    }
    gfx_text(x + 24, y, label, enabled ? UI_TEXT : UI_SHADOW, TRANSPARENT);
}

static int cursor_y(void) { return GY + ((nmodes + COLS - 1) / COLS) * (BH + 4) + 12; }

static void draw(void) {
    struct gfx_info2 gi;
    gfx_info2(&gi);
    gfx_clear(UI_FACE);
    for (int y = 0; y < 48; y++) gfx_fill_rect(0, y, W, 1, RGB(20 + y / 2, 70 + y * 2, 150 + y));
    gfx_text_scaled(16, 8, "Display", C_WHITE, 2);
    char line[96];
    snprintf(line, sizeof(line), "%s, %u MB video memory", gi.driver, gi.vram_kb / 1024);
    gfx_text(16, 58, line, UI_TEXT, TRANSPARENT);
    const char *gpu = (gi.caps & GFX_CAP_COMPOSITOR) ? "VirGL - textures, alpha blending, buffered frames"
                    : (gi.caps & GFX_CAP_GPU_ON) ? "on - copy and fill run on the video card"
                    : (gi.caps & GFX_CAP_GPU) ? "available, waiting for the card..."
                    : "none - the CPU draws everything";
    snprintf(line, sizeof(line), "GPU 2D: %s", gpu);
    gfx_text(16, 76, line, (gi.caps & GFX_CAP_GPU_ON) ? RGB(0, 120, 40) : UI_DARK, TRANSPARENT);
    snprintf(line, sizeof(line), "Resolution: %ux%u, %u bpp   (max %ux%u)", gi.width, gi.height, gi.bpp, gi.max_w, gi.max_h);
    gfx_text(16, 100, line, UI_TEXT, TRANSPARENT);
    for (int i = 0; i < nmodes; i++) {
        int x, y;
        mode_cell(i, &x, &y);
        char t[16];
        snprintf(t, sizeof(t), "%dx%d", modes[i].w, modes[i].h);
        bool cur = modes[i].w == (int)gi.width && modes[i].h == (int)gi.height;
        if (cur) {
            gfx_fill_rect(x, y, BW, BH, UI_ACCENT);
            ui_text_center(x, y, BW, BH, t, C_WHITE);
        } else if (modes[i].ok) {
            ui_button(x, y, BW, BH, t, false);
        } else {
            gfx_rect(x, y, BW, BH, UI_SHADOW);
            ui_text_center(x, y, BW, BH, t, UI_SHADOW);
        }
    }
    int cy = cursor_y();
    checkbox(GX, cy, gi.hw_cursor, (gi.caps & GFX_CAP_CURSOR) != 0,
             !(gi.caps & GFX_CAP_CURSOR) ? "Hardware mouse cursor (not supported)"
             : (gi.caps & GFX_CAP_ALPHA_CURSOR) ? "Hardware mouse cursor (card, with alpha)"
                                                : "Hardware mouse cursor (card, no alpha)");
    gfx_text(GX, H - 26, status, UI_DARK, TRANSPARENT);
    win_update(win);
}

static void save(void) {
    struct gfx_info2 gi;
    gfx_info2(&gi);
    gfx_save_config(gi.width, gi.height, hw_cursor);
}

static void click(int x, int y) {
    for (int i = 0; i < nmodes; i++) {
        int bx, by;
        mode_cell(i, &bx, &by);
        if (!ui_inside(x, y, bx, by, BW, BH)) continue;
        if (!modes[i].ok) { snprintf(status, sizeof(status), "%dx%d: not supported by this card", modes[i].w, modes[i].h); return; }
        if (gfx_set_mode(modes[i].w, modes[i].h) < 0)
            snprintf(status, sizeof(status), "Could not switch to %dx%d", modes[i].w, modes[i].h);
        else {
            save();
            snprintf(status, sizeof(status), "Switched to %dx%d and saved", modes[i].w, modes[i].h);
        }
        return;
    }
    struct gfx_info2 gi;
    gfx_info2(&gi);
    if (ui_inside(x, y, GX, cursor_y() - 2, W - 2 * GX, 20) && (gi.caps & GFX_CAP_CURSOR)) {
        hw_cursor = !gi.hw_cursor;
        struct msg m;
        memset(&m, 0, sizeof(m));
        m.type = WM_DISPLAY;
        m.a = hw_cursor;
        msg_send(win_desktop_pid(), &m);
        save();
        snprintf(status, sizeof(status), "Hardware cursor %s", hw_cursor ? "on" : "off");
    }
}

int main(void) {
    win = win_create("Display", W, H);
    if (!win) { dprintf(STDERR, "display: the desktop is not running\n"); return 1; }
    nmodes = gfx_modes(modes, 32);
    struct gfx_info2 gi;
    gfx_info2(&gi);
    hw_cursor = gi.hw_cursor;
    snprintf(status, sizeof(status), (gi.caps&GFX_CAP_FIXED_MODE)?
             "Mode switching unavailable. Run: dmesg | grep VBE":
             "Click a resolution to switch to it.");
    draw();
    struct event e;
    unsigned last = ticks();
    while (win_event(&e, 500)) {
        if (e.type == EV_CLOSE || e.type == EV_QUIT) break;
        if (e.type == EV_KEY && e.key == 27) break;
        if (e.type == EV_MOUSE_DOWN) { click(e.x, e.y); sleep_ms(80); draw(); }
        if (ticks() - last >= 500) { last = ticks(); draw(); }  /* GPU мог «проснуться», курсор — смениться */
    }
    win_destroy(win);
    return 0;
}
