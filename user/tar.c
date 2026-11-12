/* tar — распаковка ustar и чтение оглавления.
 * Флаги как у GNU tar: tar xf a.tar, tar xzf a.tar.gz, -C, -v, -k,
 * --strip-components, длинные имена GNU и заголовки pax (path, linkpath, size).
 * .tar.gz разжимается в кольцо в 32 КиБ и сразу разбирается, без файла в /tmp.
 * Обычный tar читается потоком. Жёстких ссылок нет: содержимое копируется.
 * Устройства и FIFO пропускаются с сообщением. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"

#define NAME_CAP    1024
#define PAX_MAX     (64 * 1024)

static int opt_list, opt_gzip, opt_verbose, opt_keep, opt_absolute, opt_strip;
static const char *opt_file, *opt_dir;
static char **opt_members;
static int opt_nmem;

/* ------------------------------------------------------- gzip (inflate) */

/* Сжатый поток читается кусками. pre — уже прочитанное начало файла. */
struct gz_br {
    int fd;
    unsigned char pre[512];
    int pre_len, pre_pos;
    unsigned char buf[4096];
    int len, pos, bits, err, eof;
    unsigned acc;
};

static int gz_byte(struct gz_br *b, int *iseof) {
    if (iseof) *iseof = 0;
    if (b->err) return -1;
    if (b->pre_pos < b->pre_len) return b->pre[b->pre_pos++];
    if (b->pos >= b->len) {
        int r = read(b->fd, b->buf, sizeof b->buf);
        if (r < 0) { b->err = 1; return -1; }
        if (r == 0) { if (iseof) *iseof = 1; b->eof = 1; return -1; }
        b->len = r;
        b->pos = 0;
    }
    return b->buf[b->pos++];
}

static int gz_getbits(struct gz_br *b, int n) {
    unsigned v;
    if (b->err || n < 0 || n > 16) { b->err = 1; return 0; }
    while (b->bits < n) {
        int c = gz_byte(b, NULL);
        if (c < 0) { b->err = 1; return 0; }
        b->acc |= (unsigned)c << b->bits;
        b->bits += 8;
    }
    v = b->acc & ((1u << n) - 1u);
    b->acc >>= n;
    b->bits -= n;
    return (int)v;
}

static void gz_align(struct gz_br *b) {
    int rem = b->bits & 7;
    if (rem) gz_getbits(b, rem);
}

static int gz_construct(short *count, short *symbol, const unsigned char *length, int n) {
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

static int gz_decode(struct gz_br *b, const short *count, const short *symbol) {
    int len, code = 0, first = 0, index = 0;
    for (len = 1; len <= 15; len++) {
        int count_len;
        code |= gz_getbits(b, 1);
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

#define GZ_WIN 32768

static unsigned gz_crc_table[256];
static int gz_crc_ready;
static unsigned char gz_win[GZ_WIN];

static void gz_crc_init(void) {
    int i, bit;
    if (gz_crc_ready) return;
    for (i = 0; i < 256; i++) {
        unsigned c = (unsigned)i;
        for (bit = 0; bit < 8; bit++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        gz_crc_table[i] = c;
    }
    gz_crc_ready = 1;
}

/* Окно deflate и кольцо выхода — по 32 КиБ. Парсер забирает байты из кольца,
 * поэтому несжатый архив не записывается на диск. */
#define GZ_RING 32768
#define PH_MEMBER 0
#define PH_BLOCK  1
#define PH_STORED 2
#define PH_CODES  3
#define PH_MATCH  4

struct gz_out {
    unsigned char ring[GZ_RING];
    unsigned rpos, rlen;
    unsigned member, crc;
    int phase, finished, err, members, final_block;
    int stored_left, match_left;
    unsigned match_dist;
    short lcount[16], lsym[288], dcount[16], dsym[32];
};

static const char *gz_msg;
static int gz_msg_printed;

static void gz_fail(const char *m) { if (!gz_msg) gz_msg = m; }

static int gz_put(struct gz_out *o, unsigned char c) {
    o->ring[(o->rpos + o->rlen) & (GZ_RING - 1)] = c;
    o->rlen++;
    gz_win[o->member & (GZ_WIN - 1)] = c;
    o->crc = gz_crc_table[(o->crc ^ c) & 255] ^ (o->crc >> 8);
    o->member++;
    if (opt_verbose && (o->member & ((4u << 20) - 1)) == 0)
        fprintf(stderr, "tar: decompressed %u MiB\n", o->member >> 20);
    return 0;
}

static const unsigned short gz_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};
static const unsigned char gz_len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};
static const unsigned short gz_dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577
};
static const unsigned char gz_dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};
static const unsigned char gz_clen_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static int gz_load_fixed(struct gz_out *o) {
    static short fcount[16], fsym[288], fdcount[16], fdsym[32];
    static int ready;
    int i;
    if (!ready) {
        unsigned char fl[288], fd[32];
        for (i = 0; i < 144; i++) fl[i] = 8;
        for (; i < 256; i++) fl[i] = 9;
        for (; i < 280; i++) fl[i] = 7;
        for (; i < 288; i++) fl[i] = 8;
        for (i = 0; i < 32; i++) fd[i] = 5;
        if (gz_construct(fcount, fsym, fl, 288) < 0) return -1;
        if (gz_construct(fdcount, fdsym, fd, 32) < 0) return -1;
        ready = 1;
    }
    memcpy(o->lcount, fcount, sizeof o->lcount);
    memcpy(o->lsym, fsym, sizeof o->lsym);
    memcpy(o->dcount, fdcount, sizeof o->dcount);
    memcpy(o->dsym, fdsym, sizeof o->dsym);
    return 0;
}

