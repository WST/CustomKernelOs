/* lzma.c — контейнер .xz и фильтр LZMA2.
 * Кодер кладёт несжатые куски: их принимает и Python, и GNU xz.
 * Декодер обязан читать обычный LZMA, иначе архив с хоста не раскроется.
 * Кусок короче объявленного размера не отдаём: диапазонный декодер не возобновляется с середины. */
#include "myos.h"
#include "stdlib.h"
#include "string.h"
#include "lzma.h"

#define XZ_IN_MAX (8 * 1024 * 1024)
#define XZ_DICT_MAX (64u * 1024u * 1024u)
#define LZMA2_CHUNK_MAX (2 * 1024 * 1024)

struct lzma_internal_s {
    int enc;
    int phase;
    unsigned long long memlimit;
    unsigned flags;
    int check;
    unsigned char *inbuf, *outbuf;
    int in_n, in_cap, in_pos, out_n, out_cap, out_pos;
    int ended, err;
    int nblocks, blocks_cap;
    unsigned long long *unpadded, *usize;
};

static unsigned xz_crc_table[256];
static unsigned long long xz_crc64_table[256];
static int xz_crc_ready;

static void xz_crc_init(void) {
    unsigned i, j;
    if (xz_crc_ready) return;
    for (i = 0; i < 256; i++) {
        unsigned c = i;
        unsigned long long c64 = i;
        for (j = 0; j < 8; j++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
        for (j = 0; j < 8; j++) c64 = (c64 >> 1) ^ (0xC96C5795D7870F42ULL & (0ULL - (c64 & 1ULL)));
        xz_crc_table[i] = c;
        xz_crc64_table[i] = c64;
    }
    xz_crc_ready = 1;
}

static unsigned xz_crc32(unsigned crc, const unsigned char *p, int n) {
    crc = ~crc;
    while (n--) crc = xz_crc_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

/* Та же схема, что у CRC32, полином ECMA-182. GNU xz и Python по умолчанию пишут её, не CRC32. */
static unsigned long long xz_crc64(unsigned long long crc, const unsigned char *p, int n) {
    crc = ~crc;
    while (n--) crc = xz_crc64_table[(crc ^ *p++) & 255] ^ (crc >> 8);
    return ~crc;
}

static int check_size(int check) {
    if (check == LZMA_CHECK_NONE) return 0;
    if (check == LZMA_CHECK_CRC32) return 4;
    if (check == LZMA_CHECK_CRC64) return 8;
    return -1;
}

static void put32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static unsigned get32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned long long get64(const unsigned char *p) {
    return (unsigned long long)get32(p) | ((unsigned long long)get32(p + 4) << 32);
}

static int buf_add(unsigned char **p, int *n, int *cap, const void *src, int sn, int max) {
    unsigned char *np;
    int nc;
    if (sn < 0 || *n > max - sn) return -1;
    if (*n + sn > *cap) {
        nc = *cap ? *cap * 2 : 256;
        while (nc < *n + sn) nc *= 2;
        np = realloc(*p, (size_t)nc);
        if (!np) return -1;
        *p = np;
        *cap = nc;
    }
    if (sn) memcpy(*p + *n, src, (size_t)sn);
    *n += sn;
    return 0;
}

static int vli_put(unsigned char *p, unsigned long long v) {
    int n = 0;
    do {
        unsigned char b = (unsigned char)(v & 0x7f);
        v >>= 7;
        if (v) b |= 0x80;
        p[n++] = b;
    } while (v);
    return n;
}

static int vli_get(const unsigned char *p, int n, unsigned long long *out, int *used) {
    unsigned long long v = 0;
    int i;
    for (i = 0; i < 9 && i < n; i++) {
        v |= (unsigned long long)(p[i] & 0x7f) << (7 * i);
        if ((p[i] & 0x80) == 0) { *out = v; *used = i + 1; return 0; }
        if (i == 8) return -2;
    }
    return -1;
}

/* ------------------------------------------------------- LZMA */

struct rc {
    const unsigned char *buf;
    int len, pos, err;
    unsigned range, code;
};

static void rc_norm(struct rc *rc) {
    if (rc->range >= 0x01000000u) return;
    if (rc->pos >= rc->len) { rc->err = 1; return; }
    rc->range <<= 8;
    rc->code = (rc->code << 8) | rc->buf[rc->pos++];
}

static int rc_init(struct rc *rc) {
    int i;
    rc->range = 0xffffffffu;
    rc->code = 0;
    rc->err = 0;
    if (rc->pos >= rc->len) { rc->err = 1; return -1; }
    rc->pos++;
    for (i = 0; i < 4; i++) {
        if (rc->pos >= rc->len) { rc->err = 1; return -1; }
        rc->code = (rc->code << 8) | rc->buf[rc->pos++];
    }
    return 0;
}

static int rc_bit(struct rc *rc, unsigned short *prob) {
    unsigned bound, p;
    int bit;
    if (rc->err) return -1;
    p = *prob;
    bound = (rc->range >> 11) * p;
    if (rc->code < bound) {
        rc->range = bound;
        *prob = (unsigned short)(p + ((2048 - p) >> 5));
        bit = 0;
    } else {
        rc->range -= bound;
        rc->code -= bound;
        *prob = (unsigned short)(p - (p >> 5));
        bit = 1;
    }
    rc_norm(rc);
    return rc->err ? -1 : bit;
}

static int rc_direct(struct rc *rc, int n, unsigned *out) {
    unsigned v = 0;
    while (n--) {
        if (rc->err) return -1;
        rc->range >>= 1;
        v <<= 1;
        if (rc->code >= rc->range) { rc->code -= rc->range; v |= 1; }
        rc_norm(rc);
    }
    *out = v;
    return rc->err ? -1 : 0;
}

static int rc_tree(struct rc *rc, unsigned short *probs, int bits, unsigned *out) {
    unsigned m = 1;
    int i, b;
    for (i = 0; i < bits; i++) {
        b = rc_bit(rc, &probs[m]);
        if (b < 0) return -1;
        m = (m << 1) | (unsigned)b;
    }
    *out = m - (1u << bits);
    return 0;
}

static int rc_reverse(struct rc *rc, unsigned short *probs, int bits, unsigned *out) {
    unsigned m = 1, symbol = 0;
    int i, b;
    for (i = 0; i < bits; i++) {
        b = rc_bit(rc, &probs[m]);
        if (b < 0) return -1;
        m = (m << 1) | (unsigned)b;
        symbol |= (unsigned)b << i;
    }
    *out = symbol;
    return 0;
}

struct lenprob {
    unsigned short choice, choice2;
    unsigned short low[16][8];
    unsigned short mid[16][8];
    unsigned short high[256];
};

struct lzdec {
    int lc, lp, pb;
    unsigned pb_mask, lp_mask;
    unsigned short is_match[12][16];
    unsigned short is_rep[12], is_rep0[12], is_rep1[12], is_rep2[12];
    unsigned short is_rep0_long[12][16];
    unsigned short pos_slot[4][64];
    unsigned short spec[128];
    unsigned short align[16];
    struct lenprob len, replen;
    unsigned short *lit;
    int lit_n;
    int state;
    unsigned rep0, rep1, rep2, rep3;
    unsigned char *dict;
    unsigned dict_size, dict_pos, written;
    int err;
};

static const unsigned char k_lit[12] = { 0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 4, 5 };
static const unsigned char k_match[12] = { 7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10 };
static const unsigned char k_rep[12] = { 8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11 };
static const unsigned char k_short[12] = { 9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11 };

static void probs_fill(unsigned short *p, int n) {
    int i;
    for (i = 0; i < n; i++) p[i] = 1024;
}

static void len_init(struct lenprob *l) {
    probs_fill(&l->choice, 1);
    probs_fill(&l->choice2, 1);
    probs_fill(&l->low[0][0], 16 * 8);
    probs_fill(&l->mid[0][0], 16 * 8);
    probs_fill(l->high, 256);
}

static int lz_reset(struct lzdec *d, int props) {
    int lc, lp, pb, n;
    if (props < 0 || props > 224) return -1;
    lc = props % 9;
    props /= 9;
    lp = props % 5;
    pb = props / 5;
    if (pb > 4) return -1;
    d->lc = lc;
    d->lp = lp;
    d->pb = pb;
    d->pb_mask = (1u << pb) - 1;
    d->lp_mask = (1u << lp) - 1;
    n = 0x300 << (lc + lp);
    if (d->lit_n != n) {
        unsigned short *lit = malloc((size_t)n * sizeof(unsigned short));
        if (!lit) return -2;
        free(d->lit);
        d->lit = lit;
        d->lit_n = n;
    }
    probs_fill(&d->is_match[0][0], 12 * 16);
    probs_fill(d->is_rep, 12);
    probs_fill(d->is_rep0, 12);
    probs_fill(d->is_rep1, 12);
    probs_fill(d->is_rep2, 12);
    probs_fill(&d->is_rep0_long[0][0], 12 * 16);
    probs_fill(&d->pos_slot[0][0], 4 * 64);
    probs_fill(d->spec, 128);
    probs_fill(d->align, 16);
    len_init(&d->len);
    len_init(&d->replen);
    probs_fill(d->lit, n);
    d->state = 0;
    d->rep0 = d->rep1 = d->rep2 = d->rep3 = 1;
    return 0;
}

static int len_decode(struct rc *rc, struct lenprob *l, unsigned pos_state, unsigned *out) {
    int b;
    unsigned sym;
    b = rc_bit(rc, &l->choice);
    if (b < 0) return -1;
    if (b == 0) {
        if (rc_tree(rc, l->low[pos_state], 3, &sym) < 0) return -1;
        *out = sym;
        return 0;
    }
    b = rc_bit(rc, &l->choice2);
    if (b < 0) return -1;
    if (b == 0) {
        if (rc_tree(rc, l->mid[pos_state], 3, &sym) < 0) return -1;
        *out = sym + 8;
        return 0;
    }
    if (rc_tree(rc, l->high, 8, &sym) < 0) return -1;
    *out = sym + 16;
    return 0;
}

static unsigned char dict_get(struct lzdec *d, unsigned dist) {
    if (dist == 0 || dist > d->dict_size) { d->err = 1; return 0; }
    if (dist > d->written) return 0;
    if (d->dict_pos >= dist) return d->dict[d->dict_pos - dist];
    return d->dict[d->dict_size - dist + d->dict_pos];
}

static int dict_put(struct lzdec *d, unsigned char *out, unsigned *n, unsigned limit, unsigned char b) {
    if (*n >= limit) return -1;
    d->dict[d->dict_pos] = b;
    if (++d->dict_pos == d->dict_size) d->dict_pos = 0;
    if (d->written < d->dict_size) d->written++;
    out[(*n)++] = b;
    return 0;
}

/* 0 — вышло ровно want байт, -1 порча, -2 память. */
static int lzma_packet(struct lzdec *d, const unsigned char *src, int slen, unsigned char *out, unsigned want) {
    struct rc rc;
    unsigned produced = 0;
    memset(&rc, 0, sizeof rc);
    rc.buf = src;
    rc.len = slen;
    if (rc_init(&rc) < 0) return -1;
    while (produced < want) {
        unsigned pos_state, prev, lit_state, symbol, match_byte, offs, len, len_state, pos_slot, num_direct, dist;
        int bit, b;
        unsigned short *probs;
        if (d->err) return -1;
        pos_state = d->dict_pos & d->pb_mask;
        bit = rc_bit(&rc, &d->is_match[d->state][pos_state]);
        if (bit < 0) return -1;
        if (bit == 0) {
            prev = d->written ? dict_get(d, 1) : 0;
            lit_state = ((d->dict_pos & d->lp_mask) << d->lc) + (prev >> (8 - d->lc));
            probs = d->lit + lit_state * 0x300;
            symbol = 1;
            if (d->state >= 7) {
                /* После первого несовпадения offset обнуляется и дальше берётся обычный литерал.
                 * Иначе индекс остаётся в половине «совпало» и поток разъезжается. */
                match_byte = (unsigned)dict_get(d, d->rep0);
                offs = 0x100;
                do {
                    unsigned match_bit;
                    match_byte <<= 1;
                    match_bit = match_byte & offs;
                    b = rc_bit(&rc, &probs[offs + match_bit + symbol]);
                    if (b < 0) return -1;
                    symbol = (symbol << 1) | (unsigned)b;
                    if (b) offs &= match_bit;
                    else offs &= ~match_bit;
                } while (symbol < 0x100);
            } else {
                do {
                    b = rc_bit(&rc, &probs[symbol]);
                    if (b < 0) return -1;
                    symbol = (symbol << 1) | (unsigned)b;
                } while (symbol < 0x100);
            }
            if (dict_put(d, out, &produced, want, (unsigned char)symbol) < 0) return -1;
            d->state = k_lit[d->state];
            continue;
        }
        bit = rc_bit(&rc, &d->is_rep[d->state]);
        if (bit < 0) return -1;
        if (bit == 0) {
            d->rep3 = d->rep2;
            d->rep2 = d->rep1;
            d->rep1 = d->rep0;
            if (len_decode(&rc, &d->len, pos_state, &len) < 0) return -1;
            d->state = k_match[d->state];
            len_state = len < 4 ? len : 3;
            if (rc_tree(&rc, d->pos_slot[len_state], 6, &pos_slot) < 0) return -1;
            if (pos_slot < 4) dist = pos_slot;
            else {
                num_direct = (pos_slot >> 1) - 1;
                dist = (2u | (pos_slot & 1u)) << num_direct;
                if (pos_slot < 14) {
                    unsigned add = 0;
                    if (rc_reverse(&rc, d->spec + dist - pos_slot - 1, (int)num_direct, &add) < 0) return -1;
                    dist += add;
                } else {
                    unsigned direct = 0, align = 0;
                    if (rc_direct(&rc, (int)num_direct - 4, &direct) < 0) return -1;
                    if (rc_reverse(&rc, d->align, 4, &align) < 0) return -1;
                    dist += (direct << 4) + align;
                    if (dist == 0xffffffffu) return -1;
                }
            }
            d->rep0 = dist + 1;
            len += 2;
        } else {
            bit = rc_bit(&rc, &d->is_rep0[d->state]);
            if (bit < 0) return -1;
            if (bit == 0) {
                bit = rc_bit(&rc, &d->is_rep0_long[d->state][pos_state]);
                if (bit < 0) return -1;
                if (bit == 0) {
                    d->state = k_short[d->state];
                    if (dict_put(d, out, &produced, want, dict_get(d, d->rep0)) < 0) return -1;
                    continue;
                }
            } else {
                bit = rc_bit(&rc, &d->is_rep1[d->state]);
                if (bit < 0) return -1;
                if (bit == 0) dist = d->rep1;
                else {
                    bit = rc_bit(&rc, &d->is_rep2[d->state]);
                    if (bit < 0) return -1;
                    if (bit == 0) dist = d->rep2;
                    else { dist = d->rep3; d->rep3 = d->rep2; }
                    d->rep2 = d->rep1;
                }
                d->rep1 = d->rep0;
                d->rep0 = dist;
            }
            if (len_decode(&rc, &d->replen, pos_state, &len) < 0) return -1;
            d->state = k_rep[d->state];
            len += 2;
        }
        if (produced + len > want) return -1;
        while (len--) {
            if (dict_put(d, out, &produced, want, dict_get(d, d->rep0)) < 0) return -1;
        }
    }
    return 0;
}

static void lz_dict_reset(struct lzdec *d) {
    if (d->dict) memset(d->dict, 0, d->dict_size);
    d->dict_pos = 0;
    d->written = 0;
}

/* Распаковать последовательность кусков LZMA2. 0 ок, -1 порча, -2 память. */
/* exact: длина кусков известна из заголовка блока. Иначе -3 значит «мало байт». */
static int lzma2_unpack(struct lzdec *d, const unsigned char *src, int slen, int exact,
                        unsigned char **outp, int *outn, int *used) {
    int pos = 0;
    unsigned char *out = NULL;
    int on = 0, ocap = 0;
    int have_props = 0;
    *outp = NULL;
    *outn = 0;
    *used = 0;
    while (pos < slen) {
        unsigned control = src[pos++];
        unsigned uncomp, comp;
        int props, rc;
        if (control == 0) { *outp = out; *outn = on; *used = pos; return 0; }
        if (control >= 0xe0 || control == 0x01) {
            lz_dict_reset(d);
            if (control >= 0xe0) have_props = 0;
        } else if (d->dict == NULL) return -1;
        if (control >= 0x80) {
            unsigned char *chunk;
            if (pos + 4 > slen) { free(out); return exact ? -1 : -3; }
            uncomp = ((control & 0x1f) << 16) | ((unsigned)src[pos] << 8) | src[pos + 1];
            uncomp += 1;
            comp = ((unsigned)src[pos + 2] << 8) | src[pos + 3];
            comp += 1;
            pos += 4;
            if (control >= 0xc0) {
                if (pos >= slen) { free(out); return exact ? -1 : -3; }
                props = src[pos++];
                rc = lz_reset(d, props);
                if (rc < 0) { free(out); return rc; }
                have_props = 1;
            } else if (!have_props) { free(out); return -1; }
            else if (control >= 0xa0) {
                d->state = 0;
                d->rep0 = d->rep1 = d->rep2 = d->rep3 = 1;
                probs_fill(&d->is_match[0][0], 12 * 16);
                probs_fill(d->is_rep, 12);
                probs_fill(d->is_rep0, 12);
                probs_fill(d->is_rep1, 12);
                probs_fill(d->is_rep2, 12);
                probs_fill(&d->is_rep0_long[0][0], 12 * 16);
                probs_fill(&d->pos_slot[0][0], 4 * 64);
                probs_fill(d->spec, 128);
                probs_fill(d->align, 16);
                len_init(&d->len);
                len_init(&d->replen);
                probs_fill(d->lit, d->lit_n);
            }
            if (uncomp > LZMA2_CHUNK_MAX) { free(out); return -1; }
            if (pos + (int)comp > slen) { free(out); return exact ? -1 : -3; }
            if (on + (int)uncomp > ocap) {
                int nc = ocap ? ocap * 2 : 256;
                unsigned char *np;
                while (nc < on + (int)uncomp) nc *= 2;
                np = realloc(out, (size_t)nc);
                if (!np) { free(out); return -2; }
                out = np;
                ocap = nc;
            }
            chunk = out + on;
            if (lzma_packet(d, src + pos, (int)comp, chunk, uncomp) < 0) { free(out); return -1; }
            on += (int)uncomp;
            pos += (int)comp;
        } else if (control == 0x01 || control == 0x02) {
            int i;
            if (pos + 2 > slen) { free(out); return exact ? -1 : -3; }
            uncomp = ((unsigned)src[pos] << 8) | src[pos + 1];
            uncomp += 1;
            pos += 2;
            if (pos + (int)uncomp > slen) { free(out); return exact ? -1 : -3; }
            if (on + (int)uncomp > ocap) {
                int nc = ocap ? ocap * 2 : 256;
                unsigned char *np;
                while (nc < on + (int)uncomp) nc *= 2;
                np = realloc(out, (size_t)nc);
                if (!np) { free(out); return -2; }
                out = np;
                ocap = nc;
            }
            {
                unsigned tmp = 0;
                for (i = 0; i < (int)uncomp; i++) {
                    if (dict_put(d, out + on, &tmp, uncomp, src[pos++]) < 0) { free(out); return -1; }
                }
            }
            on += (int)uncomp;
        } else { free(out); return -1; }
    }
    free(out);
    return exact ? -1 : -3;
}

static unsigned dict_from_prop(unsigned char prop, int *err) {
    unsigned dict;
    *err = 0;
    if (prop > 40) { *err = 1; return 0; }
    if (prop == 40) return 0xffffffffu;
    dict = 2u | (prop & 1u);
    dict <<= (prop / 2u) + 11u;
    return dict;
}

/* ------------------------------------------------------- контейнер .xz */

static int remember_block(struct lzma_internal_s *s, unsigned long long unpadded, unsigned long long usize) {
    if (s->nblocks == s->blocks_cap) {
        int nc = s->blocks_cap ? s->blocks_cap * 2 : 4;
        unsigned long long *a = realloc(s->unpadded, (size_t)nc * sizeof *a);
        unsigned long long *b;
        if (!a) return -1;
        s->unpadded = a;
        b = realloc(s->usize, (size_t)nc * sizeof *b);
        if (!b) return -1;
        s->usize = b;
        s->blocks_cap = nc;
    }
    s->unpadded[s->nblocks] = unpadded;
    s->usize[s->nblocks] = usize;
    s->nblocks++;
    return 0;
}

/* 0 нужно ещё, 1 есть выход, 2 конец, отрицательное — ошибка lzma_ret с минусом. */
static int xz_decode_more(struct lzma_internal_s *s) {
    const unsigned char *p;
    int n, pos;
    struct lzdec dec;
    if (s->ended) return 2;
    p = s->inbuf;
    n = s->in_n;
    pos = s->in_pos;
    memset(&dec, 0, sizeof dec);
    if (pos == 0) {
        unsigned crc;
        if (n < 12) return 0;
        if (memcmp(p, "\xfd" "7zXZ\0", 6) != 0) return -LZMA_FORMAT_ERROR;
        if (p[6] != 0 || (p[7] & 0xf0)) return -LZMA_OPTIONS_ERROR;
        s->check = p[7] & 0x0f;
        if (check_size(s->check) < 0) return -LZMA_UNSUPPORTED_CHECK;
        crc = xz_crc32(0, p + 6, 2);
        if (crc != get32(p + 8)) return -LZMA_DATA_ERROR;
        pos = 12;
        s->in_pos = 12;
    }
    while (pos < n) {
        unsigned hsize, flags, crc;
        int at, nf, i, rc, pad, check_len, comp_len;
        unsigned long long csize = 0, usize = 0, got;
        unsigned char *plain = NULL;
        int plain_n = 0;
        const unsigned char *comp;
        if (p[pos] == 0) break;
        if (n - pos < 2) { free(dec.dict); free(dec.lit); return 0; }
        hsize = ((unsigned)p[pos] + 1u) * 4u;
        if (hsize < 8 || hsize > 1024) { free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR; }
        if ((unsigned)(n - pos) < hsize) { free(dec.dict); free(dec.lit); return 0; }
        crc = xz_crc32(0, p + pos, (int)hsize - 4);
        if (crc != get32(p + pos + hsize - 4)) { free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR; }
        flags = p[pos + 1];
        if (flags & 0x3c) { free(dec.dict); free(dec.lit); return -LZMA_OPTIONS_ERROR; }
        nf = (flags & 3) + 1;
        at = pos + 2;
        if (flags & 0x40) {
            int used = 0;
            if (vli_get(p + at, (int)(pos + hsize - 4 - at), &csize, &used) < 0) {
                free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
            }
            at += used;
        }
        if (flags & 0x80) {
            int used = 0;
            if (vli_get(p + at, (int)(pos + hsize - 4 - at), &usize, &used) < 0) {
                free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
            }
            at += used;
        }
        if (nf != 1) { free(dec.dict); free(dec.lit); return -LZMA_OPTIONS_ERROR; }
        for (i = 0; i < nf; i++) {
            unsigned long long id = 0, psz = 0;
            int used = 0;
            unsigned dict;
            int bad = 0;
            if (vli_get(p + at, (int)(pos + hsize - 4 - at), &id, &used) < 0) {
                free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
            }
            at += used;
            if (vli_get(p + at, (int)(pos + hsize - 4 - at), &psz, &used) < 0) {
                free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
            }
            at += used;
            if (id != LZMA_FILTER_LZMA2 || psz != 1 || at >= (int)(pos + hsize - 4)) {
                free(dec.dict); free(dec.lit); return -LZMA_OPTIONS_ERROR;
            }
            dict = dict_from_prop(p[at++], &bad);
            if (bad || dict < 4096) { free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR; }
            if ((unsigned long long)dict > s->memlimit || dict > XZ_DICT_MAX) {
                free(dec.dict); free(dec.lit); return -LZMA_MEMLIMIT_ERROR;
            }
            if (dec.dict_size != dict) {
                unsigned char *nd = malloc(dict);
                if (!nd) { free(dec.dict); free(dec.lit); return -LZMA_MEM_ERROR; }
                free(dec.dict);
                dec.dict = nd;
                dec.dict_size = dict;
            }
        }
        comp = p + pos + hsize;
        check_len = check_size(s->check);
        if (flags & 0x40) {
            comp_len = (int)csize;
            if (comp_len < 1 || pos + (int)hsize + comp_len > n) { free(dec.dict); free(dec.lit); return 0; }
            rc = lzma2_unpack(&dec, comp, comp_len, 1, &plain, &plain_n, &i);
        } else {
            comp_len = n - (pos + (int)hsize);
            if (comp_len <= 0) { free(dec.dict); free(dec.lit); return 0; }
            rc = lzma2_unpack(&dec, comp, comp_len, 0, &plain, &plain_n, &i);
            if (rc == -3) { free(plain); free(dec.dict); free(dec.lit); return 0; }
            comp_len = i;
        }
        if (rc == -2) { free(dec.dict); free(dec.lit); free(plain); return -LZMA_MEM_ERROR; }
        if (rc < 0) { free(plain); free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR; }
        if ((flags & 0x80) && usize != (unsigned long long)plain_n) {
            free(plain); free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
        }
        pad = (int)((4 - ((hsize + (unsigned)comp_len) & 3)) & 3);
        if (pos + (int)hsize + comp_len + pad + check_len > n) {
            free(plain); free(dec.dict); free(dec.lit); return 0;
        }
        for (i = 0; i < pad; i++) if (comp[comp_len + i] != 0) {
            free(plain); free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
        }
        if (s->check == LZMA_CHECK_CRC32) {
            if (xz_crc32(0, plain, plain_n) != get32(comp + comp_len + pad)) {
                free(plain); free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
            }
        } else if (s->check == LZMA_CHECK_CRC64) {
            if (xz_crc64(0, plain, plain_n) != get64(comp + comp_len + pad)) {
                free(plain); free(dec.dict); free(dec.lit); return -LZMA_DATA_ERROR;
            }
        }
        got = (unsigned long long)(hsize + (unsigned)comp_len + (unsigned)check_len);
        if (remember_block(s, got, (unsigned long long)plain_n) < 0) {
            free(plain); free(dec.dict); free(dec.lit); return -LZMA_MEM_ERROR;
        }
        if (buf_add(&s->outbuf, &s->out_n, &s->out_cap, plain, plain_n, XZ_IN_MAX) < 0) {
            free(plain); free(dec.dict); free(dec.lit); return -LZMA_MEM_ERROR;
        }
        free(plain);
        pos += (int)hsize + comp_len + pad + check_len;
        s->in_pos = pos;
        lz_dict_reset(&dec);
    }
    free(dec.dict);
    free(dec.lit);
    /* Указатель и подвал. pos стоит на байте 0x00, если блоки кончились. */
    if (pos >= n) return 0;
    if (p[pos] != 0) return -LZMA_DATA_ERROR;
    {
        unsigned long long nrec = 0;
        int used = 0, index_at = pos, k;
        unsigned crc;
        if (vli_get(p + pos + 1, n - pos - 1, &nrec, &used) < 0) return 0;
        pos += 1 + used;
        if (nrec != (unsigned long long)s->nblocks) return -LZMA_DATA_ERROR;
        for (k = 0; k < s->nblocks; k++) {
            unsigned long long up = 0, us = 0;
            if (vli_get(p + pos, n - pos, &up, &used) < 0) return 0;
            pos += used;
            if (vli_get(p + pos, n - pos, &us, &used) < 0) return 0;
            pos += used;
            if (up != s->unpadded[k] || us != s->usize[k]) return -LZMA_DATA_ERROR;
        }
        while ((pos - index_at) & 3) {
            if (pos >= n) return 0;
            if (p[pos++] != 0) return -LZMA_DATA_ERROR;
        }
        if (pos + 4 > n) return 0;
        crc = xz_crc32(0, p + index_at, pos - index_at);
        if (crc != get32(p + pos)) return -LZMA_DATA_ERROR;
        pos += 4;
        if (pos + 12 > n) return 0;
        {
            unsigned bw = get32(p + pos + 4);
            unsigned index_len = (unsigned)(pos - index_at);
            if (p[pos + 10] != 'Y' || p[pos + 11] != 'Z') return -LZMA_DATA_ERROR;
            if (p[pos + 8] != 0 || p[pos + 9] != (unsigned char)s->check) return -LZMA_DATA_ERROR;
            if ((bw + 1u) * 4u != index_len) return -LZMA_DATA_ERROR;
            if (xz_crc32(0, p + pos + 4, 6) != get32(p + pos)) return -LZMA_DATA_ERROR;
        }
        s->in_pos = pos + 12;
        s->ended = 1;
        return 2;
    }
}

/* Сжатый поток: один блок, несжатые куски LZMA2, проверка CRC32 или без неё. */
static int xz_encode(const unsigned char *in, int n, int check, unsigned char **outp, int *outn) {
    unsigned char *o;
    int cap = n + n / 16 + 128, len = 0, chunks, off, first, comp_at, comp_len, pad, i;
    unsigned long long unpadded, usize;
    if (check != LZMA_CHECK_NONE && check != LZMA_CHECK_CRC32) return -1;
    if (cap < 128) cap = 128;
    o = malloc((size_t)cap);
    if (!o) return -1;
#define NEED(k) do { \
    if (len + (k) > cap) { \
        int nc = cap * 2 + (k); unsigned char *np = realloc(o, (size_t)nc); \
        if (!np) { free(o); return -1; } o = np; cap = nc; } \
} while (0)
    NEED(12);
    memcpy(o, "\xfd" "7zXZ\0", 6);
    o[6] = 0;
    o[7] = (unsigned char)check;
    put32(o + 8, xz_crc32(0, o + 6, 2));
    len = 12;
    NEED(12);
    o[len] = 2;
    o[len + 1] = 0;
    o[len + 2] = 0x21;
    o[len + 3] = 1;
    o[len + 4] = 8; /* словарь 64 КиБ: для несжатых кусков больше не нужно */
    o[len + 5] = o[len + 6] = o[len + 7] = 0;
    put32(o + len + 8, xz_crc32(0, o + len, 8));
    len += 12;
    comp_at = len;
    off = 0;
    first = 1;
    chunks = 0;
    while (off < n) {
        int take = n - off;
        if (take > 65536) take = 65536;
        NEED(3 + take);
        o[len++] = (unsigned char)(first ? 0x01 : 0x02);
        o[len++] = (unsigned char)((take - 1) >> 8);
        o[len++] = (unsigned char)(take - 1);
        memcpy(o + len, in + off, (size_t)take);
        len += take;
        off += take;
        first = 0;
        chunks++;
    }
    NEED(1);
    o[len++] = 0;
    comp_len = len - comp_at;
    pad = (4 - ((12 + comp_len) & 3)) & 3;
    NEED(pad + 4 + 64);
    for (i = 0; i < pad; i++) o[len++] = 0;
    if (check == LZMA_CHECK_CRC32) {
        put32(o + len, xz_crc32(0, in, n));
        len += 4;
    }
    unpadded = (unsigned long long)(12 + comp_len + (check == LZMA_CHECK_CRC32 ? 4 : 0));
    usize = (unsigned long long)n;
    {
        unsigned char rec[32];
        int rn = 0, index_at, index_len;
        rec[rn++] = 0;
        rn += vli_put(rec + rn, 1);
        rn += vli_put(rec + rn, unpadded);
        rn += vli_put(rec + rn, usize);
        while (rn & 3) rec[rn++] = 0;
        NEED(rn + 4 + 12);
        index_at = len;
        memcpy(o + len, rec, (size_t)rn);
        len += rn;
        put32(o + len, xz_crc32(0, o + index_at, rn));
        len += 4;
        index_len = len - index_at;
        {
            unsigned char foot[6];
            unsigned bw = (unsigned)(index_len / 4 - 1);
            put32(foot, bw);
            foot[4] = 0;
            foot[5] = (unsigned char)check;
            put32(o + len, xz_crc32(0, foot, 6));
            len += 4;
            memcpy(o + len, foot, 6);
            len += 6;
            o[len++] = 'Y';
            o[len++] = 'Z';
        }
    }
    (void)chunks;
    *outp = o;
    *outn = len;
    return 0;
#undef NEED
}

static void give_out(lzma_stream *strm) {
    struct lzma_internal_s *s = strm->internal;
    size_t take = (size_t)(s->out_n - s->out_pos);
    if (!s->outbuf || take == 0 || strm->avail_out == 0) return;
    if (take > strm->avail_out) take = strm->avail_out;
    memcpy(strm->next_out, s->outbuf + s->out_pos, take);
    strm->next_out += take;
    strm->avail_out -= take;
    strm->total_out += take;
    s->out_pos += (int)take;
}

static void take_in(lzma_stream *strm) {
    struct lzma_internal_s *s = strm->internal;
    int sn = (int)strm->avail_in;
    if (sn <= 0) return;
    if (buf_add(&s->inbuf, &s->in_n, &s->in_cap, strm->next_in, sn, XZ_IN_MAX) < 0) {
        s->err = LZMA_MEM_ERROR;
        return;
    }
    strm->next_in += sn;
    strm->avail_in = 0;
    strm->total_in += (unsigned long long)sn;
}

lzma_ret lzma_stream_decoder(lzma_stream *strm, unsigned long long memlimit, unsigned flags) {
    struct lzma_internal_s *s;
    xz_crc_init();
    if (!strm) return LZMA_PROG_ERROR;
    s = calloc(1, sizeof *s);
    if (!s) return LZMA_MEM_ERROR;
    s->enc = 0;
    s->memlimit = memlimit ? memlimit : XZ_DICT_MAX;
    s->flags = flags;
    strm->internal = s;
    return LZMA_OK;
}

lzma_ret lzma_stream_encoder(lzma_stream *strm, const lzma_filter *filters, lzma_check check) {
    struct lzma_internal_s *s;
    (void)filters;
    xz_crc_init();
    if (!strm) return LZMA_PROG_ERROR;
    if (check != LZMA_CHECK_NONE && check != LZMA_CHECK_CRC32) return LZMA_OPTIONS_ERROR;
    s = calloc(1, sizeof *s);
    if (!s) return LZMA_MEM_ERROR;
    s->enc = 1;
    s->check = (int)check;
    strm->internal = s;
    return LZMA_OK;
}

lzma_ret lzma_code(lzma_stream *strm, lzma_action action) {
    struct lzma_internal_s *s;
    if (!strm || !strm->internal) return LZMA_PROG_ERROR;
    s = strm->internal;
    if (s->err) return (lzma_ret)s->err;
    if (s->enc) {
        if (s->phase == 2) return LZMA_PROG_ERROR;
        take_in(strm);
        if (s->err) return (lzma_ret)s->err;
        if (action != LZMA_FINISH) return LZMA_OK;
        if (s->phase == 0) {
            if (xz_encode(s->inbuf, s->in_n, s->check, &s->outbuf, &s->out_n) < 0) {
                s->err = LZMA_MEM_ERROR;
                return LZMA_MEM_ERROR;
            }
            s->out_pos = 0;
            s->phase = 1;
        }
        give_out(strm);
        if (s->out_pos < s->out_n) return LZMA_OK;
        s->phase = 2;
        return LZMA_STREAM_END;
    }
    take_in(strm);
    if (s->err) return (lzma_ret)s->err;
    give_out(strm);
    while ((!s->outbuf || s->out_pos >= s->out_n) && !s->ended) {
        int rc = xz_decode_more(s);
        if (rc == 0) return LZMA_OK;
        if (rc < 0) { s->err = -rc; return (lzma_ret)s->err; }
        if (rc == 2) s->ended = 1;
    }
    give_out(strm);
    if (s->ended && (!s->outbuf || s->out_pos >= s->out_n)) return LZMA_STREAM_END;
    return LZMA_OK;
}

void lzma_end(lzma_stream *strm) {
    struct lzma_internal_s *s;
    if (!strm || !strm->internal) return;
    s = strm->internal;
    free(s->inbuf);
    free(s->outbuf);
    free(s->unpadded);
    free(s->usize);
    free(s);
    strm->internal = NULL;
}
