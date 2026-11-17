/* img.c — PNG, GIF, JPEG, BMP -> пиксели 0xAARRGGBB (см. img.h).
 *
 * Устройство:
 *   PNG и JPEG разбираются в libpng и libjpeg (те же алгоритмы, что жили здесь).
 *   GIF          — LZW с переменной длиной кода, первый кадр, прозрачность из GCE.
 *   BMP          — просто строки снизу вверх.
 * Защита от испорченных файлов: все чтения проверяют границы, размеры ограничены. */
#ifdef IMG_HOST
#include <stdlib.h>
#include <string.h>
#else
#include "myos.h"
#endif
#include "img.h"
#include "../include/zlib.h"
#include "../include/png.h"
#include "../include/jpeglib.h"

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

int img_inflate(const u8 *in, int len, u8 *out, int max, int zlib_header) {
    z_stream s;
    int ret, got;
    if (!in || len < 0 || !out || max < 0) return -1;
    memset(&s, 0, sizeof s);
    if (inflateInit2(&s, zlib_header ? 15 : -15) != Z_OK) return -1;
    s.next_in = (Bytef *)in;
    s.avail_in = (uInt)len;
    s.next_out = out;
    s.avail_out = (uInt)max;
    ret = inflate(&s, Z_NO_FLUSH);
    got = (int)s.total_out;
    inflateEnd(&s);
    if (ret == Z_STREAM_END) return got;
    if (got > 0) return got;
    return -1;
}

static int png_decode(const u8 *d, int len, img *out) {
    unsigned *px = NULL;
    int w = 0, h = 0, alpha = 0, r = png_decode_argb(d, len, &w, &h, &px, &alpha);
    if (r) return r;
    out->w = w; out->h = h; out->px = px; out->has_alpha = alpha;
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
static int jpeg_decode(const u8 *d, int len, img *out, int scale) {
    unsigned *px = NULL;
    int w = 0, h = 0, r = jpeg_decode_argb(d, len, &w, &h, &px, scale);
    if (r) return r;
    out->w = w; out->h = h; out->px = px; out->has_alpha = 0;
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
