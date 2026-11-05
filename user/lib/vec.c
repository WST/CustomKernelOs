/* vec.c — сглаженная векторная графика: контуры (линии и кривые Безье),
 * скруглённые прямоугольники, круги, градиенты, полупрозрачность и тени.
 *
 * Как рисуется контур. Кривые режем на короткие отрезки, отрезки храним в
 * «фиксированной точке» (1/256 пикселя). Каждую строку пикселей проходим 16
 * раз (16 подстрок); на подстроке находим, где её пересекают отрезки, и по
 * правилу «ненулевой обмотки» получаем отрезки «внутри». Их вклад копим с
 * точностью до 1/256 пикселя по горизонтали. Итог — доля покрытия каждого
 * пикселя (0..255), и цвет смешивается с фоном в этой пропорции. Отсюда
 * гладкие края без «лесенки».
 *
 * Только целые числа (и никакого long long) — файл собирается и gcc, и cc
 * внутри MyOS. */
#include "gfx.h"

#define FIX 256                         /* единица: 1/256 пикселя */

/* ------------------------------------------------------------ смешивание */

color_t gfx_mix(color_t dst, color_t src, int a) {
    if (a <= 0) return dst;
    if (a >= 255) return src;
    unsigned k = a + (a >> 7), nk = 256 - k;
    unsigned rb = ((src & 0xFF00FF) * k + (dst & 0xFF00FF) * nk) >> 8;
    unsigned g = ((src & 0xFF00) * k + (dst & 0xFF00) * nk) >> 8;
    return (rb & 0xFF00FF) | (g & 0xFF00);
}

void gfx_blend_pixel(int x, int y, color_t c, int a) {
    int x0, y0, x1, y1;
    gfx_bounds(&x0, &y0, &x1, &y1);
    if (x < x0 || y < y0 || x >= x1 || y >= y1) return;
    color_t *p = gfx_buf + y * gfx_w + x;
    *p = gfx_mix(*p, c, a);
}

void gfx_fill_rect_a(int x, int y, int w, int h, color_t c, int a) {
    int x0, y0, x1, y1;
    if (a >= 255) { gfx_fill_rect(x, y, w, h, c); return; }
    if (a <= 0) return;
    gfx_bounds(&x0, &y0, &x1, &y1);
    if (x > x0) x0 = x;
    if (y > y0) y0 = y;
    if (x + w < x1) x1 = x + w;
    if (y + h < y1) y1 = y + h;
    unsigned k = a + (a >> 7), nk = 256 - k;
    unsigned srb = (c & 0xFF00FF) * k, sg = (c & 0xFF00) * k;
    for (int j = y0; j < y1; j++) {
        color_t *p = gfx_buf + j * gfx_w;
        for (int i = x0; i < x1; i++) {
            color_t d = p[i];
            p[i] = (((srb + (d & 0xFF00FF) * nk) >> 8) & 0xFF00FF) | (((sg + (d & 0xFF00) * nk) >> 8) & 0xFF00);
        }
    }
}

/* вертикальный градиент */
void gfx_gradient(int x, int y, int w, int h, color_t top, color_t bottom) {
    if (h <= 0) return;
    for (int j = 0; j < h; j++) gfx_fill_rect(x, y + j, w, 1, gfx_mix(top, bottom, h > 1 ? j * 255 / (h - 1) : 0));
}

/* ------------------------------------------------------------ контуры */

static int fx(gfx_path *p, int v, int o) { return o * FIX + v * p->num * FIX / p->den; }

void path_begin(gfx_path *p, int ox, int oy, int num, int den) {
    memset(p, 0, sizeof(*p));
    p->ox = ox; p->oy = oy;
    p->num = num; p->den = den > 0 ? den : 1;
    p->minx = p->miny = 0x7FFFFFFF;
    p->maxx = p->maxy = -0x7FFFFFFF;
}

void path_free(gfx_path *p) {
    if (p->e) free(p->e);
    p->e = NULL;
    p->n = p->cap = 0;
}

