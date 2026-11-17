/* bzip2.c — упаковка и распаковка потока bzip2.
 * Блок целиком: пока во входе нет целого блока, наружу ничего не отдаём.
 * Короткий read не оставляет битовый курсор на половине кода Хаффмана.
 * Случайный бит старых блоков отвергаем: его нет у Python и GNU bzip2. */
#include "myos.h"
#include "stdlib.h"
#include "string.h"
#include "bzlib.h"

#define BZ_IN_MAX (2 * 1024 * 1024)
#define BZ_OUT_MAX (8 * 1024 * 1024)

static unsigned bz_crc_table[256];
static int bz_crc_ready;

static void bz_crc_init(void) {
    int i, j;
    if (bz_crc_ready) return;
    for (i = 0; i < 256; i++) {
        unsigned c = (unsigned)i << 24;
        for (j = 0; j < 8; j++)
            c = (c << 1) ^ ((c & 0x80000000u) ? 0x04c11db7u : 0);
        bz_crc_table[i] = c;
    }
    bz_crc_ready = 1;
}

static unsigned bz_crc_byte(unsigned crc, unsigned char b) {
    return (crc << 8) ^ bz_crc_table[(crc >> 24) ^ b];
}

const char *BZ2_bzlibVersion(void) { return "1.0.8, 13-Jul-2019"; }

static void add32(unsigned *lo, unsigned *hi, unsigned n) {
    unsigned s = *lo + n;
    if (s < *lo) (*hi)++;
    *lo = s;
}

/* ------------------------------------------------------- биты */

struct bitw {
    unsigned char *p;
    int n, cap, err;
    unsigned acc;
    int nbits;
};

static int bw_need(struct bitw *w, int add) {
    unsigned char *np;
    int nc;
    if (w->err) return -1;
    if (w->n + add <= w->cap) return 0;
    nc = w->cap ? w->cap * 2 : 256;
    while (nc < w->n + add) nc *= 2;
    np = realloc(w->p, (size_t)nc);
    if (!np) { w->err = 1; return -1; }
    w->p = np;
    w->cap = nc;
    return 0;
}

static void bw_bits(struct bitw *w, unsigned v, int n) {
    int i;
    for (i = n - 1; i >= 0; i--) {
        w->acc = (w->acc << 1) | ((v >> i) & 1u);
        w->nbits++;
        if (w->nbits == 8) {
            if (bw_need(w, 1) < 0) return;
            w->p[w->n++] = (unsigned char)w->acc;
            w->acc = 0;
            w->nbits = 0;
        }
    }
}

static void bw_finish(struct bitw *w) {
    if (w->nbits) bw_bits(w, 0, 8 - w->nbits);
}

struct bitr {
    const unsigned char *p;
    int len, bit, need;
};

static int br_bit(struct bitr *r) {
    int bi, b;
    if (r->need) return -1;
    bi = r->bit;
    if (bi / 8 >= r->len) { r->need = 1; return -1; }
    b = (r->p[bi / 8] >> (7 - (bi & 7))) & 1;
    r->bit++;
    return b;
}

static int br_bits(struct bitr *r, int n, unsigned *out) {
    unsigned v = 0;
    int i, b;
    for (i = 0; i < n; i++) {
        b = br_bit(r);
        if (b < 0) return -1;
        v = (v << 1) | (unsigned)b;
    }
    *out = v;
    return 0;
}

/* ------------------------------------------------------- BWT */

static int sa_n, sa_k;
static int *sa_rank;

static int sa_cmp(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    int ri = sa_rank[i], rj = sa_rank[j], i2, j2;
    if (ri != rj) return ri < rj ? -1 : 1;
    i2 = i + sa_k; if (i2 >= sa_n) i2 -= sa_n;
    j2 = j + sa_k; if (j2 >= sa_n) j2 -= sa_n;
    ri = sa_rank[i2]; rj = sa_rank[j2];
    return ri < rj ? -1 : ri > rj ? 1 : 0;
}

