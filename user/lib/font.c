/* font.c — шрифты TrueType (.ttf) со сглаживанием.
 *
 * Шрифты лежат в /lib/fonts: Ubuntu (обычный, Medium, Bold) для интерфейса и
 * Ubuntu Mono для терминала и gfx_text. Файл читаем целиком, буквы достаём из
 * таблиц: cmap (символ -> номер глифа), loca/glyf (контуры: точки на кривой и
 * контрольные точки квадратичных кривых), hmtx (ширина), kern (пары «AV»).
 * Контур отдаём растеризатору из vec.c и получаем маску покрытия; маски
 * хранятся в кэше (шрифт, размер, глиф, сдвиг на 1/4 пикселя).
 *
 * «Хинтинг» простейший: масштаб по вертикали подгоняем так, чтобы высота
 * строчной «x» была целым числом пикселей, а базовая линия — на границе
 * пикселя. Это заметно чётче для мелкого текста. */
#include "gfx.h"

struct ttf {
    uint8_t *d;
    int len;
    int upem, asc, desc, gap, xh;
    int loca, glyf, hmtx, nhm, cmap, kern, nkern, locfmt, nglyphs;
};

static const char *font_names[FONT_COUNT] = {
    "Ubuntu-R.ttf", "Ubuntu-M.ttf", "Ubuntu-B.ttf", "UbuntuMono-R.ttf", "UbuntuMono-B.ttf",
};
static const char *font_dir = "/lib/fonts";

void font_set_dir(const char *dir) { font_dir = dir; }
static struct ttf fonts[FONT_COUNT];
static int font_state[FONT_COUNT];      /* 0 — не пробовали, 1 — есть, -1 — нет файла */

static int u16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static int s16(const uint8_t *p) { int v = p[0] << 8 | p[1]; return v >= 32768 ? v - 65536 : v; }
static unsigned u32(const uint8_t *p) { return (unsigned)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static int find_table(struct ttf *f, const char *tag) {
    int n = u16(f->d + 4);
    for (int i = 0; i < n; i++) {
        const uint8_t *t = f->d + 12 + i * 16;
        if (t[0] == tag[0] && t[1] == tag[1] && t[2] == tag[2] && t[3] == tag[3]) {
            unsigned off = u32(t + 8);
            return off < (unsigned)f->len ? (int)off : 0;
        }
    }
    return 0;
}

static uint8_t *read_file(const char *path, int *len) {
    int fd = open(path, O_READ);
    if (fd < 0) return NULL;
    int cap = 65536, n = 0;
    uint8_t *buf = malloc(cap);
    while (buf) {
        if (n == cap) {
            uint8_t *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); buf = NULL; break; }
            buf = nb;
            cap *= 2;
        }
        int r = read(fd, buf + n, cap - n);
        if (r <= 0) break;
        n += r;
    }
    close(fd);
    *len = n;
    return buf;
}

static int glyph_index(struct ttf *f, int cp);