static void add_edge(gfx_path *p, int x0, int y0, int x1, int y1, int dir) {
    if (p->n == p->cap) {
        int nc = p->cap ? p->cap * 2 : 64;
        int *ne = realloc(p->e, nc * 5 * sizeof(int));
        if (!ne) return;
        p->e = ne;
        p->cap = nc;
    }
    int *e = p->e + p->n * 5;
    e[0] = x0; e[1] = y0; e[2] = x1; e[3] = y1; e[4] = dir;
    p->n++;
}

/* отрезок в единицах FIX */
void path_fix_line(gfx_path *p, int x0, int y0, int x1, int y1) {
    if (x0 < p->minx) p->minx = x0;
    if (x1 < p->minx) p->minx = x1;
    if (x0 > p->maxx) p->maxx = x0;
    if (x1 > p->maxx) p->maxx = x1;
    if (y0 < p->miny) p->miny = y0;
    if (y1 < p->miny) p->miny = y1;
    if (y0 > p->maxy) p->maxy = y0;
    if (y1 > p->maxy) p->maxy = y1;
    if (y0 == y1) return;
    int dir = 1;
    if (y0 > y1) { int t = x0; x0 = x1; x1 = t; t = y0; y0 = y1; y1 = t; dir = -1; }
    int dy = y1 - y0;
    if (dy <= 2048) { add_edge(p, x0, y0, x1, y1, dir); return; }
    /* длинный отрезок режем: тогда dx * dy не переполнит int при пересечениях */
    int k = (dy + 2047) / 2048, px = x0, py = y0;
    for (int i = 1; i <= k; i++) {
        int nx = x0 + (x1 - x0) / k * i + (x1 - x0) % k * i / k;
        int ny = y0 + dy / k * i + dy % k * i / k;
        if (i == k) { nx = x1; ny = y1; }
        add_edge(p, px, py, nx, ny, dir);
        px = nx; py = ny;
    }
}

static void line_to_fix(gfx_path *p, int x, int y) {
    path_fix_line(p, p->cx, p->cy, x, y);
    p->cx = x; p->cy = y;
}

void path_fix_lineto(gfx_path *p, int x, int y) { line_to_fix(p, x, y); }

void path_fix_move(gfx_path *p, int x, int y) {
    if (p->open) path_close(p);
    p->cx = p->sx = x;
    p->cy = p->sy = y;
    p->open = 1;
}

void path_close(gfx_path *p) {
    if (p->open && (p->cx != p->sx || p->cy != p->sy)) line_to_fix(p, p->sx, p->sy);
    p->open = 0;
}

static int iabs(int v) { return v < 0 ? -v : v; }

static int isqrt(unsigned v) {
    unsigned r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else r >>= 1;
        b >>= 2;
    }
    return (int)r;
}

/* делим кривую пополам depth раз (середины считаются точно, без переполнений) */
static void quad_rec(gfx_path *p, int x0, int y0, int x1, int y1, int x2, int y2, int depth) {
    if (depth <= 0) { line_to_fix(p, x2, y2); return; }
    int ax = (x0 + x1) >> 1, ay = (y0 + y1) >> 1, bx = (x1 + x2) >> 1, by = (y1 + y2) >> 1;
    int mx = (ax + bx) >> 1, my = (ay + by) >> 1;
    quad_rec(p, x0, y0, ax, ay, mx, my, depth - 1);
    quad_rec(p, mx, my, bx, by, x2, y2, depth - 1);
}

static int depth_for(int dd, int div) {
    int n = isqrt(dd / div) + 1, d = 0;
    while ((1 << d) < n && d < 7) d++;
    return d;
}

void path_fix_quad(gfx_path *p, int x1, int y1, int x2, int y2) {
    int dd = iabs(p->cx - 2 * x1 + x2) + iabs(p->cy - 2 * y1 + y2);
    quad_rec(p, p->cx, p->cy, x1, y1, x2, y2, depth_for(dd, 96));
}

static void cubic_rec(gfx_path *p, int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3, int depth) {
    if (depth <= 0) { line_to_fix(p, x3, y3); return; }
    int ax = (x0 + x1) >> 1, ay = (y0 + y1) >> 1;
    int bx = (x1 + x2) >> 1, by = (y1 + y2) >> 1;
    int cx = (x2 + x3) >> 1, cy = (y2 + y3) >> 1;
    int dx = (ax + bx) >> 1, dy = (ay + by) >> 1;
    int ex = (bx + cx) >> 1, ey = (by + cy) >> 1;
    int mx = (dx + ex) >> 1, my = (dy + ey) >> 1;
    cubic_rec(p, x0, y0, ax, ay, dx, dy, mx, my, depth - 1);
    cubic_rec(p, mx, my, ex, ey, cx, cy, x3, y3, depth - 1);
}

