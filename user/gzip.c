/* gzip — системный архиватор формата RFC 1952.
 * Сжатый поток — deflate из libz. Здесь только заголовок gzip: имя, CRC и длина.
 * Флаги совпадают с GNU gzip. Стандартный ввод и вывод пока не поддерживаются:
 * -c и запуск без имён файлов завершаются ошибкой.
 *
 * gunzip — тот же файл, установленный вторым именем: тогда по умолчанию -d. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "dirent.h"
#include "zlib.h"

#define OUT_MAX   (9 * 1024 * 1024)

static int opt_decomp, opt_force, opt_keep, opt_list, opt_name, opt_noname;
static int opt_quiet, opt_recur, opt_test, opt_verbose, opt_stdout, opt_level;
static const char *opt_suffix = ".gz";

struct grow {
    unsigned char *p;
    int n, cap, err;
};

static int grow_need(struct grow *g, int add) {
    int need;
    if (g->err) return -1;
    if (add < 0 || g->n > OUT_MAX - add) { g->err = 1; return -1; }
    need = g->n + add;
    if (need > g->cap) {
        int nc = g->cap ? g->cap : 4096;
        unsigned char *np;
        while (nc < need) {
            if (nc > OUT_MAX / 2) { nc = need; break; }
            nc *= 2;
        }
        np = realloc(g->p, (size_t)nc);
        if (!np) { g->err = 1; return -1; }
        g->p = np;
        g->cap = nc;
    }
    return 0;
}

/* Сырой deflate: контейнер gzip собирается вокруг него. */
static unsigned char *raw_deflate(const unsigned char *raw, int n, int level, int *out_n) {
    z_stream s;
    unsigned char *def;
    uLong bound;
    int ret;
    memset(&s, 0, sizeof s);
    if (level < 1) level = 6;
    if (deflateInit2(&s, level, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return NULL;
    bound = deflateBound(&s, (uLong)n);
    def = malloc(bound ? bound : 1);
    if (!def) { deflateEnd(&s); return NULL; }
    s.next_in = (Bytef *)raw;
    s.avail_in = (uInt)n;
    s.next_out = def;
    s.avail_out = (uInt)bound;
    ret = deflate(&s, Z_FINISH);
    *out_n = (int)s.total_out;
    deflateEnd(&s);
    if (ret != Z_STREAM_END) { free(def); return NULL; }
    return def;
}

static int raw_inflate(const unsigned char *in, int n, struct grow *g, int *used) {
    z_stream s;
    int ret;
    memset(&s, 0, sizeof s);
    *used = 0;
    if (inflateInit2(&s, -15) != Z_OK) { g->err = 1; return -1; }
    s.next_in = (Bytef *)in;
    s.avail_in = (uInt)n;
    for (;;) {
        uLong in_before, out_before;
        int room;
        if (g->n >= OUT_MAX) { g->err = 1; ret = Z_DATA_ERROR; break; }
        room = OUT_MAX - g->n;
        if (room > 8192) room = 8192;
        if (grow_need(g, room) < 0) { ret = Z_MEM_ERROR; break; }
        s.next_out = g->p + g->n;
        s.avail_out = (uInt)(g->cap - g->n);
        in_before = s.total_in;
        out_before = s.total_out;
        ret = inflate(&s, Z_NO_FLUSH);
        g->n = (int)s.total_out;
        if (ret == Z_STREAM_END) break;
        if (ret != Z_OK && ret != Z_BUF_ERROR) break;
        if (s.total_in == in_before && s.total_out == out_before) { ret = Z_DATA_ERROR; break; }
    }
    *used = (int)s.total_in;
    inflateEnd(&s);
    if (ret != Z_STREAM_END) { g->err = 1; return -1; }
    return 0;
}

/* ------------------------------------------------------- контейнер gzip */

static const char *base_of(const char *path) {
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/') s = p + 1;
    return s;
}

static unsigned char *gzip_wrap(const unsigned char *raw, int n, const char *path, int level, int save_name, int *out_n) {
    const char *base = base_of(path);
    int namelen = 0;
    unsigned crc, isize;
    unsigned char *def, *out;
    int dn, off, i;
    if (save_name && base[0]) namelen = (int)strlen(base);
    if (namelen > 255) namelen = 0;
    def = raw_deflate(raw, n, level, &dn);
    if (!def) return NULL;
    out = malloc((size_t)dn + (size_t)namelen + 32);
    if (!out) { free(def); return NULL; }
    out[0] = 0x1F; out[1] = 0x8B; out[2] = 8;
    out[3] = namelen ? 8 : 0;
    out[4] = out[5] = out[6] = out[7] = 0;
    out[8] = (unsigned char)(level >= 9 ? 2 : level <= 1 ? 4 : 0);
    out[9] = 3;
    off = 10;
    if (namelen) {
        memcpy(out + off, base, (size_t)namelen);
        off += namelen;
        out[off++] = 0;
    }
    memcpy(out + off, def, (size_t)dn);
    off += dn;
    free(def);
    crc = (unsigned)crc32(0, raw ? raw : (const Bytef *)"", (uInt)n);
    isize = (unsigned)n;
    for (i = 0; i < 4; i++) out[off++] = (unsigned char)((crc >> (8 * i)) & 255);
    for (i = 0; i < 4; i++) out[off++] = (unsigned char)((isize >> (8 * i)) & 255);
    *out_n = off;
    return out;
}

static int rd_u16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd_u32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}


/* ------------------------------------------------------- файлы */

static void warn_path(const char *path, const char *msg) {
    if (opt_quiet) return;
    fprintf(stderr, "%s: %s: %s\n", __progname ? __progname : "gzip", path, msg);
}

static void err_path(const char *path, const char *msg) {
    fprintf(stderr, "%s: %s: %s\n", __progname ? __progname : "gzip", path, msg);
}

static int read_whole(const char *path, unsigned char **out, int *out_n) {
    struct stat st;
    int fd, off = 0;
    unsigned char *buf;
    if (stat(path, &st) < 0) { err_path(path, strerror(errno)); return -1; }
    if (S_ISDIR(st.st_mode)) { warn_path(path, "is a directory"); return -2; }
    if (!S_ISREG(st.st_mode)) { warn_path(path, "not a regular file"); return -2; }
    if (st.st_size > (unsigned)OUT_MAX) { err_path(path, "file too large"); return -1; }
    fd = open(path, O_RDONLY);
    if (fd < 0) { err_path(path, strerror(errno)); return -1; }
    buf = malloc(st.st_size ? st.st_size : 1);
    if (!buf) { close(fd); err_path(path, "out of memory"); return -1; }
    while (off < (int)st.st_size) {
        int r = read(fd, buf + off, (size_t)((int)st.st_size - off));
        if (r < 0) { close(fd); free(buf); err_path(path, strerror(errno)); return -1; }
        if (r == 0) break;
        off += r;
    }
    close(fd);
    *out = buf;
    *out_n = off;
    return 0;
}

static int write_whole(const char *path, const unsigned char *p, int n) {
    int fd, off = 0;
    int flags = O_WRONLY | O_CREAT | O_TRUNC;
    if (!opt_force) flags |= O_EXCL;
    fd = open(path, flags);
    if (fd < 0) { err_path(path, strerror(errno)); return -1; }
    while (off < n) {
        int r = write(fd, p + off, (size_t)(n - off));
        if (r <= 0) {
            int e = errno;
            close(fd);
            remove(path);
            errno = e;
            err_path(path, strerror(errno));
            return -1;
        }
        off += r;
    }
    if (close(fd) < 0) { err_path(path, strerror(errno)); return -1; }
    return 0;
}

static int ends_with(const char *name, const char *suf) {
    int n = (int)strlen(name), m = (int)strlen(suf);
    if (m <= 0 || n < m) return 0;
    return memcmp(name + n - m, suf, (size_t)m) == 0;
}

static int join_path(char *dst, int cap, const char *dir, const char *name) {
    int nd = (int)strlen(dir), nn = (int)strlen(name), slash;
    slash = nd > 0 && dir[nd - 1] != '/';
    if (nd == 1 && dir[0] == '/') slash = 0;
    if (nd + slash + nn + 1 > cap) return -1;
    memcpy(dst, dir, (size_t)nd);
    if (slash) dst[nd++] = '/';
    memcpy(dst + nd, name, (size_t)nn + 1);
    return 0;
}

static void ratio_text(char *dst, int cap, int comp, int uncomp) {
    int tenths = 0, sign = 0;
    if (uncomp > 0) {
        long long v = ((long long)uncomp - comp) * 1000 / uncomp;
        if (v < 0) { sign = 1; v = -v; }
        tenths = (int)v;
    }
    snprintf(dst, (size_t)cap, "%s%d.%d%%", sign ? "-" : "", tenths / 10, tenths % 10);
}

static int name_ok(const char *s) {
    int i;
    if (!s || !s[0] || !strcmp(s, ".") || !strcmp(s, "..")) return 0;
    for (i = 0; s[i]; i++) if (s[i] == '/') return 0;
    return 1;
}

/* Распаковка с известной длиной. */
static unsigned char *gzip_unwrap_n(const unsigned char *in, int n, char *saved, int saved_cap, int *out_n, int *trailing, const char **err) {
    struct grow g;
    int off = 0;
    memset(&g, 0, sizeof g);
    if (trailing) *trailing = 0;
    if (saved && saved_cap) saved[0] = 0;
    if (n < 10) { *err = "not in gzip format"; return NULL; }
    while (off + 10 <= n) {
        int flg, pos, xlen;
        struct grow part;
        unsigned want, got;
        int used;
        if (!(in[off] == 0x1F && in[off + 1] == 0x8B)) {
            if (off == 0) { *err = "not in gzip format"; free(g.p); return NULL; }
            if (trailing) *trailing = 1;
            break;
        }
        if (in[off + 2] != 8) { *err = "unknown method"; free(g.p); return NULL; }
        flg = in[off + 3];
        if (flg & 0xE0) { *err = "reserved flags"; free(g.p); return NULL; }
        pos = off + 10;
        if (flg & 4) {
            if (pos + 2 > n) { *err = "unexpected end of file"; free(g.p); return NULL; }
            xlen = rd_u16(in + pos);
            pos += 2;
            if (xlen < 0 || pos > n - xlen) { *err = "unexpected end of file"; free(g.p); return NULL; }
            pos += xlen;
        }
        if (flg & 8) {
            int k = 0;
            if (saved && saved_cap && !saved[0]) {
                while (pos < n && in[pos] && k + 1 < saved_cap) saved[k++] = (char)in[pos++];
                saved[k] = 0;
            }
            while (pos < n && in[pos]) pos++;
            if (pos >= n) { *err = "unexpected end of file"; free(g.p); return NULL; }
            pos++;
        }
        if (flg & 16) {
            while (pos < n && in[pos]) pos++;
            if (pos >= n) { *err = "unexpected end of file"; free(g.p); return NULL; }
            pos++;
        }
        if (flg & 2) {
            if (pos + 2 > n) { *err = "unexpected end of file"; free(g.p); return NULL; }
            pos += 2;
        }
        memset(&part, 0, sizeof part);
        if (raw_inflate(in + pos, n - pos, &part, &used) < 0 || part.err) {
            free(part.p); free(g.p); *err = "invalid compressed data"; return NULL;
        }
        if (pos + used + 8 > n) { free(part.p); free(g.p); *err = "unexpected end of file"; return NULL; }
        want = rd_u32(in + pos + used);
        got = (unsigned)crc32(0, part.p ? part.p : (const Bytef *)"", (uInt)part.n);
        if (want != got) { free(part.p); free(g.p); *err = "crc error"; return NULL; }
        if ((int)rd_u32(in + pos + used + 4) != part.n) {
            free(part.p); free(g.p); *err = "length error"; return NULL;
        }
        if (grow_need(&g, part.n) < 0) { free(part.p); free(g.p); *err = "out of memory"; return NULL; }
        if (part.n && part.p) memcpy(g.p + g.n, part.p, (size_t)part.n);
        g.n += part.n;
        free(part.p);
        off = pos + used + 8;
    }
    if (!g.p) {
        g.p = malloc(1);
        if (!g.p) { *err = "out of memory"; return NULL; }
    }
    *out_n = g.n;
    *err = NULL;
    return g.p;
}

/* Сравнение абсолютных путей. Символические ссылки здесь не раскрываются:
 * проверка только не даёт ./ и .. указать вывод на входной файл. */
static int normalized_path(const char *path, char out[256]) {
    char joined[384];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof joined) return -1;
        strcpy(joined, path);
    } else {
        char cwd[128];
        if (!getcwd(cwd, sizeof cwd) || strlen(cwd) + strlen(path) + 2 > sizeof joined) return -1;
        snprintf(joined, sizeof joined, "%s/%s", cwd, path);
    }
    int used = 1;
    out[0] = '/'; out[1] = 0;
    const char *p = joined;
    while (*p) {
        while (*p == '/') p++;
        const char *start = p;
        while (*p && *p != '/') p++;
        int n = (int)(p - start);
        if (!n || (n == 1 && start[0] == '.')) continue;
        if (n == 2 && start[0] == '.' && start[1] == '.') {
            while (used > 1 && out[used - 1] != '/') used--;
            if (used > 1) used--;
        } else {
            if (used + n + 2 > 256) return -1;
            if (used > 1) out[used++] = '/';
            memcpy(out + used, start, (size_t)n); used += n;
        }
        out[used] = 0;
    }
    return 0;
}