static int load_font(int id) {
    if (id < 0 || id >= FONT_COUNT) return 0;
    if (font_state[id]) return font_state[id] > 0;
    font_state[id] = -1;
    struct ttf *f = &fonts[id];
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", font_dir, font_names[id]);
    f->d = read_file(path, &f->len);
    if (!f->d) return 0;
    if (f->len < 12) return 0;
    int head = find_table(f, "head"), hhea = find_table(f, "hhea"), maxp = find_table(f, "maxp");
    int cmap = find_table(f, "cmap");
    f->loca = find_table(f, "loca");
    f->glyf = find_table(f, "glyf");
    f->hmtx = find_table(f, "hmtx");
    if (!head || !hhea || !maxp || !cmap || !f->loca || !f->glyf || !f->hmtx) return 0;
    f->upem = u16(f->d + head + 18);
    f->locfmt = s16(f->d + head + 50);
    f->asc = s16(f->d + hhea + 4);
    f->desc = s16(f->d + hhea + 6);
    f->gap = s16(f->d + hhea + 8);
    f->nhm = u16(f->d + hhea + 34);
    f->nglyphs = u16(f->d + maxp + 4);
    /* cmap: ищем подтаблицу формата 4 (Unicode BMP) */
    int n = u16(f->d + cmap + 2);
    for (int i = 0; i < n; i++) {
        const uint8_t *r = f->d + cmap + 4 + i * 8;
        int plat = u16(r), enc = u16(r + 2);
        int off = cmap + (int)u32(r + 4);
        if (off + 4 > f->len || u16(f->d + off) != 4) continue;
        if ((plat == 3 && enc == 1) || plat == 0) { f->cmap = off; break; }
    }
    if (!f->cmap) return 0;
    /* kern, формат 0: число пар берём из заголовка (длина подтаблицы может переполниться) */
    int kern = find_table(f, "kern");
    if (kern && u16(f->d + kern) == 0 && u16(f->d + kern + 2) > 0) {
        const uint8_t *st = f->d + kern + 4;
        if ((u16(st + 4) >> 8) == 0) {
            f->nkern = u16(st + 6);
            f->kern = kern + 4 + 14;
            if (f->kern + f->nkern * 6 > f->len) f->nkern = 0;
        }
    }
    /* высота «x» — по контуру буквы x */
    f->xh = f->upem / 2;
    int gx = glyph_index(f, 'x');
    if (gx > 0) {
        int a = f->locfmt ? (int)u32(f->d + f->loca + gx * 4) : u16(f->d + f->loca + gx * 2) * 2;
        if (f->glyf + a + 10 <= f->len) f->xh = s16(f->d + f->glyf + a + 8);
    }
    if (f->xh <= 0) f->xh = f->upem / 2;
    font_state[id] = 1;
    return 1;
}

int font_available(int id) { return load_font(id); }

static int glyph_index(struct ttf *f, int cp) {
    if (cp < 0 || cp > 0xFFFF) return 0;
    const uint8_t *t = f->d + f->cmap;
    int segs = u16(t + 6) / 2;
    const uint8_t *ends = t + 14, *starts = ends + segs * 2 + 2;
    const uint8_t *deltas = starts + segs * 2, *ranges = deltas + segs * 2;
    int lo = 0, hi = segs - 1;
    while (lo < hi) {                                /* первый сегмент с end >= cp */
        int mid = (lo + hi) / 2;
        if (u16(ends + mid * 2) < cp) lo = mid + 1;
        else hi = mid;
    }
    if (u16(ends + lo * 2) < cp) return 0;
    int start = u16(starts + lo * 2);
    if (cp < start) return 0;
    int delta = u16(deltas + lo * 2), ro = u16(ranges + lo * 2);
    if (!ro) return (cp + delta) & 0xFFFF;
    const uint8_t *g = ranges + lo * 2 + ro + (cp - start) * 2;
    if (g + 2 > f->d + f->len) return 0;
    int gi = u16(g);
    return gi ? (gi + delta) & 0xFFFF : 0;
}

static int advance_units(struct ttf *f, int gid) {
    int i = gid < f->nhm ? gid : f->nhm - 1;
    return u16(f->d + f->hmtx + i * 4);
}

static int kern_units(struct ttf *f, int left, int right) {
    if (!f->nkern) return 0;
    unsigned key = (unsigned)left << 16 | right;
    int lo = 0, hi = f->nkern - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        const uint8_t *p = f->d + f->kern + mid * 6;
        unsigned k = (unsigned)u16(p) << 16 | u16(p + 2);
        if (k == key) return s16(p + 4);
        if (k < key) lo = mid + 1;
        else hi = mid - 1;
    }
    return 0;
}

/* ------------------------------------------------------------ контур глифа */

struct gscale {
    int size;                       /* размер шрифта в пикселях (em) */
    int xnum, xden;                 /* X_fix = x * xnum / xden */
    int ynum, yden;
};

static void make_scale(struct ttf *f, int size, struct gscale *s) {
    s->size = size;
    s->xnum = size * 256; s->xden = f->upem;
    int xh_px = (f->xh * size * 2 + f->upem) / (2 * f->upem);   /* округлённая высота x */
    if (xh_px < 1) xh_px = 1;
    if (size >= 40) { s->ynum = size * 256; s->yden = f->upem; }  /* крупно — без подгонки */
    else { s->ynum = xh_px * 256; s->yden = f->xh; }
}