void path_fix_cubic(gfx_path *p, int x1, int y1, int x2, int y2, int x3, int y3) {
    int d1 = iabs(p->cx - 2 * x1 + x2) + iabs(p->cy - 2 * y1 + y2);
    int d2 = iabs(x1 - 2 * x2 + x3) + iabs(y1 - 2 * y2 + y3);
    cubic_rec(p, p->cx, p->cy, x1, y1, x2, y2, x3, y3, depth_for(d1 > d2 ? d1 : d2, 32));
}

/* то же в единицах пользователя: X = ox + x * num / den пикселей */
void path_move(gfx_path *p, int x, int y) { path_fix_move(p, fx(p, x, p->ox), fx(p, y, p->oy)); }
void path_line(gfx_path *p, int x, int y) {
    if (!p->open) path_move(p, x, y);
    else line_to_fix(p, fx(p, x, p->ox), fx(p, y, p->oy));
}
void path_quad(gfx_path *p, int cx, int cy, int x, int y) {
    path_fix_quad(p, fx(p, cx, p->ox), fx(p, cy, p->oy), fx(p, x, p->ox), fx(p, y, p->oy));
}
void path_cubic(gfx_path *p, int c1x, int c1y, int c2x, int c2y, int x, int y) {
    path_fix_cubic(p, fx(p, c1x, p->ox), fx(p, c1y, p->oy), fx(p, c2x, p->ox), fx(p, c2y, p->oy),
                   fx(p, x, p->ox), fx(p, y, p->oy));
}

void path_rect(gfx_path *p, int x, int y, int w, int h) {
    path_move(p, x, y);
    path_line(p, x + w, y);
    path_line(p, x + w, y + h);
    path_line(p, x, y + h);
    path_close(p);
}

/* четверть окружности кубической кривой: контрольные точки на 0.5523 радиуса */
#define KAPPA(r) ((r) * 4477 / 10000)

void path_round_rect_ex(gfx_path *p, int x, int y, int w, int h, int r, int corners) {
    if (r * 2 > w) r = w / 2;
    if (r * 2 > h) r = h / 2;
    if (r <= 0) { path_rect(p, x, y, w, h); return; }
    int k = KAPPA(r);
    int rt = corners & 1 ? r : 0, rr = corners & 2 ? r : 0, rb = corners & 4 ? r : 0, rl = corners & 8 ? r : 0;
    path_move(p, x + rt, y);
    path_line(p, x + w - rr, y);
    if (rr) path_cubic(p, x + w - k, y, x + w, y + k, x + w, y + r);
    path_line(p, x + w, y + h - rb);
    if (rb) path_cubic(p, x + w, y + h - k, x + w - k, y + h, x + w - r, y + h);
    path_line(p, x + rl, y + h);
    if (rl) path_cubic(p, x + k, y + h, x, y + h - k, x, y + h - r);
    path_line(p, x, y + rt);
    if (rt) path_cubic(p, x, y + k, x + k, y, x + r, y);
    path_close(p);
}

void path_round_rect(gfx_path *p, int x, int y, int w, int h, int r) { path_round_rect_ex(p, x, y, w, h, r, 15); }

void path_ellipse(gfx_path *p, int x, int y, int w, int h) {
    int rx = w / 2, ry = h / 2, cx = x + rx, cy = y + ry;
    int kx = rx - KAPPA(rx), ky = ry - KAPPA(ry);         /* 0.5523 * r */
    path_move(p, cx + rx, cy);
    path_cubic(p, cx + rx, cy + ky, cx + kx, cy + ry, cx, cy + ry);
    path_cubic(p, cx - kx, cy + ry, cx - rx, cy + ky, cx - rx, cy);
    path_cubic(p, cx - rx, cy - ky, cx - kx, cy - ry, cx, cy - ry);
    path_cubic(p, cx + kx, cy - ry, cx + rx, cy - ky, cx + rx, cy);
    path_close(p);
}

