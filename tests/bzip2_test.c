/* Круг bzip2 и распаковка потока по одному байту. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "bzlib.h"

static int fails;

static void expect(int cond, const char *msg) {
    if (cond) return;
    fprintf(stderr, "FAIL %s\n", msg);
    fails++;
}

static void roundtrip(const unsigned char *src, unsigned n, const char *name) {
    unsigned clen = n + n / 2 + 128;
    unsigned dlen = n + 8;
    char *comp = malloc(clen), *out = malloc(dlen ? dlen : 1);
    int r;
    expect(comp && out, "alloc");
    r = BZ2_bzBuffToBuffCompress(comp, &clen, (char *)src, n, 1, 0, 0);
    expect(r == BZ_OK, name);
    r = BZ2_bzBuffToBuffDecompress(out, &dlen, comp, clen, 0, 0);
    expect(r == BZ_OK && dlen == n && (n == 0 || memcmp(out, src, n) == 0), name);
    free(comp);
    free(out);
}

static void bybyte(const unsigned char *src, unsigned n) {
    unsigned clen = n + 256, dlen = 0;
    char *comp = malloc(clen), *out = malloc(n + 8);
    bz_stream s;
    unsigned i;
    int r;
    expect(BZ2_bzBuffToBuffCompress(comp, &clen, (char *)src, n, 1, 0, 0) == BZ_OK, "byte-comp");
    memset(&s, 0, sizeof s);
    expect(BZ2_bzDecompressInit(&s, 0, 0) == BZ_OK, "byte-init");
    s.next_out = out;
    s.avail_out = n + 8;
    for (i = 0; i < clen; i++) {
        s.next_in = comp + i;
        s.avail_in = 1;
        r = BZ2_bzDecompress(&s);
        if (r == BZ_STREAM_END) break;
        expect(r == BZ_OK, "byte-step");
    }
    if (r != BZ_STREAM_END) r = BZ2_bzDecompress(&s);
    expect(r == BZ_STREAM_END && s.total_out_lo32 == n && memcmp(out, src, n) == 0, "byte-out");
    BZ2_bzDecompressEnd(&s);
    free(comp);
    free(out);
    (void)dlen;
}

int main(void) {
    static const unsigned char hello[] = "hello bzip2";
    unsigned char ramp[300];
    unsigned i;
    roundtrip((const unsigned char *)"", 0, "empty");
    roundtrip(hello, sizeof hello - 1, "hello");
    roundtrip((const unsigned char *)"a", 1, "a");
    roundtrip((const unsigned char *)"aaaaaa", 6, "run");
    roundtrip((const unsigned char *)"banana", 6, "banana");
    for (i = 0; i < sizeof ramp; i++) ramp[i] = (unsigned char)(i * 17);
    roundtrip(ramp, sizeof ramp, "ramp");
    bybyte(hello, sizeof hello - 1);
    if (fails) { fprintf(stderr, "%d failed\n", fails); return 1; }
    printf("bzip2 ok\n");
    return 0;
}
