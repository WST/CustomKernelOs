/* gd.h — truecolor: пиксель, PNG и JPEG. Палитры и шрифтов нет. */
#ifndef GD_H
#define GD_H

typedef struct gdImageStruct *gdImagePtr;

gdImagePtr gdImageCreateTrueColor(int sx, int sy);
void gdImageDestroy(gdImagePtr im);
void gdImageSetPixel(gdImagePtr im, int x, int y, int color);
int gdImageGetPixel(gdImagePtr im, int x, int y);
int gdImageColorAllocate(gdImagePtr im, int r, int g, int b);
void *gdImagePngPtr(gdImagePtr im, int *size);
void *gdImageJpegPtr(gdImagePtr im, int *size, int quality);
gdImagePtr gdImageCreateFromPngPtr(int size, void *data);
gdImagePtr gdImageCreateFromJpegPtr(int size, void *data);

#define gdTrueColor(r, g, b) ((((r) & 255) << 16) | (((g) & 255) << 8) | ((b) & 255))
#endif
