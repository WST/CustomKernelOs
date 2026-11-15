/* xz — программа для liblzma. Только контейнер .xz.
 * unxz — тот же файл вторым именем, тогда по умолчанию -d.
 * Стандартный ввод и вывод не поддерживаются, как и у gzip. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "lzma.h"

#define FILE_MAX (8 * 1024 * 1024)

static int opt_decomp, opt_keep, opt_test, opt_force, opt_stdout;
static const char *opt_suffix = ".xz";

static const char *base_of(const char *path) {
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/') s = p + 1;
    return s;
}

static void err_path(const char *path, const char *msg) {
    fprintf(stderr, "%s: %s: %s\n", __progname ? __progname : "xz", path, msg);
}

static int ends_with(const char *s, const char *suf) {
    int ns = (int)strlen(s), nf = (int)strlen(suf);
    return ns >= nf && strcmp(s + ns - nf, suf) == 0;
}

static int read_whole(const char *path, unsigned char **out, int *n) {
    int fd, cap = 0, got = 0;
    unsigned char *p = NULL;
    fd = open(path, O_RDONLY);
    if (fd < 0) { err_path(path, strerror(errno)); return -1; }
    for (;;) {
        int r;
        if (got == cap) {
            int nc = cap ? cap * 2 : 4096;
            unsigned char *np;
            if (nc > FILE_MAX) nc = FILE_MAX;
            if (got >= FILE_MAX) { free(p); close(fd); err_path(path, "file too large"); return -1; }
            np = realloc(p, (size_t)nc);
            if (!np) { free(p); close(fd); err_path(path, strerror(errno)); return -1; }
            p = np;
            cap = nc;
        }
        r = read(fd, p + got, (size_t)(cap - got));
        if (r < 0) { free(p); close(fd); err_path(path, strerror(errno)); return -1; }
        if (r == 0) break;
        got += r;
    }
    close(fd);
    *out = p;
    *n = got;
    return 0;
}

static int write_whole(const char *path, const unsigned char *p, int n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int off = 0;
    if (fd < 0) { err_path(path, strerror(errno)); return -1; }
    while (off < n) {
        int w = write(fd, p + off, (size_t)(n - off));
        if (w <= 0) { close(fd); err_path(path, strerror(errno)); return -1; }
        off += w;
    }
    close(fd);
    return 0;
}

static const char *xz_why(lzma_ret r) {
    if (r == LZMA_FORMAT_ERROR) return "not in xz format";
    if (r == LZMA_DATA_ERROR) return "compressed data is corrupt";
    if (r == LZMA_MEM_ERROR || r == LZMA_MEMLIMIT_ERROR) return "out of memory";
    if (r == LZMA_BUF_ERROR) return "unexpected end of file";
    return "compressed data error";
}

static int xz_convert(const unsigned char *in, int n, int dec, unsigned char **outp, int *outn) {
    lzma_stream s = LZMA_STREAM_INIT;
    unsigned char *out;
    int cap, off = 0;
    lzma_ret r;
    lzma_filter filters[2];
    memset(filters, 0, sizeof filters);
    filters[0].id = LZMA_FILTER_LZMA2;
    filters[1].id = LZMA_VLI_UNKNOWN;
    if (dec) r = lzma_stream_decoder(&s, 64ull << 20, 0);
    else r = lzma_stream_encoder(&s, filters, LZMA_CHECK_CRC32);
    if (r != LZMA_OK) return -1;
    cap = dec ? n * 4 + 64 : n + 128;
    if (cap < 64) cap = 64;
    if (cap > FILE_MAX) cap = FILE_MAX;
    out = malloc((size_t)cap);
    if (!out) { lzma_end(&s); return -1; }
    s.next_in = in;
    s.avail_in = (size_t)n;
    for (;;) {
        if (off == cap) {
            int nc = cap * 2;
            unsigned char *np;
            if (nc > FILE_MAX) { free(out); lzma_end(&s); return -2; }
            np = realloc(out, (size_t)nc);
            if (!np) { free(out); lzma_end(&s); return -1; }
            out = np;
            cap = nc;
        }
        s.next_out = out + off;
        s.avail_out = (size_t)(cap - off);
        r = lzma_code(&s, LZMA_FINISH);
        off = cap - (int)s.avail_out;
        if (r == LZMA_STREAM_END) break;
        if (r != LZMA_OK) { free(out); lzma_end(&s); return (int)r; }
    }
    lzma_end(&s);
    *outp = out;
    *outn = off;
    return 0;
}

static int decompress_file(const char *path) {
    unsigned char *in = NULL, *out = NULL;
    int n = 0, on = 0, r;
    char outpath[160];
    struct stat st;
    if (read_whole(path, &in, &n) < 0) return 1;
    r = xz_convert(in, n, 1, &out, &on);
    if (r != 0) { err_path(path, r < 0 ? "out of memory" : xz_why((lzma_ret)r)); free(in); return 1; }
    if (opt_test) { free(in); free(out); return 0; }
    if (ends_with(path, opt_suffix)) {
        int nl = (int)strlen(path) - (int)strlen(opt_suffix);
        if (nl <= 0 || nl >= (int)sizeof outpath) { err_path(path, "file name too long"); free(in); free(out); return 1; }
        memcpy(outpath, path, (size_t)nl);
        outpath[nl] = 0;
    } else {
        if ((int)strlen(path) + 4 >= (int)sizeof outpath) { err_path(path, "file name too long"); free(in); free(out); return 1; }
        memcpy(outpath, path, strlen(path) + 1);
        memcpy(outpath + strlen(outpath), ".out", 5);
    }
    if (!opt_force && stat(outpath, &st) == 0) { err_path(outpath, "already exists"); free(in); free(out); return 1; }
    if (write_whole(outpath, out, on) < 0) { free(in); free(out); return 1; }
    free(in); free(out);
    if (!opt_keep) remove(path);
    return 0;
}

static int compress_file(const char *path) {
    unsigned char *in = NULL, *out = NULL;
    int n = 0, on = 0, r;
    char outpath[160];
    struct stat st;
    if (!opt_force && ends_with(path, opt_suffix)) return 0;
    if ((int)strlen(path) + (int)strlen(opt_suffix) + 1 > (int)sizeof outpath) { err_path(path, "file name too long"); return 1; }
    memcpy(outpath, path, strlen(path));
    memcpy(outpath + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
    if (!opt_force && stat(outpath, &st) == 0) { err_path(outpath, "already exists"); return 1; }
    if (read_whole(path, &in, &n) < 0) return 1;
    r = xz_convert(in, n, 0, &out, &on);
    if (r != 0) { err_path(path, "out of memory"); free(in); return 1; }
    if (write_whole(outpath, out, on) < 0) { free(in); free(out); return 1; }
    free(in); free(out);
    if (!opt_keep) remove(path);
    return 0;
}

static int process(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (opt_decomp && !ends_with(path, opt_suffix)) {
            char alt[160];
            if ((int)strlen(path) + (int)strlen(opt_suffix) + 1 <= (int)sizeof alt) {
                memcpy(alt, path, strlen(path));
                memcpy(alt + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
                if (stat(alt, &st) == 0) return decompress_file(alt);
            }
        }
        err_path(path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) { err_path(path, "is a directory -- ignored"); return 0; }
    if (opt_decomp || opt_test) {
        if (!ends_with(path, opt_suffix)) {
            char alt[160];
            struct stat xz;
            if ((int)strlen(path) + (int)strlen(opt_suffix) + 1 <= (int)sizeof alt) {
                memcpy(alt, path, strlen(path));
                memcpy(alt + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
                if (stat(alt, &xz) == 0) return decompress_file(alt);
            }
            err_path(path, "unknown suffix -- ignored");
            return 0;
        }
        return decompress_file(path);
    }
    return compress_file(path);
}

int main(int argc, char **argv) {
    int i, rc = 0, files = 0;
    const char *me = __progname ? __progname : (argc > 0 ? base_of(argv[0]) : "xz");
    if (!strcmp(me, "unxz")) opt_decomp = 1;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) { files++; continue; }
        if (a[1] == '-' && a[2] == 0) { files += argc - i - 1; break; }
        if (a[1] == '-') {
            if (!strcmp(a, "--decompress") || !strcmp(a, "--uncompress")) opt_decomp = 1;
            else if (!strcmp(a, "--keep")) opt_keep = 1;
            else if (!strcmp(a, "--test")) opt_test = 1;
            else if (!strcmp(a, "--force")) opt_force = 1;
            else if (!strcmp(a, "--help")) {
                printf("usage: %s [-d] [-k] [-t] [-f] file...\n", me);
                return 0;
            }
            else { fprintf(stderr, "%s: unrecognized option '%s'\n", me, a); return 1; }
            continue;
        }
        {
            char *p;
            for (p = a + 1; *p; p++) {
                if (*p == 'd' || *p == 'z') { if (*p == 'd') opt_decomp = 1; continue; }
                if (*p == 'k') { opt_keep = 1; continue; }
                if (*p == 't') { opt_test = 1; continue; }
                if (*p == 'f') { opt_force = 1; continue; }
                if (*p == 'c') { opt_stdout = 1; continue; }
                if (*p == 'q' || *p == 'v' || (*p >= '0' && *p <= '9')) continue;
                fprintf(stderr, "%s: unrecognized option '%c'\n", me, *p);
                return 1;
            }
        }
    }
    if (opt_stdout || files == 0) {
        fprintf(stderr, "%s: standard input/output is not supported\n", me);
        return 1;
    }
    if (opt_test) opt_decomp = 1;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (!strcmp(a, "--")) continue;
        if (a[0] == '-' && a[1]) continue;
        if (process(a)) rc = 1;
    }
    return rc;
}
