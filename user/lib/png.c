#ifdef IMG_HOST
#include <stdlib.h>
#include <string.h>
#else
#include "myos.h"
#endif
#include "img.h"
#include "../include/zlib.h"
#include "../include/png.h"

typedef unsigned char u8;
typedef unsigned int u32;
static int rd16be(const u8 *p) { return (p[0] << 8) | p[1]; }
static u32 rd32be(const u8 *p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static int dims_ok(int w, int h) {
    return w > 0 && h > 0 && w <= 16384 && h <= 16384 && w <= (24 * 1024 * 1024) / h;
}
static unsigned *new_px(int w, int h) { return malloc(sizeof(unsigned) * (size_t)w * (size_t)h); }

static int png_inflate(const u8 *in, int len, u8 *out, int max, int zlib_header) {
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
    int got = png_inflate(idat, ilen, raw, total, 1);
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

int png_decode_argb(const unsigned char *d, int len, int *w, int *h, unsigned **px, int *alpha) {
    img im;
    int r = png_decode((const u8 *)d, len, &im);
    if (r) return r;
    *w = im.w; *h = im.h; *px = im.px; if (alpha) *alpha = im.has_alpha;
    return 0;
}

/* Запись 8-битных PNG и тонкая обёртка вызовов. Чтение в png_read_image
 * всегда уже раскрыто в 8 бит RGB или RGBA: отдельного сырого режима нет. */
static const u8 png_sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

struct png_struct_def {
    int writing, error, has_alpha, rows;
    unsigned w, h;
    int depth, color;
    unsigned *px;
    u8 *plte; int plte_n;
    u8 *trns; int trns_n;
    u8 *file; int file_n, file_cap;
    u8 *raw; int raw_n, raw_cap;
    void *io;
    png_rw_ptr rw;
};
struct png_info_def { int unused; };

static int file_add(png_struct *png, const u8 *p, int n) {
    if (n <= 0) return 0;
    if (png->file_n > 32 * 1024 * 1024 - n) return -1;
    if (png->file_n + n > png->file_cap) {
        int c = png->file_cap ? png->file_cap * 2 : 256;
        u8 *q;
        while (c < png->file_n + n) c *= 2;
        q = realloc(png->file, c);
        if (!q) return -1;
        png->file = q;
        png->file_cap = c;
    }
    memcpy(png->file + png->file_n, p, n);
    png->file_n += n;
    return 0;
}

static int pull(png_struct *png, u8 *dst, int n) {
    if (!png->rw || n < 0) return -1;
    png->rw(png, dst, (png_size_t)n);
    if (png->error) return -1;
    return file_add(png, dst, n);
}

static void be32(u8 *p, u32 v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }

static int chunk_add(png_struct *png, const char *type, const u8 *data, int n) {
    u8 len[4], crc[4];
    uLong c;
    be32(len, (u32)n);
    if (file_add(png, len, 4) || file_add(png, (const u8 *)type, 4)) return -1;
    if (n && file_add(png, data, n)) return -1;
    c = crc32(0, (const Bytef *)type, 4);
    if (n) c = crc32(c, data, (uInt)n);
    be32(crc, (u32)c);
    return file_add(png, crc, 4);
}

static int channels_of(int color) {
    if (color == 0 || color == 3) return 1;
    if (color == 2) return 3;
    if (color == 4) return 2;
    if (color == 6) return 4;
    return 0;
}

int png_encode_argb(const unsigned *px, int w, int h, int with_alpha, unsigned char **out, int *out_n) {
    png_struct png;
    int ch = with_alpha ? 4 : 3, y, x, raw_n, bound;
    u8 *raw, *comp;
    uLong clen;
    u8 ihdr[13];
    if (!px || !out || w < 1 || h < 1 || !dims_ok(w, h)) return -1;
    raw_n = h * (1 + w * ch);
    raw = malloc(raw_n);
    if (!raw) return -2;
    for (y = 0; y < h; y++) {
        u8 *row = raw + y * (1 + w * ch);
        row[0] = 0;
        for (x = 0; x < w; x++) {
            unsigned p = px[y * w + x];
            u8 *d = row + 1 + x * ch;
            d[0] = p >> 16; d[1] = p >> 8; d[2] = p;
            if (with_alpha) d[3] = p >> 24;
        }
    }
    bound = (int)compressBound((uLong)raw_n);
    comp = malloc(bound > 0 ? bound : 1);
    if (!comp) { free(raw); return -2; }
    clen = (uLong)bound;
    if (compress(comp, &clen, raw, (uLong)raw_n) != Z_OK) { free(raw); free(comp); return -1; }
    free(raw);
    memset(&png, 0, sizeof png);
    if (file_add(&png, png_sig, 8)) { free(comp); free(png.file); return -2; }
    be32(ihdr, (u32)w); be32(ihdr + 4, (u32)h);
    ihdr[8] = 8; ihdr[9] = with_alpha ? 6 : 2; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (chunk_add(&png, "IHDR", ihdr, 13) || chunk_add(&png, "IDAT", comp, (int)clen) || chunk_add(&png, "IEND", NULL, 0)) {
        free(comp); free(png.file); return -2;
    }
    free(comp);
    *out = png.file;
    if (out_n) *out_n = png.file_n;
    return 0;
}

static png_structp png_new(int writing) {
    png_structp png = calloc(1, sizeof *png);
    if (png) png->writing = writing;
    return png;
}

png_structp png_create_read_struct(const char *ver, void *error_ptr, void *error_fn, void *warn_fn) {
    (void)ver; (void)error_ptr; (void)error_fn; (void)warn_fn;
    return png_new(0);
}
png_structp png_create_write_struct(const char *ver, void *error_ptr, void *error_fn, void *warn_fn) {
    (void)ver; (void)error_ptr; (void)error_fn; (void)warn_fn;
    return png_new(1);
}
png_infop png_create_info_struct(png_structp png) {
    (void)png;
    return calloc(1, sizeof(png_info));
}
static void png_free_body(png_structp png) {
    if (!png) return;
    free(png->px); free(png->plte); free(png->trns); free(png->file); free(png->raw);
    free(png);
}
void png_destroy_read_struct(png_structpp png, png_infopp info, png_infopp end) {
    if (png) { png_free_body(*png); *png = NULL; }
    if (info) { free(*info); *info = NULL; }
    if (end && *end) { free(*end); *end = NULL; }
}
void png_destroy_write_struct(png_structpp png, png_infopp info) {
    png_destroy_read_struct(png, info, NULL);
}
void png_init_io(png_structp png, void *file) { if (png) png->io = file; }
void *png_get_io_ptr(png_structp png) { return png ? png->io : NULL; }
void png_set_read_fn(png_structp png, void *io_ptr, png_rw_ptr read_fn) {
    if (!png) return;
    png->io = io_ptr; png->rw = read_fn;
}
void png_set_write_fn(png_structp png, void *io_ptr, png_rw_ptr write_fn, void *flush_fn) {
    (void)flush_fn;
    if (!png) return;
    png->io = io_ptr; png->rw = write_fn;
}
int png_sig_cmp(png_const_bytep sig, png_size_t start, png_size_t num) {
    if (!sig || start > 8) return 1;
    if (start + num > 8) num = 8 - start;
    return memcmp(sig + start, png_sig + start, num) != 0;
}
void png_set_expand(png_structp png) { (void)png; }
void png_set_strip_16(png_structp png) { (void)png; }
void png_set_packing(png_structp png) { (void)png; }
void png_set_palette_to_rgb(png_structp png) { (void)png; }
void png_set_gray_to_rgb(png_structp png) { (void)png; }
void png_set_tRNS_to_alpha(png_structp png) { (void)png; }
void png_read_info(png_structp png, png_infop info) {
    u8 hdr[8], chunk[8];
    int alpha = 0;
    unsigned *px = NULL;
    int w = 0, h = 0;
    (void)info;
    if (!png || !png->rw) return;
    if (pull(png, hdr, 8)) { png->error = 1; return; }
    for (;;) {
        u32 len;
        u8 *data;
        if (pull(png, chunk, 8)) { png->error = 1; return; }
        len = rd32be(chunk);
        if (len > 32u * 1024u * 1024u) { png->error = 1; return; }
        data = malloc(len + 4);
        if (!data) { png->error = 1; return; }
        if (pull(png, data, (int)len + 4)) { free(data); png->error = 1; return; }
        free(data);
        if (!memcmp(chunk + 4, "IEND", 4)) break;
    }
    if (png_decode_argb(png->file, png->file_n, &w, &h, &px, &alpha)) { png->error = 1; return; }
    png->w = (unsigned)w; png->h = (unsigned)h; png->px = px; png->has_alpha = alpha;
    png->depth = 8; png->color = alpha ? 6 : 2;
}
void png_read_update_info(png_structp png, png_infop info) { (void)png; (void)info; }
png_uint_32 png_get_image_width(png_structp png, png_infop info) { (void)info; return png ? png->w : 0; }
png_uint_32 png_get_image_height(png_structp png, png_infop info) { (void)info; return png ? png->h : 0; }
int png_get_color_type(png_structp png, png_infop info) { (void)info; return png ? png->color : 0; }
int png_get_bit_depth(png_structp png, png_infop info) { (void)info; return png ? png->depth : 0; }
void png_read_image(png_structp png, png_bytep *rows) {
    unsigned y, x, ch;
    if (!png || !rows || !png->px) return;
    ch = png->has_alpha ? 4 : 3;
    for (y = 0; y < png->h; y++) {
        u8 *row = rows[y];
        if (!row) continue;
        for (x = 0; x < png->w; x++) {
            unsigned p = png->px[y * png->w + x];
            u8 *d = row + x * ch;
            d[0] = p >> 16; d[1] = p >> 8; d[2] = p;
            if (ch == 4) d[3] = p >> 24;
        }
    }
}
void png_read_end(png_structp png, png_infop info) { (void)png; (void)info; }

void png_set_IHDR(png_structp png, png_infop info, png_uint_32 w, png_uint_32 h, int depth, int color, int interlace, int compression, int filter) {
    (void)info; (void)compression; (void)filter;
    if (!png || interlace) { if (png) png->error = 1; return; }
    if (depth != 8 || !channels_of(color)) { png->error = 1; return; }
    png->w = w; png->h = h; png->depth = depth; png->color = color;
}
void png_set_PLTE(png_structp png, png_infop info, const unsigned char *rgb, int n) {
    (void)info;
    if (!png || !rgb || n < 1 || n > 256) { if (png) png->error = 1; return; }
    free(png->plte);
    png->plte = malloc(n * 3);
    if (!png->plte) { png->error = 1; return; }
    memcpy(png->plte, rgb, n * 3);
    png->plte_n = n;
}
void png_set_tRNS(png_structp png, png_infop info, unsigned char *trans, int num, png_color_16 *color) {
    (void)info; (void)color;
    if (!png || !trans || num < 1 || num > 256) return;
    free(png->trns);
    png->trns = malloc(num);
    if (!png->trns) { png->error = 1; return; }
    memcpy(png->trns, trans, num);
    png->trns_n = num;
}
void png_write_info(png_structp png, png_infop info) { (void)png; (void)info; }
void png_write_image(png_structp png, png_bytep *rows) {
    int ch, y, stride;
    if (!png || !rows || png->error) return;
    ch = channels_of(png->color);
    stride = 1 + (int)png->w * ch;
    for (y = 0; y < (int)png->h; y++) {
        if (png->raw_n + stride > png->raw_cap) {
            int c = png->raw_cap ? png->raw_cap * 2 : 256;
            u8 *q;
            while (c < png->raw_n + stride) c *= 2;
            q = realloc(png->raw, c);
            if (!q) { png->error = 1; return; }
            png->raw = q; png->raw_cap = c;
        }
        png->raw[png->raw_n] = 0;
        if (rows[y]) memcpy(png->raw + png->raw_n + 1, rows[y], (size_t)png->w * ch);
        png->raw_n += stride;
        png->rows++;
    }
}
void png_write_end(png_structp png, png_infop info) {
    u8 ihdr[13];
    u8 *comp;
    uLong clen;
    int bound;
    (void)info;
    if (!png || png->error || !png->rw || !png->w || !png->h || png->rows != (int)png->h) {
        if (png) png->error = 1;
        return;
    }
    bound = (int)compressBound((uLong)png->raw_n);
    comp = malloc(bound > 0 ? bound : 1);
    if (!comp) { png->error = 1; return; }
    clen = (uLong)bound;
    if (compress(comp, &clen, png->raw, (uLong)png->raw_n) != Z_OK) { free(comp); png->error = 1; return; }
    png->file_n = 0;
    if (file_add(png, png_sig, 8)) { free(comp); png->error = 1; return; }
    be32(ihdr, png->w); be32(ihdr + 4, png->h);
    ihdr[8] = 8; ihdr[9] = (u8)png->color; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    if (chunk_add(png, "IHDR", ihdr, 13)) { free(comp); png->error = 1; return; }
    if (png->color == 3 && png->plte) {
        if (chunk_add(png, "PLTE", png->plte, png->plte_n * 3)) { free(comp); png->error = 1; return; }
        if (png->trns && chunk_add(png, "tRNS", png->trns, png->trns_n)) { free(comp); png->error = 1; return; }
    }
    if (chunk_add(png, "IDAT", comp, (int)clen) || chunk_add(png, "IEND", NULL, 0)) {
        free(comp); png->error = 1; return;
    }
    free(comp);
    png->rw(png, png->file, (png_size_t)png->file_n);
}
