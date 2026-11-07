/* gzip — системный архиватор формата RFC 1952.
 * Поток внутри — deflate (RFC 1951): на запись фиксированные коды Хаффмана
 * либо несжатые блоки, на чтение ещё и динамические коды (их пишет GNU gzip).
 * Флаги совпадают с GNU gzip. Стандартный ввод и вывод пока не поддерживаются:
 * -c и запуск без имён файлов завершаются ошибкой.
 *
 * gunzip — тот же файл, установленный вторым именем: тогда по умолчанию -d. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "dirent.h"

#define WSIZE     32768
#define HASH_SIZE 32768
#define MAX_MATCH 258
#define OUT_MAX   (9 * 1024 * 1024)

static const unsigned short len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const unsigned char clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static unsigned crc_table[256];
static int crc_ready;
static unsigned gz_head[HASH_SIZE];
static unsigned gz_prev[WSIZE];
static unsigned short lit_code[288];
static unsigned char lit_nbits[288];
static unsigned short dist_code[30];
static unsigned char dist_nbits[30];
static int codes_ready;

static int opt_decomp, opt_force, opt_keep, opt_list, opt_name, opt_noname;
static int opt_quiet, opt_recur, opt_test, opt_verbose, opt_stdout, opt_level;
static const char *opt_suffix = ".gz";

static void crc_init(void) {
    unsigned i, c, bit;
    if (crc_ready) return;
    for (i = 0; i < 256; i++) {
        c = i;
        for (bit = 0; bit < 8; bit++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        crc_table[i] = c;
    }
    crc_ready = 1;
}

static unsigned crc32_bytes(const unsigned char *p, int n) {
    unsigned crc = 0xFFFFFFFFu;
    int i;
    crc_init();
    for (i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 255] ^ (crc >> 8);
    return ~crc;
}

/* ------------------------------------------------------- битовый поток */

struct br {
    const unsigned char *in;
    int len, pos;
    unsigned buf;
    int bits;
    int err;
};

static int getbits(struct br *b, int n) {
    unsigned v;
    if (b->err || n < 0 || n > 16) { b->err = 1; return 0; }
    while (b->bits < n) {
        if (b->pos >= b->len) { b->err = 1; return 0; }
        b->buf |= (unsigned)b->in[b->pos++] << b->bits;
        b->bits += 8;
    }
    v = b->buf & ((1u << n) - 1u);
    b->buf >>= n;
    b->bits -= n;
    return (int)v;
}

/* Отбросить хвост текущего байта (заголовок stored-блока на границе байта). */
static void align_byte(struct br *b) {
    int rem = b->bits & 7;
    if (rem) getbits(b, rem);
}

struct bout {
    unsigned char *p;
    int n, cap, bits, err;
    unsigned acc;
};

static void put_byte(struct bout *o, unsigned char c) {
    if (o->err) return;
    if (o->n >= o->cap) {
        int nc = o->cap ? o->cap * 2 : 256;
        unsigned char *np;
        if (nc < o->n + 1) nc = o->n + 1;
        np = realloc(o->p, (size_t)nc);
        if (!np) { o->err = 1; return; }
        o->p = np;
        o->cap = nc;
    }
    o->p[o->n++] = c;
}

static void putbit(struct bout *o, int bit) {
    if (o->err) return;
    o->acc |= (unsigned)(bit & 1) << o->bits;
    if (++o->bits == 8) { put_byte(o, (unsigned char)o->acc); o->acc = 0; o->bits = 0; }
}

static void put_msb(struct bout *o, unsigned v, int n) {
    int i;
    for (i = n - 1; i >= 0; i--) putbit(o, (v >> i) & 1);
}

static void put_lsb(struct bout *o, unsigned v, int n) {
    int i;
    for (i = 0; i < n; i++) putbit(o, (v >> i) & 1);
}

static void bout_flush(struct bout *o) {
    if (o->bits) put_byte(o, (unsigned char)o->acc);
    o->acc = 0;
    o->bits = 0;
}

/* ------------------------------------------------------- Хаффман */

static int construct(short *count, short *symbol, const unsigned char *length, int n) {
    int len, sym, left;
    short offs[16];
    for (len = 0; len <= 15; len++) count[len] = 0;
    for (sym = 0; sym < n; sym++) {
        if (length[sym] > 15) return -1;
        count[length[sym]]++;
    }
    if (count[0] == n) return 0;
    left = 1;
    for (len = 1; len <= 15; len++) {
        left <<= 1;
        left -= count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + count[len]);
    for (sym = 0; sym < n; sym++)
        if (length[sym]) symbol[offs[length[sym]]++] = (short)sym;
    return left;
}