/* ------------------------------------------------------------ растеризация */

enum { PAINT_SOLID, PAINT_GRAD, PAINT_MASK };

static int *r_cov, *r_dif, r_cap;
static int *r_act, r_act_cap;
static int r_xs[512];

static int ensure_rows(int w) {
    if (w + 2 <= r_cap) return 1;
    int n = w + 64;
    int *a = realloc(r_cov, n * sizeof(int)), *b;
    if (!a) return 0;
    r_cov = a;
    b = realloc(r_dif, n * sizeof(int));
    if (!b) return 0;
    r_dif = b;
    r_cap = n;
    return 1;
}

/* сортировка рёбер по верхнему y (Шелл) */
static void sort_edges(gfx_path *p) {
    int n = p->n, *e = p->e, t[5];
    for (int gap = n / 2; gap > 0; gap /= 2)
        for (int i = gap; i < n; i++) {
            for (int k = 0; k < 5; k++) t[k] = e[i * 5 + k];
            int j = i;
            while (j >= gap && e[(j - gap) * 5 + 1] > t[1]) {
                for (int k = 0; k < 5; k++) e[j * 5 + k] = e[(j - gap) * 5 + k];
                j -= gap;
            }
            for (int k = 0; k < 5; k++) e[j * 5 + k] = t[k];
        }
}

/* Общий растеризатор. mode: заливка цветом, градиентом, или запись маски
 * (mask: w x h байт, начало в пикселе (mx, my) — для букв и значков). */
static void raster(gfx_path *p, int mode, color_t c0, color_t c1, int gy0, int gy1, int alpha,
                   uint8_t *mask, int mx, int my, int mw, int mh) {
    if (p->open) path_close(p);
    if (!p->n || alpha <= 0) return;
    int bx0, by0, bx1, by1;
    if (mode == PAINT_MASK) { bx0 = mx; by0 = my; bx1 = mx + mw; by1 = my + mh; }
    else gfx_bounds(&bx0, &by0, &bx1, &by1);
    int px0 = p->minx >> 8, py0 = p->miny >> 8;
    int px1 = (p->maxx + 255) >> 8, py1 = (p->maxy + 255) >> 8;
    if (px0 < bx0) px0 = bx0;
    if (py0 < by0) py0 = by0;
    if (px1 > bx1) px1 = bx1;
    if (py1 > by1) py1 = by1;
    if (px0 >= px1 || py0 >= py1) return;
    int W = px1 - px0;
    if (!ensure_rows(W)) return;
    if (r_act_cap < p->n) {
        int *na = realloc(r_act, p->n * sizeof(int));
        if (!na) return;
        r_act = na;
        r_act_cap = p->n;
    }
    sort_edges(p);
    int *e = p->e, next = 0, nact = 0;
    int lo = px0 * FIX, hi = px1 * FIX;
    unsigned ak = alpha + (alpha >> 7);
    color_t col = c0;

    for (int y = py0; y < py1; y++) {
        int ytop = y * FIX, ybot = ytop + FIX;
        /* активные рёбра: добавить начавшиеся, убрать закончившиеся */
        while (next < p->n && e[next * 5 + 1] < ybot) r_act[nact++] = next++;
        int k = 0;
        for (int i = 0; i < nact; i++) if (e[r_act[i] * 5 + 3] > ytop) r_act[k++] = r_act[i];
        nact = k;
        if (!nact) continue;
        memset(r_cov, 0, (W + 2) * sizeof(int));
        memset(r_dif, 0, (W + 2) * sizeof(int));
        int any = 0;
        for (int s = 0; s < 16; s++) {
            int ys = ytop + s * 16 + 8, n = 0;
            for (int i = 0; i < nact && n < 510; i++) {
                int *q = e + r_act[i] * 5;
                if (q[1] > ys || q[3] <= ys) continue;
                int x = q[0] + (q[2] - q[0]) * (ys - q[1]) / (q[3] - q[1]);
                int v = x * 2 + (q[4] > 0 ? 1 : 0);      /* x и направление в одном числе */
                int j = n++;
                while (j > 0 && r_xs[j - 1] > v) { r_xs[j] = r_xs[j - 1]; j--; }
                r_xs[j] = v;
            }
            int wind = 0, start = 0;
            for (int i = 0; i < n; i++) {
                int x = r_xs[i] >> 1;
                if (wind == 0) start = x;
                wind += (r_xs[i] & 1) ? 1 : -1;
                if (wind != 0) continue;
                int a = start, b = x;               /* отрезок «внутри» */
                if (a < lo) a = lo;
                if (b > hi) b = hi;
                if (b <= a) continue;
                a -= lo; b -= lo;
                int pa = a >> 8, pb = b >> 8;
                any = 1;
                if (pa == pb) r_cov[pa] += b - a;
                else {
                    r_cov[pa] += 256 - (a & 255);
                    r_dif[pa + 1] += 256;
                    r_dif[pb] -= 256;
                    r_cov[pb] += b & 255;
                }
            }
        }
        if (!any) continue;
        if (mode == PAINT_MASK) {
            uint8_t *row = mask + (y - my) * mw + (px0 - mx);
            int run = 0;
            for (int i = 0; i < W; i++) {
                run += r_dif[i];
                int v = (r_cov[i] + run) >> 4;
                row[i] = v > 255 ? 255 : v;
            }
            continue;
        }
        if (mode == PAINT_GRAD) {
            int t = gy1 > gy0 ? (y - gy0) * 255 / (gy1 - gy0) : 0;
            if (t < 0) t = 0;
            if (t > 255) t = 255;
            col = gfx_mix(c0, c1, t);
        }
        color_t *row = gfx_buf + y * gfx_w + px0;
        int run = 0;
        for (int i = 0; i < W; i++) {
            run += r_dif[i];
            int v = (r_cov[i] + run) >> 4;
            if (v <= 0) continue;
            if (v > 255) v = 255;
            v = (v * ak) >> 8;
            row[i] = gfx_mix(row[i], col, v);
        }
    }
}

