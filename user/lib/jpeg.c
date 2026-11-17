#ifdef IMG_HOST
#include <stdlib.h>
#include <string.h>
#else
#include "myos.h"
#endif
#include "img.h"
#include "../include/jpeglib.h"

typedef unsigned char u8;
typedef unsigned int u32;
static int rd16be(const u8 *p) { return (p[0] << 8) | p[1]; }
static int dims_ok(int w, int h) {
    return w > 0 && h > 0 && w <= 16384 && h <= 16384 && w <= (24 * 1024 * 1024) / h;
}
static unsigned *new_px(int w, int h) { return malloc(sizeof(unsigned) * (size_t)w * (size_t)h); }

static const u8 zz[64 + 16] = {                    /* зигзаг -> обычный порядок (+16 на случай мусора) */
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63, 63 };

struct jhuff {
    u8 fast[512];                      /* первые 9 бит -> номер символа + 1 (0 — длиннее) */
    u8 fastlen[512];
    u8 sym[256];
    int maxcode[18];                   /* для длины L: самый большой код + 1 (сдвинутый) */
    int delta[17];                     /* номер символа = код + delta[L] */
    int ok;
};

struct jcomp {
    int id, h, v, tq, td, ta;
    int bw, bh;                        /* блоков по горизонтали/вертикали (с запасом до MCU) */
    short *coef;                       /* bw * bh * 64 */
    int dc;                            /* предсказание DC */
    u8 *plane;                         /* после ДКП: (bw*8/scale) x (bh*8/scale) */
    int pw, ph;
};

struct jpeg {
    const u8 *d, *end, *p;
    unsigned short qt[4][64];
    struct jhuff hdc[4], hac[4];
    struct jcomp c[4];
    int nc, w, h, hmax, vmax, mcux, mcuy, progressive, restart;
    int adobe, transform;
    u32 acc;
    int nbits, marker;                 /* marker — встретили маркер внутри данных */
    int eobrun;
};

static int jh_build(struct jhuff *h, const u8 *counts, const u8 *syms, int n) {
    memset(h, 0, sizeof(*h));
    memcpy(h->sym, syms, n);
    int code = 0, k = 0;
    for (int len = 1; len <= 16; len++) {
        h->delta[len] = k - code;
        for (int i = 0; i < counts[len - 1]; i++, k++, code++) {
            if (len <= 9) {
                int sh = 9 - len;
                for (int f = code << sh; f < (code + 1) << sh && f < 512; f++) { h->fast[f] = k + 1; h->fastlen[f] = len; }
            }
        }
        if (code > (1 << len)) return -1;
        h->maxcode[len] = code << (16 - len);
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    h->ok = 1;
    return 0;
}

static void jfill(struct jpeg *j) {
    while (j->nbits <= 24) {
        int b = 0;
        if (!j->marker && j->p < j->end) {
            b = *j->p;
            if (b == 0xFF) {
                int c = j->p + 1 < j->end ? j->p[1] : 0xD9;
                if (c == 0) j->p += 2;
                else { j->marker = c; b = 0; }               /* маркер: дальше подаём нули */
            } else j->p++;
        }
        j->acc |= (u32)b << (24 - j->nbits);
        j->nbits += 8;
    }
}

static int jbits(struct jpeg *j, int n) {
    if (!n) return 0;
    if (j->nbits < n) jfill(j);
    int v = j->acc >> (32 - n);
    j->acc <<= n;
    j->nbits -= n;
    return v;
}

static int jbit(struct jpeg *j) { return jbits(j, 1); }

static int jextend(int v, int n) { return n && v < (1 << (n - 1)) ? v - (1 << n) + 1 : v; }

static int jdecode(struct jpeg *j, const struct jhuff *h) {
    if (j->nbits < 16) jfill(j);
    int f = j->acc >> 23;
    if (h->fast[f]) {
        int len = h->fastlen[f];
        j->acc <<= len; j->nbits -= len;
        return h->sym[h->fast[f] - 1];
    }
    u32 t = j->acc >> 16;
    int len = 10;
    while (len <= 16 && (int)t >= h->maxcode[len]) len++;
    if (len > 16) { j->nbits = 0; return -1; }
    int code = j->acc >> (32 - len);
    j->acc <<= len; j->nbits -= len;
    int k = code + h->delta[len];
    return k >= 0 && k < 256 ? h->sym[k] : -1;
}

/* один блок 8x8 текущего скана */
static int jblock(struct jpeg *j, struct jcomp *c, short *b, int ss, int se, int ah, int al) {
    if (!j->progressive) {
        int t = jdecode(j, &j->hdc[c->td]);
        if (t < 0 || t > 16) return -1;
        c->dc += jextend(jbits(j, t), t);
        b[0] = c->dc;
        for (int k = 1; k < 64;) {
            int rs = jdecode(j, &j->hac[c->ta]);
            if (rs < 0) return -1;
            int r = rs >> 4, s = rs & 15;
            if (!s) { if (r != 15) break; k += 16; continue; }
            k += r;
            if (k > 63) return -1;
            b[zz[k++]] = jextend(jbits(j, s), s);
        }
        return 0;
    }
    if (ss == 0) {                                          /* DC */
        if (ah == 0) {
            int t = jdecode(j, &j->hdc[c->td]);
            if (t < 0 || t > 16) return -1;
            c->dc += jextend(jbits(j, t), t);
            b[0] = c->dc * (1 << al);
        } else if (jbit(j)) b[0] |= 1 << al;
        return 0;
    }
    if (ah == 0) {                                          /* AC, первый проход */
        if (j->eobrun) { j->eobrun--; return 0; }
        for (int k = ss; k <= se;) {
            int rs = jdecode(j, &j->hac[c->ta]);
            if (rs < 0) return -1;
            int r = rs >> 4, s = rs & 15;
            if (!s) {
                if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += jbits(j, r); break; }
                k += 16;
                continue;
            }
            k += r;
            if (k > 63) return -1;
            b[zz[k++]] = jextend(jbits(j, s), s) * (1 << al);
        }
        return 0;
    }
    /* AC, уточнение */
    int bit = 1 << al;
    if (j->eobrun) {
        j->eobrun--;
        for (int k = ss; k <= se; k++) {
            short *p = &b[zz[k]];
            if (*p && jbit(j) && !(*p & bit)) *p += *p > 0 ? bit : -bit;
        }
        return 0;
    }
    int k = ss;
    do {
        int rs = jdecode(j, &j->hac[c->ta]);
        if (rs < 0) return -1;
        int r = rs >> 4, s = rs & 15;
        if (!s) {
            if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += jbits(j, r); r = 64; }
        } else s = jbit(j) ? bit : -bit;
        while (k <= se) {
            short *p = &b[zz[k++]];
            if (*p) { if (jbit(j) && !(*p & bit)) *p += *p > 0 ? bit : -bit; }
            else {
                if (!r) { *p = s; break; }
                r--;
            }
        }
    } while (k <= se);
    return 0;
}