/* преобразование составного глифа: смещение и масштаб (2.14) */
struct gxf { int dx, dy, sx, sy; };

static int glyph_offset(struct ttf *f, int gid, int *len) {
    if (gid < 0 || gid >= f->nglyphs) return -1;
    int a, b;
    if (f->locfmt) { a = (int)u32(f->d + f->loca + gid * 4); b = (int)u32(f->d + f->loca + gid * 4 + 4); }
    else { a = u16(f->d + f->loca + gid * 2) * 2; b = u16(f->d + f->loca + gid * 2 + 2) * 2; }
    *len = b - a;
    if (b <= a || f->glyf + b > f->len) return -1;
    return f->glyf + a;
}

static int tx(struct gscale *s, struct gxf *t, int x, int y, int ox) {
    int v = (x * t->sx + y * 0) / 16384 + t->dx;
    return ox + v * s->xnum / s->xden;
}
static int ty(struct gscale *s, struct gxf *t, int x, int y, int oy) {
    int v = (y * t->sy + x * 0) / 16384 + t->dy;
    return oy - v * s->ynum / s->yden;
}

/* контур глифа -> path (ox, oy — начало координат глифа в FIX) */
static void glyph_path(struct ttf *f, int gid, struct gscale *s, struct gxf *t, int ox, int oy, gfx_path *p, int depth) {
    int len, off = glyph_offset(f, gid, &len);
    if (off < 0 || depth > 4) return;
    const uint8_t *g = f->d + off, *end = g + len;
    int nc = s16(g);
    if (nc < 0) {                                       /* составной: собран из других глифов */
        const uint8_t *q = g + 10;
        for (;;) {
            if (q + 4 > end) break;
            int flags = u16(q), sub = u16(q + 2);
            q += 4;
            int a1, a2;
            if (flags & 1) { a1 = s16(q); a2 = s16(q + 2); q += 4; }
            else { a1 = (signed char)q[0]; a2 = (signed char)q[1]; q += 2; }
            struct gxf ct;
            ct.sx = ct.sy = 16384;
            if (flags & 8) { ct.sx = ct.sy = s16(q); q += 2; }
            else if (flags & 0x40) { ct.sx = s16(q); ct.sy = s16(q + 2); q += 4; }
            else if (flags & 0x80) { ct.sx = s16(q); ct.sy = s16(q + 6); q += 8; }
            if (!(flags & 2)) { a1 = 0; a2 = 0; }       /* привязка по точкам — редкость, пропускаем */
            ct.dx = t->dx + a1 * t->sx / 16384;
            ct.dy = t->dy + a2 * t->sy / 16384;
            ct.sx = ct.sx * t->sx / 16384;
            ct.sy = ct.sy * t->sy / 16384;
            glyph_path(f, sub, s, &ct, ox, oy, p, depth + 1);
            if (!(flags & 0x20)) break;
        }
        return;
    }
    if (nc == 0) return;
    const uint8_t *endpts = g + 10;
    int npts = u16(endpts + (nc - 1) * 2) + 1;
    int ilen = u16(endpts + nc * 2);
    const uint8_t *fl = endpts + nc * 2 + 2 + ilen;
    if (npts <= 0 || npts > 4000 || fl >= end) return;
    uint8_t *flags = malloc(npts);
    int *xs = malloc(npts * sizeof(int)), *ys = malloc(npts * sizeof(int));
    if (!flags || !xs || !ys) { free(flags); free(xs); free(ys); return; }
    for (int i = 0; i < npts && fl < end;) {            /* флаги (с повторами) */
        uint8_t c = *fl++;
        flags[i++] = c;
        if (c & 8) {
            int r = fl < end ? *fl++ : 0;
            while (r-- > 0 && i < npts) flags[i++] = c;
        }
    }
    int v = 0;
    for (int i = 0; i < npts; i++) {                    /* x: приращения */
        uint8_t c = flags[i];
        if (c & 2) { int d = fl < end ? *fl++ : 0; v += (c & 16) ? d : -d; }
        else if (!(c & 16)) { if (fl + 1 < end) v += s16(fl); fl += 2; }
        xs[i] = v;
    }
    v = 0;
    for (int i = 0; i < npts; i++) {                    /* y */
        uint8_t c = flags[i];
        if (c & 4) { int d = fl < end ? *fl++ : 0; v += (c & 32) ? d : -d; }
        else if (!(c & 32)) { if (fl + 1 < end) v += s16(fl); fl += 2; }
        ys[i] = v;
    }
    int st = 0;
    for (int c = 0; c < nc; c++) {
        int en = u16(endpts + c * 2);
        if (en < st || en >= npts) break;
        int n = en - st + 1, first = -1;
        for (int i = 0; i < n; i++) if (flags[st + i] & 1) { first = i; break; }
        int sx, sy, have_ctrl = 0, cx = 0, cy = 0;
        if (first >= 0) {                                 /* начинаем с точки на кривой */
            sx = tx(s, t, xs[st + first], ys[st + first], ox);
            sy = ty(s, t, xs[st + first], ys[st + first], oy);
        } else {                                          /* все контрольные: с середины */
            sx = (tx(s, t, xs[st], ys[st], ox) + tx(s, t, xs[en], ys[en], ox)) / 2;
            sy = (ty(s, t, xs[st], ys[st], oy) + ty(s, t, xs[en], ys[en], oy)) / 2;
        }
        path_fix_move(p, sx, sy);
        int k0 = first >= 0 ? 1 : 0, k1 = first >= 0 ? n : n - 1;
        for (int k = k0; k <= k1; k++) {
            int i = st + (first >= 0 ? (first + k) % n : k);
            int px = tx(s, t, xs[i], ys[i], ox), py = ty(s, t, xs[i], ys[i], oy);
            if (flags[i] & 1) {
                if (have_ctrl) path_fix_quad(p, cx, cy, px, py);
                else path_fix_lineto(p, px, py);
                have_ctrl = 0;
            } else {
                if (have_ctrl) path_fix_quad(p, cx, cy, (cx + px) / 2, (cy + py) / 2);
                cx = px; cy = py;
                have_ctrl = 1;
            }
        }
        if (have_ctrl) path_fix_quad(p, cx, cy, sx, sy);
        path_close(p);
        st = en + 1;
    }
    free(flags); free(xs); free(ys);
}