void path_fill(gfx_path *p, color_t c, int alpha) { raster(p, PAINT_SOLID, c, c, 0, 0, alpha, NULL, 0, 0, 0, 0); }

void path_fill_grad(gfx_path *p, color_t top, int y0, color_t bottom, int y1, int alpha) {
    raster(p, PAINT_GRAD, top, bottom, y0, y1, alpha, NULL, 0, 0, 0, 0);
}

void path_mask(gfx_path *p, uint8_t *mask, int x, int y, int w, int h) {
    raster(p, PAINT_MASK, 0, 0, 0, 0, 255, mask, x, y, w, h);
}

/* ------------------------------------------------------------ готовые фигуры */

void gfx_round_rect_ex(int x, int y, int w, int h, int r, int corners, color_t c, int alpha) {
    gfx_path p;
    if (w <= 0 || h <= 0) return;
    path_begin(&p, 0, 0, 1, 1);
    path_round_rect_ex(&p, x, y, w, h, r, corners);
    path_fill(&p, c, alpha);
    path_free(&p);
}

void gfx_round_rect(int x, int y, int w, int h, int r, color_t c) { gfx_round_rect_ex(x, y, w, h, r, 15, c, 255); }
void gfx_round_rect_a(int x, int y, int w, int h, int r, color_t c, int alpha) { gfx_round_rect_ex(x, y, w, h, r, 15, c, alpha); }

/* рамка толщиной 1 пиксель + заливка (как у кнопок и полей ввода) */
void gfx_round_frame(int x, int y, int w, int h, int r, color_t border, color_t fill) {
    gfx_path p;
    if (w <= 2 || h <= 2) return;
    path_begin(&p, 0, 0, 1, 16);
    path_round_rect(&p, x * 16, y * 16, w * 16, h * 16, r * 16);
    path_fill(&p, border, 255);
    path_free(&p);
    path_begin(&p, 0, 0, 1, 16);
    path_round_rect(&p, x * 16 + 16, y * 16 + 16, w * 16 - 32, h * 16 - 32, r * 16 - 12);
    path_fill(&p, fill, 255);
    path_free(&p);
}

void gfx_ellipse(int x, int y, int w, int h, color_t c, int alpha) {
    gfx_path p;
    path_begin(&p, 0, 0, 1, 16);
    path_ellipse(&p, x * 16, y * 16, w * 16, h * 16);
    path_fill(&p, c, alpha);
    path_free(&p);
}