static int decode(struct br *b, const short *count, const short *symbol) {
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= 15; len++) {
        int count_len;
        code |= getbits(b, 1);
        if (b->err) return -1;
        count_len = count[len];
        if (code - count_len < first) return symbol[index + (code - first)];
        index += count_len;
        first += count_len;
        first <<= 1;
        code <<= 1;
    }
    return -1;
}

static void codes_from_lengths(unsigned short *code, unsigned char *nbits, const unsigned char *len, int n) {
    int count[16], nextc[16], i, bits, c;
    memset(count, 0, sizeof count);
    for (i = 0; i < n; i++) if (len[i]) count[len[i]]++;
    c = 0;
    for (bits = 1; bits <= 15; bits++) {
        c = (c + count[bits - 1]) << 1;
        nextc[bits] = c;
    }
    for (i = 0; i < n; i++) {
        nbits[i] = len[i];
        code[i] = 0;
        if (len[i]) code[i] = (unsigned short)nextc[len[i]]++;
    }
}

static void codes_init(void) {
    unsigned char llen[288], dlen[30];
    int i;
    if (codes_ready) return;
    for (i = 0; i < 144; i++) llen[i] = 8;
    for (; i < 256; i++) llen[i] = 9;
    for (; i < 280; i++) llen[i] = 7;
    for (; i < 288; i++) llen[i] = 8;
    for (i = 0; i < 30; i++) dlen[i] = 5;
    codes_from_lengths(lit_code, lit_nbits, llen, 288);
    codes_from_lengths(dist_code, dist_nbits, dlen, 30);
    codes_ready = 1;
}

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

static int emit_lit(struct bout *o, int sym) {
    if (sym < 0 || sym > 287 || !lit_nbits[sym]) return -1;
    put_msb(o, lit_code[sym], lit_nbits[sym]);
    return o->err ? -1 : 0;
}

static int emit_match(struct bout *o, int length, int dist) {
    int i, ls, ds;
    if (length < 3 || length > 258 || dist < 1 || dist > WSIZE) return -1;
    ls = 285;
    for (i = 0; i < 29; i++) {
        int max = len_base[i] + ((1 << len_extra[i]) - 1);
        if (length <= max) { ls = 257 + i; break; }
    }
    if (emit_lit(o, ls) < 0) return -1;
    put_lsb(o, (unsigned)(length - len_base[ls - 257]), len_extra[ls - 257]);
    ds = 29;
    for (i = 0; i < 30; i++) {
        int max = dist_base[i] + ((1 << dist_extra[i]) - 1);
        if (dist <= max) { ds = i; break; }
    }
    put_msb(o, dist_code[ds], dist_nbits[ds]);
    put_lsb(o, (unsigned)(dist - dist_base[ds]), dist_extra[ds]);
    return o->err ? -1 : 0;
}

/* ------------------------------------------------------- распаковка */

