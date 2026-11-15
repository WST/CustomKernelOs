/* lzma.h — подмножество liblzma: один поток .xz и фильтр LZMA2.
 * Кодер пишет несжатые куски LZMA2. Декодер читает и их, и обычный LZMA. */
#ifndef LZMA_H
#define LZMA_H
#include "stddef.h"

typedef unsigned long long lzma_vli;

typedef enum {
    LZMA_OK = 0,
    LZMA_STREAM_END = 1,
    LZMA_NO_CHECK = 2,
    LZMA_UNSUPPORTED_CHECK = 3,
    LZMA_GET_CHECK = 4,
    LZMA_MEM_ERROR = 5,
    LZMA_MEMLIMIT_ERROR = 6,
    LZMA_FORMAT_ERROR = 7,
    LZMA_OPTIONS_ERROR = 8,
    LZMA_DATA_ERROR = 9,
    LZMA_BUF_ERROR = 10,
    LZMA_PROG_ERROR = 11
} lzma_ret;

typedef enum {
    LZMA_RUN = 0,
    LZMA_SYNC_FLUSH = 1,
    LZMA_FULL_FLUSH = 2,
    LZMA_FINISH = 3
} lzma_action;

typedef enum {
    LZMA_CHECK_NONE = 0,
    LZMA_CHECK_CRC32 = 1,
    LZMA_CHECK_CRC64 = 4,
    LZMA_CHECK_SHA256 = 10
} lzma_check;

typedef struct {
    lzma_vli id;
    void *options;
} lzma_filter;

typedef struct {
    unsigned dict_size;
    unsigned preset_dict_size;
    const unsigned char *preset_dict;
} lzma_options_lzma;

#define LZMA_FILTER_LZMA2 ((lzma_vli)0x21)
#define LZMA_VLI_UNKNOWN (~(lzma_vli)0)
#define LZMA_TELL_NO_CHECK 0x01u
#define LZMA_TELL_UNSUPPORTED_CHECK 0x02u
#define LZMA_CONCATENATED 0x08u
#define LZMA_DICT_SIZE_DEFAULT (1u << 23)

typedef struct lzma_internal_s lzma_internal;

typedef struct {
    const unsigned char *next_in;
    size_t avail_in;
    unsigned long long total_in;
    unsigned char *next_out;
    size_t avail_out;
    unsigned long long total_out;
    const void *allocator;
    lzma_internal *internal;
    void *reserved_ptr1;
    void *reserved_ptr2;
    void *reserved_ptr3;
    void *reserved_ptr4;
    unsigned long long reserved_int1;
    unsigned long long reserved_int2;
    size_t reserved_int3;
    size_t reserved_int4;
    unsigned reserved_enum1;
    unsigned reserved_enum2;
} lzma_stream;

#define LZMA_STREAM_INIT { NULL, 0, 0, NULL, 0, 0, NULL, NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0, 0, 0 }

lzma_ret lzma_stream_decoder(lzma_stream *strm, unsigned long long memlimit, unsigned flags);
lzma_ret lzma_stream_encoder(lzma_stream *strm, const lzma_filter *filters, lzma_check check);
lzma_ret lzma_code(lzma_stream *strm, lzma_action action);
void lzma_end(lzma_stream *strm);

#endif
