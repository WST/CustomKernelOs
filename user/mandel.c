/* mandel — множество Мандельброта.
 *   левая кнопка мыши — приблизить (x2) в точке   правая — отдалить
 *   стрелки — двигать   + / - — детальнее / грубее   R — сначала   Q, Esc — выход
 *
 * Считается на double: ядро сохраняет регистры FPU при переключении задач,
 * так что программы могут спокойно пользоваться плавающей точкой.
 * Рисуем в два прохода — сначала крупными квадратами 8x8, потом по пикселю, —
 * и между кусками проверяем мышь и клавиши, чтобы не «зависать» на время счёта. */
#include "gfx.h"

static double cx = -0.6, cy = 0.0, scale;     /* центр и размер пикселя */
static int max_iter = 200;
static color_t palette[1024];
static struct mouse m, prev;
static int pass, line;                        /* где сейчас рисуем */
static bool done;
static unsigned t_start, t_last;

static void make_palette(void) {
    /* плавный переход: тёмно-синий -> голубой -> белый -> жёлтый -> оранжевый -> тёмный */
    static const int stops[][3] = { {0, 7, 100}, {32, 107, 203}, {237, 255, 255},
                                    {255, 170, 0}, {120, 30, 0}, {0, 2, 0}, {0, 7, 100} };
    for (int i = 0; i < 1024; i++) {
        int seg = i * 6 / 1024, off = i * 6 % 1024;
        const int *a = stops[seg], *b = stops[seg + 1];
        palette[i] = RGB(a[0] + (b[0] - a[0]) * off / 1024,
                         a[1] + (b[1] - a[1]) * off / 1024,
                         a[2] + (b[2] - a[2]) * off / 1024);
    }
}

static color_t point(double x0, double y0) {
    double x = 0, y = 0, x2 = 0, y2 = 0;
    int i = 0;
    while (x2 + y2 <= 4.0 && i < max_iter) {
        y = 2 * x * y + y0;
        x = x2 - y2 + x0;
        x2 = x * x;
        y2 = y * y;
        i++;
    }
    if (i == max_iter) return C_BLACK;
    return palette[(i * 16) % 1024];
}

static void restart(void) {
    pass = 0; line = 0; done = false;
    t_start = ticks();
}

/* нарисовать ещё немного строк; -> сколько строк сделано */
static int render_some(void) {
    int rows = 0;
    unsigned t0 = ticks();
    while (!done && ticks() - t0 < 40) {      /* не дольше 40 мс за раз */
        int step = pass == 0 ? 8 : 1;         /* проход может смениться посреди вызова */
        double y0 = cy + (line - gfx_h / 2) * scale;
        for (int x = 0; x < gfx_w; x += step) {
            color_t c = point(cx + (x - gfx_w / 2) * scale, y0);
            if (step == 1) gfx_buf[line * gfx_w + x] = c;
            else gfx_fill_rect(x, line, step, step, c);
        }
        line += step;
        rows++;
        if (line >= gfx_h) {
            line = 0;
            if (++pass == 2) { done = true; t_last = ticks() - t_start; }
        }
    }
    return rows;
}

static void present(void) {
    char info[160];
    double zoom = (3.2 / gfx_w) / scale;
    snprintf(info, sizeof(info), " x %.6f  y %.6f  zoom %.0fx  iterations %d  %s ",
             cx, cy, zoom, max_iter, done ? "" : "(drawing...)");
    char time[40] = "";
    if (done) snprintf(time, sizeof(time), " %u ms ", t_last);
    /* панель поверх картинки — рисуем, показываем и тут же стираем из буфера не нужно:
     * строку под панелью мы всё равно перерисуем при следующем проходе */
    static color_t saved[1024 * 36];
    for (int y = 0; y < 36; y++) memcpy(saved + y * gfx_w, gfx_buf + y * gfx_w, gfx_w * 4);
    gfx_fill_rect(0, 0, gfx_w, 36, RGB(20, 20, 30));
    int w = gfx_text(4, 2, info, C_YELLOW, TRANSPARENT);
    gfx_text(4 + w, 2, time, C_GREEN, TRANSPARENT);
    gfx_text(4, 18, " LMB zoom in   RMB zoom out   arrows move   +/- detail   R reset   Q quit",
             RGB(170, 170, 190), TRANSPARENT);
    gfx_cursor_draw(m.x, m.y);
    gfx_flip();
    gfx_cursor_erase();
    for (int y = 0; y < 36; y++) memcpy(gfx_buf + y * gfx_w, saved + y * gfx_w, gfx_w * 4);
}

int main(void) {
    if (gfx_open() < 0) { printf("mandel: graphics mode is not available\n"); return 1; }
    scale = 3.2 / gfx_w;
    make_palette();
    gfx_clear(C_BLACK);
    restart();
    mouse_get(&m);
    prev = m;
    for (;;) {
        bool changed = false;
        int k;
        while ((k = readkey()) >= 0) {
            if (k == 'q' || k == 'Q' || k == 27) goto quit;
            if (k == KEY_LEFT)  { cx -= gfx_w / 8 * scale; changed = true; }
            if (k == KEY_RIGHT) { cx += gfx_w / 8 * scale; changed = true; }
            if (k == KEY_UP)    { cy -= gfx_h / 8 * scale; changed = true; }
            if (k == KEY_DOWN)  { cy += gfx_h / 8 * scale; changed = true; }
            if (k == '+' || k == '=') { max_iter = max_iter * 2 > 4000 ? 4000 : max_iter * 2; changed = true; }
            if (k == '-') { max_iter = max_iter / 2 < 50 ? 50 : max_iter / 2; changed = true; }
            if (k == 'r' || k == 'R') { cx = -0.6; cy = 0; scale = 3.2 / gfx_w; max_iter = 200; changed = true; }
        }
        prev = m;
        mouse_get(&m);
        bool pressed_l = (m.buttons & MOUSE_LEFT) && !(prev.buttons & MOUSE_LEFT);
        bool pressed_r = (m.buttons & MOUSE_RIGHT) && !(prev.buttons & MOUSE_RIGHT);
        if (pressed_l || pressed_r) {
            cx += (m.x - gfx_w / 2) * scale;    /* щелчок становится центром */
            cy += (m.y - gfx_h / 2) * scale;
            scale *= pressed_l ? 0.5 : 2.0;
            changed = true;
        }
        if (changed) restart();
        if (!done) { render_some(); present(); }
        else if (m.events != prev.events) present();   /* только курсор */
        else sleep_ms(10);
    }
quit:
    gfx_close();
    return 0;
}