/* ------------------------------------------------------------ кэш глифов */

struct glyph {
    struct glyph *next;
    short font, size, sub;
    int gid;
    int w, h, left, top;            /* маска w x h; левый верхний угол относительно пера/базовой линии */
    int adv;                        /* ширина в FIX */
    uint8_t *a;
};

#define GCACHE 1024
static struct glyph *gcache[GCACHE];
static uint8_t gamma_lut[256];
static int gamma_ready;

static void make_gamma(void) {
    /* лёгкое усиление полутонов: мелкий текст без хинтинга иначе бледноват */
    for (int i = 0; i < 256; i++) {
        int v = i + i * (255 - i) / 600;
        gamma_lut[i] = v > 255 ? 255 : v;
    }
    gamma_ready = 1;
}

static struct glyph *get_glyph(int fid, int size, int gid, int sub) {
    unsigned h = ((unsigned)gid * 31 + size * 7 + fid * 131 + sub * 17) % GCACHE;
    struct glyph *g;
    for (g = gcache[h]; g; g = g->next)
        if (g->gid == gid && g->size == size && g->font == fid && g->sub == sub) return g;
    struct ttf *f = &fonts[fid];
    struct gscale s;
    make_scale(f, size, &s);
    g = calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->font = fid; g->size = size; g->gid = gid; g->sub = sub;
    g->adv = advance_units(f, gid) * s.xnum / s.xden;
    gfx_path p;
    path_begin(&p, 0, 0, 1, 1);
    struct gxf t;
    t.dx = 0; t.dy = 0; t.sx = 16384; t.sy = 16384;
    /* начало глифа: x = sub/4 пикселя, базовая линия y = 0 */
    glyph_path(f, gid, &s, &t, sub * 64, 0, &p, 0);
    if (p.n) {
        int x0 = p.minx >> 8, y0 = p.miny >> 8;          /* >> у отрицательных — вниз, как floor */
        int x1 = (p.maxx + 255) >> 8, y1 = (p.maxy + 255) >> 8;
        g->w = x1 - x0; g->h = y1 - y0;
        g->left = x0; g->top = y0;
        if (g->w > 0 && g->h > 0 && g->w < 1024 && g->h < 1024) {
            g->a = calloc(g->w * g->h, 1);
            if (g->a) {
                path_mask(&p, g->a, x0, y0, g->w, g->h);
                if (!gamma_ready) make_gamma();
                for (int i = 0; i < g->w * g->h; i++) g->a[i] = gamma_lut[g->a[i]];
            }
        }
    }
    path_free(&p);
    g->next = gcache[h];
    gcache[h] = g;
    return g;
}

