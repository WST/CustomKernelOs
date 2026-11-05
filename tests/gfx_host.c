/* tests/gfx_host.c — сглаженная графика и шрифты на Linux: рисует пробную
 * картинку в PPM (stdout).  make gfxtest -> build/gfxtest.ppm */
#include "gfx.h"

int ttymode(int m) { return m; }

int main(int argc, char **argv) {
    font_set_dir(argc > 1 ? argv[1] : "fonts");
    gfx_w = 720; gfx_h = 520;
    gfx_buf = malloc(gfx_w * gfx_h * 4);
    gfx_gradient(0, 0, gfx_w, gfx_h, RGB(119, 33, 111), RGB(233, 84, 32));
    gfx_shadow(40, 40, 400, 300, 12, 18, 4, 110, 40, 40, 400, 300);
    gfx_round_rect_ex(40, 40, 400, 38, 12, 3, RGB(235, 235, 235), 255);
    gfx_fill_rect(40, 78, 400, 262, RGB(250, 250, 250));
    text_draw(FONT_UI_BOLD, 15, 180, 50, "Terminal — Files", RGB(46, 52, 54));
    gfx_disc16((40 + 400 - 22) * 16, (40 + 19) * 16, 12 * 16, RGB(218, 218, 218), 255);
    gfx_line16((418 - 4) * 16, 55 * 16, (418 + 4) * 16, 63 * 16, 24, RGB(46, 52, 54), 255);
    gfx_line16((418 + 4) * 16, 55 * 16, (418 - 4) * 16, 63 * 16, 24, RGB(46, 52, 54), 255);
    int y = 90;
    for (int sz = 11; sz <= 20; sz += 3) {
        text_draw(FONT_UI, sz, 56, y, "The quick brown fox — Съешь же ещё этих мягких булок", RGB(40, 40, 40));
        y += text_line_h(FONT_UI, sz) + 2;
    }
    text_draw(FONT_UI_MEDIUM, 14, 56, y, "Medium: AVATAR Wave Toyota 1234567890", RGB(40, 40, 40)); y += 20;
    gfx_text(56, y, "gfx_text: ls -la /bin  Привет, мир! {}[]<>", RGB(30, 30, 30), TRANSPARENT); y += 16;
    gfx_fill_rect(56, y, 360, 48, RGB(48, 10, 36));
    gfx_text(60, y + 2, "user@myos:~$ cc hello.c -o hello", RGB(138, 226, 52), TRANSPARENT);
    gfx_text(60, y + 18, "\xC9\xCD\xCD\xCD\xBB box \xB0\xB1\xB2\xDB", C_WHITE, TRANSPARENT);
    gfx_text_scaled(60 + 280, y + 2, "7", C_WHITE, 2);
    y += 60;
    gfx_round_frame(56, y, 100, 32, 6, RGB(205, 199, 194), RGB(246, 245, 244));
    text_draw(FONT_UI, 14, 56 + (100 - text_width(FONT_UI, 14, "Button")) / 2, y + 8, "Button", RGB(46, 52, 54));
    gfx_round_rect(170, y, 100, 32, 6, RGB(233, 84, 32));
    text_draw(FONT_UI_MEDIUM, 14, 170 + (100 - text_width(FONT_UI_MEDIUM, 14, "OK")) / 2, y + 8, "OK", C_WHITE);
    gfx_ellipse(290, y, 32, 32, RGB(52, 101, 164), 255);
    gfx_ellipse(330, y + 4, 60, 24, RGB(78, 154, 6), 180);
    text_draw(FONT_UI_BOLD, 40, 470, 380, "MyOS", C_WHITE);
    text_draw(FONT_UI, 13, 470, 430, "Activities", C_WHITE);
    gfx_cursor_draw(600, 200);
    printf("P6\n%d %d\n255\n", gfx_w, gfx_h);
    for (int i = 0; i < gfx_w * gfx_h; i++) { putchar(gfx_buf[i] >> 16 & 255); putchar(gfx_buf[i] >> 8 & 255); putchar(gfx_buf[i] & 255); }
    return 0;
}