static int gz_u32(struct gz_br *b, unsigned *out);

/* Конец блока deflate. У последнего блока следом идут CRC и длина. */
static int gz_end_block(struct gz_br *b, struct gz_out *o) {
    if (o->final_block) {
        unsigned want, isize;
        b->acc = 0;
        b->bits = 0;
        if (gz_u32(b, &want) < 0 || gz_u32(b, &isize) < 0) { gz_fail("unexpected end of file"); return -1; }
        if (want != ~o->crc) { gz_fail("crc error"); return -1; }
        if (isize != o->member) { gz_fail("length error"); return -1; }
        o->phase = PH_MEMBER;
        o->final_block = 0;
        return 0;
    }
    o->phase = PH_BLOCK;
    return 0;
}

static int gz_start_block(struct gz_br *b, struct gz_out *o) {
    int type, len, nlen, sym, i;
    o->final_block = gz_getbits(b, 1);
    type = gz_getbits(b, 2);
    if (b->err) { gz_fail("unexpected end of file"); return -1; }
    if (type == 3) { gz_fail("invalid compressed data"); return -1; }
    if (type == 0) {
        gz_align(b);
        b->acc = 0;
        b->bits = 0;
        len = gz_getbits(b, 16);
        nlen = gz_getbits(b, 16);
        if (b->err) { gz_fail("unexpected end of file"); return -1; }
        if (nlen != (len ^ 0xFFFF)) { gz_fail("invalid compressed data"); return -1; }
        b->acc = 0;
        b->bits = 0;
        o->stored_left = len;
        o->phase = PH_STORED;
        return 0;
    }
    if (type == 1) {
        if (gz_load_fixed(o) < 0) { gz_fail("invalid compressed data"); return -1; }
    } else {
        int nlit, ndist, nclen, nlen_tot;
        unsigned char llen[288], dlen[32], clen[19];
        short ccount[16], csym[19];
        memset(clen, 0, sizeof clen);
        memset(llen, 0, sizeof llen);
        memset(dlen, 0, sizeof dlen);
        nlit = gz_getbits(b, 5) + 257;
        ndist = gz_getbits(b, 5) + 1;
        nclen = gz_getbits(b, 4) + 4;
        if (b->err || nlit > 286 || ndist > 32 || nclen > 19) {
            gz_fail(b->err ? "unexpected end of file" : "invalid compressed data");
            return -1;
        }
        for (i = 0; i < nclen; i++) clen[gz_clen_order[i]] = (unsigned char)gz_getbits(b, 3);
        if (b->err || gz_construct(ccount, csym, clen, 19) < 0) {
            gz_fail(b->err ? "unexpected end of file" : "invalid compressed data");
            return -1;
        }
        nlen_tot = nlit + ndist;
        i = 0;
        while (i < nlen_tot) {
            int rep, what;
            sym = gz_decode(b, ccount, csym);
            if (sym < 0 || sym > 18) { gz_fail(b->err ? "unexpected end of file" : "invalid compressed data"); return -1; }
            if (sym < 16) {
                if (i < nlit) llen[i] = (unsigned char)sym;
                else dlen[i - nlit] = (unsigned char)sym;
                i++;
                continue;
            }
            if (sym == 16) {
                if (i == 0) { gz_fail("invalid compressed data"); return -1; }
                what = i <= nlit ? llen[i - 1] : dlen[i - nlit - 1];
                rep = 3 + gz_getbits(b, 2);
            } else if (sym == 17) {
                what = 0;
                rep = 3 + gz_getbits(b, 3);
            } else {
                what = 0;
                rep = 11 + gz_getbits(b, 7);
            }
            if (b->err || i + rep > nlen_tot) {
                gz_fail(b->err ? "unexpected end of file" : "invalid compressed data");
                return -1;
            }
            while (rep--) {
                if (i < nlit) llen[i] = (unsigned char)what;
                else dlen[i - nlit] = (unsigned char)what;
                i++;
            }
        }
        if (gz_construct(o->lcount, o->lsym, llen, nlit) < 0) { gz_fail("invalid compressed data"); return -1; }
        if (gz_construct(o->dcount, o->dsym, dlen, ndist) < 0) { gz_fail("invalid compressed data"); return -1; }
    }
    o->phase = PH_CODES;
    return 0;
}

static int gz_u32(struct gz_br *b, unsigned *out) {
    int i, c[4];
    for (i = 0; i < 4; i++) {
        c[i] = gz_byte(b, NULL);
        if (c[i] < 0) return -1;
    }
    *out = (unsigned)c[0] | ((unsigned)c[1] << 8) | ((unsigned)c[2] << 16) | ((unsigned)c[3] << 24);
    return 0;
}