static int inflate_block(struct br *b, struct grow *g, int *bfinal) {
    int type, len, nlen, sym;
    short lcount[16], lsym[288], dcount[16], dsym[32];
    unsigned char llen[288], dlen[32], clen[19];
    int nlit, ndist, nclen, i, nlen_tot;

    *bfinal = getbits(b, 1);
    type = getbits(b, 2);
    if (b->err) return -1;
    if (type == 3) return -1;

    if (type == 0) {
        align_byte(b);
        len = getbits(b, 16);
        nlen = getbits(b, 16);
        if (b->err || nlen != (len ^ 0xFFFF)) return -1;
        if (grow_need(g, len) < 0) return -1;
        while (b->bits >= 8 && len > 0) {
            g->p[g->n++] = (unsigned char)(b->buf & 255);
            b->buf >>= 8;
            b->bits -= 8;
            len--;
        }
        if (b->pos + len > b->len) { b->err = 1; return -1; }
        if (len) {
            memcpy(g->p + g->n, b->in + b->pos, (size_t)len);
            g->n += len;
            b->pos += len;
        }
        return 0;
    }

    if (type == 1) {
        static short fcount[16], fsym[288], fdcount[16], fdsym[32];
        static int ready;
        if (!ready) {
            unsigned char fl[288], fd[32];
            for (i = 0; i < 144; i++) fl[i] = 8;
            for (; i < 256; i++) fl[i] = 9;
            for (; i < 280; i++) fl[i] = 7;
            for (; i < 288; i++) fl[i] = 8;
            for (i = 0; i < 32; i++) fd[i] = 5;
            if (construct(fcount, fsym, fl, 288) < 0) return -1;
            if (construct(fdcount, fdsym, fd, 32) < 0) return -1;
            ready = 1;
        }
        memcpy(lcount, fcount, sizeof lcount);
        memcpy(lsym, fsym, sizeof lsym);
        memcpy(dcount, fdcount, sizeof dcount);
        memcpy(dsym, fdsym, sizeof dsym);
    } else {
        memset(clen, 0, sizeof clen);
        memset(llen, 0, sizeof llen);
        memset(dlen, 0, sizeof dlen);
        nlit = getbits(b, 5) + 257;
        ndist = getbits(b, 5) + 1;
        nclen = getbits(b, 4) + 4;
        if (b->err || nlit > 286 || ndist > 32 || nclen > 19) return -1;
        for (i = 0; i < nclen; i++) clen[clen_order[i]] = (unsigned char)getbits(b, 3);
        if (b->err || construct(lcount, lsym, clen, 19) < 0) return -1;
        nlen_tot = nlit + ndist;
        i = 0;
        while (i < nlen_tot) {
            int rep, what;
            sym = decode(b, lcount, lsym);
            if (sym < 0 || sym > 18) return -1;
            if (sym < 16) {
                if (i < nlit) llen[i] = (unsigned char)sym;
                else dlen[i - nlit] = (unsigned char)sym;
                i++;
                continue;
            }
            if (sym == 16) {
                if (i == 0) return -1;
                what = i <= nlit ? llen[i - 1] : dlen[i - nlit - 1];
                rep = 3 + getbits(b, 2);
            } else if (sym == 17) {
                what = 0;
                rep = 3 + getbits(b, 3);
            } else {
                what = 0;
                rep = 11 + getbits(b, 7);
            }
            if (b->err || i + rep > nlen_tot) return -1;
            while (rep--) {
                if (i < nlit) llen[i] = (unsigned char)what;
                else dlen[i - nlit] = (unsigned char)what;
                i++;
            }
        }
        if (construct(lcount, lsym, llen, nlit) < 0) return -1;
        if (construct(dcount, dsym, dlen, ndist) < 0) return -1;
    }

    for (;;) {
        sym = decode(b, lcount, lsym);
        if (sym < 0 || sym > 285) return -1;
        if (sym < 256) {
            if (grow_need(g, 1) < 0) return -1;
            g->p[g->n++] = (unsigned char)sym;
            continue;
        }
        if (sym == 256) return 0;
        {
            int length = len_base[sym - 257] + getbits(b, len_extra[sym - 257]);
            int ds = decode(b, dcount, dsym);
            int dist, j;
            if (ds < 0 || ds > 29 || b->err) return -1;
            dist = dist_base[ds] + getbits(b, dist_extra[ds]);
            if (b->err || dist < 1 || dist > g->n || length < 3) return -1;
            if (grow_need(g, length) < 0) return -1;
            for (j = 0; j < length; j++) g->p[g->n + j] = g->p[g->n - dist + j];
            g->n += length;
        }
    }
}

static int inflate(const unsigned char *in, int n, struct grow *g) {
    struct br b;
    int final = 0;
    memset(&b, 0, sizeof b);
    b.in = in;
    b.len = n;
    while (!final) {
        if (inflate_block(&b, g, &final) < 0) return -1;
    }
    /* Непрочитанные биты последнего байта — заполнение. Следующий байт уже в pos. */
    b.buf = 0;
    b.bits = 0;
    return b.pos;
}

/* ------------------------------------------------------- упаковка */

static unsigned hash3(const unsigned char *s) {
    return (((unsigned)s[0] << 10) ^ ((unsigned)s[1] << 5) ^ (unsigned)s[2]) & (HASH_SIZE - 1);
}

static void insert_hash(const unsigned char *in, int n, int pos) {
    unsigned h;
    if (pos < 0 || pos + 2 >= n) return;
    h = hash3(in + pos);
    gz_prev[pos & (WSIZE - 1)] = gz_head[h];
    gz_head[h] = (unsigned)pos + 1;
}

