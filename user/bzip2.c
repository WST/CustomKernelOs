/* bzip2 — программа для libbz2. Флаги как у gzip: -d -k -t -f.
 * bunzip2 — тот же файл вторым именем, тогда по умолчанию -d.
 * Стандартный ввод и вывод не поддерживаются, как и у gzip. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "bzlib.h"

#define FILE_MAX (8 * 1024 * 1024)

static int opt_decomp, opt_keep, opt_test, opt_force, opt_stdout, opt_level;
static const char *opt_suffix = ".bz2";

static const char *base_of(const char *path) {
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/') s = p + 1;
    return s;
}

static void err_path(const char *path, const char *msg) {
    fprintf(stderr, "%s: %s: %s\n", __progname ? __progname : "bzip2", path, msg);
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
            if (!np) { free(p); close(fd); err_path(path, "out of memory"); return -1; }
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

static const char *bz_why(int r) {
    if (r == BZ_DATA_ERROR_MAGIC) return "not in bzip2 format";
    if (r == BZ_DATA_ERROR) return "data integrity error";
    if (r == BZ_MEM_ERROR) return "out of memory";
    if (r == BZ_UNEXPECTED_EOF) return "unexpected end of file";
    return "compressed data error";
}

static int decompress_file(const char *path) {
    unsigned char *in = NULL, *out = NULL;
    unsigned int on;
    int n = 0, r;
    char outpath[160];
    struct stat st;
    if (read_whole(path, &in, &n) < 0) return 1;
    on = FILE_MAX;
    out = malloc(on ? on : 1);
    if (!out) { free(in); err_path(path, "out of memory"); return 1; }
    r = BZ2_bzBuffToBuffDecompress((char *)out, &on, (char *)in, (unsigned)n, 0, 0);
    if (r != BZ_OK) { err_path(path, bz_why(r)); free(in); free(out); return 1; }
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
    if (write_whole(outpath, out, (int)on) < 0) { free(in); free(out); return 1; }
    free(in); free(out);
    if (!opt_keep) remove(path);
    return 0;
}

static int compress_file(const char *path) {
    unsigned char *in = NULL, *out = NULL;
    unsigned int on;
    int n = 0, r, level;
    char outpath[160];
    struct stat st;
    if (!opt_force && ends_with(path, opt_suffix)) return 0;
    if ((int)strlen(path) + (int)strlen(opt_suffix) + 1 > (int)sizeof outpath) { err_path(path, "file name too long"); return 1; }
    memcpy(outpath, path, strlen(path));
    memcpy(outpath + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
    if (!opt_force && stat(outpath, &st) == 0) { err_path(outpath, "already exists"); return 1; }
    if (read_whole(path, &in, &n) < 0) return 1;
    level = opt_level;
    if (level < 1) level = 9;
    on = (unsigned)n + (unsigned)n / 2 + 1024;
    if (on < 64) on = 64;
    out = malloc(on);
    if (!out) { free(in); err_path(path, "out of memory"); return 1; }
    r = BZ2_bzBuffToBuffCompress((char *)out, &on, (char *)in, (unsigned)n, level, 0, 0);
    if (r != BZ_OK) { err_path(path, bz_why(r)); free(in); free(out); return 1; }
    if (write_whole(outpath, out, (int)on) < 0) { free(in); free(out); return 1; }
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
            struct stat bz;
            if ((int)strlen(path) + (int)strlen(opt_suffix) + 1 <= (int)sizeof alt) {
                memcpy(alt, path, strlen(path));
                memcpy(alt + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
                if (stat(alt, &bz) == 0) return decompress_file(alt);
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
    const char *me = __progname ? __progname : (argc > 0 ? base_of(argv[0]) : "bzip2");
    opt_level = 9;
    if (!strcmp(me, "bunzip2")) opt_decomp = 1;
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
                printf("usage: %s [-d] [-k] [-t] [-f] [-1..-9] file...\n", me);
                return 0;
            }
            else { fprintf(stderr, "%s: unrecognized option '%s'\n", me, a); return 1; }
            continue;
        }
        {
            char *p;
            for (p = a + 1; *p; p++) {
                if (*p >= '1' && *p <= '9') { opt_level = *p - '0'; continue; }
                if (*p == 'd' || *p == 'z') { if (*p == 'd') opt_decomp = 1; continue; }
                if (*p == 'k') { opt_keep = 1; continue; }
                if (*p == 't') { opt_test = 1; continue; }
                if (*p == 'f') { opt_force = 1; continue; }
                if (*p == 'c') { opt_stdout = 1; continue; }
                if (*p == 'q' || *p == 'v') continue;
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
