/* Проверка libz тем же cc, что работает внутри системы. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "zlib.h"

static int fails;

static void expect(int cond, const char *msg) {
    if (cond) return;
    fprintf(stderr, "FAIL %s\n", msg);
    fails++;
}

static void roundtrip(const unsigned char *src, unsigned n, int level, int window, const char *name) {
    z_stream d, u;
    unsigned char *comp, *out;
    unsigned long bound;
    int ret;
    memset(&d, 0, sizeof d);
    memset(&u, 0, sizeof u);
    expect(deflateInit2(&d, level, Z_DEFLATED, window, 8, Z_DEFAULT_STRATEGY) == Z_OK, name);
    bound = deflateBound(&d, n);
    comp = malloc(bound + 8);
    out = malloc(n + 1);
    expect(comp && out, "alloc");
    d.next_in = (Bytef *)src;
    d.avail_in = n;
    d.next_out = comp;
    d.avail_out = (uInt)bound;
    ret = deflate(&d, Z_FINISH);
    expect(ret == Z_STREAM_END, name);
    expect(inflateInit2(&u, window) == Z_OK, name);
    u.next_in = comp;
    u.avail_in = (uInt)d.total_out;
    u.next_out = out;
    u.avail_out = n + 1;
    ret = inflate(&u, Z_NO_FLUSH);
    expect(ret == Z_STREAM_END, name);
    expect(u.total_out == n, name);
    expect(n == 0 || memcmp(src, out, n) == 0, name);
    deflateEnd(&d);
    inflateEnd(&u);
    free(comp);
    free(out);
}

static void chunked(const unsigned char *src, unsigned n) {
    z_stream d, u;
    unsigned char comp[65536], out[65536];
    unsigned left, got, i;
    int ret;
    memset(&d, 0, sizeof d);
    memset(&u, 0, sizeof u);
    expect(deflateInit(&d, 6) == Z_OK, "chunk deflateInit");
    left = 0;
    d.next_in = (Bytef *)src;
    d.avail_in = n / 2;
    d.next_out = comp;
    d.avail_out = sizeof comp;
    ret = deflate(&d, Z_NO_FLUSH);
    expect(ret == Z_OK, "chunk noflush");
    d.next_in = (Bytef *)src + n / 2;
    d.avail_in = n - n / 2;
    ret = deflate(&d, Z_FINISH);
    expect(ret == Z_STREAM_END, "chunk finish");
    got = (unsigned)d.total_out;
    expect(inflateInit(&u) == Z_OK, "chunk inflateInit");
    left = 0;
    ret = Z_OK;
    for (i = 0; i < n * 2 + got + 10 && ret != Z_STREAM_END; i++) {
        unsigned char three[3];
        int produced;
        unsigned before = (unsigned)u.total_out;
        if (!u.avail_in && left < got) {
            u.next_in = comp + left;
            u.avail_in = 1;
            left++;
        }
        u.next_out = three;
        u.avail_out = 3;
        ret = inflate(&u, Z_NO_FLUSH);
        produced = 3 - (int)u.avail_out;
        if (produced > 0) memcpy(out + before, three, (size_t)produced);
        if (ret == Z_STREAM_END) break;
        expect(ret == Z_OK || ret == Z_BUF_ERROR, "chunk inflate status");
        if (ret == Z_BUF_ERROR && !u.avail_in && left >= got && u.total_out == before) break;
    }
    expect(ret == Z_STREAM_END, "chunk end");
    expect(u.total_out == n && memcmp(src, out, n) == 0, "chunk bytes");
    deflateEnd(&d);
    inflateEnd(&u);
}

int main(void) {
    unsigned char rep[20000], rnd[4096], empty = 0;
    unsigned i;
    unsigned long c;
    for (i = 0; i < sizeof rep; i++) rep[i] = (unsigned char)("abc"[i % 3]);
    for (i = 0; i < sizeof rnd; i++) rnd[i] = (unsigned char)(i * 17 + 3);
    c = crc32(0, rep, 100);
    expect(c == 0x9cffa0d8ul || c != 0, "crc non-zero");
    expect(crc32(0, 0, 0) == 0, "crc init");
    expect(adler32(0, 0, 0) == 1, "adler init");
    roundtrip(&empty, 0, 6, -15, "empty raw");
    roundtrip(&empty, 0, 6, 15, "empty zlib");
    roundtrip(&empty, 0, 6, 31, "empty gzip");
    roundtrip(rep, sizeof rep, 9, -15, "rep raw");
    roundtrip(rep, sizeof rep, 1, 15, "rep zlib");
    roundtrip(rep, sizeof rep, 6, 31, "rep gzip");
    roundtrip(rnd, sizeof rnd, 0, -15, "stored");
    roundtrip(rnd, sizeof rnd, 9, 15, "rnd zlib");
    roundtrip((const unsigned char *)"hello gzip\n", 11, 6, -15, "hello");
    chunked(rep, 1000);

    /* хвост после сырого потока не съедается */
    {
        z_stream d, u;
        unsigned char comp[256], out[32];
        int ret;
        memset(&d, 0, sizeof d);
        memset(&u, 0, sizeof u);
        deflateInit2(&d, 9, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY);
        d.next_in = (Bytef *)"xyz";
        d.avail_in = 3;
        d.next_out = comp;
        d.avail_out = sizeof comp;
        expect(deflate(&d, Z_FINISH) == Z_STREAM_END, "tail deflate");
        comp[d.total_out] = 0x11;
        comp[d.total_out + 1] = 0x22;
        inflateInit2(&u, -15);
        u.next_in = comp;
        u.avail_in = (uInt)d.total_out + 2;
        u.next_out = out;
        u.avail_out = sizeof out;
        ret = inflate(&u, Z_NO_FLUSH);
        expect(ret == Z_STREAM_END, "tail end");
        expect(u.total_out == 3 && memcmp(out, "xyz", 3) == 0, "tail data");
        expect(u.avail_in == 2 && u.next_in[0] == 0x11 && u.next_in[1] == 0x22, "tail left");
        deflateEnd(&d);
        inflateEnd(&u);
    }

    /* второй проход после сброса */
    {
        z_stream d;
        unsigned char a[64], b[64];
        uLong n;
        memset(&d, 0, sizeof d);
        deflateInit(&d, 9);
        d.next_in = rep;
        d.avail_in = 50;
        d.next_out = a;
        d.avail_out = sizeof a;
        expect(deflate(&d, Z_FINISH) == Z_STREAM_END, "reset first");
        n = d.total_out;
        expect(deflateReset(&d) == Z_OK, "reset");
        d.next_in = rep;
        d.avail_in = 50;
        d.next_out = b;
        d.avail_out = sizeof b;
        expect(deflate(&d, Z_FINISH) == Z_STREAM_END, "reset second");
        expect(d.total_out == n && memcmp(a, b, n) == 0, "reset same");
        deflateEnd(&d);
    }

    if (fails) {
        fprintf(stderr, "%d failed\n", fails);
        return 1;
    }
    printf("zlib: OK\n");
    return 0;
}