/* ------------------------------------------------------------ UTF-8 и CP437 */

static const unsigned short cp437[128] = {
    0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
    0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, 0x20A7, 0x192,
    0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, 0x2310, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x3B1, 0xDF, 0x393, 0x3C0, 0x3A3, 0x3C3, 0xB5, 0x3C4, 0x3A6, 0x398, 0x3A9, 0x3B4, 0x221E, 0x3C6, 0x3B5, 0x2229,
    0x2261, 0xB1, 0x2265, 0x2264, 0x2320, 0x2321, 0xF7, 0x2248, 0xB0, 0x2219, 0xB7, 0x221A, 0x207F, 0xB2, 0x25A0, 0xA0,
};

/* следующий символ строки: правильный UTF-8 -> Unicode, одиночный байт >= 0x80 -> CP437 */
int utf8_next(const char **ps) {
    const uint8_t *s = (const uint8_t *)*ps;
    int c = s[0];
    if (c < 0x80) { *ps += 1; return c; }
    int need = c >= 0xF0 && c < 0xF8 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 && c < 0xE0 ? 1 : 0;
    if (need) {
        int cp = c & (0x3F >> need), ok = 1;
        for (int i = 1; i <= need; i++) {
            if ((s[i] & 0xC0) != 0x80) { ok = 0; break; }
            cp = cp << 6 | (s[i] & 0x3F);
        }
        if (ok) { *ps += need + 1; return cp; }
    }
    *ps += 1;
    return 0x10000 | c;                 /* метка «байт CP437» */
}

static int cp_of(int c) { return c & 0x10000 ? cp437[(c & 0xFF) - 0x80] : c; }

/* ------------------------------------------------------------ текст */

static void blit_glyph(struct glyph *g, int x, int y, color_t c) {
    if (g->a) gfx_draw_mask(x + g->left, y + g->top, g->a, g->w, g->h, c, 255);
}

static int line_px(struct ttf *f, int size, int v) { return (v * size * 2 + (v >= 0 ? f->upem : -f->upem)) / (2 * f->upem); }

int text_ascent(int font, int size) {
    if (!load_font(font)) return size * 3 / 4;
    return line_px(&fonts[font], size, fonts[font].asc);
}

int text_line_h(int font, int size) {
    if (!load_font(font)) return size;
    struct ttf *f = &fonts[font];
    return line_px(f, size, f->asc) + line_px(f, size, -f->desc);
}

/* общий проход по строке: рисовать (draw) или только мерить; max_w — обрезать по ширине */
static int text_run(int font, int size, int x, int y, const char *s, int n, color_t c, int draw) {
    if (!load_font(font)) {                       /* шрифта нет — запасной растровый 8x16 */
        int w = 0;
        while (*s && n-- > 0) {             /* n — счётчик, не указатель: переполнения нет */
            char ch[2]; ch[0] = *s++; ch[1] = 0;
            if (draw) gfx_text_bitmap(x + w, y + (text_line_h(font, size) - 16) / 2, ch, c, TRANSPARENT, 1);
            w += 8;
        }
        return w;
    }
    struct ttf *f = &fonts[font];
    int base = y + text_ascent(font, size);
    int pen = x * 256, prev = -1;
    /* n = 0x7FFFFFFF — «до конца строки»; s + n переполнил бы указатель */
    const char *end = n >= 0x7FFFFFFF ? NULL : s + n;
    struct gscale sc;
    make_scale(f, size, &sc);
    while (*s && (!end || s < end)) {
        int cp = cp_of(utf8_next(&s));
        if (cp == '\t') cp = ' ';
        int gid = glyph_index(f, cp);
        if (!gid && cp != ' ') gid = glyph_index(f, '?');
        if (prev >= 0) pen += kern_units(f, prev, gid) * sc.xnum / sc.xden;
        int sub = (pen & 255) >> 6;
        struct glyph *g = get_glyph(font, size, gid, sub);
        if (!g) break;
        if (draw) blit_glyph(g, pen >> 8, base, c);
        pen += g->adv;
        prev = gid;
    }
    return (pen + 255) / 256 - x;
}