/* Вращения циклические: второй ключ заворачивается на начало блока. */
static int bwt_make(const unsigned char *block, int n, unsigned char *out, int *orig) {
    int *sa, *rank, *tmp, i, k;
    if (n <= 0) return -1;
    sa = malloc((size_t)n * sizeof(int));
    rank = malloc((size_t)n * sizeof(int));
    tmp = malloc((size_t)n * sizeof(int));
    if (!sa || !rank || !tmp) { free(sa); free(rank); free(tmp); return -1; }
    for (i = 0; i < n; i++) { sa[i] = i; rank[i] = block[i]; }
    sa_n = n;
    sa_rank = rank;
    for (k = 1; k < n; k <<= 1) {
        int r = 0;
        sa_k = k;
        qsort(sa, (size_t)n, sizeof(int), sa_cmp);
        tmp[sa[0]] = 0;
        for (i = 1; i < n; i++) {
            if (sa_cmp(&sa[i - 1], &sa[i]) != 0) r++;
            tmp[sa[i]] = r;
        }
        memcpy(rank, tmp, (size_t)n * sizeof(int));
        if (r == n - 1) break;
    }
    sa_k = n;
    qsort(sa, (size_t)n, sizeof(int), sa_cmp);
    *orig = -1;
    for (i = 0; i < n; i++) {
        out[i] = block[(sa[i] + n - 1) % n];
        if (sa[i] == 0) *orig = i;
    }
    free(sa); free(rank); free(tmp);
    return *orig < 0 ? -1 : 0;
}

static int bwt_undo(const unsigned char *tt, int n, int orig, unsigned char *out) {
    int count[256], cum[256], *next, i, idx;
    if (orig < 0 || orig >= n) return -1;
    next = malloc((size_t)n * sizeof(int));
    if (!next) return -1;
    memset(count, 0, sizeof count);
    for (i = 0; i < n; i++) count[tt[i]]++;
    cum[0] = 0;
    for (i = 1; i < 256; i++) cum[i] = cum[i - 1] + count[i - 1];
    for (i = 0; i < n; i++) next[cum[tt[i]]++] = i;
    idx = orig;
    for (i = 0; i < n; i++) {
        idx = next[idx];
        out[i] = tt[idx];
    }
    free(next);
    return 0;
}

/* ------------------------------------------------------- RLE перед BWT */

static int rle1(const unsigned char *in, int n, unsigned char *out, int cap) {
    int i = 0, o = 0;
    while (i < n) {
        unsigned char c = in[i];
        int run = 1, k, put;
        while (i + run < n && in[i + run] == c && run < 259) run++;
        put = run < 4 ? run : 4;
        if (o + put + (run >= 4) > cap) return -1;
        for (k = 0; k < put; k++) out[o++] = c;
        if (run >= 4) out[o++] = (unsigned char)(run - 4);
        i += run;
    }
    return o;
}

static int unrle1(const unsigned char *in, int n, unsigned char **outp, int *outn) {
    unsigned char *out = NULL;
    int cap = 0, o = 0, i = 0, run = 0;
    unsigned char prev = 0;
    while (i < n) {
        unsigned char c = in[i++];
        int add = run == 4 ? c : 1;
        if (o + add > BZ_OUT_MAX) { free(out); return -1; }
        if (o + add > cap) {
            int nc = cap ? cap * 2 : 256;
            unsigned char *np;
            while (nc < o + add) nc *= 2;
            np = realloc(out, (size_t)nc);
            if (!np) { free(out); return -1; }
            out = np;
            cap = nc;
        }
        if (run == 4) {
            int k;
            for (k = 0; k < c; k++) out[o++] = prev;
            run = 0;
            continue;
        }
        out[o++] = c;
        if (run > 0 && c == prev) run++;
        else { prev = c; run = 1; }
    }
    if (run == 4) { free(out); return -1; }
    *outp = out;
    *outn = o;
    return 0;
}

/* ------------------------------------------------------- Хаффман */

