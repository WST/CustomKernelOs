/* img.c — PNG, GIF, JPEG, BMP -> пиксели 0xAARRGGBB (см. img.h).
 *
 * Устройство:
 *   inflate      — распаковка zlib/deflate (PNG): блоки без сжатия, с фиксированным
 *                  и с переданным кодом Хаффмана; окно не нужно — пишем сразу в выход.
 *   PNG          — склеить IDAT, inflate, снять фильтры строк (Sub, Up, Average, Paeth),
 *                  развернуть в ARGB; черезстрочный Adam7 — 7 проходов.
 *   GIF          — LZW с переменной длиной кода, первый кадр, прозрачность из GCE.
 *   JPEG         — маркеры, таблицы Хаффмана и квантования; все сканы складываем в
 *                  коэффициенты (так прогрессивный и обычный JPEG — один и тот же код),
 *                  в конце целочисленное обратное ДКП (как в libjpeg), YCbCr -> RGB.
 *   BMP          — просто строки снизу вверх.
 * Защита от испорченных файлов: все чтения проверяют границы, размеры ограничены. */
#ifdef IMG_HOST
#include <stdlib.h>
#include <string.h>
#else
#include "myos.h"
#endif
#include "img.h"

typedef unsigned char u8;
typedef unsigned int u32;

#define MAX_DIM 16384
#define MAX_PIXELS (24 * 1024 * 1024)     /* до уменьшения */

