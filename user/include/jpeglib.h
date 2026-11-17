/* jpeglib.h — чтение и запись baseline/progressive JPEG.
 * jpeg_mem_src и jpeg_read_scanlines читают тем же декодером, что раньше жил в img.c. */
#ifndef JPEGLIB_H
#define JPEGLIB_H
#include "jconfig.h"
#include "jmorecfg.h"

#define JPEG_LIB_VERSION 80

struct jpeg_common_struct;
typedef struct jpeg_error_mgr {
    void (*error_exit)(struct jpeg_common_struct *);
    int msg_code;
} jpeg_error_mgr;
typedef struct jpeg_error_mgr *jpeg_error_mgrp;

struct jpeg_common_struct {
    jpeg_error_mgrp err;
    int is_decompressor;
};
typedef struct jpeg_common_struct *j_common_ptr;

struct jpeg_decompress_struct {
    jpeg_error_mgrp err;
    int is_decompressor;
    JDIMENSION image_width;
    JDIMENSION image_height;
    int num_components;
    int jpeg_color_space;
    int out_color_space;
    unsigned scale_num;
    unsigned scale_denom;
    JDIMENSION output_width;
    JDIMENSION output_height;
    int output_components;
    JDIMENSION output_scanline;
    const unsigned char *src;
    unsigned long src_len;
    unsigned *argb;
    int argb_w, argb_h;
};
typedef struct jpeg_decompress_struct *j_decompress_ptr;

struct jpeg_compress_struct {
    jpeg_error_mgrp err;
    int is_decompressor;
    JDIMENSION image_width;
    JDIMENSION image_height;
    int input_components;
    int in_color_space;
    int data_precision;
    unsigned char **outbuffer;
    unsigned long *outsize;
    unsigned char *samples;
    unsigned long samples_n, samples_cap;
    JDIMENSION next_scanline;
    int quality;
    int started;
};
typedef struct jpeg_compress_struct *j_compress_ptr;

#define JCS_UNKNOWN 0
#define JCS_GRAYSCALE 1
#define JCS_RGB 2
#define JCS_YCbCr 3

typedef int boolean;
#define FALSE 0
#define TRUE 1

struct jpeg_error_mgr *jpeg_std_error(jpeg_error_mgr *err);
void jpeg_create_decompress(j_decompress_ptr cinfo);
void jpeg_mem_src(j_decompress_ptr cinfo, const unsigned char *inbuffer, unsigned long insize);
int jpeg_read_header(j_decompress_ptr cinfo, int require_image);
int jpeg_start_decompress(j_decompress_ptr cinfo);
JDIMENSION jpeg_read_scanlines(j_decompress_ptr cinfo, JSAMPARRAY scanlines, JDIMENSION max_lines);
int jpeg_finish_decompress(j_decompress_ptr cinfo);
void jpeg_destroy_decompress(j_decompress_ptr cinfo);

void jpeg_create_compress(j_compress_ptr cinfo);
void jpeg_mem_dest(j_compress_ptr cinfo, unsigned char **outbuffer, unsigned long *outsize);
void jpeg_set_defaults(j_compress_ptr cinfo);
void jpeg_set_quality(j_compress_ptr cinfo, int quality, int force_baseline);
void jpeg_start_compress(j_compress_ptr cinfo, int write_all_tables);
JDIMENSION jpeg_write_scanlines(j_compress_ptr cinfo, JSAMPARRAY scanlines, JDIMENSION num_lines);
void jpeg_finish_compress(j_compress_ptr cinfo);
void jpeg_destroy_compress(j_compress_ptr cinfo);

int jpeg_decode_argb(const unsigned char *data, int len, int *w, int *h, unsigned **px, int scale);
#endif