/* Заголовок очередного члена gzip. 0 — можно читать блоки, finished — поток кончился. */
static int gz_open_member(struct gz_br *b, struct gz_out *o) {
    int eof = 0, magic, method, flg, skip, xlen;
    magic = gz_byte(b, &eof);
    if (eof) {
        if (!o->members) { gz_fail("not in gzip format"); return -1; }
        o->finished = 1;
        return 0;
    }
    if (magic < 0) { gz_fail("unexpected end of file"); return -1; }
    if (magic != 0x1f) {
        if (!o->members) { gz_fail("not in gzip format"); return -1; }
        o->finished = 1;
        return 0;
    }
    magic = gz_byte(b, NULL);
    if (magic != 0x8b) {
        gz_fail(o->members ? "invalid compressed data" : "not in gzip format");
        return -1;
    }
    method = gz_byte(b, NULL);
    flg = gz_byte(b, NULL);
    if (method < 0 || flg < 0) { gz_fail("unexpected end of file"); return -1; }
    if (method != 8) { gz_fail("unknown compression method"); return -1; }
    if (flg & 0xE0) { gz_fail("reserved gzip flags"); return -1; }
    for (skip = 0; skip < 6; skip++) if (gz_byte(b, NULL) < 0) { gz_fail("unexpected end of file"); return -1; }
    if (flg & 4) {
        int lo = gz_byte(b, NULL), hi = gz_byte(b, NULL);
        if (lo < 0 || hi < 0) { gz_fail("unexpected end of file"); return -1; }
        xlen = lo | (hi << 8);
        while (xlen--) if (gz_byte(b, NULL) < 0) { gz_fail("unexpected end of file"); return -1; }
    }
    if (flg & 8) while ((skip = gz_byte(b, NULL)) > 0) {}
    if (skip < 0) { gz_fail("unexpected end of file"); return -1; }
    if (flg & 16) while ((skip = gz_byte(b, NULL)) > 0) {}
    if (skip < 0) { gz_fail("unexpected end of file"); return -1; }
    if (flg & 2) {
        if (gz_byte(b, NULL) < 0 || gz_byte(b, NULL) < 0) { gz_fail("unexpected end of file"); return -1; }
    }
    o->member = 0;
    o->crc = 0xFFFFFFFFu;
    o->phase = PH_BLOCK;
    o->members++;
    return 0;
}

/* Один шаг: 1 — в кольцо положен байт, 0 — состояние сдвинулось, 2 — поток кончился. */
static int gz_step(struct gz_br *b, struct gz_out *o) {
    if (o->phase == PH_MEMBER) {
        if (gz_open_member(b, o) < 0) return -1;
        return o->finished ? 2 : 0;
    }
    if (o->phase == PH_BLOCK) return gz_start_block(b, o);
    if (o->phase == PH_STORED) {
        int c;
        if (o->stored_left == 0) return gz_end_block(b, o);
        c = gz_byte(b, NULL);
        if (c < 0) { gz_fail("unexpected end of file"); return -1; }
        gz_put(o, (unsigned char)c);
        o->stored_left--;
        if (o->stored_left == 0 && gz_end_block(b, o) < 0) return -1;
        return 1;
    }
    if (o->phase == PH_MATCH) {
        unsigned char c = gz_win[(o->member - o->match_dist) & (GZ_WIN - 1)];
        gz_put(o, c);
        if (--o->match_left == 0) o->phase = PH_CODES;
        return 1;
    }
    {
        int sym = gz_decode(b, o->lcount, o->lsym);
        int length, ds, dist;
        if (sym < 0 || sym > 285) {
            gz_fail(b->err ? "unexpected end of file" : "invalid compressed data");
            return -1;
        }
        if (sym < 256) {
            gz_put(o, (unsigned char)sym);
            return 1;
        }
        if (sym == 256) return gz_end_block(b, o);
        length = gz_len_base[sym - 257] + gz_getbits(b, gz_len_extra[sym - 257]);
        ds = gz_decode(b, o->dcount, o->dsym);
        if (ds < 0 || ds > 29 || b->err) {
            gz_fail(b->err ? "unexpected end of file" : "invalid compressed data");
            return -1;
        }
        dist = gz_dist_base[ds] + gz_getbits(b, gz_dist_extra[ds]);
        if (b->err || dist < 1 || dist > GZ_WIN || (unsigned)dist > o->member || length < 3) {
            gz_fail(b->err ? "unexpected end of file" : "invalid compressed data");
            return -1;
        }
        o->match_left = length;
        o->match_dist = (unsigned)dist;
        o->phase = PH_MATCH;
        return 0;
    }
}

/* Доливает кольцо, пока в нём есть место. Ошибка оставляет уже разжатые байты на месте. */
static int gz_pump(struct gz_br *b, struct gz_out *o) {
    if (o->err) return -1;
    while (o->rlen < GZ_RING && !o->finished) {
        int r = gz_step(b, o);
        if (r < 0) { o->err = 1; return -1; }
        if (r == 2) return 0;
    }
    return 0;
}

/* ------------------------------------------------------- чтение архива */

struct src {
    int fd;
    struct gz_br *gz;
    struct gz_out *gzout;
    const unsigned char *mem;
    unsigned memlen, mempos;
    unsigned char peek[512];
    int peek_len, peek_off;
};

static void short_archive(void) {
    if (gz_msg) {
        fprintf(stderr, "tar: %s: %s\n", opt_file ? opt_file : "archive", gz_msg);
        gz_msg_printed = 1;
    } else {
        fprintf(stderr, "tar: unexpected end of file\n");
    }
}

static int src_read(struct src *s, void *buf, int n) {
    unsigned char *p = buf;
    int got = 0;
    while (got < n) {
        int r, take;
        if (s->peek_off < s->peek_len) {
            take = n - got;
            if (take > s->peek_len - s->peek_off) take = s->peek_len - s->peek_off;
            memcpy(p + got, s->peek + s->peek_off, (size_t)take);
            s->peek_off += take;
            got += take;
            continue;
        }
        if (s->mem) {
            unsigned left = s->memlen - s->mempos;
            take = n - got;
            if ((unsigned)take > left) take = (int)left;
            if (!take) break;
            memcpy(p + got, s->mem + s->mempos, (size_t)take);
            s->mempos += (unsigned)take;
            got += take;
            continue;
        }
        if (s->gz) {
            struct gz_out *o = s->gzout;
            unsigned pos, first;
            if (!o->rlen) {
                if (gz_pump(s->gz, o) < 0) return -1;
                if (!o->rlen) break;
            }
            take = n - got;
            if ((unsigned)take > o->rlen) take = (int)o->rlen;
            pos = o->rpos & (GZ_RING - 1);
            first = GZ_RING - pos;
            if ((unsigned)take < first) first = (unsigned)take;
            memcpy(p + got, o->ring + pos, first);
            if ((unsigned)take > first) memcpy(p + got + first, o->ring, (size_t)((unsigned)take - first));
            o->rpos += (unsigned)take;
            o->rlen -= (unsigned)take;
            got += take;
            continue;
        }
        r = read(s->fd, p + got, (size_t)(n - got));
        if (r < 0) return -1;
        if (r == 0) break;
        got += r;
    }
    return got;
}