/* круг по центру (cx, cy) с радиусом r — в 1/16 пикселя, чтобы ставить точно */
void gfx_disc16(int cx, int cy, int r, color_t c, int alpha) {
    gfx_path p;
    path_begin(&p, 0, 0, 1, 16);
    path_ellipse(&p, cx - r, cy - r, 2 * r, 2 * r);
    path_fill(&p, c, alpha);
    path_free(&p);
}

/* линия толщиной t/16 пикселя (координаты тоже в 1/16) — прямоугольник вдоль отрезка */
void gfx_line16(int x0, int y0, int x1, int y1, int t, color_t c, int alpha) {
    int dx = x1 - x0, dy = y1 - y0, len = isqrt(dx * dx + dy * dy);
    if (!len) return;
    int nx = -dy * t / (2 * len), ny = dx * t / (2 * len);
    gfx_path p;
    path_begin(&p, 0, 0, 1, 16);
    path_move(&p, x0 + nx, y0 + ny);
    path_line(&p, x1 + nx, y1 + ny);
    path_line(&p, x1 - nx, y1 - ny);
    path_line(&p, x0 - nx, y0 - ny);
    path_close(&p);
    path_fill(&p, c, alpha);
    path_free(&p);
}

/* ------------------------------------------------------------ тень */

/* Мягкая тень под прямоугольником (x, y, w, h) со скруглением r: размытие
 * blur пикселей, сдвиг вниз dy, прозрачность alpha. Внутри окна hole (если
 * hole_h > 0) не рисуем — там всё равно будет окно. */
void gfx_shadow(int x, int y, int w, int h, int r, int blur, int dy, int alpha,
                int hx, int hy, int hw, int hh) {
    static uint8_t lut[256];
    static int lut_blur = -1;
    if (blur < 1) blur = 1;
    if (blur > 60) blur = 60;
    if (lut_blur != blur) {
        /* d — расстояние до фигуры в 1/4 пикселя, от -blur до +blur -> 0..255 */
        for (int i = 0; i < 256; i++) {
            int t = i;                                    /* 0..255 = -blur..+blur */
            int sm = t * t / 255 * (765 - 2 * t) / 255;   /* smoothstep */
            lut[i] = (uint8_t)(255 - sm);
        }
        lut_blur = blur;
    }
    int bx0, by0, bx1, by1;
    gfx_bounds(&bx0, &by0, &bx1, &by1);
    y += dy;
    int X0 = x - blur, Y0 = y - blur, X1 = x + w + blur, Y1 = y + h + blur;
    if (X0 < bx0) X0 = bx0;
    if (Y0 < by0) Y0 = by0;
    if (X1 > bx1) X1 = bx1;
    if (Y1 > by1) Y1 = by1;
    int ix0 = x + r, ix1 = x + w - r, iy0 = y + r, iy1 = y + h - r;
    for (int py = Y0; py < Y1; py++) {
        color_t *row = gfx_buf + py * gfx_w;
        int ddy = py * 4 + 2 < iy0 * 4 ? iy0 * 4 - (py * 4 + 2) : py * 4 + 2 > iy1 * 4 ? py * 4 + 2 - iy1 * 4 : 0;
        int s0 = X1, s1 = X1;                             /* пропуск: под окном */
        if (hh > 0 && py >= hy && py < hy + hh) {
            if (py < hy + r) { s0 = hx + r; s1 = hx + hw - r; }
            else { s0 = hx; s1 = hx + hw; }
        }
        for (int px = X0; px < X1; px++) {
            if (px >= s0 && px < s1) { px = s1 - 1; continue; }
            int ddx = px * 4 + 2 < ix0 * 4 ? ix0 * 4 - (px * 4 + 2) : px * 4 + 2 > ix1 * 4 ? px * 4 + 2 - ix1 * 4 : 0;
            int d;
            if (!ddx) d = ddy;
            else if (!ddy) d = ddx;
            else d = isqrt(ddx * ddx + ddy * ddy);
            d -= r * 4;                                   /* <0 — внутри фигуры */
            int t = (d + blur * 4) * 255 / (blur * 8);
            if (t >= 255) continue;
            if (t < 0) t = 0;
            int a = lut[t] * alpha / 255;
            if (a > 0) row[px] = gfx_mix(row[px], 0, a);
        }
    }
}