static int hb_tables(int *limit, int *base, int *perm, const unsigned char *length,
                     int minLen, int maxLen, int alpha) {
    int i, j, pp = 0, vec = 0;
    for (i = minLen; i <= maxLen; i++)
        for (j = 0; j < alpha; j++)
            if (length[j] == i) perm[pp++] = j;
    if (pp != alpha) return -1;
    memset(base, 0, 24 * sizeof(int));
    for (i = 0; i < alpha; i++) base[length[i] + 1]++;
    for (i = 1; i < 23; i++) base[i] += base[i - 1];
    memset(limit, 0, 24 * sizeof(int));
    for (i = minLen; i <= maxLen; i++) {
        vec += base[i + 1] - base[i];
        limit[i] = vec - 1;
        vec <<= 1;
    }
    for (i = minLen + 1; i <= maxLen; i++)
        base[i] = ((limit[i - 1] + 1) << 1) - base[i];
    return 0;
}

static int hb_decode(struct bitr *r, int minLen, int maxLen, const int *limit,
                     const int *base, const int *perm, int alpha) {
    unsigned zvec = 0;
    int zn, idx;
    if (br_bits(r, minLen, &zvec) < 0) return -1;
    zn = minLen;
    while (zn <= maxLen && (int)zvec > limit[zn]) {
        int b = br_bit(r);
        if (b < 0) return -1;
        zvec = (zvec << 1) | (unsigned)b;
        zn++;
    }
    if (zn > maxLen) return -2;
    idx = (int)zvec - base[zn];
    if (idx < 0 || idx >= alpha) return -2;
    return perm[idx];
}

static void emit_zeros(int *syms, int *ns, int z) {
    if (z <= 0) return;
    z--;
    for (;;) {
        syms[(*ns)++] = (z & 1) ? 1 : 0;
        if (z < 2) break;
        z = (z - 2) / 2;
    }
}

/* ------------------------------------------------------- один блок */