static int src_exact(struct src *s, void *buf, int n) {
    int g = src_read(s, buf, n);
    if (g < 0) return -1;
    return g == n ? 0 : -2;
}

/* ------------------------------------------------------- заголовок tar */

static int parse_u32(const char *p, int n, unsigned *out) {
    unsigned v = 0;
    int i = 0;
    if ((unsigned char)p[0] & 0x80) {
        v = (unsigned char)p[0] & 0x7f;
        for (i = 1; i < n; i++) {
            if (v > 0x00ffffffu) return -1;
            v = (v << 8) | (unsigned char)p[i];
        }
        *out = v;
        return 0;
    }
    while (i < n && (p[i] == ' ' || p[i] == 0)) i++;
    for (; i < n && p[i] >= '0' && p[i] <= '7'; i++) {
        if (v > 0x1fffffffu) return -1;
        v = (v << 3) | (unsigned)(p[i] - '0');
    }
    *out = v;
    return 0;
}

static void field_copy(char *dst, int cap, const char *src, int n) {
    int i = 0;
    while (i < n && i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static int header_sum(const unsigned char *h, unsigned *sum, int *zero) {
    unsigned s = 0;
    int i, z = 1;
    for (i = 0; i < 512; i++) {
        unsigned char c = h[i];
        if (c) z = 0;
        if (i >= 148 && i < 156) c = ' ';
        s += c;
    }
    *sum = s;
    *zero = z;
    return 0;
}

static int header_ok(const unsigned char *h) {
    unsigned sum, stored;
    int zero;
    header_sum(h, &sum, &zero);
    if (zero) return 0;
    if (parse_u32((const char *)h + 148, 8, &stored) < 0) return -1;
    return stored == sum ? 1 : -1;
}

struct saved {
    char path[NAME_CAP];
    char link[NAME_CAP];
    int has_path, has_link, has_size;
    unsigned size;
};

static void saved_clear(struct saved *s) { memset(s, 0, sizeof *s); }

static int key_is(const char *k, int kn, const char *lit) {
    int n = (int)strlen(lit);
    return kn == n && memcmp(k, lit, (size_t)n) == 0;
}

static int pax_parse(const char *body, unsigned n, struct saved *s) {
    unsigned i = 0;
    while (i < n && body[i]) {
        unsigned start = i, rec = 0, end, ks, vs;
        int kn;
        while (i < n && body[i] >= '0' && body[i] <= '9') {
            if (rec > 10000000u) return -1;
            rec = rec * 10u + (unsigned)(body[i] - '0');
            i++;
        }
        if (i >= n || body[i] != ' ' || rec < 3 || start + rec > n) return -1;
        end = start + rec;
        i++;
        ks = i;
        while (i < end && body[i] != '=') i++;
        if (i >= end || body[i] != '=') return -1;
        kn = (int)(i - ks);
        vs = ++i;
        if (end == 0 || body[end - 1] != '\n') return -1;
        if (key_is(body + ks, kn, "path") || key_is(body + ks, kn, "linkpath")) {
            int len = (int)(end - 1 - vs);
            char *dst = key_is(body + ks, kn, "path") ? s->path : s->link;
            if (len <= 0 || len >= NAME_CAP) return -1;
            memcpy(dst, body + vs, (size_t)len);
            dst[len] = 0;
            if (dst == s->path) s->has_path = 1;
            else s->has_link = 1;
        } else if (key_is(body + ks, kn, "size")) {
            unsigned v = 0;
            unsigned j;
            for (j = vs; j + 1 < end; j++) {
                if (body[j] < '0' || body[j] > '9') return -1;
                if (v > 0x19999999u) return -1;
                v = v * 10u + (unsigned)(body[j] - '0');
            }
            s->size = v;
            s->has_size = 1;
        }
        i = end;
    }
    return 0;
}

/* 0 — путь готов, 1 — после strip ничего не осталось, -1 — «..» или слишком длинно. */
static int place_name(char *dst, int cap, const char *src, int strip, int absolute) {
    const char *p = src;
    int comp = 0, o = 0, lead = src[0] == '/';
    while (*p == '/') p++;
    while (comp < strip && *p) {
        while (*p && *p != '/') p++;
        while (*p == '/') p++;
        comp++;
    }
    if (!*p) return 1;
    if (absolute && lead) {
        if (cap < 2) return -1;
        dst[o++] = '/';
    }
    while (*p) {
        const char *s = p;
        int n = 0;
        while (p[n] && p[n] != '/') n++;
        if (!(n == 1 && s[0] == '.')) {
            if (n == 2 && s[0] == '.' && s[1] == '.') return -1;
            if (o && dst[o - 1] != '/') {
                if (o + 1 >= cap) return -1;
                dst[o++] = '/';
            }
            if (o + n >= cap) return -1;
            memcpy(dst + o, s, (size_t)n);
            o += n;
        }
        p += n;
        while (*p == '/') p++;
    }
    if (!o || (o == 1 && dst[0] == '/')) return 1;
    dst[o] = 0;
    return 0;
}

static int name_wanted(const char *raw) {
    int i;
    const char *a = raw;
    if (!opt_nmem) return 1;
    while (a[0] == '.' && a[1] == '/') a += 2;
    for (i = 0; i < opt_nmem; i++) {
        const char *w = opt_members[i];
        int n;
        while (w[0] == '.' && w[1] == '/') w += 2;
        n = (int)strlen(w);
        while (n > 1 && w[n - 1] == '/') n--;
        if (strncmp(a, w, (size_t)n) == 0 && (a[n] == 0 || a[n] == '/')) return 1;
    }
    return 0;
}

static int err_code(int r) { return r == -1 ? errno : -r; }

static void warn_sys(const char *path, int r) {
    fprintf(stderr, "tar: %s: %s\n", path, strerror(err_code(r)));
}

static int is_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    return S_ISDIR(st.st_mode);
}

/* New directories stay private during extraction; apply modes inside-out. */
struct directory_mode {char *path;unsigned mode;struct directory_mode *next;};
static struct directory_mode *directory_modes;
static unsigned extraction_mask;
static void directory_header_mode(const char *path,unsigned mode) {
    for(struct directory_mode *d=directory_modes;d;d=d->next)
        if(!strcmp(d->path,path)){d->mode=mode&0777&~extraction_mask;return;}
}
static int finish_directory_modes(int success) {
    int rc=0;while(directory_modes){struct directory_mode *d=directory_modes;directory_modes=d->next;
        if(success && chmod(d->path,d->mode)<0){warn_sys(d->path,-1);rc=1;}
        free(d->path);free(d);
    }return rc;
}
static int mkdir_one(const char *path) {
    int r=mkdir(path,0700);
    if(r>=0){struct directory_mode *d=malloc(sizeof(*d));if(!d)return -1;
        d->path=malloc(strlen(path)+1);if(!d->path){free(d);return -1;}strcpy(d->path,path);
        d->mode=0777&~extraction_mask;d->next=directory_modes;directory_modes=d;return 0;
    }
    if (r == -1) r = -errno;
    if (r == -EEXIST && is_dir(path)) return 0;
    warn_sys(path, r);
    return -1;
}

static int mkdir_p(const char *path) {
    char buf[NAME_CAP];
    int n = (int)strlen(path), i;
    if (n <= 0 || n >= NAME_CAP) return -1;
    memcpy(buf, path, (size_t)n + 1);
    i = buf[0] == '/' ? 1 : 0;
    for (; buf[i]; i++) {
        if (buf[i] != '/') continue;
        buf[i] = 0;
        if (buf[0] && mkdir_one(buf) < 0) return -1;
        buf[i] = '/';
    }
    return mkdir_one(buf);
}

static int ensure_parent(const char *path) {
    char buf[NAME_CAP];
    char *slash;
    int n = (int)strlen(path);
    if (n >= NAME_CAP) return -1;
    memcpy(buf, path, (size_t)n + 1);
    slash = strrchr(buf, '/');
    if (!slash || slash == buf) return 0;
    *slash = 0;
    return mkdir_p(buf);
}

static int write_full(int fd, const void *buf, unsigned n);

static int copy_file(const char *src, const char *dst) {
    char buf[4096];
    int in, out, r;
    if (ensure_parent(dst) < 0) return -1;
    in = open(src, O_RDONLY);
    if (in < 0) return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) { close(in); return -1; }
    for (;;) {
        r = read(in, buf, sizeof buf);
        if (r < 0) { close(in); close(out); return -1; }
        if (r == 0) break;
        if (write_full(out, buf, (unsigned)r) < 0) { close(in); close(out); return -1; }
    }
    close(in);
    close(out);
    return 0;
}