/* ------------------------------------------------------------ картинки с прозрачностью */

/* маска (w x h, 0..255) — залить цветом c */
void gfx_draw_mask(int x, int y, const uint8_t *m, int w, int h, color_t c, int alpha) {
    int bx0, by0, bx1, by1;
    gfx_bounds(&bx0, &by0, &bx1, &by1);
    unsigned ak = alpha + (alpha >> 7);
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < by0 || py >= by1) continue;
        color_t *row = gfx_buf + py * gfx_w;
        const uint8_t *mr = m + j * w;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            if (!mr[i] || px < bx0 || px >= bx1) continue;
            row[px] = gfx_mix(row[px], c, (mr[i] * ak) >> 8);
        }
    }
}

/* картинка 0xAARRGGBB (AA — непрозрачность, 0 — пусто) */
void gfx_draw_argb(int x, int y, const uint32_t *img, int w, int h) {
    int bx0, by0, bx1, by1;
    gfx_bounds(&bx0, &by0, &bx1, &by1);
    for (int j = 0; j < h; j++) {
        int py = y + j;
        if (py < by0 || py >= by1) continue;
        color_t *row = gfx_buf + py * gfx_w;
        const uint32_t *ir = img + j * w;
        for (int i = 0; i < w; i++) {
            int px = x + i;
            unsigned a = ir[i] >> 24;
            if (!a || px < bx0 || px >= bx1) continue;
            row[px] = gfx_mix(row[px], ir[i] & 0xFFFFFF, a);
        }
    }
}

/* Нарисовать что-то «на прозрачном»: рисуем на чёрном и на белом фоне, и по
 * разнице восстанавливаем непрозрачность каждого пикселя. Так любые функции
 * gfx_* (контуры, текст) годятся для значков с прозрачными краями.
 *   gfx_canvas_begin(&cv, 48, 48); ... рисуем ... img = gfx_canvas_end(&cv); */
void gfx_canvas_begin(struct gfx_canvas *cv, int w, int h, int pass) {
    if (pass == 0) {
        cv->w = w; cv->h = h;
        cv->save_buf = gfx_buf; cv->save_w = gfx_w; cv->save_h = gfx_h;
        cv->black = malloc(w * h * 4);
        cv->white = malloc(w * h * 4);
    }
    color_t *b = pass == 0 ? cv->black : cv->white;
    for (int i = 0; i < w * h; i++) b[i] = pass == 0 ? 0 : 0xFFFFFF;
    if (pass == 0) cv->saved_clip = gfx_clip_get(&cv->cx, &cv->cy, &cv->cw, &cv->ch);
    gfx_no_clip();
    gfx_buf = b; gfx_w = w; gfx_h = h;
}

uint32_t *gfx_canvas_end(struct gfx_canvas *cv) {
    gfx_buf = cv->save_buf; gfx_w = cv->save_w; gfx_h = cv->save_h;
    if (cv->saved_clip) gfx_set_clip(cv->cx, cv->cy, cv->cw, cv->ch);
    int n = cv->w * cv->h;
    uint32_t *out = (uint32_t *)cv->black;
    for (int i = 0; i < n; i++) {
        color_t b = cv->black[i], wt = cv->white[i];
        int a = 255 - (((wt >> 8) & 255) - ((b >> 8) & 255));     /* по зелёному каналу */
        int a2 = 255 - ((wt & 255) - (b & 255));
        int a3 = 255 - (((wt >> 16) & 255) - ((b >> 16) & 255));
        a = (a + a2 + a3) / 3;
        if (a <= 0) { out[i] = 0; continue; }
        if (a > 255) a = 255;
        /* цвет = чёрный_результат / a */
        int r = ((b >> 16) & 255) * 255 / a, g = ((b >> 8) & 255) * 255 / a, bl = (b & 255) * 255 / a;
        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (bl > 255) bl = 255;
        out[i] = (unsigned)a << 24 | (unsigned)r << 16 | (unsigned)g << 8 | (unsigned)bl;
    }
    free(cv->white);
    return out;
}