/* 1 блок, 0 конец потока, -1 мало байт, -2 порча, -3 память. */
static int decode_one(struct bitr *r, int block_max, unsigned *stream_crc,
                      unsigned char **raw_out, int *raw_n) {
    unsigned hi = 0, lo = 0, stored = 0, ng = 0, nsel = 0, curr = 0, row = 0;
    unsigned char used[256], lengths[6][258], mtfb[6];
    int *selectors = NULL, *mtfv = NULL;
    int limit[6][24], base[6][24], perm[6][258];
    int minl[6], maxl[6];
    unsigned char *bwt = NULL, *mid = NULL, *plain = NULL;
    int i, j, nuse, alpha, eob, nmtf, zeros, shift, ns, sym, orig, plain_n, g;
    unsigned crc;
    *raw_out = NULL;
    *raw_n = 0;
    if (br_bits(r, 32, &hi) < 0 || br_bits(r, 16, &lo) < 0) return -1;
    if (hi == 0x17724538u && lo == 0x5090u) {
        unsigned got = 0;
        if (br_bits(r, 32, &got) < 0) return -1;
        if (got != *stream_crc) return -2;
        return 0;
    }
    if (hi != 0x31415926u || lo != 0x5359u) return -2;
    if (br_bits(r, 32, &stored) < 0) return -1;
    if (br_bit(r) != 0) return r->need ? -1 : -2;
    if (br_bits(r, 24, &hi) < 0) return -1;
    orig = (int)hi;
    if (br_bits(r, 16, &row) < 0) return -1;
    memset(used, 0, sizeof used);
    nuse = 0;
    for (i = 0; i < 16; i++) {
        unsigned bits = 0;
        if ((row & (0x8000u >> i)) == 0) continue;
        if (br_bits(r, 16, &bits) < 0) return -1;
        for (j = 0; j < 16; j++)
            if (bits & (0x8000u >> j)) { used[i * 16 + j] = 1; nuse++; }
    }
    if (nuse == 0) return -2;
    alpha = nuse + 2;
    eob = nuse + 1;
    if (br_bits(r, 3, &ng) < 0) return -1;
    if (ng < 2 || ng > 6) return -2;
    if (br_bits(r, 15, &nsel) < 0) return -1;
    if (nsel < 1 || nsel > 18002) return -2;
    selectors = malloc((size_t)nsel * sizeof(int));
    if (!selectors) return -3;
    for (i = 0; i < (int)ng; i++) mtfb[i] = (unsigned char)i;
    for (i = 0; i < (int)nsel; i++) {
        int n = 0, b, v;
        while ((b = br_bit(r)) == 1) {
            n++;
            if (n >= (int)ng) { free(selectors); return -2; }
        }
        if (b < 0) { free(selectors); return -1; }
        v = mtfb[n];
        while (n > 0) { mtfb[n] = mtfb[n - 1]; n--; }
        mtfb[0] = (unsigned char)v;
        selectors[i] = v;
    }
    for (g = 0; g < (int)ng; g++) {
        if (br_bits(r, 5, &curr) < 0) { free(selectors); return -1; }
        minl[g] = 20;
        maxl[g] = 0;
        for (i = 0; i < alpha; i++) {
            for (;;) {
                int b;
                if (curr < 1 || curr > 20) { free(selectors); return -2; }
                b = br_bit(r);
                if (b < 0) { free(selectors); return -1; }
                if (b == 0) break;
                b = br_bit(r);
                if (b < 0) { free(selectors); return -1; }
                curr = b ? curr - 1 : curr + 1;
            }
            lengths[g][i] = (unsigned char)curr;
            if ((int)curr < minl[g]) minl[g] = (int)curr;
            if ((int)curr > maxl[g]) maxl[g] = (int)curr;
        }
        if (hb_tables(limit[g], base[g], perm[g], lengths[g], minl[g], maxl[g], alpha) < 0) {
            free(selectors);
            return -2;
        }
    }
    mtfv = malloc((size_t)block_max * sizeof(int));
    if (!mtfv) { free(selectors); return -3; }
    nmtf = 0;
    zeros = 0;
    shift = 0;
    ns = 0;
    for (;;) {
        if (ns / 50 >= (int)nsel) { free(selectors); free(mtfv); return -2; }
        g = selectors[ns / 50];
        sym = hb_decode(r, minl[g], maxl[g], limit[g], base[g], perm[g], alpha);
        if (sym == -1) { free(selectors); free(mtfv); return -1; }
        if (sym < 0) { free(selectors); free(mtfv); return -2; }
        ns++;
        if (sym == 0 || sym == 1) {
            zeros += (sym + 1) << shift;
            shift++;
            if (zeros > block_max) { free(selectors); free(mtfv); return -2; }
            continue;
        }
        while (zeros > 0) {
            if (nmtf >= block_max) { free(selectors); free(mtfv); return -2; }
            mtfv[nmtf++] = 0;
            zeros--;
        }
        shift = 0;
        if (sym == eob) break;
        if (sym < 1 || sym > nuse) { free(selectors); free(mtfv); return -2; }
        if (nmtf >= block_max) { free(selectors); free(mtfv); return -2; }
        mtfv[nmtf++] = sym - 1;
    }
    free(selectors);
    if (nmtf == 0 || orig >= nmtf) { free(mtfv); return -2; }
    bwt = malloc((size_t)nmtf);
    if (!bwt) { free(mtfv); return -3; }
    {
        unsigned char order[256];
        int m = 0;
        for (i = 0; i < 256; i++) if (used[i]) order[m++] = (unsigned char)i;
        for (i = 0; i < nmtf; i++) {
            int pos = mtfv[i];
            unsigned char c;
            if (pos < 0 || pos >= m) { free(bwt); free(mtfv); return -2; }
            c = order[pos];
            bwt[i] = c;
            if (pos) {
                memmove(order + 1, order, (size_t)pos);
                order[0] = c;
            }
        }
    }
    free(mtfv);
    mid = malloc((size_t)nmtf);
    if (!mid || bwt_undo(bwt, nmtf, orig, mid) < 0) { free(bwt); free(mid); return -3; }
    free(bwt);
    if (unrle1(mid, nmtf, &plain, &plain_n) < 0) { free(mid); free(plain); return -2; }
    free(mid);
    crc = 0xffffffffu;
    for (i = 0; i < plain_n; i++) crc = bz_crc_byte(crc, plain[i]);
    crc = ~crc;
    if (crc != stored) { free(plain); return -2; }
    *stream_crc = ((*stream_crc << 1) | (*stream_crc >> 31)) ^ crc;
    *raw_out = plain;
    *raw_n = plain_n;
    return 1;
}