#define WRITE_CHUNK (32 * 1024)
static unsigned char io_buf[WRITE_CHUNK];

static int skip_bytes(struct src *s, unsigned size) {
    unsigned rest = (size + 511u) & ~511u;
    while (rest) {
        unsigned n = rest > WRITE_CHUNK ? WRITE_CHUNK : rest;
        if (src_exact(s, io_buf, (int)n) < 0) return -1;
        rest -= n;
    }
    return 0;
}

static int write_full(int fd, const void *buf, unsigned n) {
    const unsigned char *p = buf;
    unsigned off = 0;
    while (off < n) {
        int w = write(fd, p + off, n - off);
        if (w < 0) return -1;
        if (w == 0) return -1;
        off += (unsigned)w;
    }
    return 0;
}

/* Данные члена всегда дочитываются до конца блока, чтобы следующий заголовок не съехал.
 * -2 — обрыв архива, -1 — файл не записан. */
static int write_member(struct src *s, const char *path, unsigned size,unsigned mode) {
    int fd = -1, err = 0;
    unsigned rest, filled = 0;
    if (ensure_parent(path) < 0) err = -1;
    else {
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,mode&0777);
        if (fd < 0) err = -1;
    }
    rest = (size + 511u) & ~511u;
    while (rest) {
        unsigned n = rest > WRITE_CHUNK ? WRITE_CHUNK : rest;
        unsigned chunk;
        if (src_exact(s, io_buf, (int)n) < 0) { if (fd >= 0) close(fd); return -2; }
        if (!err && filled < size) {
            chunk = n;
            if (filled + chunk > size) chunk = size - filled;
            if (write_full(fd, io_buf, chunk) < 0) err = -1;
        }
        filled += n;
        rest -= n;
    }
    if (fd >= 0) close(fd);
    return err;
}

