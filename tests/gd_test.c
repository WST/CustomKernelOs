/* Запись PNG через gd и чтение обратно. JPEG сверяется по близости цвета. */
#include "myos.h"
#include "stdio.h"
#include "gd.h"

int main(void) {
    gdImagePtr im = gdImageCreateTrueColor(4, 2), back;
    int n = 0, color, p, r, g, b;
    void *png, *jpg;
    if (!im) return 1;
    color = gdImageColorAllocate(im, 10, 20, 30);
    for (int y = 0; y < 2; y++) for (int x = 0; x < 4; x++) gdImageSetPixel(im, x, y, color);
    png = gdImagePngPtr(im, &n);
    if (!png || n < 24) return 1;
    back = gdImageCreateFromPngPtr(n, png);
    if (!back || gdImageGetPixel(back, 1, 1) != color) return 1;
    gdImageDestroy(back);
    free(png);
    jpg = gdImageJpegPtr(im, &n, 90);
    if (!jpg || n < 50) return 1;
    back = gdImageCreateFromJpegPtr(n, jpg);
    if (!back) return 1;
    p = gdImageGetPixel(back, 1, 1);
    r = (p >> 16) & 255; g = (p >> 8) & 255; b = p & 255;
    gdImageDestroy(back);
    free(jpg);
    gdImageDestroy(im);
    if (r < 5 || r > 20 || g < 10 || g > 35 || b < 20 || b > 45) return 1;
    printf("gd ok\n");
    return 0;
}
