/* png.h — чтение и запись 8-битных PNG поверх libz.
 * 1/2/4/16 бит при чтении раскрываются в 8. Это не полное дерево libpng. */
#ifndef PNG_H
#define PNG_H
#include "pngconf.h"

#define PNG_LIBPNG_VER_STRING "1.6-myos"

typedef struct png_struct_def png_struct;
typedef struct png_info_def png_info;
typedef png_struct *png_structp;
typedef png_info *png_infop;
typedef png_struct **png_structpp;
typedef png_info **png_infopp;

png_structp png_create_read_struct(const char *ver, void *error_ptr, void *error_fn, void *warn_fn);
png_infop png_create_info_struct(png_structp png);
void png_destroy_read_struct(png_structpp png, png_infopp info, png_infopp end);
void png_init_io(png_structp png, void *file);
typedef void (*png_rw_ptr)(png_structp, png_bytep, png_size_t);
void png_set_read_fn(png_structp png, void *io_ptr, png_rw_ptr read_fn);
int png_sig_cmp(png_const_bytep sig, png_size_t start, png_size_t num);
void png_read_info(png_structp png, png_infop info);
png_uint_32 png_get_image_width(png_structp png, png_infop info);
png_uint_32 png_get_image_height(png_structp png, png_infop info);
int png_get_color_type(png_structp png, png_infop info);
int png_get_bit_depth(png_structp png, png_infop info);
void png_set_expand(png_structp png);
void png_set_strip_16(png_structp png);
void png_set_packing(png_structp png);
void png_set_palette_to_rgb(png_structp png);
void png_set_gray_to_rgb(png_structp png);
void png_set_tRNS_to_alpha(png_structp png);
void png_read_update_info(png_structp png, png_infop info);
void png_read_image(png_structp png, png_bytep *rows);
void png_read_end(png_structp png, png_infop info);

png_structp png_create_write_struct(const char *ver, void *error_ptr, void *error_fn, void *warn_fn);
void png_destroy_write_struct(png_structpp png, png_infopp info);
void png_set_write_fn(png_structp png, void *io_ptr, png_rw_ptr write_fn, void *flush_fn);
void *png_get_io_ptr(png_structp png);
void png_set_IHDR(png_structp png, png_infop info, png_uint_32 w, png_uint_32 h, int depth, int color, int interlace, int compression, int filter);
void png_set_PLTE(png_structp png, png_infop info, const unsigned char *rgb, int n);
typedef struct png_color_16 { unsigned short index, red, green, blue, gray; } png_color_16;
void png_set_tRNS(png_structp png, png_infop info, unsigned char *trans, int num, png_color_16 *color);
void png_write_info(png_structp png, png_infop info);
void png_write_image(png_structp png, png_bytep *rows);
void png_write_end(png_structp png, png_infop info);

/* Прямой разбор в 0xAARRGGBB: им пользуется img, второй декодер не пишется. */
int png_decode_argb(const unsigned char *data, int len, int *w, int *h, unsigned **px, int *alpha);
int png_encode_argb(const unsigned *px, int w, int h, int with_alpha, unsigned char **out, int *out_n);

#define PNG_COLOR_TYPE_GRAY 0
#define PNG_COLOR_TYPE_RGB 2
#define PNG_COLOR_TYPE_PALETTE 3
#define PNG_COLOR_TYPE_GRAY_ALPHA 4
#define PNG_COLOR_TYPE_RGB_ALPHA 6
#define PNG_INTERLACE_NONE 0
#define PNG_COMPRESSION_TYPE_BASE 0
#define PNG_FILTER_TYPE_BASE 0
#endif