static int emit_block(struct bitw *w, const unsigned char *in, int n, unsigned *comb) {
    unsigned crc, row = 0;
    unsigned char *rle, *bwts, order[256], used[256];
    int *syms, rn, orig, i, nuse, alpha, L, ns, z, nsel, s, g, m;
    int limit = 100000 * 9;
    (void)limit;
    rle = malloc((size_t)n + (size_t)n / 4 + 8);
    if (!rle) return -1;
    rn = rle1(in, n, rle, n + n / 4 + 8);
    if (rn <= 0) { free(rle); return -1; }
    bwts = malloc((size_t)rn);
    syms = malloc(((size_t)rn * 2 + 8) * sizeof(int));
    if (!bwts || !syms || bwt_make(rle, rn, bwts, &orig) < 0) {
        free(rle); free(bwts); free(syms);
        return -1;
    }
    free(rle);
    crc = 0xffffffffu;
    for (i = 0; i < n; i++) crc = bz_crc_byte(crc, in[i]);
    crc = ~crc;
    *comb = ((*comb << 1) | (*comb >> 31)) ^ crc;
    memset(used, 0, sizeof used);
    nuse = 0;
    for (i = 0; i < rn; i++) if (!used[bwts[i]]) { used[bwts[i]] = 1; nuse++; }
    m = 0;
    for (i = 0; i < 256; i++) if (used[i]) order[m++] = (unsigned char)i;
    ns = 0;
    z = 0;
    for (i = 0; i < rn; i++) {
        int pos = 0;
        unsigned char c = bwts[i];
        while (order[pos] != c) pos++;
        if (pos) {
            memmove(order + 1, order, (size_t)pos);
            order[0] = c;
        }
        if (pos == 0) z++;
        else {
            emit_zeros(syms, &ns, z);
            z = 0;
            syms[ns++] = pos + 1;
        }
    }
    emit_zeros(syms, &ns, z);
    syms[ns++] = nuse + 1;
    alpha = nuse + 2;
    L = 1;
    while ((1 << L) < alpha) L++;
    bw_bits(w, 0x31415926u, 32);
    bw_bits(w, 0x5359u, 16);
    bw_bits(w, crc, 32);
    bw_bits(w, 0, 1);
    bw_bits(w, (unsigned)orig, 24);
    for (i = 0; i < 16; i++) {
        int any = 0;
        for (s = 0; s < 16; s++) if (used[i * 16 + s]) any = 1;
        if (any) row |= 0x8000u >> i;
    }
    bw_bits(w, row, 16);
    for (i = 0; i < 16; i++) {
        unsigned bits = 0;
        if ((row & (0x8000u >> i)) == 0) continue;
        for (s = 0; s < 16; s++) if (used[i * 16 + s]) bits |= 0x8000u >> s;
        bw_bits(w, bits, 16);
    }
    nsel = (ns + 49) / 50;
    bw_bits(w, 2, 3);
    bw_bits(w, (unsigned)nsel, 15);
    for (i = 0; i < nsel; i++) bw_bits(w, 0, 1);
    for (g = 0; g < 2; g++) {
        bw_bits(w, (unsigned)L, 5);
        for (s = 0; s < alpha; s++) bw_bits(w, 0, 1);
    }
    for (i = 0; i < ns; i++) bw_bits(w, (unsigned)syms[i], L);
    free(bwts);
    free(syms);
    return w->err ? -1 : 0;
}

static int compress_all(const unsigned char *in, int n, int bs, struct bitw *w) {
    unsigned comb = 0;
    int limit = 100000 * bs;
    int chunk = limit * 4 / 5;
    int off = 0;
    if (chunk < 1) chunk = 1;
    w->p = NULL; w->n = w->cap = w->err = w->nbits = 0; w->acc = 0;
    bw_bits(w, 'B', 8); bw_bits(w, 'Z', 8); bw_bits(w, 'h', 8); bw_bits(w, '0' + bs, 8);
    while (off < n) {
        int take = n - off;
        if (take > chunk) take = chunk;
        if (emit_block(w, in + off, take, &comb) < 0) { free(w->p); return -1; }
        off += take;
    }
    bw_bits(w, 0x17724538u, 32);
    bw_bits(w, 0x5090u, 16);
    bw_bits(w, comb, 32);
    bw_finish(w);
    return w->err ? -1 : 0;
}