static void jrestart(struct jpeg *j) {
    j->acc = 0; j->nbits = 0; j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dc = 0;
    if (j->marker >= 0xD0 && j->marker <= 0xD7) { j->p += 2; j->marker = 0; return; }
    if (!j->marker) {                                       /* маркер ещё впереди — найти */
        while (j->p + 1 < j->end && !(j->p[0] == 0xFF && j->p[1] >= 0xD0 && j->p[1] <= 0xD7)) j->p++;
        if (j->p + 1 < j->end) j->p += 2;
    }
}

static int jscan(struct jpeg *j, const u8 *s, int slen) {
    if (slen < 1) return -1;
    int ns = s[0];
    if (ns < 1 || ns > 4 || slen < 1 + ns * 2 + 3) return -1;
    struct jcomp *sc[4];
    for (int i = 0; i < ns; i++) {
        int id = s[1 + i * 2], t = s[2 + i * 2];
        sc[i] = NULL;
        for (int k = 0; k < j->nc; k++) if (j->c[k].id == id) sc[i] = &j->c[k];
        if (!sc[i]) return -1;
        sc[i]->td = (t >> 4) & 3; sc[i]->ta = t & 3;
    }
    int ss = s[1 + ns * 2], se = s[2 + ns * 2], ah = s[3 + ns * 2] >> 4, al = s[3 + ns * 2] & 15;
    if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
    if (ss > 63 || se > 63 || ss > se || al > 13) return -1;
    for (int i = 0; i < ns; i++) {                          /* нужные таблицы есть? */
        if ((ss == 0 && ah == 0 && !j->hdc[sc[i]->td].ok) || (se > 0 && !j->hac[sc[i]->ta].ok)) return -1;
    }
    j->acc = 0; j->nbits = 0; j->marker = 0; j->eobrun = 0;
    for (int i = 0; i < j->nc; i++) j->c[i].dc = 0;
    int todo = j->restart;
    if (ns == 1) {                                          /* один компонент: блоки по его настоящему размеру */
        struct jcomp *c = sc[0];
        int cw = ((j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8, ch = ((j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8;
        for (int by = 0; by < ch; by++)
            for (int bx = 0; bx < cw; bx++) {
                if (jblock(j, c, c->coef + (by * c->bw + bx) * 64, ss, se, ah, al) < 0) return 0;
                if (j->restart && --todo == 0) { jrestart(j); todo = j->restart; }
            }
        return 0;
    }
    for (int my = 0; my < j->mcuy; my++)
        for (int mx = 0; mx < j->mcux; mx++) {
            for (int i = 0; i < ns; i++) {
                struct jcomp *c = sc[i];
                for (int y = 0; y < c->v; y++)
                    for (int x = 0; x < c->h; x++) {
                        int bx = mx * c->h + x, by = my * c->v + y;
                        if (jblock(j, c, c->coef + (by * c->bw + bx) * 64, ss, se, ah, al) < 0) return 0;
                    }
            }
            if (j->restart && --todo == 0) { jrestart(j); todo = j->restart; }
        }
    return 0;
}

static u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

#define F2F(x) ((int)((x) * 4096 + 0.5))
#define FSH(x) ((x) * 4096)
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7) \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3; \
    p2 = s2; p3 = s6; \
    p1 = (p2 + p3) * F2F(0.5411961); \
    t2 = p1 + p3 * F2F(-1.847759065); \
    t3 = p1 + p2 * F2F(0.765366865); \
    p2 = s0; p3 = s4; \
    t0 = FSH(p2 + p3); t1 = FSH(p2 - p3); \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2; \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1; \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2; \
    p5 = (p3 + p4) * F2F(1.175875602); \
    t0 = t0 * F2F(0.298631336); t1 = t1 * F2F(2.053119869); \
    t2 = t2 * F2F(3.072711026); t3 = t3 * F2F(1.501321110); \
    p1 = p5 + p1 * F2F(-0.899976223); p2 = p5 + p2 * F2F(-2.562915447); \
    p3 = p3 * F2F(-1.961570560); p4 = p4 * F2F(-0.390180644); \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

/* обратное ДКП блока (коэффициенты уже умножены на квантование) -> 8x8 байт */
static void jidct(const int *d, u8 *out, int stride) {
    int val[64], *v = val;
    for (int i = 0; i < 8; i++, d++, v++) {
        if (!d[8] && !d[16] && !d[24] && !d[32] && !d[40] && !d[48] && !d[56]) {
            int dc = d[0] * 4;
            v[0] = v[8] = v[16] = v[24] = v[32] = v[40] = v[48] = v[56] = dc;
        } else {
            IDCT_1D(d[0], d[8], d[16], d[24], d[32], d[40], d[48], d[56])
            x0 += 512; x1 += 512; x2 += 512; x3 += 512;
            v[0] = (x0 + t3) >> 10; v[56] = (x0 - t3) >> 10;
            v[8] = (x1 + t2) >> 10; v[48] = (x1 - t2) >> 10;
            v[16] = (x2 + t1) >> 10; v[40] = (x2 - t1) >> 10;
            v[24] = (x3 + t0) >> 10; v[32] = (x3 - t0) >> 10;
        }
    }
    v = val;
    for (int i = 0; i < 8; i++, v += 8, out += stride) {
        IDCT_1D(v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17);
        x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        out[0] = clamp8((x0 + t3) >> 17); out[7] = clamp8((x0 - t3) >> 17);
        out[1] = clamp8((x1 + t2) >> 17); out[6] = clamp8((x1 - t2) >> 17);
        out[2] = clamp8((x2 + t1) >> 17); out[5] = clamp8((x2 - t1) >> 17);
        out[3] = clamp8((x3 + t0) >> 17); out[4] = clamp8((x3 - t0) >> 17);
    }
}

static void jfree(struct jpeg *j) {
    for (int i = 0; i < 4; i++) { free(j->c[i].coef); free(j->c[i].plane); j->c[i].coef = NULL; j->c[i].plane = NULL; }
}

/* scale: 1, 2, 4 или 8 — во сколько раз уменьшить при распаковке (огромные фото) */
static int jpeg_decode(const u8 *d, int len, img *out, int scale) {
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return -1;
    static struct jpeg j;                                   /* ~10 КБ таблиц — не на стеке */
    memset(&j, 0, sizeof(j));
    j.d = d; j.end = d + len;
    int p = 2, frame = 0, scans = 0;
    while (p + 4 <= len) {
        if (d[p] != 0xFF) { p++; continue; }
        int m = d[p + 1];
        if (m == 0xFF) { p++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01) { p += 2; continue; }
        if (m == 0xD9) break;
        int sl = rd16be(d + p + 2);
        if (sl < 2 || p + 2 + sl > len) { if (m == 0xDA && scans) break; jfree(&j); return scans ? 0 : -1; }
        const u8 *s = d + p + 4;
        int n = sl - 2;
        if (m == 0xDB) {                                     /* таблицы квантования */
            for (int q = 0; q < n;) {
                int pq = s[q] >> 4, tq = s[q] & 3;
                q++;
                for (int i = 0; i < 64 && q < n; i++) { j.qt[tq][zz[i]] = pq ? rd16be(s + q) : s[q]; q += pq ? 2 : 1; }
            }
        } else if (m == 0xC4) {                              /* таблицы Хаффмана */
            for (int q = 0; q + 17 <= n;) {
                int tc = s[q] >> 4, th = s[q] & 3, cnt = 0;
                for (int i = 0; i < 16; i++) cnt += s[q + 1 + i];
                if (cnt > 256 || q + 17 + cnt > n) break;
                jh_build(tc ? &j.hac[th] : &j.hdc[th], s + q + 1, s + q + 17, cnt);
                q += 17 + cnt;
            }
        } else if (m == 0xDD && n >= 2) j.restart = rd16be(s);
        else if (m == 0xEE && n >= 12 && !memcmp(s, "Adobe", 5)) { j.adobe = 1; j.transform = s[11]; }
        else if ((m >= 0xC0 && m <= 0xC2) && !frame) {       /* кадр: baseline, extended, progressive */
            if (n < 6) return -1;
            j.progressive = m == 0xC2;
            j.h = rd16be(s + 1); j.w = rd16be(s + 3); j.nc = s[5];
            if (s[0] != 8 || (j.nc != 1 && j.nc != 3 && j.nc != 4) || n < 6 + j.nc * 3 || !dims_ok(j.w, j.h)) return -1;
            j.hmax = j.vmax = 1;
            for (int i = 0; i < j.nc; i++) {
                struct jcomp *c = &j.c[i];
                c->id = s[6 + i * 3]; c->h = s[7 + i * 3] >> 4; c->v = s[7 + i * 3] & 15; c->tq = s[8 + i * 3] & 3;
                if (c->h < 1 || c->h > 4 || c->v < 1 || c->v > 4) return -1;
                if (c->h > j.hmax) j.hmax = c->h;
                if (c->v > j.vmax) j.vmax = c->v;
            }
            j.mcux = (j.w + 8 * j.hmax - 1) / (8 * j.hmax);
            j.mcuy = (j.h + 8 * j.vmax - 1) / (8 * j.vmax);
            int total = 0;
            for (int i = 0; i < j.nc; i++) {
                struct jcomp *c = &j.c[i];
                c->bw = j.mcux * c->h; c->bh = j.mcuy * c->v;
                total += c->bw * c->bh;
            }
            if (total > (12 << 20) / 64) return -2;          /* коэффициенты больше 24 МБ — слишком */
            for (int i = 0; i < j.nc; i++) {
                struct jcomp *c = &j.c[i];
                c->coef = calloc(c->bw * c->bh * 64, sizeof(short));
                if (!c->coef) { jfree(&j); return -2; }
            }
            frame = 1;
        } else if (m >= 0xC3 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { jfree(&j); return -1; }   /* lossless, арифм. */
        else if (m == 0xDA) {                                /* скан: данные идут сразу за заголовком */
            if (!frame) return -1;
            j.p = s + n;
            jscan(&j, s, n);
            scans++;
            /* найти следующий маркер (не RST и не 0xFF00) */
            const u8 *q = j.p;
            while (q + 1 < j.end && !(q[0] == 0xFF && q[1] != 0 && !(q[1] >= 0xD0 && q[1] <= 0xD7) && q[1] != 0xFF)) q++;
            p = q - d;
            continue;
        }
        p += 2 + sl;
    }
    if (!frame || !scans) { jfree(&j); return -1; }
    /* ДКП -> плоскости компонентов (с уменьшением в scale раз) */
    int bs = 8 / scale;                                      /* сторона блока на выходе */
    for (int i = 0; i < j.nc; i++) {
        struct jcomp *c = &j.c[i];
        c->pw = c->bw * bs; c->ph = c->bh * bs;
        c->plane = malloc(c->pw * c->ph);
        if (!c->plane) { jfree(&j); return -2; }
        const unsigned short *q = j.qt[c->tq];
        int deq[64];
        u8 tmp[64];
        for (int by = 0; by < c->bh; by++)
            for (int bx = 0; bx < c->bw; bx++) {
                short *b = c->coef + (by * c->bw + bx) * 64;
                u8 *o = c->plane + by * bs * c->pw + bx * bs;
                if (scale == 8) { o[0] = clamp8(((b[0] * q[0]) >> 3) + 128); continue; }   /* только DC */
                for (int k = 0; k < 64; k++) { int v = b[k] * q[k]; deq[k] = v > 32767 ? 32767 : v < -32767 ? -32767 : v; }
                if (scale == 1) { jidct(deq, o, c->pw); continue; }
                jidct(deq, tmp, 8);
                for (int y = 0; y < bs; y++)
                    for (int x = 0; x < bs; x++) {
                        int sum = 0;
                        for (int yy = 0; yy < scale; yy++) for (int xx = 0; xx < scale; xx++) sum += tmp[(y * scale + yy) * 8 + x * scale + xx];
                        o[y * c->pw + x] = sum / (scale * scale);
                    }
            }
        free(c->coef);
        c->coef = NULL;
    }
    int ow = (j.w + scale - 1) / scale, oh = (j.h + scale - 1) / scale;
    unsigned *px = new_px(ow, oh);
    if (!px) { jfree(&j); return -2; }
    for (int y = 0; y < oh; y++) {
        for (int x = 0; x < ow; x++) {
            int v[4];
            for (int i = 0; i < j.nc; i++) {
                struct jcomp *c = &j.c[i];
                if (c->h == j.hmax && c->v == j.vmax) { v[i] = c->plane[y * c->pw + x]; continue; }
                /* прорежённый цвет: билинейно между соседними образцами (как «fancy upsampling» libjpeg) */
                int fx = ((2 * x + 1) * c->h * 128) / j.hmax - 128, fy = ((2 * y + 1) * c->v * 128) / j.vmax - 128;
                if (fx < 0) fx = 0;
                if (fy < 0) fy = 0;
                int x0 = fx >> 8, y0 = fy >> 8, wx = fx & 255, wy = fy & 255;
                if (x0 >= c->pw) x0 = c->pw - 1;
                if (y0 >= c->ph) y0 = c->ph - 1;
                int x1 = x0 + 1 < c->pw ? x0 + 1 : x0, y1 = y0 + 1 < c->ph ? y0 + 1 : y0;
                const u8 *r0 = c->plane + y0 * c->pw, *r1 = c->plane + y1 * c->pw;
                int top = r0[x0] * (256 - wx) + r0[x1] * wx, bot = r1[x0] * (256 - wx) + r1[x1] * wx;
                v[i] = (top * (256 - wy) + bot * wy + 32768) >> 16;
            }
            int R, G, B;
            if (j.nc == 1) R = G = B = v[0];
            else {
                int ycc = j.nc == 3 ? !(j.adobe && j.transform == 0) : j.transform == 2;
                if (j.nc == 3 && !j.adobe && j.c[0].id == 'R' && j.c[1].id == 'G' && j.c[2].id == 'B') ycc = 0;
                if (ycc) {
                    int Y = v[0] << 16, cb = v[1] - 128, cr = v[2] - 128;
                    R = clamp8((Y + 91881 * cr + 32768) >> 16);
                    G = clamp8((Y - 22554 * cb - 46802 * cr + 32768) >> 16);
                    B = clamp8((Y + 116130 * cb + 32768) >> 16);
                } else { R = v[0]; G = v[1]; B = v[2]; }
                if (j.nc == 4) {                             /* CMYK (Adobe хранит наоборот) */
                    int k = v[3];
                    if (j.transform == 2) { R = 255 - R; G = 255 - G; B = 255 - B; }
                    R = R * k / 255; G = G * k / 255; B = B * k / 255;
                }
            }
            px[y * ow + x] = 0xFF000000u | R << 16 | G << 8 | B;
        }
    }
    jfree(&j);
    out->w = ow; out->h = oh; out->px = px; out->has_alpha = 0;
    return 0;
}

int jpeg_decode_argb(const unsigned char *d, int len, int *w, int *h, unsigned **px, int scale) {
    img im;
    int r = jpeg_decode((const u8 *)d, len, &im, scale > 0 ? scale : 1);
    if (r) return r;
    *w = im.w; *h = im.h; *px = im.px;
    return 0;
}

/* Кодер baseline: серый или YCbCr 4:4:4, фиксированные длины кодов Хаффмана.
 * Значения кодов — полный набор символов, не таблица IJG: файл всё равно
 * читается, потому что длины и символы записаны в DHT. */

static const int je_cos[8][8] = {
    {4096,4096,4096,4096,4096,4096,4096,4096},
    {4017,3406,2276,799,-799,-2276,-3406,-4017},
    {3784,1567,-1567,-3784,-3784,-1567,1567,3784},
    {3406,-799,-4017,-2276,2276,4017,799,-3406},
    {2896,-2896,-2896,2896,2896,-2896,-2896,2896},
    {2276,-4017,799,3406,-3406,-799,4017,-2276},
    {1567,-3784,3784,-1567,-1567,3784,-3784,1567},
    {799,-2276,3406,-4017,4017,-3406,2276,-799}
};
static const u8 je_zz[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21,
    28, 35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61,
    54, 47, 55, 62, 63
};
static const u8 je_q_y[64] = {
    16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55, 14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
    18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92, 49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99
};
static const u8 je_q_c[64] = {
    17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99, 24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
    99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99
};
static const u8 je_dc_l_bits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
static const u8 je_dc_c_bits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
static const u8 je_ac_l_bits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
static const u8 je_ac_c_bits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};

struct je_huff { unsigned short code[256]; unsigned char len[256]; };
struct je_bit {
    u8 *p; int n, cap; unsigned long long acc; int bits;
};

static void je_fdct(int b[64]) {
    int tmp[64], y, u, v, x;
    long long den = 4096LL * 4096 * 4096 * 4096 * 4;
    for (y = 0; y < 8; y++) for (u = 0; u < 8; u++) {
        int s = 0;
        for (x = 0; x < 8; x++) s += b[y * 8 + x] * je_cos[u][x];
        tmp[y * 8 + u] = s;
    }
    for (u = 0; u < 8; u++) for (v = 0; v < 8; v++) {
        long long s = 0, num;
        int cu = u ? 4096 : 2896, cv = v ? 4096 : 2896;
        for (y = 0; y < 8; y++) s += (long long)tmp[y * 8 + u] * je_cos[v][y];
        num = s * cu * cv;
        if (num >= 0) num += den / 2; else num -= den / 2;
        b[v * 8 + u] = (int)(num / den);
    }
}

static void je_huff_build(struct je_huff *h, const u8 *bits, const u8 *val, int nval) {
    unsigned c = 0;
    int i, j, k = 0;
    memset(h, 0, sizeof *h);
    for (i = 1; i <= 16; i++) {
        for (j = 0; j < bits[i - 1]; j++) {
            if (k < nval) { h->code[val[k]] = (unsigned short)c; h->len[val[k]] = (unsigned char)i; k++; }
            c++;
        }
        c <<= 1;
    }
}

static void je_ac_vals(u8 *v) {
    int n = 0, r, s;
    v[n++] = 0x00; v[n++] = 0xF0;
    for (r = 0; r < 16; r++) for (s = 1; s <= 10; s++) v[n++] = (u8)((r << 4) | s);
}

static int je_byte(struct je_bit *b, unsigned byte, int stuff) {
    if (b->n + 2 > b->cap) {
        int c = b->cap ? b->cap * 2 : 4096;
        u8 *p;
        if (c < b->n + 2) c = b->n + 2;
        p = realloc(b->p, c);
        if (!p) return -1;
        b->p = p; b->cap = c;
    }
    b->p[b->n++] = (u8)byte;
    if (stuff && byte == 255) b->p[b->n++] = 0;
    return 0;
}
static int je_raw(struct je_bit *b, const void *s, int n) {
    const u8 *p = s;
    int i;
    for (i = 0; i < n; i++) if (je_byte(b, p[i], 0)) return -1;
    return 0;
}
static int je_bits(struct je_bit *b, unsigned val, int len) {
    if (len <= 0) return 0;
    b->acc = (b->acc << len) | (val & ((1ull << len) - 1));
    b->bits += len;
    while (b->bits >= 8) {
        b->bits -= 8;
        if (je_byte(b, (unsigned)((b->acc >> b->bits) & 255), 1)) return -1;
    }
    return 0;
}
static int je_flush(struct je_bit *b) {
    unsigned byte;
    if (!b->bits) return 0;
    byte = ((unsigned)(b->acc << (8 - b->bits)) | ((1u << (8 - b->bits)) - 1)) & 255;
    b->bits = 0; b->acc = 0;
    return je_byte(b, byte, 1);
}
static int je_u16(struct je_bit *b, unsigned v) {
    u8 d[2]; d[0] = v >> 8; d[1] = v; return je_raw(b, d, 2);
}

static int je_cat(int v) {
    int n = 0;
    if (v < 0) v = -v;
    while (v) { n++; v >>= 1; }
    return n;
}
static unsigned je_extra(int v, int n) {
    if (n <= 0) return 0;
    if (v < 0) v += (1 << n) - 1;
    return (unsigned)v & ((1u << n) - 1);
}
static int je_sym(struct je_bit *b, const struct je_huff *h, int sym) {
    if ((unsigned)sym > 255 || !h->len[sym]) return -1;
    return je_bits(b, h->code[sym], h->len[sym]);
}
static int je_clamp(int v) {
    if (v > 1023) return 1023;
    if (v < -1023) return -1023;
    return v;
}
static int je_block(struct je_bit *b, int blk[64], int *pred, const struct je_huff *dc, const struct je_huff *ac, const int *qt) {
    int coef[64], i, run, diff, n;
    for (i = 0; i < 64; i++) {
        int v = blk[je_zz[i]], q = qt[je_zz[i]];
        if (v >= 0) v = (v + q / 2) / q; else v = -((-v + q / 2) / q);
        coef[i] = je_clamp(v);
    }
    diff = coef[0] - *pred; *pred = coef[0];
    n = je_cat(diff);
    if (je_sym(b, dc, n) || je_bits(b, je_extra(diff, n), n)) return -1;
    run = 0;
    for (i = 1; i < 64; i++) {
        if (!coef[i]) { run++; continue; }
        while (run >= 16) { if (je_sym(b, ac, 0xF0)) return -1; run -= 16; }
        n = je_cat(coef[i]);
        if (je_sym(b, ac, (run << 4) | n) || je_bits(b, je_extra(coef[i], n), n)) return -1;
        run = 0;
    }
    if (run && je_sym(b, ac, 0x00)) return -1;
    return 0;
}

static int je_sample(const u8 *s, int w, int h, int ch, int x, int y, int c) {
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w) x = w - 1;
    if (y >= h) y = h - 1;
    return s[((size_t)y * w + x) * ch + (ch == 1 ? 0 : c)];
}
static int je_div256(int v) { return v >= 0 ? v >> 8 : -((-v) >> 8); }

static int je_qscale(int q) {
    if (q < 1) q = 1;
    if (q > 100) q = 100;
    return q < 50 ? 5000 / q : 200 - 2 * q;
}
static int je_qv(int base, int scale) {
    int v = (base * scale + 50) / 100;
    if (v < 1) v = 1;
    if (v > 255) v = 255;
    return v;
}

static int je_dht(struct je_bit *b, int klass, int id, const u8 *bits, const u8 *val, int nval) {
    int i, n = 0;
    u8 hdr[2];
    for (i = 0; i < 16; i++) n += bits[i];
    if (n > nval) n = nval;
    if (je_raw(b, "\xff\xc4", 2) || je_u16(b, (unsigned)(2 + 1 + 16 + n))) return -1;
    hdr[0] = (u8)((klass << 4) | id);
    if (je_raw(b, hdr, 1) || je_raw(b, bits, 16) || je_raw(b, val, n)) return -1;
    return 0;
}

static int je_encode(const u8 *samples, int w, int h, int ch, int quality, u8 **out, unsigned long *out_n) {
    struct je_bit b;
    struct je_huff dc_l, dc_c, ac_l, ac_c;
    u8 acv[162], dcv[12];
    int qy[64], qc[64], scale, i, pred[3];
    int my, mx;
    memset(&b, 0, sizeof b);
    for (i = 0; i < 12; i++) dcv[i] = (u8)i;
    je_ac_vals(acv);
    je_huff_build(&dc_l, je_dc_l_bits, dcv, 12);
    je_huff_build(&dc_c, je_dc_c_bits, dcv, 12);
    je_huff_build(&ac_l, je_ac_l_bits, acv, 162);
    je_huff_build(&ac_c, je_ac_c_bits, acv, 162);
    scale = je_qscale(quality);
    for (i = 0; i < 64; i++) { qy[i] = je_qv(je_q_y[i], scale); qc[i] = je_qv(je_q_c[i], scale); }
    if (je_raw(&b, "\xff\xd8\xff\xe0", 4) || je_u16(&b, 16) || je_raw(&b, "JFIF", 5) || je_raw(&b, "\x01\x01\x00\x00\x01\x00\x01\x00\x00", 9))
        goto fail;
    if (je_raw(&b, "\xff\xdb", 2) || je_u16(&b, ch == 1 ? 67 : 132)) goto fail;
    if (je_byte(&b, 0, 0)) goto fail;
    for (i = 0; i < 64; i++) if (je_byte(&b, (unsigned)qy[je_zz[i]], 0)) goto fail;
    if (ch != 1) {
        if (je_byte(&b, 1, 0)) goto fail;
        for (i = 0; i < 64; i++) if (je_byte(&b, (unsigned)qc[je_zz[i]], 0)) goto fail;
    }
    if (je_raw(&b, "\xff\xc0", 2) || je_u16(&b, ch == 1 ? 11 : 17) || je_byte(&b, 8, 0) || je_u16(&b, (unsigned)h) || je_u16(&b, (unsigned)w) || je_byte(&b, (unsigned)ch, 0))
        goto fail;
    if (je_raw(&b, ch == 1 ? "\x01\x11\x00" : "\x01\x11\x00\x02\x11\x01\x03\x11\x01", ch == 1 ? 3 : 9)) goto fail;
    if (je_dht(&b, 0, 0, je_dc_l_bits, dcv, 12) || je_dht(&b, 1, 0, je_ac_l_bits, acv, 162)) goto fail;
    if (ch != 1 && (je_dht(&b, 0, 1, je_dc_c_bits, dcv, 12) || je_dht(&b, 1, 1, je_ac_c_bits, acv, 162))) goto fail;
    if (je_raw(&b, "\xff\xda", 2) || je_u16(&b, ch == 1 ? 8 : 12) || je_byte(&b, (unsigned)ch, 0)) goto fail;
    if (je_raw(&b, ch == 1 ? "\x01\x00" : "\x01\x00\x02\x11\x03\x11", ch == 1 ? 2 : 6)) goto fail;
    if (je_raw(&b, "\x00\x3f\x00", 3)) goto fail;
    pred[0] = pred[1] = pred[2] = 0;
    for (my = 0; my < h; my += 8) for (mx = 0; mx < w; mx += 8) {
        int comp, ncomp = ch == 1 ? 1 : 3;
        int blocks[3][64];
        for (comp = 0; comp < ncomp; comp++) {
            int yy, xx;
            for (yy = 0; yy < 8; yy++) for (xx = 0; xx < 8; xx++) {
                int v;
                if (ch == 1) v = je_sample(samples, w, h, 1, mx + xx, my + yy, 0);
                else {
                    int R = je_sample(samples, w, h, 3, mx + xx, my + yy, 0);
                    int G = je_sample(samples, w, h, 3, mx + xx, my + yy, 1);
                    int B = je_sample(samples, w, h, 3, mx + xx, my + yy, 2);
                    int Y = je_div256(77 * R + 150 * G + 29 * B);
                    int Cb = je_div256(-43 * R - 85 * G + 128 * B) + 128;
                    int Cr = je_div256(128 * R - 107 * G - 21 * B) + 128;
                    if (comp == 0) v = Y; else if (comp == 1) v = Cb; else v = Cr;
                    if (v < 0) v = 0;
                    if (v > 255) v = 255;
                }
                blocks[comp][yy * 8 + xx] = v - 128;
            }
            je_fdct(blocks[comp]);
            if (je_block(&b, blocks[comp], &pred[comp], comp ? &dc_c : &dc_l, comp ? &ac_c : &ac_l, comp ? qc : qy))
                goto fail;
        }
    }
    if (je_flush(&b) || je_raw(&b, "\xff\xd9", 2)) goto fail;
    *out = b.p; *out_n = (unsigned long)b.n;
    return 0;
fail:
    free(b.p);
    return -1;
}

static int je_sof(const u8 *p, int n, int *w, int *h, int *comp) {
    int i;
    if (n < 4 || p[0] != 0xff || p[1] != 0xd8) return -1;
    i = 2;
    while (i + 1 < n) {
        int m, len;
        if (p[i] != 0xff) return -1;
        while (i < n && p[i] == 0xff) i++;
        if (i >= n) return -1;
        m = p[i++];
        if (m == 0xd8) continue;
        if (m == 0xd9 || m == 0xda) return -1;
        if (m == 0x01 || (m >= 0xd0 && m <= 0xd7)) continue;
        if (i + 2 > n) return -1;
        len = (p[i] << 8) | p[i + 1];
        if (len < 2 || i + len > n) return -1;
        if ((m >= 0xc0 && m <= 0xcf) && m != 0xc4 && m != 0xc8 && m != 0xcc) {
            if (len < 8) return -1;
            *h = (p[i + 3] << 8) | p[i + 4];
            *w = (p[i + 5] << 8) | p[i + 6];
            *comp = p[i + 7];
            return 0;
        }
        i += len;
    }
    return -1;
}

struct jpeg_error_mgr *jpeg_std_error(jpeg_error_mgr *err) {
    if (!err) return NULL;
    memset(err, 0, sizeof *err);
    return err;
}
void jpeg_create_decompress(j_decompress_ptr c) {
    jpeg_error_mgrp e = c->err;
    memset(c, 0, sizeof *c);
    c->err = e; c->is_decompressor = 1; c->scale_num = 1; c->scale_denom = 1;
}
void jpeg_create_compress(j_compress_ptr c) {
    jpeg_error_mgrp e = c->err;
    memset(c, 0, sizeof *c);
    c->err = e; c->is_decompressor = 0;
}
void jpeg_mem_src(j_decompress_ptr c, const unsigned char *in, unsigned long n) {
    c->src = in; c->src_len = n;
}
void jpeg_mem_dest(j_compress_ptr c, unsigned char **out, unsigned long *size) {
    c->outbuffer = out; c->outsize = size;
    if (out) *out = NULL;
    if (size) *size = 0;
}
int jpeg_read_header(j_decompress_ptr c, int require_image) {
    int w = 0, h = 0, comp = 0, scale, r;
    unsigned *px = NULL;
    (void)require_image;
    if (!c || !c->src || je_sof(c->src, (int)c->src_len, &w, &h, &comp)) return 0;
    scale = 1;
    if (c->scale_denom == 2 || c->scale_denom == 4 || c->scale_denom == 8) scale = (int)c->scale_denom;
    r = jpeg_decode_argb(c->src, (int)c->src_len, &w, &h, &px, scale);
    if (r) return 0;
    c->image_width = c->output_width = (JDIMENSION)w;
    c->image_height = c->output_height = (JDIMENSION)h;
    c->num_components = comp > 0 ? comp : 3;
    c->out_color_space = comp == 1 ? JCS_GRAYSCALE : JCS_RGB;
    c->output_components = comp == 1 ? 1 : 3;
    c->argb = px; c->argb_w = w; c->argb_h = h;
    return 1;
}
int jpeg_start_decompress(j_decompress_ptr c) {
    if (!c || !c->argb) return 0;
    c->output_scanline = 0;
    return 1;
}
JDIMENSION jpeg_read_scanlines(j_decompress_ptr c, JSAMPARRAY rows, JDIMENSION max_lines) {
    JDIMENSION n = 0;
    if (!c || !rows || !c->argb) return 0;
    while (n < max_lines && c->output_scanline < c->output_height) {
        unsigned y = c->output_scanline, x;
        JSAMPLE *d = rows[n];
        if (d) for (x = 0; x < c->output_width; x++) {
            unsigned p = c->argb[y * c->argb_w + x];
            if (c->output_components == 1) d[x] = (JSAMPLE)(p >> 16);
            else { d[x * 3] = p >> 16; d[x * 3 + 1] = p >> 8; d[x * 3 + 2] = p; }
        }
        c->output_scanline++;
        n++;
    }
    return n;
}
int jpeg_finish_decompress(j_decompress_ptr c) { return c && c->argb ? 1 : 0; }
void jpeg_destroy_decompress(j_decompress_ptr c) {
    if (!c) return;
    free(c->argb); c->argb = NULL;
}
void jpeg_set_defaults(j_compress_ptr c) {
    if (!c) return;
    if (!c->input_components) c->input_components = 3;
    if (!c->in_color_space) c->in_color_space = c->input_components == 1 ? JCS_GRAYSCALE : JCS_RGB;
    if (!c->quality) c->quality = 75;
    c->data_precision = 8;
}
void jpeg_set_quality(j_compress_ptr c, int quality, int force_baseline) {
    (void)force_baseline;
    if (!c) return;
    if (quality < 1) quality = 1;
    if (quality > 100) quality = 100;
    c->quality = quality;
}
void jpeg_start_compress(j_compress_ptr c, int write_all_tables) {
    (void)write_all_tables;
    if (!c) return;
    c->started = 1; c->next_scanline = 0; c->samples_n = 0;
}
JDIMENSION jpeg_write_scanlines(j_compress_ptr c, JSAMPARRAY rows, JDIMENSION num) {
    JDIMENSION n = 0;
    int ch;
    if (!c || !rows || !c->started) return 0;
    ch = c->input_components == 1 ? 1 : 3;
    while (n < num && c->next_scanline < c->image_height) {
        unsigned long need = (unsigned long)c->image_width * ch;
        if (!rows[n]) break;
        if (c->samples_n + need > c->samples_cap) {
            unsigned long cap = c->samples_cap ? c->samples_cap * 2 : 4096;
            u8 *p;
            while (cap < c->samples_n + need) cap *= 2;
            p = realloc(c->samples, cap);
            if (!p) return n;
            c->samples = p; c->samples_cap = cap;
        }
        memcpy(c->samples + c->samples_n, rows[n], need);
        c->samples_n += need;
        c->next_scanline++;
        n++;
    }
    return n;
}
void jpeg_finish_compress(j_compress_ptr c) {
    u8 *out = NULL;
    unsigned long n = 0;
    int ch;
    if (!c || !c->started || !c->outbuffer) return;
    ch = c->input_components == 1 ? 1 : 3;
    if (!c->image_width || !c->image_height || c->image_width > 16384 || c->image_height > 16384) return;
    if (je_encode(c->samples, (int)c->image_width, (int)c->image_height, ch, c->quality ? c->quality : 75, &out, &n)) return;
    *c->outbuffer = out;
    if (c->outsize) *c->outsize = n;
    c->started = 0;
}
void jpeg_destroy_compress(j_compress_ptr c) {
    if (!c) return;
    free(c->samples); c->samples = NULL;
}