static int rd16be(const u8 *p) { return (p[0] << 8) | p[1]; }
static u32 rd32be(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static int rd16le(const u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32le(const u8 *p) { return p[0] | (p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }

static int dims_ok(int w, int h) { return w > 0 && h > 0 && w <= MAX_DIM && h <= MAX_DIM && w <= MAX_PIXELS / h; }

static unsigned *new_px(int w, int h) { return malloc(sizeof(unsigned) * w * h); }

/* ====================================================================== inflate */

struct huff {                       /* канонический код Хаффмана: сколько кодов каждой длины и символы */
    unsigned short count[16];
    unsigned short sym[320];
};

struct bits {
    const u8 *p, *end;
    u32 buf;
    int n;
    int bad;
};

static int getbit(struct bits *b) {
    if (!b->n) {
        if (b->p >= b->end) { b->bad = 1; return 0; }
        b->buf = *b->p++;
        b->n = 8;
    }
    int v = b->buf & 1;
    b->buf >>= 1;
    b->n--;
    return v;
}

static u32 getbits(struct bits *b, int n) {
    u32 v = 0;
    for (int i = 0; i < n; i++) v |= (u32)getbit(b) << i;
    return v;
}

static int huff_build(struct huff *h, const u8 *lens, int n) {
    int offs[16];
    for (int i = 0; i < 16; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[lens[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int i = 1; i < 16; i++) { left <<= 1; left -= h->count[i]; if (left < 0) return -1; }
    offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = offs[i] + h->count[i];
    for (int i = 0; i < n; i++) if (lens[i]) h->sym[offs[lens[i]]++] = i;
    return 0;
}

static int huff_decode(struct bits *b, const struct huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= getbit(b);
        int count = h->count[len];
        if (code - count < first) return h->sym[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (b->bad) return -1;
    }
    return -1;
}

static const unsigned short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59,
                                             67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769,
                                              1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

int img_inflate(const u8 *in, int len, u8 *out, int max, int zlib_header) {
    struct bits b = { in, in + len, 0, 0, 0 };
    if (zlib_header) {
        if (len < 2 || (in[0] & 15) != 8 || ((in[0] << 8) | in[1]) % 31) return -1;
        if (in[1] & 0x20) return -1;                       /* словарь — не бывает в PNG */
        b.p += 2;
    }
    static struct huff lit, dist;                          /* 1.4 КБ — не на стеке */
    int o = 0, last;
    do {
        last = getbit(&b);
        int type = getbits(&b, 2);
        if (b.bad) return o ? o : -1;
        if (type == 0) {                                   /* без сжатия */
            b.n = 0;
            if (b.end - b.p < 4) return o ? o : -1;
            int n = rd16le(b.p);
            b.p += 4;
            if (b.end - b.p < n) n = b.end - b.p;
            if (n > max - o) n = max - o;
            memcpy(out + o, b.p, n);
            b.p += n;
            o += n;
            continue;
        }
        if (type == 3) return o ? o : -1;
        if (type == 1) {                                   /* фиксированный код */
            static u8 l[320];
            int i = 0;
            for (; i < 144; i++) l[i] = 8;
            for (; i < 256; i++) l[i] = 9;
            for (; i < 280; i++) l[i] = 7;
            for (; i < 288; i++) l[i] = 8;
            huff_build(&lit, l, 288);
            for (i = 0; i < 30; i++) l[i] = 5;
            huff_build(&dist, l, 30);
        } else {                                           /* свой код в начале блока */
            int hlit = getbits(&b, 5) + 257, hdist = getbits(&b, 5) + 1, hclen = getbits(&b, 4) + 4;
            static const u8 order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
            u8 cl[19], l[320];
            struct huff ch;
            memset(cl, 0, sizeof(cl));
            for (int i = 0; i < hclen; i++) cl[order[i]] = getbits(&b, 3);
            if (huff_build(&ch, cl, 19) < 0) return o ? o : -1;
            int i = 0;
            while (i < hlit + hdist) {
                int s = huff_decode(&b, &ch);
                if (s < 0 || b.bad) return o ? o : -1;
                if (s < 16) { l[i++] = s; continue; }
                int rep, v = 0;
                if (s == 16) { if (!i) return o ? o : -1; v = l[i - 1]; rep = 3 + getbits(&b, 2); }
                else if (s == 17) rep = 3 + getbits(&b, 3);
                else rep = 11 + getbits(&b, 7);
                if (i + rep > hlit + hdist) return o ? o : -1;
                while (rep--) l[i++] = v;
            }
            if (huff_build(&lit, l, hlit) < 0 || huff_build(&dist, l + hlit, hdist) < 0) return o ? o : -1;
        }
        for (;;) {
            int s = huff_decode(&b, &lit);
            if (s < 0 || b.bad) return o ? o : -1;
            if (s < 256) { if (o >= max) return o; out[o++] = s; continue; }
            if (s == 256) break;
            s -= 257;
            if (s >= 29) return o ? o : -1;
            int n = len_base[s] + getbits(&b, len_extra[s]);
            int d = huff_decode(&b, &dist);
            if (d < 0 || d >= 30) return o ? o : -1;
            int back = dist_base[d] + getbits(&b, dist_extra[d]);
            if (back > o) return o ? o : -1;
            if (n > max - o) n = max - o;
            for (int k = 0; k < n; k++, o++) out[o] = out[o - back];
            if (o >= max) return o;
        }
    } while (!last);
    return o;
}

/* ====================================================================== PNG */

static int paeth(int a, int b, int c) {
    int p = a + b - c, pa = p > a ? p - a : a - p, pb = p > b ? p - b : b - p, pc = p > c ? p - c : c - p;
    return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}

/* снять фильтры с прохода w x h; raw — строки «фильтр + байты»; -> 0 */
static int unfilter(u8 *raw, int rowbytes, int h, int bpp) {
    u8 *prev = NULL;
    for (int y = 0; y < h; y++) {
        u8 *row = raw + y * (rowbytes + 1);
        int f = row[0];
        u8 *r = row + 1;
        for (int i = 0; i < rowbytes; i++) {
            int a = i >= bpp ? r[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0;
            switch (f) {
            case 0: break;
            case 1: r[i] += a; break;
            case 2: r[i] += b; break;
            case 3: r[i] += (a + b) >> 1; break;
            case 4: r[i] += paeth(a, b, c); break;
            default: return -1;
            }
        }
        prev = r;
    }
    return 0;
}

struct png_info { int w, h, depth, ctype, channels; u8 pal[256][4]; int npal; int trns_gray, trns_r, trns_g, trns_b, has_trns; };

/* одна строка прохода -> пиксели out[x * step] */
static void png_row(const struct png_info *pi, const u8 *r, int w, unsigned *out, int step, int *alpha) {
    int d = pi->depth;
    for (int x = 0; x < w; x++) {
        unsigned a = 255, R, G, B;
        int s;                                              /* номер образца в строке */
        #define SAMPLE(i) (d == 16 ? r[(i) * 2] : d == 8 ? r[i] : \
            (int)(((r[(i) * d / 8] >> (8 - d - ((i) * d) % 8)) & ((1 << d) - 1))))
        #define SAMPLE16(i) (d == 16 ? (r[(i) * 2] << 8 | r[(i) * 2 + 1]) : -1)
        switch (pi->ctype) {
        case 0: {
            s = x;
            int v = SAMPLE(s);
            int raw = d == 16 ? SAMPLE16(s) : v;
            if (pi->has_trns && raw == pi->trns_gray) a = 0;
            if (d < 8) v = v * 255 / ((1 << d) - 1);
            R = G = B = v;
            break;
        }
        case 2:
            s = x * 3;
            R = SAMPLE(s); G = SAMPLE(s + 1); B = SAMPLE(s + 2);
            if (pi->has_trns) {
                int r0 = d == 16 ? SAMPLE16(s) : (int)R, g0 = d == 16 ? SAMPLE16(s + 1) : (int)G, b0 = d == 16 ? SAMPLE16(s + 2) : (int)B;
                if (r0 == pi->trns_r && g0 == pi->trns_g && b0 == pi->trns_b) a = 0;
            }
            break;
        case 3: {
            int i = SAMPLE(x);
            if (i >= pi->npal) { R = G = B = 0; a = 255; }
            else { R = pi->pal[i][0]; G = pi->pal[i][1]; B = pi->pal[i][2]; a = pi->pal[i][3]; }
            break;
        }
        case 4:
            s = x * 2;
            R = G = B = SAMPLE(s);
            a = SAMPLE(s + 1);
            break;
        default:
            s = x * 4;
            R = SAMPLE(s); G = SAMPLE(s + 1); B = SAMPLE(s + 2);
            a = SAMPLE(s + 3);
            break;
        }
        #undef SAMPLE
        #undef SAMPLE16
        if (a != 255) *alpha = 1;
        out[x * step] = a << 24 | R << 16 | G << 8 | B;
    }
}

static int png_decode(const u8 *d, int len, img *out) {
    if (len < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return -1;
    struct png_info pi;
    memset(&pi, 0, sizeof(pi));
    int interlace = 0;
    u8 *idat = NULL;
    int ilen = 0, icap = 0;
    int p = 8, have_ihdr = 0;
    while (p + 8 <= len) {
        u32 clen = rd32be(d + p);
        const u8 *type = d + p + 4, *c = d + p + 8;
        if (clen > (u32)(len - p - 8)) clen = len - p - 8;           /* обрезанный файл — что есть */
        if (!memcmp(type, "IHDR", 4) && clen >= 13) {
            pi.w = rd32be(c); pi.h = rd32be(c + 4);
            pi.depth = c[8]; pi.ctype = c[9]; interlace = c[12];
            have_ihdr = 1;
        } else if (!memcmp(type, "PLTE", 4)) {
            pi.npal = clen / 3 > 256 ? 256 : clen / 3;
            for (int i = 0; i < pi.npal; i++) { pi.pal[i][0] = c[i * 3]; pi.pal[i][1] = c[i * 3 + 1]; pi.pal[i][2] = c[i * 3 + 2]; pi.pal[i][3] = 255; }
        } else if (!memcmp(type, "tRNS", 4)) {
            pi.has_trns = 1;
            if (pi.ctype == 3) for (u32 i = 0; i < clen && i < 256; i++) pi.pal[i][3] = c[i];
            else if (pi.ctype == 0 && clen >= 2) pi.trns_gray = rd16be(c);
            else if (pi.ctype == 2 && clen >= 6) { pi.trns_r = rd16be(c); pi.trns_g = rd16be(c + 2); pi.trns_b = rd16be(c + 4); }
            if (pi.ctype == 3) pi.has_trns = 0;                        /* у палитры — прямо в pal */
        } else if (!memcmp(type, "IDAT", 4)) {
            if (ilen + (int)clen > icap) {
                int nc = (ilen + clen) * 2 + 4096;
                u8 *n = realloc(idat, nc);
                if (!n) { free(idat); return -2; }
                idat = n; icap = nc;
            }
            memcpy(idat + ilen, c, clen);
            ilen += clen;
        } else if (!memcmp(type, "IEND", 4)) break;
        p += 12 + clen;
    }
    if (!have_ihdr || !idat || !dims_ok(pi.w, pi.h)) { free(idat); return -1; }
    int ch = pi.ctype == 0 ? 1 : pi.ctype == 2 ? 3 : pi.ctype == 3 ? 1 : pi.ctype == 4 ? 2 : pi.ctype == 6 ? 4 : 0;
    int dok = pi.depth == 1 || pi.depth == 2 || pi.depth == 4 || pi.depth == 8 || pi.depth == 16;
    if (!ch || !dok || (pi.ctype == 3 && pi.depth == 16) || ((pi.ctype == 2 || pi.ctype >= 4) && pi.depth < 8)) { free(idat); return -1; }
    pi.channels = ch;
    int bpp = (ch * pi.depth + 7) / 8;                                  /* байт на пиксель для фильтров */
    /* размер распакованного: для Adam7 — сумма проходов */
    static const int ax[7] = { 0, 4, 0, 2, 0, 1, 0 }, ay[7] = { 0, 0, 4, 0, 2, 0, 1 },
                     dx[7] = { 8, 8, 4, 4, 2, 2, 1 }, dy[7] = { 8, 8, 8, 4, 4, 2, 2 };
    int passes = interlace ? 7 : 1;
    int total = 0;
    for (int k = 0; k < passes; k++) {
        int pw = interlace ? (pi.w - ax[k] + dx[k] - 1) / dx[k] : pi.w;
        int ph = interlace ? (pi.h - ay[k] + dy[k] - 1) / dy[k] : pi.h;
        if (pw <= 0 || ph <= 0) continue;
        int rb = (pw * ch * pi.depth + 7) / 8;
        if (rb + 1 > (0x7fffffff - total) / ph) { free(idat); return -1; }
        total += (rb + 1) * ph;
    }
    u8 *raw = malloc(total + 1);
    unsigned *px = new_px(pi.w, pi.h);
    if (!raw || !px) { free(idat); free(raw); free(px); return -2; }
    int got = img_inflate(idat, ilen, raw, total, 1);
    free(idat);
    if (got < 0) { free(raw); free(px); return -1; }
    if (got < total) memset(raw + got, 0, total - got);               /* обрезанная картинка: низ пустой */
    for (int i = 0; i < pi.w * pi.h; i++) px[i] = 0;
    int alpha = 0, off = 0;
    for (int k = 0; k < passes; k++) {
        int pw = interlace ? (pi.w - ax[k] + dx[k] - 1) / dx[k] : pi.w;
        int ph = interlace ? (pi.h - ay[k] + dy[k] - 1) / dy[k] : pi.h;
        if (pw <= 0 || ph <= 0) continue;
        int rb = (pw * ch * pi.depth + 7) / 8;
        u8 *r = raw + off;
        if (unfilter(r, rb, ph, bpp) < 0) {                           /* дальше мусор — оставим, что есть */
            if (off + (rb + 1) * ph > got) break;
        }
        for (int y = 0; y < ph; y++) {
            int yy = interlace ? ay[k] + y * dy[k] : y;
            int x0 = interlace ? ax[k] : 0, st = interlace ? dx[k] : 1;
            png_row(&pi, r + y * (rb + 1) + 1, pw, px + yy * pi.w + x0, st, &alpha);
        }
        off += (rb + 1) * ph;
    }
    free(raw);
    out->w = pi.w; out->h = pi.h; out->px = px; out->has_alpha = alpha;
    return 0;
}

/* ====================================================================== GIF */

static int gif_decode(const u8 *d, int len, img *out) {
    if (len < 13 || (memcmp(d, "GIF87a", 6) && memcmp(d, "GIF89a", 6))) return -1;
    int sw = rd16le(d + 6), sh = rd16le(d + 8), flags = d[10];
    int p = 13;
    unsigned gpal[256], lpal[256];
    int gn = 0;
    if (flags & 0x80) {
        gn = 2 << (flags & 7);
        if (p + gn * 3 > len) return -1;
        for (int i = 0; i < gn; i++) gpal[i] = 0xFF000000u | d[p + i * 3] << 16 | d[p + i * 3 + 1] << 8 | d[p + i * 3 + 2];
        p += gn * 3;
    }
    int transp = -1;
    while (p < len) {
        int b = d[p++];
        if (b == 0x3B) break;                                          /* конец */
        if (b == 0x21) {                                               /* расширение */
            if (p >= len) break;
            int label = d[p++];
            if (label == 0xF9 && p + 5 < len && d[p] >= 4 && (d[p + 1] & 1)) transp = d[p + 4];
            while (p < len && d[p]) p += d[p] + 1;                      /* подблоки */
            p++;
            continue;
        }
        if (b != 0x2C) return -1;
        if (p + 9 > len) return -1;
        int ix = rd16le(d + p), iy = rd16le(d + p + 2), iw = rd16le(d + p + 4), ih = rd16le(d + p + 6), f = d[p + 8];
        p += 9;
        unsigned *pal = gpal;
        int npal = gn;
        if (f & 0x80) {
            npal = 2 << (f & 7);
            if (p + npal * 3 > len) return -1;
            for (int i = 0; i < npal; i++) lpal[i] = 0xFF000000u | d[p + i * 3] << 16 | d[p + i * 3 + 1] << 8 | d[p + i * 3 + 2];
            p += npal * 3;
            pal = lpal;
        }
        if (sw <= 0 || sh <= 0) { sw = ix + iw; sh = iy + ih; }
        if (!dims_ok(sw, sh) || iw <= 0 || ih <= 0 || iw > MAX_DIM || ih > MAX_DIM || iw > MAX_PIXELS / ih) return -1;
        int interlaced = f & 0x40;
        if (p >= len) return -1;
        int minbits = d[p++];
        if (minbits < 2 || minbits > 11) return -1;
        /* склеить подблоки данных */
        int dl = 0, q = p;
        while (q < len && d[q]) { dl += d[q]; q += d[q] + 1; }
        u8 *data = malloc(dl + 4);
        u8 *idx = malloc(iw * ih);
        unsigned *px = new_px(sw, sh);
        static unsigned short prefix[4096];
        static u8 suffix[4096], stack[4097];
        if (!data || !idx || !px) { free(data); free(idx); free(px); return -2; }
        dl = 0;
        while (p < len && d[p]) {
            int n = d[p];
            if (p + 1 + n > len) n = len - p - 1;
            memcpy(data + dl, d + p + 1, n);
            dl += n;
            p += d[p] + 1;
        }
        memset(idx, 0, iw * ih);
        /* LZW */
        int clear = 1 << minbits, eoi = clear + 1, avail = clear + 2, bits = minbits + 1, mask = (1 << bits) - 1;
        int old = -1, first = 0, o = 0, total = iw * ih;
        u32 acc = 0;
        int nacc = 0, bp = 0;
        for (int i = 0; i < clear; i++) { prefix[i] = 0; suffix[i] = i; }
        while (o < total) {
            while (nacc < bits && bp < dl) { acc |= (u32)data[bp++] << nacc; nacc += 8; }
            if (nacc < bits) break;
            int code = acc & mask;
            acc >>= bits; nacc -= bits;
            if (code == clear) { bits = minbits + 1; mask = (1 << bits) - 1; avail = clear + 2; old = -1; continue; }
            if (code == eoi) break;
            if (old < 0) {
                if (code >= clear) break;
                idx[o++] = code; old = first = code;
                continue;
            }
            int in = code, sp = 0;
            if (code > avail) break;
            if (code == avail) { stack[sp++] = first; code = old; }
            while (code >= clear && sp < 4096) { stack[sp++] = suffix[code]; code = prefix[code]; }
            if (code >= clear) break;
            first = suffix[code];
            stack[sp++] = first;
            if (avail < 4096) {
                prefix[avail] = old; suffix[avail] = first; avail++;
                if (!(avail & mask) && avail < 4096) { bits++; mask = (1 << bits) - 1; }
            }
            old = in;
            while (sp && o < total) idx[o++] = stack[--sp];
        }
        free(data);
        int alpha = transp >= 0;
        for (int i = 0; i < sw * sh; i++) px[i] = 0;                    /* за кадром — прозрачно */
        if (ix + iw > sw || iy + ih > sh) alpha = 1;
        if (ix > 0 || iy > 0 || iw < sw || ih < sh) alpha = 1;
        for (int r = 0; r < ih; r++) {
            int row = r;
            if (interlaced) {                                          /* строки: 0,8,16.. 4,12.. 2,6.. 1,3.. */
                int n1 = (ih + 7) / 8, n2 = (ih + 3) / 8, n3 = (ih + 1) / 4;
                if (r < n1) row = r * 8;
                else if (r < n1 + n2) row = (r - n1) * 8 + 4;
                else if (r < n1 + n2 + n3) row = (r - n1 - n2) * 4 + 2;
                else row = (r - n1 - n2 - n3) * 2 + 1;
            }
            int yy = iy + row;
            if (yy >= sh || row >= ih) continue;
            for (int x = 0; x < iw; x++) {
                int xx = ix + x;
                if (xx >= sw) break;
                int c = idx[r * iw + x];
                px[yy * sw + xx] = c == transp || c >= npal ? 0 : pal[c];
            }
        }
        free(idx);
        out->w = sw; out->h = sh; out->px = px; out->has_alpha = alpha;
        return 0;
    }
    return -1;
}

/* ====================================================================== BMP */

static int bmp_decode(const u8 *d, int len, img *out) {
    if (len < 26 || d[0] != 'B' || d[1] != 'M') return -1;
    u32 off = rd32le(d + 10), hs = rd32le(d + 14);
    int w, h, bpp, comp = 0;
    if (hs == 12) { w = rd16le(d + 18); h = (short)rd16le(d + 20); bpp = rd16le(d + 24); }
    else {
        if (len < 54) return -1;
        w = (int)rd32le(d + 18); h = (int)rd32le(d + 22); bpp = rd16le(d + 28); comp = rd32le(d + 30);
    }
    int topdown = h < 0;
    if (topdown) h = -h;
    if (!dims_ok(w, h) || off >= (u32)len) return -1;
    if (comp != 0 && comp != 3) return -1;                              /* RLE не умеем */
    u32 rm = 0xFF0000, gm = 0xFF00, bm = 0xFF, am = 0;
    if (bpp == 16) { rm = 0x7C00; gm = 0x3E0; bm = 0x1F; }
    if (comp == 3 && 14 + hs + 12 <= (u32)len) {
        const u8 *m = d + 14 + (hs >= 52 ? 40 : hs);
        if (hs == 40 && 14 + 40 + 12 <= len) m = d + 54;
        rm = rd32le(m); gm = rd32le(m + 4); bm = rd32le(m + 8);
        if (hs >= 56) am = rd32le(d + 14 + 52);
    } else if (bpp == 32) am = 0;
    unsigned pal[256];
    int npal = 0;
    if (bpp <= 8) {
        int ps = hs == 12 ? 3 : 4;
        npal = (off - 14 - hs) / ps;
        if (npal > 256) npal = 256;
        for (int i = 0; i < npal; i++) {
            const u8 *c = d + 14 + hs + i * ps;
            pal[i] = c + 3 <= d + len ? 0xFF000000u | c[2] << 16 | c[1] << 8 | c[0] : 0xFF000000u;
        }
    } else if (bpp != 16 && bpp != 24 && bpp != 32) return -1;
    int stride = ((w * bpp + 31) / 32) * 4;
    unsigned *px = new_px(w, h);
    if (!px) return -2;
    int alpha = 0;
    #define CH(v, m) ((m) ? (int)(((v) & (m)) * 255u / ((m) / ((m) & -(m)) * ((m) & -(m)))) : 0)
    for (int y = 0; y < h; y++) {
        const u8 *r = d + off + (topdown ? y : h - 1 - y) * stride;
        unsigned *o = px + y * w;
        for (int x = 0; x < w; x++) {
            unsigned c = 0xFF000000u;
            if (r + (x * bpp + bpp) / 8 > d + len) { o[x] = c; continue; }
            if (bpp <= 8) {
                int i = (r[x * bpp / 8] >> (8 - bpp - (x * bpp) % 8)) & ((1 << bpp) - 1);
                c = i < npal ? pal[i] : 0xFF000000u;
            } else if (bpp == 24) c = 0xFF000000u | r[x * 3 + 2] << 16 | r[x * 3 + 1] << 8 | r[x * 3];
            else {
                u32 v = bpp == 16 ? (u32)rd16le(r + x * 2) : rd32le(r + x * 4);
                int R = 0, G = 0, B = 0, A = 255;
                u32 ms[4] = { rm, gm, bm, am };
                int vals[4];
                for (int k = 0; k < 4; k++) {
                    u32 m = ms[k];
                    if (!m) { vals[k] = -1; continue; }
                    int sh = 0;
                    while (!((m >> sh) & 1)) sh++;
                    u32 mx = m >> sh;
                    vals[k] = (int)(((v & m) >> sh) * 255u / mx);
                }
                R = vals[0]; G = vals[1]; B = vals[2];
                if (vals[3] >= 0) { A = vals[3]; if (A != 255) alpha = 1; }
                c = (u32)A << 24 | R << 16 | G << 8 | B;
            }
            o[x] = c;
        }
    }
    #undef CH
    if (alpha) {                                                        /* 32 бита, где альфа вся 0 — значит её нет */
        int any = 0;
        for (int i = 0; i < w * h && !any; i++) if (px[i] >> 24) any = 1;
        if (!any) { for (int i = 0; i < w * h; i++) px[i] |= 0xFF000000u; alpha = 0; }
    }
    out->w = w; out->h = h; out->px = px; out->has_alpha = alpha;
    return 0;
}

/* ====================================================================== JPEG */

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

/* ====================================================================== общее */

const char *img_type(const u8 *d, int len) {
    if (len >= 8 && !memcmp(d, "\x89PNG", 4)) return "png";
    if (len >= 6 && !memcmp(d, "GIF8", 4)) return "gif";
    if (len >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpeg";
    if (len >= 2 && d[0] == 'B' && d[1] == 'M') return "bmp";
    return NULL;
}

int img_size(const u8 *d, int len, int *w, int *h) {
    const char *t = img_type(d, len);
    if (!t) return -1;
    if (t[0] == 'p' && len >= 24) { *w = rd32be(d + 16); *h = rd32be(d + 20); return 0; }
    if (t[0] == 'g' && len >= 10) { *w = rd16le(d + 6); *h = rd16le(d + 8); return 0; }
    if (t[0] == 'b' && len >= 26) { *w = (int)rd32le(d + 18); int hh = (int)rd32le(d + 22); *h = hh < 0 ? -hh : hh; return 0; }
    if (t[0] == 'j') {
        for (int p = 2; p + 9 < len;) {
            if (d[p] != 0xFF) { p++; continue; }
            int m = d[p + 1];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) { *h = rd16be(d + p + 5); *w = rd16be(d + p + 7); return 0; }
            if (m == 0xD8 || m == 0xFF || (m >= 0xD0 && m <= 0xD7)) { p += m == 0xFF ? 1 : 2; continue; }
            p += 2 + rd16be(d + p + 2);
        }
    }
    return -1;
}

unsigned *img_scale(const unsigned *src, int sw, int sh, int dw, int dh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return NULL;
    unsigned *dst = new_px(dw, dh);
    if (!dst) return NULL;
    if (dw <= sw && dh <= sh) {                              /* уменьшение: среднее по площади (премножив альфу) */
        for (int y = 0; y < dh; y++) {
            int y0 = y * sh / dh, y1 = (y + 1) * sh / dh;
            if (y1 <= y0) y1 = y0 + 1;
            for (int x = 0; x < dw; x++) {
                int x0 = x * sw / dw, x1 = (x + 1) * sw / dw;
                if (x1 <= x0) x1 = x0 + 1;
                unsigned a = 0, r = 0, g = 0, b = 0, n = 0;
                int stepy = (y1 - y0 + 3) / 4, stepx = (x1 - x0 + 3) / 4;   /* не больше 4x4 образцов */
                for (int yy = y0; yy < y1; yy += stepy)
                    for (int xx = x0; xx < x1; xx += stepx) {
                        unsigned c = src[yy * sw + xx], ca = c >> 24;
                        a += ca; r += ((c >> 16) & 255) * ca; g += ((c >> 8) & 255) * ca; b += (c & 255) * ca; n++;
                    }
                dst[y * dw + x] = a ? (a / n) << 24 | (r / a) << 16 | (g / a) << 8 | (b / a) : 0;
            }
        }
        return dst;
    }
    for (int y = 0; y < dh; y++) {                           /* увеличение: билинейно, в 1/256 */
        int fy = (y * 2 + 1) * sh * 128 / dh - 128;
        if (fy < 0) fy = 0;
        int y0 = fy >> 8, wy = fy & 255, y1 = y0 + 1 < sh ? y0 + 1 : y0;
        for (int x = 0; x < dw; x++) {
            int fx = (x * 2 + 1) * sw * 128 / dw - 128;
            if (fx < 0) fx = 0;
            int x0 = fx >> 8, wx = fx & 255, x1 = x0 + 1 < sw ? x0 + 1 : x0;
            if (x0 >= sw) x0 = sw - 1;
            if (y0 >= sh) y0 = sh - 1;
            unsigned c00 = src[y0 * sw + x0], c01 = src[y0 * sw + x1], c10 = src[y1 * sw + x0], c11 = src[y1 * sw + x1];
            unsigned res = 0;
            for (int sft = 0; sft < 32; sft += 8) {
                int a = (c00 >> sft) & 255, b = (c01 >> sft) & 255, c = (c10 >> sft) & 255, d = (c11 >> sft) & 255;
                int top = a * (256 - wx) + b * wx, bot = c * (256 - wx) + d * wx;
                int v = (top * (256 - wy) + bot * wy) >> 16;
                res |= (unsigned)v << sft;
            }
            dst[y * dw + x] = res;
        }
    }
    return dst;
}

int img_decode(const u8 *d, int len, img *out, int max_side) {
    memset(out, 0, sizeof(*out));
    const char *t = img_type(d, len);
    if (!t) return -1;
    int r;
    if (t[0] == 'j') {
        int w = 0, h = 0, scale = 1;                          /* огромное фото — распаковать сразу мельче */
        img_size(d, len, &w, &h);
        int side = w > h ? w : h;
        while (max_side > 0 && scale < 8 && side / (scale * 2) >= max_side) scale *= 2;
        r = jpeg_decode(d, len, out, scale);
        if (r == -2 && scale < 8) {                           /* не хватило памяти — попробуем мельче */
            for (scale *= 2; r == -2 && scale <= 8; scale *= 2) r = jpeg_decode(d, len, out, scale);
        }
    } else if (t[0] == 'p') r = png_decode(d, len, out);
    else if (t[0] == 'g') r = gif_decode(d, len, out);
    else r = bmp_decode(d, len, out);
    if (r < 0) return r;
    int side = out->w > out->h ? out->w : out->h;
    if (max_side > 0 && side > max_side) {
        int nw = (int)((unsigned)out->w * max_side / side), nh = (int)((unsigned)out->h * max_side / side);
        if (nw < 1) nw = 1;
        if (nh < 1) nh = 1;
        unsigned *s = img_scale(out->px, out->w, out->h, nw, nh);
        if (s) { free(out->px); out->px = s; out->w = nw; out->h = nh; }
    }
    return 0;
}

void img_free(img *im) { free(im->px); im->px = NULL; }