/* ------------------------------------------------------- поток */

struct bz_c {
    int bs, phase;
    unsigned char *in, *out;
    int in_n, in_cap, out_n, out_pos;
};

struct bz_d {
    int header, block_max, ended, bad;
    unsigned stream_crc;
    unsigned char *in, *out;
    int in_n, in_cap, bitpos, out_n, out_pos;
};

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

static void take_in(bz_stream *s, unsigned char **p, int *n, int *cap, int max) {
    int sn = (int)s->avail_in;
    if (sn <= 0) return;
    if (buf_add(p, n, cap, s->next_in, sn, max) < 0) return;
    s->next_in += sn;
    add32(&s->total_in_lo32, &s->total_in_hi32, (unsigned)sn);
    s->avail_in = 0;
}

static void give_out(bz_stream *s, unsigned char *buf, int n, int *pos) {
    int take = n - *pos;
    if (take > (int)s->avail_out) take = (int)s->avail_out;
    if (take <= 0) return;
    memcpy(s->next_out, buf + *pos, (size_t)take);
    s->next_out += take;
    s->avail_out -= (unsigned)take;
    add32(&s->total_out_lo32, &s->total_out_hi32, (unsigned)take);
    *pos += take;
}

int BZ2_bzCompressInit(bz_stream *strm, int blockSize100k, int verbosity, int workFactor) {
    struct bz_c *c;
    (void)verbosity; (void)workFactor;
    bz_crc_init();
    if (!strm || blockSize100k < 1 || blockSize100k > 9) return BZ_PARAM_ERROR;
    c = calloc(1, sizeof *c);
    if (!c) return BZ_MEM_ERROR;
    c->bs = blockSize100k;
    strm->state = c;
    strm->total_in_lo32 = strm->total_in_hi32 = 0;
    strm->total_out_lo32 = strm->total_out_hi32 = 0;
    return BZ_OK;
}

int BZ2_bzCompress(bz_stream *strm, int action) {
    struct bz_c *c;
    struct bitw w;
    if (!strm || !strm->state) return BZ_PARAM_ERROR;
    c = strm->state;
    if (c->phase == 2) return BZ_SEQUENCE_ERROR;
    if (action != BZ_RUN && action != BZ_FLUSH && action != BZ_FINISH) return BZ_PARAM_ERROR;
    take_in(strm, &c->in, &c->in_n, &c->in_cap, BZ_IN_MAX);
    if (strm->avail_in) return BZ_MEM_ERROR;
    if (action != BZ_FINISH) return action == BZ_FLUSH ? BZ_FLUSH_OK : BZ_RUN_OK;
    if (c->phase == 0) {
        if (compress_all(c->in, c->in_n, c->bs, &w) < 0) return BZ_MEM_ERROR;
        c->out = w.p;
        c->out_n = w.n;
        c->out_pos = 0;
        c->phase = 1;
    }
    give_out(strm, c->out, c->out_n, &c->out_pos);
    if (c->out_pos < c->out_n) return BZ_FINISH_OK;
    c->phase = 2;
    return BZ_STREAM_END;
}

int BZ2_bzCompressEnd(bz_stream *strm) {
    struct bz_c *c;
    if (!strm || !strm->state) return BZ_PARAM_ERROR;
    c = strm->state;
    free(c->in); free(c->out); free(c);
    strm->state = NULL;
    return BZ_OK;
}

int BZ2_bzDecompressInit(bz_stream *strm, int verbosity, int small) {
    struct bz_d *d;
    (void)verbosity; (void)small;
    bz_crc_init();
    if (!strm) return BZ_PARAM_ERROR;
    d = calloc(1, sizeof *d);
    if (!d) return BZ_MEM_ERROR;
    strm->state = d;
    strm->total_in_lo32 = strm->total_in_hi32 = 0;
    strm->total_out_lo32 = strm->total_out_hi32 = 0;
    return BZ_OK;
}