static int decompress_file(const char *path) {
    unsigned char *in = NULL, *raw = NULL;
    int n = 0, un = 0, rc, trailing = 0;
    char saved[256], outpath[128];
    const char *err = NULL;
    char alt[128];
    struct stat st;
    const char *src = path;

    if (!ends_with(path, opt_suffix)) {
        struct stat st;
        if (stat(path, &st) < 0) {
            if (strlen(path) + strlen(opt_suffix) + 1 > sizeof alt) { err_path(path, "file name too long"); return 1; }
            memcpy(alt, path, strlen(path));
            memcpy(alt + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
            src = alt;
        }
    }
    rc = read_whole(src, &in, &n);
    if (rc == -2) return 0;
    if (rc < 0) return 1;
    raw = gzip_unwrap_n(in, n, saved, (int)sizeof saved, &un, &trailing, &err);
    if (!raw) { err_path(src, err ? err : "invalid compressed data"); free(in); return 1; }
    if (trailing) warn_path(src, "trailing garbage ignored");
    if (opt_list) {
        const char *show = name_ok(saved) ? saved : base_of(src);
        static int header;
        char trimmed[128];
        int sl = (int)strlen(show);
        if (!header && !opt_quiet) {
            printf("         compressed        uncompressed  ratio uncompressed_name\n");
            header = 1;
        }
        if (ends_with(show, opt_suffix)) {
            if (sl > (int)sizeof trimmed) sl = (int)sizeof trimmed;
            memcpy(trimmed, show, (size_t)sl - strlen(opt_suffix));
            trimmed[sl - (int)strlen(opt_suffix)] = 0;
            show = trimmed;
        }
        {
            char ratio[16];
            ratio_text(ratio, (int)sizeof ratio, n, un);
            printf("%20d %20d %8s %s\n", n, un, ratio, show);
        }
        free(in); free(raw);
        return 0;
    }
    if (opt_test) { free(in); free(raw); return 0; }

    if (ends_with(src, opt_suffix)) {
        int keep = (int)strlen(src) - (int)strlen(opt_suffix);
        if (keep <= 0 || keep >= (int)sizeof outpath) { err_path(src, "file name too long"); free(in); free(raw); return 1; }
        memcpy(outpath, src, (size_t)keep);
        outpath[keep] = 0;
    } else {
        if (strlen(src) + 4 >= sizeof outpath) { err_path(src, "file name too long"); free(in); free(raw); return 1; }
        memcpy(outpath, src, strlen(src) + 1);
        memcpy(outpath + strlen(outpath), ".out", 5);
    }
    if (!opt_noname && opt_name && name_ok(saved)) {
        char parent[128];
        const char *slash = strrchr(src, '/');
        if (!slash) {
            if (strlen(saved) >= sizeof outpath) { err_path(src, "file name too long"); free(in); free(raw); return 1; }
            memcpy(outpath, saved, strlen(saved) + 1);
        } else {
            int nd = (int)(slash - src);
            if (nd >= (int)sizeof parent) { err_path(src, "file name too long"); free(in); free(raw); return 1; }
            memcpy(parent, src, (size_t)nd);
            parent[nd] = 0;
            if (join_path(outpath, (int)sizeof outpath, parent, saved) < 0) {
                err_path(src, "file name too long"); free(in); free(raw); return 1;
            }
        }
    }
    char input_abs[256], output_abs[256];
    if (normalized_path(src, input_abs) < 0 || normalized_path(outpath, output_abs) < 0) {
        err_path(src, "cannot resolve input/output path"); free(in); free(raw); return 1;
    }
    if (!strcmp(input_abs, output_abs)) {
        err_path(src, "input and output are the same file"); free(in); free(raw); return 1;
    }
    if (!opt_force && stat(outpath, &st) == 0) {
        err_path(outpath, "already exists");
        free(in); free(raw);
        return 1;
    }
    if (write_whole(outpath, raw, un) < 0) { free(in); free(raw); return 1; }
    if (opt_verbose) fprintf(stderr, "%s: decompressed into %s\n", src, outpath);
    free(in); free(raw);
    if (!opt_keep && !opt_test && !opt_list) {
        if (remove(src) < 0) warn_path(src, strerror(errno));
    }
    return 0;
}

static int compress_file(const char *path) {
    unsigned char *in = NULL, *out = NULL;
    int n = 0, on = 0, rc;
    char outpath[160];
    if (!opt_force && ends_with(path, opt_suffix)) {
        warn_path(path, "already has suffix -- unchanged");
        return 0;
    }
    if (strlen(path) + strlen(opt_suffix) + 1 > sizeof outpath) { err_path(path, "file name too long"); return 1; }
    memcpy(outpath, path, strlen(path));
    memcpy(outpath + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
    if (!opt_force) {
        struct stat st;
        if (stat(outpath, &st) == 0) { err_path(outpath, "already exists"); return 1; }
    }
    rc = read_whole(path, &in, &n);
    if (rc == -2) return 0;
    if (rc < 0) return 1;
    out = gzip_wrap(in, n, path, opt_level, !opt_noname, &on);
    if (!out) { err_path(path, "out of memory"); free(in); return 1; }
    if (write_whole(outpath, out, on) < 0) { free(in); free(out); return 1; }
    if (opt_verbose) {
        char ratio[16];
        ratio_text(ratio, (int)sizeof ratio, on, n);
        fprintf(stderr, "%s: %s -- replaced with %s\n", path, ratio, outpath);
    }
    free(in); free(out);
    if (!opt_keep) {
        if (remove(path) < 0) warn_path(path, strerror(errno));
    }
    return 0;
}

static int process(const char *path);

static int process_dir(const char *path) {
    DIR *d = opendir(path);
    struct dirent *ent;
    char **names = NULL;
    int n = 0, cap = 0, i, rc = 0;
    if (!d) { err_path(path, strerror(errno)); return 1; }
    while ((ent = readdir(d))) {
        char *copy;
        if (n >= cap) {
            int nc = cap ? cap * 2 : 16;
            char **nn = realloc(names, (size_t)nc * sizeof *nn);
            if (!nn) { closedir(d); err_path(path, "out of memory"); return 1; }
            names = nn;
            cap = nc;
        }
        copy = malloc(strlen(ent->d_name) + 1);
        if (!copy) {
            int j;
            closedir(d);
            for (j = 0; j < n; j++) free(names[j]);
            free(names);
            err_path(path, "out of memory");
            return 1;
        }
        memcpy(copy, ent->d_name, strlen(ent->d_name) + 1);
        names[n++] = copy;
    }
    closedir(d);
    for (i = 0; i < n; i++) {
        char child[128];
        if (join_path(child, (int)sizeof child, path, names[i]) < 0) {
            err_path(path, "file name too long");
            rc = 1;
        } else if (process(child)) rc = 1;
        free(names[i]);
    }
    free(names);
    return rc;
}

static int process(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) {
        if (opt_decomp && !ends_with(path, opt_suffix)) {
            char alt[160];
            if (strlen(path) + strlen(opt_suffix) + 1 <= sizeof alt) {
                memcpy(alt, path, strlen(path));
                memcpy(alt + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
                if (stat(alt, &st) == 0) return decompress_file(path);
            }
        }
        err_path(path, strerror(errno));
        return 1;
    }
    if (S_ISDIR(st.st_mode)) {
        if (opt_recur) return process_dir(path);
        warn_path(path, "is a directory -- ignored");
        return 0;
    }
    if (opt_decomp || opt_test || opt_list) {
        /* Имя без суффикса — это либо «распакуй path.gz», либо чужой файл. */
        if (!ends_with(path, opt_suffix)) {
            char alt[160];
            struct stat gz;
            if ((int)(strlen(path) + strlen(opt_suffix) + 1) <= (int)sizeof alt) {
                memcpy(alt, path, strlen(path));
                memcpy(alt + strlen(path), opt_suffix, strlen(opt_suffix) + 1);
                if (stat(alt, &gz) == 0) return decompress_file(alt);
            }
            warn_path(path, "unknown suffix -- ignored");
            return 0;
        }
        return decompress_file(path);
    }
    return compress_file(path);
}

static void usage(int ok) {
    const char *p = __progname ? __progname : "gzip";
    fprintf(ok ? stdout : stderr,
        "usage: %s [OPTION]... [FILE]...\n"
        "Compress or uncompress FILEs in place. Standard input and output are not supported.\n"
        "\n"
        "  -a, --ascii       accepted and ignored\n"
        "  -c, --stdout      (not supported yet)\n"
        "  -d, --decompress  decompress\n"
        "  -f, --force       overwrite output and compress files that already have the suffix\n"
        "  -h, --help        give this help\n"
        "  -k, --keep        keep input files\n"
        "  -l, --list        list compressed file contents\n"
        "  -L, --license     display software license\n"
        "  -n, --no-name     do not save or restore the original file name\n"
        "  -N, --name        save or restore the original file name\n"
        "  -q, --quiet       suppress warnings\n"
        "  -r, --recursive   operate on directories\n"
        "  -S, --suffix=SUF  use suffix SUF instead of .gz\n"
        "  -t, --test        test compressed file integrity\n"
        "  -v, --verbose     verbose mode\n"
        "  -V, --version     display version number\n"
        "  -1, --fast        compress faster\n"
        "  -9, --best        compress better\n"
        "      --rsyncable   accepted; blocks are not aligned for rsync\n",
        p);
}

static int long_opt(const char *a, int *i, int argc, char **argv) {
    const char *eq = strchr(a, '=');
    char name[32];
    const char *val = NULL;
    int nlen;
    if (eq) {
        nlen = (int)(eq - a);
        val = eq + 1;
    } else nlen = (int)strlen(a);
    if (nlen <= 2 || nlen >= (int)sizeof name) return -1;
    memcpy(name, a, (size_t)nlen);
    name[nlen] = 0;
    if (!strcmp(name, "--stdout") || !strcmp(name, "--to-stdout")) { opt_stdout = 1; return 0; }
    if (!strcmp(name, "--decompress") || !strcmp(name, "--uncompress")) { opt_decomp = 1; return 0; }
    if (!strcmp(name, "--force")) { opt_force = 1; return 0; }
    if (!strcmp(name, "--help")) { usage(1); exit(0); }
    if (!strcmp(name, "--keep")) { opt_keep = 1; return 0; }
    if (!strcmp(name, "--list")) { opt_list = 1; return 0; }
    if (!strcmp(name, "--license")) {
        printf("MyOS gzip. gzip container (RFC 1952), deflate (RFC 1951).\n");
        exit(0);
    }
    if (!strcmp(name, "--no-name")) { opt_noname = 1; opt_name = 0; return 0; }
    if (!strcmp(name, "--name")) { opt_name = 1; opt_noname = 0; return 0; }
    if (!strcmp(name, "--quiet")) { opt_quiet = 1; return 0; }
    if (!strcmp(name, "--recursive")) { opt_recur = 1; return 0; }
    if (!strcmp(name, "--suffix")) {
        if (!val) {
            if (*i + 1 >= argc) return -1;
            val = argv[++*i];
        }
        if (!val[0]) return -1;
        opt_suffix = val;
        return 0;
    }
    if (!strcmp(name, "--test")) { opt_test = 1; return 0; }
    if (!strcmp(name, "--verbose")) { opt_verbose = 1; return 0; }
    if (!strcmp(name, "--version")) { printf("gzip 0.1\n"); exit(0); }
    if (!strcmp(name, "--fast")) { opt_level = 1; return 0; }
    if (!strcmp(name, "--best")) { opt_level = 9; return 0; }
    if (!strcmp(name, "--rsyncable") || !strcmp(name, "--ascii")) return 0;
    return -1;
}

int main(int argc, char **argv) {
    int i, rc = 0, files = 0;
    const char *me;
    opt_level = 6;
    me = __progname ? __progname : (argc > 0 ? base_of(argv[0]) : "gzip");
    if (!strcmp(me, "gunzip") || !strcmp(me, "uncompress")) opt_decomp = 1;
    for (i = 1; i < argc; i++) {
        char *a = argv[i];
        if (a[0] != '-' || a[1] == 0) { files++; continue; }
        if (a[1] == '-' && a[2] == 0) {
            files += argc - i - 1;
            break;
        }
        if (a[1] == '-') {
            if (long_opt(a, &i, argc, argv) < 0) {
                fprintf(stderr, "%s: unrecognized option '%s'\n", me, a);
                usage(0);
                return 1;
            }
            continue;
        }
        {
            char *p;
            for (p = a + 1; *p; p++) {
                if (*p >= '1' && *p <= '9') { opt_level = *p - '0'; continue; }
                switch (*p) {
                case 'a': break;
                case 'c': opt_stdout = 1; break;
                case 'd': opt_decomp = 1; break;
                case 'f': opt_force = 1; break;
                case 'h': usage(1); return 0;
                case 'k': opt_keep = 1; break;
                case 'l': opt_list = 1; break;
                case 'L':
                    printf("MyOS gzip. gzip container (RFC 1952), deflate (RFC 1951).\n");
                    return 0;
                case 'n': opt_noname = 1; opt_name = 0; break;
                case 'N': opt_name = 1; opt_noname = 0; break;
                case 'q': opt_quiet = 1; break;
                case 'r': opt_recur = 1; break;
                case 'S':
                    if (p[1]) { opt_suffix = p + 1; p += strlen(p) - 1; }
                    else if (i + 1 < argc) opt_suffix = argv[++i];
                    else { fprintf(stderr, "%s: option requires an argument -- S\n", me); return 1; }
                    if (!opt_suffix[0]) { fprintf(stderr, "%s: empty suffix\n", me); return 1; }
                    break;
                case 't': opt_test = 1; break;
                case 'v': opt_verbose = 1; break;
                case 'V': printf("gzip 0.1\n"); return 0;
                default:
                    fprintf(stderr, "%s: unrecognized option '%c'\n", me, *p);
                    usage(0);
                    return 1;
                }
            }
        }
    }
    if (opt_stdout || files == 0) {
        fprintf(stderr, "%s: standard input/output is not supported\n", me);
        return 1;
    }
    if (opt_test || opt_list) opt_decomp = 1;
    {
        int dash = 0;
        for (i = 1; i < argc; i++) {
            char *a = argv[i];
            if (!dash && !strcmp(a, "--")) { dash = 1; continue; }
            if (!dash && a[0] == '-' && a[1]) {
                if ((a[1] != '-' && a[strlen(a) - 1] == 'S') || !strcmp(a, "--suffix")) i++;
                continue;
            }
            if (!strcmp(a, "-")) {
                fprintf(stderr, "%s: standard input is not supported\n", me);
                rc = 1;
                continue;
            }
            if (process(a)) rc = 1;
        }
    }
    return rc;
}
