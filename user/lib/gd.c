/* gd.c — truecolor поверх libpng и libjpeg. Цвет пикселя — 0x00RRGGBB. */
#include "myos.h"
#include "../include/gd.h"
#include "../include/png.h"
#include "../include/jpeglib.h"

struct gdImageStruct {
    int sx, sy;
    int trueColor;
    int *pixels;
};

gdImagePtr gdImageCreateTrueColor(int sx, int sy) {
    gdImagePtr im;
    if (sx < 1 || sy < 1 || sx > 16384 || sy > 16384) return NULL;
    im = calloc(1, sizeof *im);
    if (!im) return NULL;
    im->sx = sx;
    im->sy = sy;
    im->trueColor = 1;
    im->pixels = calloc((size_t)sx * (size_t)sy, sizeof(int));
    if (!im->pixels) { free(im); return NULL; }
    return im;
}

void gdImageDestroy(gdImagePtr im) {
    if (!im) return;
    free(im->pixels);
    free(im);
}

void gdImageSetPixel(gdImagePtr im, int x, int y, int color) {
    if (!im || (unsigned)x >= (unsigned)im->sx || (unsigned)y >= (unsigned)im->sy) return;
    im->pixels[y * im->sx + x] = color & 0xffffff;
}

int gdImageGetPixel(gdImagePtr im, int x, int y) {
    if (!im || (unsigned)x >= (unsigned)im->sx || (unsigned)y >= (unsigned)im->sy) return 0;
    return im->pixels[y * im->sx + x];
}

int gdImageColorAllocate(gdImagePtr im, int r, int g, int b) {
    (void)im;
    return gdTrueColor(r, g, b);
}

void *gdImagePngPtr(gdImagePtr im, int *size) {
    unsigned char *out = NULL;
    int n = 0, i;
    unsigned *px;
    if (!im) return NULL;
    px = malloc(sizeof(unsigned) * (size_t)im->sx * (size_t)im->sy);
    if (!px) return NULL;
    for (i = 0; i < im->sx * im->sy; i++) px[i] = 0xff000000u | (unsigned)im->pixels[i];
    if (png_encode_argb(px, im->sx, im->sy, 0, &out, &n)) { free(px); return NULL; }
    free(px);
    if (size) *size = n;
    return out;
}

void *gdImageJpegPtr(gdImagePtr im, int *size, int quality) {
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr err;
    unsigned char *out = NULL, *row = NULL;
    unsigned long n = 0;
    int y, x;
    if (!im) return NULL;
    memset(&c, 0, sizeof c);
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &n);
    c.image_width = (unsigned)im->sx;
    c.image_height = (unsigned)im->sy;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, quality > 0 ? quality : 75, 1);
    jpeg_start_compress(&c, 1);
    row = malloc((size_t)im->sx * 3);
    if (!row) { jpeg_destroy_compress(&c); return NULL; }
    for (y = 0; y < im->sy; y++) {
        unsigned char *scan = row;
        for (x = 0; x < im->sx; x++) {
            unsigned p = (unsigned)im->pixels[y * im->sx + x];
            row[x * 3] = p >> 16;
            row[x * 3 + 1] = p >> 8;
            row[x * 3 + 2] = p;
        }
        if (jpeg_write_scanlines(&c, &scan, 1) != 1) { free(row); jpeg_destroy_compress(&c); free(out); return NULL; }
    }
    free(row);
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    if (size) *size = (int)n;
    return out;
}

gdImagePtr gdImageCreateFromPngPtr(int size, void *data) {
    int w = 0, h = 0, a = 0, i;
    unsigned *px = NULL;
    gdImagePtr im;
    if (png_decode_argb(data, size, &w, &h, &px, &a)) return NULL;
    im = gdImageCreateTrueColor(w, h);
    if (!im) { free(px); return NULL; }
    for (i = 0; i < w * h; i++) im->pixels[i] = (int)(px[i] & 0xffffffu);
    free(px);
    return im;
}

gdImagePtr gdImageCreateFromJpegPtr(int size, void *data) {
    struct jpeg_decompress_struct c;
    struct jpeg_error_mgr err;
    gdImagePtr im = NULL;
    memset(&c, 0, sizeof c);
    c.err = jpeg_std_error(&err);
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, data, (unsigned long)size);
    if (!jpeg_read_header(&c, 1) || !jpeg_start_decompress(&c)) { jpeg_destroy_decompress(&c); return NULL; }
    im = gdImageCreateTrueColor((int)c.output_width, (int)c.output_height);
    if (!im) { jpeg_destroy_decompress(&c); return NULL; }
    while (c.output_scanline < c.output_height) {
        unsigned char *row = malloc((size_t)c.output_width * (size_t)c.output_components);
        unsigned char *scan = row;
        unsigned x, y;
        if (!row) { gdImageDestroy(im); jpeg_destroy_decompress(&c); return NULL; }
        y = c.output_scanline;
        if (jpeg_read_scanlines(&c, &scan, 1) != 1) { free(row); gdImageDestroy(im); jpeg_destroy_decompress(&c); return NULL; }
        for (x = 0; x < c.output_width; x++) {
            int color;
            if (c.output_components == 1) color = gdTrueColor(row[x], row[x], row[x]);
            else color = gdTrueColor(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]);
            gdImageSetPixel(im, (int)x, (int)y, color);
        }
        free(row);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return im;
}