int text_draw(int font, int size, int x, int y, const char *s, color_t c) {
    return text_run(font, size, x, y, s, 0x7FFFFFFF, c, 1);
}

int text_draw_n(int font, int size, int x, int y, const char *s, int n, color_t c) {
    return text_run(font, size, x, y, s, n, c, 1);
}

int text_width(int font, int size, const char *s) { return text_run(font, size, 0, 0, s, 0x7FFFFFFF, 0, 0); }
int text_width_n(int font, int size, const char *s, int n) { return text_run(font, size, 0, 0, s, n, 0, 0); }

/* сколько байт строки влезает в max_w пикселей (по целым символам) */
int text_fit(int font, int size, const char *s, int max_w) {
    const char *p = s;
    int last = 0;
    while (*p) {
        const char *q = p;
        utf8_next(&q);
        if (text_width_n(font, size, s, (int)(q - s)) > max_w) break;
        p = q;
        last = (int)(p - s);
    }
    return last;
}

/* текст, при нехватке места — с многоточием в конце */
int text_draw_fit(int font, int size, int x, int y, const char *s, int max_w, color_t c) {
    if (text_width(font, size, s) <= max_w) return text_draw(font, size, x, y, s, c);
    int dots = text_width(font, size, "\xE2\x80\xA6");
    int n = text_fit(font, size, s, max_w - dots);
    int w = text_draw_n(font, size, x, y, s, n, c);
    return w + text_draw(font, size, x + w, y, "\xE2\x80\xA6", c);
}

/* ------------------------------------------------------------ моноширинный текст (gfx_text) */

/* Клетка 8x16 (x scale). Возвращает ширину или -1, если шрифта нет. */
int font_mono_draw(int x, int y, const char *s, color_t fg, color_t bg, int scale) {
    if (!load_font(FONT_MONO)) return -1;
    struct ttf *f = &fonts[FONT_MONO];
    int size = 16 * scale, cw = 8 * scale, x0 = x;
    int base = y + line_px(f, size, f->asc);
    while (*s) {
        const char *at = s;
        int c = utf8_next(&s);
        if (bg != TRANSPARENT) gfx_fill_rect(x, y, cw, 16 * scale, bg);
        int cp = cp_of(c);
        int gid = cp > ' ' ? glyph_index(f, cp) : 0;
        /* псевдографика (рамки, блоки) и то, чего нет в шрифте — из растрового шрифта */
        bool boxy = (cp >= 0x2500 && cp < 0x25A0) || cp < ' ';
        if (cp > ' ' && (boxy || !gid)) {
            char one[2];
            one[0] = (char)((c & 0x10000) ? (c & 0xFF) : (cp < 0x80 ? cp : '?'));
            if (!(c & 0x10000) && cp >= 0x80) {          /* Unicode-символ рамки -> байт CP437 */
                for (int i = 0; i < 128; i++) if (cp437[i] == cp) { one[0] = (char)(0x80 + i); break; }
            }
            one[1] = 0;
            if (scale == 1) gfx_text_bitmap(x, y, one, fg, TRANSPARENT, 1);
            else gfx_text_bitmap(x, y, one, fg, TRANSPARENT, scale);
        } else if (gid) {
            struct glyph *g = get_glyph(FONT_MONO, size, gid, 0);
            if (g) blit_glyph(g, x, base, fg);
        }
        (void)at;
        x += cw;
    }
    return x - x0;
}