static int exists_keep(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

static void show(char type, unsigned mode, unsigned size, const char *name, const char *link) {
    if (!opt_verbose && !opt_list) return;
    if (!opt_verbose) {
        printf("%s\n", name);
        return;
    }
    if (opt_list) {
        char m[11];
        const char *rwx = "rwxrwxrwx";
        int i;
        m[0] = type;
        for (i = 0; i < 9; i++) m[1 + i] = (mode & (1u << (8 - i))) ? rwx[i] : '-';
        m[10] = 0;
        printf("%s %u %s", m, size, name);
        if (link && type == 'l') printf(" -> %s", link);
        printf("\n");
        return;
    }
    if (link && type == 'l') printf("%s -> %s\n", name, link);
    else printf("%s\n", name);
}

static int take_override(struct src *s, unsigned size, char *dst, int cap, int nul) {
    unsigned char *buf;
    unsigned ncopy;
    int rc;
    if (size == 0 || size >= (unsigned)cap) {
        fprintf(stderr, "tar: name is too long\n");
        return skip_bytes(s, size) < 0 ? -1 : -2;
    }
    buf = malloc(size + 1);
    if (!buf) return -1;
    rc = 0;
    {
        unsigned rest = (size + 511u) & ~511u, filled = 0;
        while (rest) {
            unsigned char blk[512];
            if (src_exact(s, blk, 512) < 0) { free(buf); return -1; }
            if (filled < size) {
                unsigned n = 512;
                if (filled + n > size) n = size - filled;
                memcpy(buf + filled, blk, n);
            }
            filled += 512;
            rest -= 512;
        }
    }
    ncopy = size;
    if (nul && ncopy && buf[ncopy - 1] == 0) ncopy--;
    if (ncopy >= (unsigned)cap) { free(buf); fprintf(stderr, "tar: name is too long\n"); return -2; }
    memcpy(dst, buf, ncopy);
    dst[ncopy] = 0;
    free(buf);
    return rc;
}

static int run_archive(struct src *s) {
    struct saved global, local;
    char longname[NAME_CAP], longlink[NAME_CAP];
    int have_longname = 0, have_longlink = 0, rc = 0;
    saved_clear(&global);
    saved_clear(&local);
    for (;;) {
        unsigned char h[512];
        unsigned sum_unused, size, mode;
        int zero = 0, ok, kind, pr;
        char name[NAME_CAP], link[NAME_CAP], dest[NAME_CAP], raw[NAME_CAP];
        const char *np, *lp;
        (void)sum_unused;
        if (src_exact(s, h, 512) < 0) {
            short_archive();
            return 1;
        }
        header_sum(h, &sum_unused, &zero);
        if (zero) break;
        ok = header_ok(h);
        if (ok < 0) { fprintf(stderr, "tar: checksum error\n"); return 1; }
        kind = h[156];
        if (parse_u32((const char *)h + 124, 12, &size) < 0) {
            fprintf(stderr, "tar: file is too large\n");
            return 1;
        }
        if (kind == 'L' || kind == 'K') {
            char *dst = kind == 'L' ? longname : longlink;
            int r = take_override(s, size, dst, NAME_CAP, 1);
            if (r == -1) return 1;
            if (r == 0) {
                if (kind == 'L') have_longname = 1;
                else have_longlink = 1;
            }
            continue;
        }
        if (kind == 'x' || kind == 'g') {
            unsigned char *body;
            unsigned rest, filled = 0;
            if (size > PAX_MAX) {
                fprintf(stderr, "tar: pax header is too large\n");
                return 1;
            }
            body = malloc(size + 1);
            if (!body) return 1;
            rest = (size + 511u) & ~511u;
            while (rest) {
                unsigned char blk[512];
                if (src_exact(s, blk, 512) < 0) { free(body); short_archive(); return 1; }
                if (filled < size) {
                    unsigned n = filled + 512 > size ? size - filled : 512;
                    memcpy(body + filled, blk, n);
                }
                filled += 512;
                rest -= 512;
            }
            body[size] = 0;
            if (pax_parse((char *)body, size, kind == 'g' ? &global : &local) < 0) {
                free(body);
                fprintf(stderr, "tar: malformed pax header\n");
                return 1;
            }
            free(body);
            continue;
        }
        field_copy(name, (int)sizeof name, (const char *)h, 100);
        field_copy(link, (int)sizeof link, (const char *)h + 157, 100);
        if (memcmp(h + 257, "ustar", 5) == 0) {
            char prefix[156];
            field_copy(prefix, (int)sizeof prefix, (const char *)h + 345, 155);
            if (prefix[0] && !have_longname && !local.has_path && !global.has_path) {
                if (strlen(prefix) + strlen(name) + 2 > sizeof name) {
                    fprintf(stderr, "tar: name is too long\n");
                    rc = 1;
                    if (skip_bytes(s, size) < 0) return 1;
                    continue;
                }
                {
                    char joined[NAME_CAP];
                    snprintf(joined, sizeof joined, "%s/%s", prefix, name);
                    memcpy(name, joined, strlen(joined) + 1);
                }
            }
        }
        np = local.has_path ? local.path : global.has_path ? global.path : have_longname ? longname : name;
        lp = local.has_link ? local.link : global.has_link ? global.link : have_longlink ? longlink : link;
        if (local.has_size) size = local.size;
        mode = 0;
        parse_u32((const char *)h + 100, 8, &mode);
        memcpy(raw, np, strlen(np) + 1);
        have_longname = have_longlink = 0;
        saved_clear(&local);
        if (!name_wanted(raw)) {
            if (skip_bytes(s, size) < 0) { short_archive(); return 1; }
            continue;
        }
        pr = place_name(dest, NAME_CAP, raw, opt_strip, opt_absolute);
        if (pr < 0) {
            fprintf(stderr, "tar: %s: member name contains '..' or is too long\n", raw);
            rc = 1;
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (pr > 0) {
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '5') {
            if (!opt_list) {
                if (mkdir_p(dest) < 0) rc = 1;
                else directory_header_mode(dest,mode);
            }
            show('d', mode, size, dest, NULL);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '2') {
            char target[NAME_CAP];
            if (strlen(lp) >= sizeof target) {
                fprintf(stderr, "tar: %s: link target is too long\n", dest);
                rc = 1;
                if (skip_bytes(s, size) < 0) return 1;
                continue;
            }
            memcpy(target, lp, strlen(lp) + 1);
            if (!opt_list) {
                if (!(opt_keep && exists_keep(dest))) {
                    if (ensure_parent(dest) < 0) rc = 1;
                    else {
                        if (exists_keep(dest)) unlink(dest);
                        if (symlink(target, dest) < 0) { warn_sys(dest, -1); rc = 1; }
                    }
                }
            }
            show('l', mode, size, dest, target);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '1') {
            char target[NAME_CAP];
            int tr = place_name(target, NAME_CAP, lp, opt_strip, opt_absolute);
            if (!opt_list && !(opt_keep && exists_keep(dest))) {
                if (tr != 0 || copy_file(target, dest) < 0) {
                    fprintf(stderr, "tar: %s: skipping hard link to %s\n", dest, lp);
                    rc = 1;
                }
            }
            show('h', mode, size, dest, lp);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind == '3' || kind == '4' || kind == '6') {
            fprintf(stderr, "tar: %s: skipping special file\n", dest);
            show(kind == '6' ? 'p' : 'c', mode, size, dest, NULL);
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (kind != '0' && kind != 0 && kind != '7') {
            fprintf(stderr, "tar: %s: unknown file type '%c'\n", dest, kind ? kind : '0');
            rc = 1;
            if (skip_bytes(s, size) < 0) return 1;
            continue;
        }
        if (!opt_list) {
            if (opt_keep && exists_keep(dest)) {
                if (skip_bytes(s, size) < 0) return 1;
                continue;
            }
            if (exists_keep(dest)) unlink(dest);
            show('-', mode, size, dest, NULL);
            {
                int wr = write_member(s,dest,size,mode);
                if (wr == -2) { short_archive(); return 1; }
                if (wr < 0) { warn_sys(dest, -1); rc = 1; }
            }
        } else {
            show('-', mode, size, dest, NULL);
            if (skip_bytes(s, size) < 0) return 1;
        }
    }
    return rc;
}

/* ------------------------------------------------------- флаги GNU tar */

static void usage(void) {
    fprintf(stderr,
            "usage: tar -x|-t [-z] [-v] [-k] [-P] [-C DIR] [-f ARCHIVE]\n"
            "           [--strip-components=N] [--gzip] [MEMBER...]\n"
            "       tar xf ARCHIVE, tar xzf ARCHIVE.tgz\n");
}

static int need_op;
static int bad_op;

static int set_op(int list) {
    if (need_op && opt_list != list) { bad_op = 1; return -1; }
    need_op = 1;
    opt_list = list;
    return 0;
}

static int apply_short(int c, const char *arg) {
    if (c == 'x') return set_op(0);
    if (c == 't') return set_op(1);
    if (c == 'c' || c == 'r' || c == 'u' || c == 'd' || c == 'A') {
        fprintf(stderr, "tar: operation '%c' is not supported\n", c);
        return -1;
    }
    if (c == 'f') { if (!arg) return -1; opt_file = arg; return 0; }
    if (c == 'C') { if (!arg) return -1; opt_dir = arg; return 0; }
    if (c == 'z') { opt_gzip = 1; return 0; }
    if (c == 'v') { opt_verbose++; return 0; }
    if (c == 'k') { opt_keep = 1; return 0; }
    if (c == 'P') { opt_absolute = 1; return 0; }
    if (c == 'a' || c == 'm' || c == 'p' || c == 'o' || c == 'h') return 0;
    if (c == 'j' || c == 'J') {
        fprintf(stderr, "tar: option '%c' is not supported\n", c);
        return -1;
    }
    fprintf(stderr, "tar: unknown option '%c'\n", c);
    return -1;
}

static int parse_cluster(const char *s, int *i, int argc, char **argv) {
    int k = 0;
    while (s[k]) {
        char c = s[k++];
        const char *arg = NULL;
        if (c == 'f' || c == 'C') {
            if (s[k]) { arg = s + k; k += (int)strlen(s + k); }
            else if (*i + 1 < argc) arg = argv[++*i];
            else { fprintf(stderr, "tar: option '%c' requires an argument\n", c); return -1; }
        }
        if (apply_short(c, arg) < 0) return -1;
    }
    return 0;
}

static int starts(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }

static int parse_long(const char *s, int *i, int argc, char **argv) {
    const char *val = NULL;
    const char *name = s;
    if (strcmp(s, "--") == 0) return 1;
    if (starts(s, "--") && strchr(s, '=')) {
        /* значение после «=» */
    }
    if (!strcmp(s, "--help")) { usage(); exit(0); }
    if (!strcmp(s, "--version")) { printf("tar (MyOS)\n"); exit(0); }
    if (!strcmp(s, "--extract") || !strcmp(s, "--get")) return set_op(0);
    if (!strcmp(s, "--list")) return set_op(1);
    if (!strcmp(s, "--create") || !strcmp(s, "--append") || !strcmp(s, "--update") ||
        !strcmp(s, "--diff") || !strcmp(s, "--compare") || !strcmp(s, "--concatenate") ||
        !strcmp(s, "--catenate")) {
        fprintf(stderr, "tar: %s is not supported\n", s);
        return -1;
    }
    if (!strcmp(s, "--gzip") || !strcmp(s, "--gunzip") || !strcmp(s, "--ungzip")) { opt_gzip = 1; return 0; }
    if (!strcmp(s, "--verbose")) { opt_verbose++; return 0; }
    if (!strcmp(s, "--keep-old-files") || !strcmp(s, "--skip-old-files")) { opt_keep = 1; return 0; }
    if (!strcmp(s, "--absolute-names")) { opt_absolute = 1; return 0; }
    if (!strcmp(s, "--overwrite")) { opt_keep = 0; return 0; }
    if (!strcmp(s, "--auto-compress") || !strcmp(s, "--touch") ||
        !strcmp(s, "--preserve-permissions") || !strcmp(s, "--same-permissions") ||
        !strcmp(s, "--no-same-owner")) return 0;
    if (!strcmp(s, "--bzip2") || !strcmp(s, "--xz")) {
        fprintf(stderr, "tar: %s is not supported\n", s);
        return -1;
    }
    name = s;
    if (starts(s, "--file")) name = "--file";
    else if (starts(s, "--directory")) name = "--directory";
    else if (starts(s, "--strip-components")) name = "--strip-components";
    else { fprintf(stderr, "tar: unknown option '%s'\n", s); return -1; }
    if (s[strlen(name)] == '=') val = s + strlen(name) + 1;
    else if (s[strlen(name)] == 0) {
        if (*i + 1 >= argc) { fprintf(stderr, "tar: %s requires an argument\n", name); return -1; }
        val = argv[++*i];
    } else { fprintf(stderr, "tar: unknown option '%s'\n", s); return -1; }
    if (!strcmp(name, "--file")) { opt_file = val; return 0; }
    if (!strcmp(name, "--directory")) { opt_dir = val; return 0; }
    opt_strip = 0;
    if (!val || !*val) { fprintf(stderr, "tar: --strip-components requires a number\n"); return -1; }
    while (*val) {
        if (*val < '0' || *val > '9' || opt_strip > 100000) {
            fprintf(stderr, "tar: --strip-components requires a number\n");
            return -1;
        }
        opt_strip = opt_strip * 10 + (*val - '0');
        val++;
    }
    return 0;
}

static int old_style(const char *s) {
    int i, op = 0;
    if (!s || !s[0]) return 0;
    for (i = 0; s[i]; i++) {
        char c = s[i];
        if (c == 'x' || c == 't' || c == 'c' || c == 'r' || c == 'u' || c == 'd' || c == 'A') op = 1;
        else if (!strchr("zvkaPmpohfCjJ", c)) return 0;
    }
    return op;
}

int main(int argc, char **argv) {
    int i = 1, fd = -1, nfirst, rc;
    unsigned char first[512];
    struct src src;
    struct gz_br zin;
    static struct gz_out gz_stream;
    if (argc > 1 && argv[1][0] != '-' && old_style(argv[1])) {
        if (parse_cluster(argv[1], &i, argc, argv) < 0) return 2;
        i++;
    }
    for (; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) {
            int r;
            if (argv[i][1] == '-') {
                if (!argv[i][2]) { i++; break; }
                r = parse_long(argv[i], &i, argc, argv);
                if (r < 0) return 2;
                continue;
            }
            if (parse_cluster(argv[i] + 1, &i, argc, argv) < 0) return 2;
            continue;
        }
        break;
    }
    if (bad_op) { fprintf(stderr, "tar: you may not specify more than one operation\n"); return 2; }
    if (!need_op) { usage(); return 2; }
    if (!opt_file) { fprintf(stderr, "tar: option -f is required\n"); return 2; }
    opt_members = argv + i;
    opt_nmem = argc - i;
    if (!strcmp(opt_file, "-")) fd = 0;
    else {
        fd = open(opt_file, O_RDONLY);
        if (fd < 0) { warn_sys(opt_file, -1); return 2; }
    }
    memset(&src, 0, sizeof src);
    src.fd = fd;
    nfirst = src_read(&src, first, 512);
    if (nfirst < 0) { warn_sys(opt_file, -1); return 2; }
    if (nfirst >= 2 && first[0] == 0x1f && first[1] == 0x8b) opt_gzip = 1;
    else if (opt_gzip) { fprintf(stderr, "tar: %s: not in gzip format\n", opt_file); return 2; }
    if (opt_gzip) {
        gz_crc_init();
        memset(&zin, 0, sizeof zin);
        memset(&gz_stream, 0, sizeof gz_stream);
        zin.fd = fd;
        if (nfirst > (int)sizeof zin.pre) nfirst = (int)sizeof zin.pre;
        if (nfirst > 0) memcpy(zin.pre, first, (size_t)nfirst);
        zin.pre_len = nfirst;
        memset(&src, 0, sizeof src);
        src.gz = &zin;
        src.gzout = &gz_stream;
    } else if (nfirst == 512) {
        memcpy(src.peek, first, 512);
        src.peek_len = 512;
    } else if (nfirst == 0) {
        if (fd > 0) close(fd);
        return 0;
    } else {
        fprintf(stderr, "tar: unexpected end of file\n");
        if (fd > 0) close(fd);
        return 2;
    }
    if (opt_dir) {
        int cr = chdir(opt_dir);
        if (cr < 0) {
            warn_sys(opt_dir, cr);
            if (fd > 0) close(fd);
            return 2;
        }
    }
    extraction_mask=umask(0);umask(extraction_mask);
    rc = run_archive(&src);
    if (src.gz) {
        unsigned char sink[4096];
        int n = 0;
        while (!gz_msg && (n = src_read(&src, sink, (int)sizeof sink)) > 0) {}
        if (gz_msg || n < 0) rc = 1;
    }
    if (gz_msg && !gz_msg_printed) {
        fprintf(stderr, "tar: %s: %s\n", opt_file, gz_msg);
        rc = 1;
    }
    rc |= finish_directory_modes(!rc);
    if (fd > 0) close(fd);
    return rc ? 1 : 0;
}
