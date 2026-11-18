/* Круг .xz: наш кодер и декодер, затем куски по несколько байт. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "lzma.h"

static int fails;

static void expect(int cond, const char *msg) {
    if (cond) return;
    fprintf(stderr, "FAIL %s\n", msg);
    fails++;
}

static int roundtrip(const unsigned char *src, unsigned n, const char *name) {
    lzma_options_lzma opt;
    lzma_filter filters[2];
    lzma_stream enc = LZMA_STREAM_INIT, dec = LZMA_STREAM_INIT;
    unsigned char *comp, *out;
    unsigned cap = n + 256;
    memset(&opt, 0, sizeof opt);
    opt.dict_size = 1u << 16;
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[0].options = &opt;
    filters[1].id = LZMA_VLI_UNKNOWN;
    filters[1].options = NULL;
    comp = malloc(cap);
    out = malloc(n + 8);
    expect(comp && out, "alloc");
    expect(lzma_stream_encoder(&enc, filters, LZMA_CHECK_CRC32) == LZMA_OK, "enc-init");
    enc.next_in = src;
    enc.avail_in = n;
    enc.next_out = comp;
    enc.avail_out = cap;
    expect(lzma_code(&enc, LZMA_FINISH) == LZMA_STREAM_END, name);
    cap = (unsigned)enc.total_out;
    lzma_end(&enc);
    expect(lzma_stream_decoder(&dec, 64ull << 20, 0) == LZMA_OK, "dec-init");
    dec.next_in = comp;
    dec.avail_in = cap;
    dec.next_out = out;
    dec.avail_out = n + 8;
    expect(lzma_code(&dec, LZMA_FINISH) == LZMA_STREAM_END, name);
    expect(dec.total_out == n && (n == 0 || memcmp(out, src, n) == 0), name);
    lzma_end(&dec);
    free(comp);
    free(out);
    return fails;
}

int main(void) {
    static const unsigned char hello[] = "hello xz";
    unsigned char ramp[300];
    unsigned i;
    roundtrip((const unsigned char *)"", 0, "empty");
    roundtrip(hello, sizeof hello - 1, "hello");
    for (i = 0; i < sizeof ramp; i++) ramp[i] = (unsigned char)(i * 3);
    roundtrip(ramp, sizeof ramp, "ramp");
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("xz ok\n");
    return 0;
}