int BZ2_bzDecompress(bz_stream *strm) {
    struct bz_d *d;
    if (!strm || !strm->state) return BZ_PARAM_ERROR;
    d = strm->state;
    if (d->bad) return d->bad;
    take_in(strm, &d->in, &d->in_n, &d->in_cap, BZ_IN_MAX);
    if (strm->avail_in) { d->bad = BZ_DATA_ERROR; return BZ_DATA_ERROR; }
    for (;;) {
        struct bitr r;
        int rc;
        if (d->out && d->out_pos < d->out_n) {
            unsigned before = strm->avail_out;
            give_out(strm, d->out, d->out_n, &d->out_pos);
            if (d->out_pos < d->out_n) return BZ_OK;
            if (strm->avail_out == before && d->out_n > d->out_pos) return BZ_OK;
        }
        if (d->ended) return BZ_STREAM_END;
        free(d->out);
        d->out = NULL;
        d->out_n = d->out_pos = 0;
        if (!d->header) {
            if (d->in_n < 4) return BZ_OK;
            if (d->in[0] != 'B' || d->in[1] != 'Z' || d->in[2] != 'h' ||
                d->in[3] < '1' || d->in[3] > '9') {
                d->bad = BZ_DATA_ERROR_MAGIC;
                return BZ_DATA_ERROR_MAGIC;
            }
            d->block_max = 100000 * (d->in[3] - '0');
            d->bitpos = 32;
            d->header = 1;
        }
        r.p = d->in;
        r.len = d->in_n;
        r.bit = d->bitpos;
        r.need = 0;
        rc = decode_one(&r, d->block_max, &d->stream_crc, &d->out, &d->out_n);
        if (rc == -1) return BZ_OK;
        if (rc == -3) { d->bad = BZ_MEM_ERROR; return BZ_MEM_ERROR; }
        if (rc < 0) { d->bad = BZ_DATA_ERROR; return BZ_DATA_ERROR; }
        {
            int drop = r.bit / 8;
            if (drop) {
                memmove(d->in, d->in + drop, (size_t)(d->in_n - drop));
                d->in_n -= drop;
            }
            d->bitpos = r.bit & 7;
        }
        d->out_pos = 0;
        if (rc == 0) d->ended = 1;
    }
}

int BZ2_bzDecompressEnd(bz_stream *strm) {
    struct bz_d *d;
    if (!strm || !strm->state) return BZ_PARAM_ERROR;
    d = strm->state;
    free(d->in); free(d->out); free(d);
    strm->state = NULL;
    return BZ_OK;
}

int BZ2_bzBuffToBuffCompress(char *dest, unsigned int *destLen, char *source, unsigned int sourceLen,
                             int blockSize100k, int verbosity, int workFactor) {
    bz_stream s;
    int r;
    memset(&s, 0, sizeof s);
    r = BZ2_bzCompressInit(&s, blockSize100k, verbosity, workFactor);
    if (r != BZ_OK) return r;
    s.next_in = source;
    s.avail_in = sourceLen;
    s.next_out = dest;
    s.avail_out = *destLen;
    r = BZ2_bzCompress(&s, BZ_FINISH);
    *destLen = s.total_out_lo32;
    BZ2_bzCompressEnd(&s);
    if (r == BZ_STREAM_END) return BZ_OK;
    if (r == BZ_FINISH_OK) return BZ_OUTBUFF_FULL;
    return r;
}

int BZ2_bzBuffToBuffDecompress(char *dest, unsigned int *destLen, char *source, unsigned int sourceLen,
                               int small, int verbosity) {
    bz_stream s;
    int r;
    memset(&s, 0, sizeof s);
    r = BZ2_bzDecompressInit(&s, verbosity, small);
    if (r != BZ_OK) return r;
    s.next_in = source;
    s.avail_in = sourceLen;
    s.next_out = dest;
    s.avail_out = *destLen;
    r = BZ2_bzDecompress(&s);
    *destLen = s.total_out_lo32;
    BZ2_bzDecompressEnd(&s);
    if (r == BZ_STREAM_END) return BZ_OK;
    if (r == BZ_OK) return BZ_UNEXPECTED_EOF;
    return r;
}
