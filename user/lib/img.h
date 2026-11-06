/* img.h — картинки для программ MyOS: PNG, GIF, JPEG, BMP -> пиксели.
 *
 *   img im;
 *   if (img_decode(data, len, &im, 1600) == 0) {     не шире/выше 1600 — иначе уменьшит
 *       ... im.px[y * im.w + x] — 0xAARRGGBB (AA = 255 — непрозрачный) ...
 *       img_free(&im);
 *   }
 *
 * Что понимает:
 *   PNG  — все виды цвета (серый, RGB, палитра, с прозрачностью), 1..16 бит, черезстрочные;
 *   GIF  — первый кадр, прозрачный цвет, черезстрочные;
 *   JPEG — обычные (baseline) и прогрессивные, серые, YCbCr с любым прореживанием, CMYK;
 *   BMP  — 1/4/8/16/24/32 бита без сжатия.
 * Всё написано с нуля (user/lib/img.c): inflate для PNG, LZW для GIF, Хаффман и
 * обратное ДКП для JPEG. Только int, без 64-битных чисел. */
#ifndef IMG_H
#define IMG_H

typedef struct {
    int w, h;
    unsigned *px;                   /* w * h пикселей 0xAARRGGBB, malloc */
    int has_alpha;                  /* есть (полу)прозрачные пиксели */
} img;

/* 0 — получилось; -1 — не картинка или испорчена; -2 — не хватило памяти.
 * max_side > 0: картинку крупнее уменьшить так, чтобы большая сторона была max_side */
int  img_decode(const unsigned char *data, int len, img *out, int max_side);
void img_free(img *im);
const char *img_type(const unsigned char *data, int len);   /* "png", "gif", "jpeg", "bmp" или NULL */
int  img_size(const unsigned char *data, int len, int *w, int *h);   /* размер, не распаковывая: 0 — ок */

/* уменьшить/увеличить (усреднение при уменьшении, билинейно при увеличении): malloc */
unsigned *img_scale(const unsigned *src, int sw, int sh, int dw, int dh);

/* zlib (inflate): распаковать в out (не больше max байт) -> сколько вышло или -1 */
int  img_inflate(const unsigned char *in, int len, unsigned char *out, int max, int zlib_header);

#endif