static int deflate_huff(const unsigned char *in, int n, int level, struct bout *o) {
    static const int chains[10] = { 0, 4, 8, 16, 32, 64, 128, 256, 1024, 4096 };
    static const int nices[10] = { 0, 8, 16, 32, 48, 64, 128, 128, 258, 258 };
    int pos = 0, chain, nice;
    if (level < 1) level = 1;
    if (level > 9) level = 9;
    chain = chains[level];
    nice = nices[level];
    codes_init();
    memset(gz_head, 0, sizeof gz_head);
    putbit(o, 1);
    putbit(o, 1);
    putbit(o, 0);
    while (pos < n) {
        int len = 0, dist = 0, k;
        if (pos + 3 <= n) {
            unsigned cur;
            int steps, max, best = 0, best_d = 0;
            insert_hash(in, n, pos);
            cur = gz_prev[pos & (WSIZE - 1)];
            max = n - pos;
            if (max > MAX_MATCH) max = MAX_MATCH;
            steps = chain;
            while (cur && steps--) {
                int p = (int)cur - 1;
                int d = pos - p;
                int m = 0;
                if (p < 0 || d <= 0 || d > WSIZE) break;
                while (m < max && in[p + m] == in[pos + m]) m++;
                if (m >= 3 && m > best) { best = m; best_d = d; }
                if (best >= nice || best == max) break;
                cur = gz_prev[p & (WSIZE - 1)];
            }
            len = best;
            dist = best_d;
        }
        if (len >= 3) {
            if (emit_match(o, len, dist) < 0) return -1;
            for (k = 1; k < len; k++) insert_hash(in, n, pos + k);
            pos += len;
        } else {
            if (emit_lit(o, in[pos]) < 0) return -1;
            pos++;
        }
    }
    if (emit_lit(o, 256) < 0) return -1;
    bout_flush(o);
    return o->err ? -1 : 0;
}

static int deflate_stored(const unsigned char *in, int n, struct bout *o) {
    int off = 0;
    while (off < n) {
        int chunk = n - off;
        int last;
        if (chunk > 65535) chunk = 65535;
        last = off + chunk == n;
        putbit(o, last);
        putbit(o, 0);
        putbit(o, 0);
        bout_flush(o);
        put_byte(o, (unsigned char)(chunk & 255));
        put_byte(o, (unsigned char)((chunk >> 8) & 255));
        put_byte(o, (unsigned char)((~chunk) & 255));
        put_byte(o, (unsigned char)(((~chunk) >> 8) & 255));
        if (o->err) return -1;
        if (o->n > OUT_MAX - chunk) return -1;
        if (o->n + chunk > o->cap) {
            unsigned char *np = realloc(o->p, (size_t)(o->n + chunk));
            if (!np) return -1;
            o->p = np;
            o->cap = o->n + chunk;
        }
        memcpy(o->p + o->n, in + off, (size_t)chunk);
        o->n += chunk;
        off += chunk;
    }
    if (n == 0) {
        putbit(o, 1);
        putbit(o, 0);
        putbit(o, 0);
        bout_flush(o);
        put_byte(o, 0); put_byte(o, 0); put_byte(o, 0xFF); put_byte(o, 0xFF);
    }
    return o->err ? -1 : 0;
}

static unsigned char *deflate(const unsigned char *in, int n, int level, int *out_n) {
    struct bout huff, stored;
    memset(&huff, 0, sizeof huff);
    memset(&stored, 0, sizeof stored);
    if (n && deflate_huff(in, n, level, &huff) == 0 && !huff.err && huff.n < n) {
        *out_n = huff.n;
        return huff.p;
    }
    free(huff.p);
    if (deflate_stored(in, n, &stored) < 0 || stored.err) { free(stored.p); return NULL; }
    *out_n = stored.n;
    return stored.p;
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
    def = deflate(raw, n, level, &dn);
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
    crc = crc32_bytes(raw, n);
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
        used = inflate(in + pos, n - pos, &part);
        if (used < 0 || part.err) { free(part.p); free(g.p); *err = "invalid compressed data"; return NULL; }
        if (pos + used + 8 > n) { free(part.p); free(g.p); *err = "unexpected end of file"; return NULL; }
        want = rd_u32(in + pos + used);
        got = crc32_bytes(part.p ? part.p : (const unsigned char *)"", part.n);
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

/* MyOS has no symlinks. Compare absolute, normalized paths so ./ and ..
 * cannot disguise writing the decompressed output over its input. */
static int normalized_path(const char *path, char out[256]) {
    char joined[384];
    if (path[0] == '/') {
        if (strlen(path) >= sizeof joined) return -1;
        strcpy(joined, path);
    } else {
        char cwd[128];
        if (getcwd(cwd, sizeof cwd) < 0 || strlen(cwd) + strlen(path) + 2 > sizeof joined) return -1;
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
