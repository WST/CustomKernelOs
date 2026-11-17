/* iconv — перекодирование файла. -f и -t задают кодировки, -o пишет в файл.
 * Без -o результат идёт на стандартный вывод, без имени файла читается stdin. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "iconv.h"

static int convert_fd(iconv_t cd, int in, int out) {
    char ib[4096], ob[8192];
    int in_n = 0, eof = 0;
    for (;;) {
        char *ip, *op;
        size_t il, ol, r;
        if (!eof && in_n < (int)sizeof ib) {
            int n = read(in, ib + in_n, sizeof ib - (size_t)in_n);
            if (n < 0) return -1;
            if (n == 0) eof = 1;
            in_n += n;
        }
        if (in_n == 0 && eof) return 0;
        ip = ib;
        il = (size_t)in_n;
        op = ob;
        ol = sizeof ob;
        r = iconv(cd, &ip, &il, &op, &ol);
        if ((int)(sizeof ob - ol) > 0) {
            int off = 0, wn = (int)(sizeof ob - ol);
            while (off < wn) {
                int w = write(out, ob + off, (size_t)(wn - off));
                if (w <= 0) return -1;
                off += w;
            }
        }
        if (ip != ib) {
            int used = (int)(ip - ib);
            memmove(ib, ip, (size_t)(in_n - used));
            in_n -= used;
        }
        if (r == (size_t)-1 && errno == E2BIG) continue;
        if (r == (size_t)-1) return -1;
        if (eof && in_n == 0) return 0;
        if (r == 0 && in_n > 0 && ip == ib) return -1;
    }
}

int main(int argc, char **argv) {
    const char *from = NULL, *to = NULL, *outpath = NULL;
    int i, in = 0, out = 1, rc = 0;
    iconv_t cd;
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f") && i + 1 < argc) from = argv[++i];
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) to = argv[++i];
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) outpath = argv[++i];
        else if (argv[i][0] == '-') { fprintf(stderr, "iconv: unknown option %s\n", argv[i]); return 1; }
        else break;
    }
    if (!from || !to) { fprintf(stderr, "usage: iconv -f FROM -t TO [-o OUT] [file]\n"); return 1; }
    cd = iconv_open(to, from);
    if (cd == (iconv_t)-1) { fprintf(stderr, "iconv: unsupported encoding\n"); return 1; }
    if (outpath) {
        out = open(outpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (out < 0) { perror(outpath); iconv_close(cd); return 1; }
    }
    if (i >= argc) {
        if (convert_fd(cd, 0, out) < 0) { fprintf(stderr, "iconv: conversion failed\n"); rc = 1; }
    } else for (; i < argc; i++) {
        in = open(argv[i], O_RDONLY);
        if (in < 0) { perror(argv[i]); rc = 1; continue; }
        if (convert_fd(cd, in, out) < 0) { fprintf(stderr, "iconv: %s: conversion failed\n", argv[i]); rc = 1; }
        close(in);
    }
    if (outpath) close(out);
    iconv_close(cd);
    return rc;
}
