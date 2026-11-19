#include "wpa.h"
#ifdef MYOS_H
#else
#include <string.h>
#endif

static unsigned rotl(unsigned x, int n) { return (x << n) | (x >> (32 - n)); }

static void sha1_block(unsigned st[5], const unsigned char blk[64]) {
    unsigned w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((unsigned)blk[i*4] << 24) | ((unsigned)blk[i*4+1] << 16) | ((unsigned)blk[i*4+2] << 8) | blk[i*4+3];
    for (int i = 16; i < 80; i++) w[i] = rotl(w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16], 1);
    unsigned a = st[0], b = st[1], c = st[2], d = st[3], e = st[4];
    for (int i = 0; i < 80; i++) {
        unsigned f, k;
        if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5a827999; }
        else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc; }
        else { f = b ^ c ^ d; k = 0xca62c1d6; }
        unsigned t = rotl(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rotl(b, 30); b = a; a = t;
    }
    st[0] += a; st[1] += b; st[2] += c; st[3] += d; st[4] += e;
}

static void sha1(const unsigned char *msg, unsigned len, unsigned char out[20]) {
    unsigned st[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    unsigned char blk[64];
    unsigned off = 0;
    while (len - off >= 64) { sha1_block(st, msg + off); off += 64; }
    unsigned rem = len - off;
    memset(blk, 0, 64);
    memcpy(blk, msg + off, rem);
    blk[rem] = 0x80;
    if (rem >= 56) { sha1_block(st, blk); memset(blk, 0, 64); }
    unsigned long long bits = (unsigned long long)len * 8;
    for (int i = 0; i < 8; i++) blk[63 - i] = (unsigned char)(bits >> (8 * i));
    sha1_block(st, blk);
    for (int i = 0; i < 5; i++) {
        out[i*4] = (unsigned char)(st[i] >> 24);
        out[i*4+1] = (unsigned char)(st[i] >> 16);
        out[i*4+2] = (unsigned char)(st[i] >> 8);
        out[i*4+3] = (unsigned char)st[i];
    }
}

static void hmac_sha1(const unsigned char *key, unsigned klen, const unsigned char *msg, unsigned mlen, unsigned char out[20]) {
    unsigned char k[64], ipad[64], opad[64], inner[20];
    memset(k, 0, 64);
    if (klen > 64) sha1(key, klen, k);
    else memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) { ipad[i] = (unsigned char)(k[i] ^ 0x36); opad[i] = (unsigned char)(k[i] ^ 0x5c); }
    unsigned char *buf = 0;
    unsigned char stack[64 + 256];
    unsigned char big_i[64 + 128];
    (void)buf; (void)stack;
    if (mlen <= 128) {
        memcpy(big_i, ipad, 64);
        memcpy(big_i + 64, msg, mlen);
        sha1(big_i, 64 + mlen, inner);
    } else {
        /* Длинный HMAC собирается двумя проходами без кучи: соль PBKDF2 короче. */
        unsigned char first[64 + 64];
        memcpy(first, ipad, 64);
        unsigned left = mlen;
        const unsigned char *p = msg;
        unsigned st[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
        unsigned char blk[64];
        memcpy(blk, ipad, 64);
        sha1_block(st, blk);
        while (left >= 64) { sha1_block(st, p); p += 64; left -= 64; }
        memset(blk, 0, 64);
        memcpy(blk, p, left);
        blk[left] = 0x80;
        unsigned long long bits = (unsigned long long)(64 + mlen) * 8;
        if (left >= 56) { sha1_block(st, blk); memset(blk, 0, 64); }
        for (int i = 0; i < 8; i++) blk[63 - i] = (unsigned char)(bits >> (8 * i));
        sha1_block(st, blk);
        for (int i = 0; i < 5; i++) {
            inner[i*4] = (unsigned char)(st[i] >> 24);
            inner[i*4+1] = (unsigned char)(st[i] >> 16);
            inner[i*4+2] = (unsigned char)(st[i] >> 8);
            inner[i*4+3] = (unsigned char)st[i];
        }
        (void)first;
    }
    unsigned char outer[84];
    memcpy(outer, opad, 64);
    memcpy(outer + 64, inner, 20);
    sha1(outer, 84, out);
}

static void pbkdf2(const char *pass, const unsigned char *salt, unsigned slen, unsigned rounds, unsigned char *out, unsigned outlen) {
    unsigned klen = (unsigned)strlen(pass);
    unsigned blocks = (outlen + 19) / 20;
    for (unsigned b = 1; b <= blocks; b++) {
        unsigned char msg[128], u[20], t[20];
        memcpy(msg, salt, slen);
        msg[slen] = 0; msg[slen+1] = 0; msg[slen+2] = 0; msg[slen+3] = (unsigned char)b;
        hmac_sha1((const unsigned char *)pass, klen, msg, slen + 4, u);
        memcpy(t, u, 20);
        for (unsigned r = 1; r < rounds; r++) {
            hmac_sha1((const unsigned char *)pass, klen, u, 20, u);
            for (int i = 0; i < 20; i++) t[i] ^= u[i];
        }
        unsigned n = outlen - (b - 1) * 20;
        if (n > 20) n = 20;
        memcpy(out + (b - 1) * 20, t, n);
    }
}

void wpa_pmk(const char *pass, const char *ssid, unsigned char pmk[32]) {
    pbkdf2(pass, (const unsigned char *)ssid, (unsigned)strlen(ssid), 4096, pmk, 32);
}

static void prf(const unsigned char pmk[32], const unsigned char *label, const unsigned char aa[6], const unsigned char spa[6],
                const unsigned char anonce[32], const unsigned char snonce[32], unsigned char *out, unsigned outlen) {
    unsigned char msg[128];
    unsigned nlab = (unsigned)strlen((const char *)label);
    memcpy(msg, label, nlab);
    msg[nlab] = 0;
    const unsigned char *mac1 = aa, *mac2 = spa;
    if (memcmp(aa, spa, 6) > 0) { mac1 = spa; mac2 = aa; }
    memcpy(msg + nlab + 1, mac1, 6);
    memcpy(msg + nlab + 7, mac2, 6);
    const unsigned char *n1 = anonce, *n2 = snonce;
    if (memcmp(anonce, snonce, 32) > 0) { n1 = snonce; n2 = anonce; }
    memcpy(msg + nlab + 13, n1, 32);
    memcpy(msg + nlab + 45, n2, 32);
    unsigned base = nlab + 77;
    unsigned off = 0;
    for (unsigned char i = 0; off < outlen; i++) {
        msg[base] = i;
        unsigned char hash[20];
        hmac_sha1(pmk, 32, msg, base + 1, hash);
        unsigned n = outlen - off;
        if (n > 20) n = 20;
        memcpy(out + off, hash, n);
        off += n;
    }
}

void wpa_ptk(const unsigned char pmk[32], const unsigned char aa[6], const unsigned char spa[6],
             const unsigned char anonce[32], const unsigned char snonce[32], unsigned char ptk[64]) {
    prf(pmk, (const unsigned char *)"Pairwise key expansion", aa, spa, anonce, snonce, ptk, 64);
}

static void put16b(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v >> 8); p[1] = (unsigned char)v; }
static unsigned get16b(const unsigned char *p) { return ((unsigned)p[0] << 8) | p[1]; }

int wpa_eapol_mic(const unsigned char kck[16], unsigned char *frame, unsigned len) {
    if (!frame || len < 99) return -1;
    memset(frame + 81, 0, 16);
    unsigned char mac[20];
    hmac_sha1(kck, 16, frame, len, mac);
    memcpy(frame + 81, mac, 16);
    return 0;
}

int wpa_eapol_ok(const unsigned char kck[16], const unsigned char *frame, unsigned len) {
    if (!frame || len < 99) return 0;
    unsigned char copy[512];
    if (len > sizeof(copy)) return 0;
    memcpy(copy, frame, len);
    unsigned char mic[16];
    memcpy(mic, copy + 81, 16);
    wpa_eapol_mic(kck, copy, len);
    return memcmp(mic, copy + 81, 16) == 0;
}

int wpa_read_m1(const unsigned char *frame, unsigned len, unsigned char anonce[32], unsigned char replay[8]) {
    if (!frame || len < 99 || frame[1] != 3) return -1;
    unsigned info = get16b(frame + 5);
    if ((info & 7) != 2 || !(info & 0x80) || (info & 0x100)) return -1;
    memcpy(replay, frame + 9, 8);
    memcpy(anonce, frame + 17, 32);
    return 0;
}

const unsigned char wpa_rsn_ie[22] = {
    48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0
};

/* Key Data второго сообщения — RSN из assoc. Без него точка обрывает рукопожатие. */
int wpa_build_m2(unsigned char *out, const unsigned char replay[8], const unsigned char snonce[32], const unsigned char kck[16]) {
    unsigned n = 99 + sizeof(wpa_rsn_ie);
    memset(out, 0, n);
    out[0] = 1; out[1] = 3; put16b(out + 2, n - 4);
    out[4] = 2;
    put16b(out + 5, 0x010a); /* MIC, pairwise, version 2 */
    memcpy(out + 9, replay, 8);
    memcpy(out + 17, snonce, 32);
    put16b(out + 97, sizeof(wpa_rsn_ie));
    memcpy(out + 99, wpa_rsn_ie, sizeof(wpa_rsn_ie));
    wpa_eapol_mic(kck, out, n);
    return (int)n;
}

static const unsigned char sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};
static const unsigned char invs[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

static unsigned char xt(unsigned char a) { return (unsigned char)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }
static unsigned char mul(unsigned char a, unsigned char b) {
    unsigned char r = 0;
    for (int i = 0; i < 8; i++) { if (b & 1) r ^= a; a = xt(a); b = (unsigned char)(b >> 1); }
    return r;
}

static void aes_key(const unsigned char key[16], unsigned char rk[176]) {
    static const unsigned char rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        unsigned char t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if ((i % 4) == 0) {
            unsigned char x = t[0];
            t[0] = (unsigned char)(sbox[t[1]] ^ rcon[i / 4 - 1]);
            t[1] = sbox[t[2]]; t[2] = sbox[t[3]]; t[3] = sbox[x];
        }
        for (int j = 0; j < 4; j++) rk[i * 4 + j] = (unsigned char)(rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static void aes_encrypt(const unsigned char *rk, const unsigned char in[16], unsigned char out[16]) {
    unsigned char s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= 10; round++) {
        unsigned char t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        unsigned char r1 = t[1];
        t[1] = t[5]; t[5] = t[9]; t[9] = t[13]; t[13] = r1;
        r1 = t[2]; t[2] = t[10]; t[10] = r1; r1 = t[6]; t[6] = t[14]; t[14] = r1;
        r1 = t[15]; t[15] = t[11]; t[11] = t[7]; t[7] = t[3]; t[3] = r1;
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                unsigned char a0 = t[c*4], a1 = t[c*4+1], a2 = t[c*4+2], a3 = t[c*4+3];
                t[c*4] = (unsigned char)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                t[c*4+1] = (unsigned char)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                t[c*4+2] = (unsigned char)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                t[c*4+3] = (unsigned char)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = (unsigned char)(t[i] ^ rk[round * 16 + i]);
    }
    memcpy(out, s, 16);
}

static void aes_decrypt(const unsigned char *rk, const unsigned char in[16], unsigned char out[16]) {
    unsigned char s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[160 + i];
    for (int round = 9; round >= 0; round--) {
        unsigned char t[16];
        t[0] = s[0]; t[1] = s[13]; t[2] = s[10]; t[3] = s[7];
        t[4] = s[4]; t[5] = s[1]; t[6] = s[14]; t[7] = s[11];
        t[8] = s[8]; t[9] = s[5]; t[10] = s[2]; t[11] = s[15];
        t[12] = s[12]; t[13] = s[9]; t[14] = s[6]; t[15] = s[3];
        for (int i = 0; i < 16; i++) t[i] = invs[t[i]];
        for (int i = 0; i < 16; i++) t[i] ^= rk[round * 16 + i];
        if (round) {
            for (int c = 0; c < 4; c++) {
                unsigned char a0 = t[c*4], a1 = t[c*4+1], a2 = t[c*4+2], a3 = t[c*4+3];
                s[c*4] = (unsigned char)(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
                s[c*4+1] = (unsigned char)(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
                s[c*4+2] = (unsigned char)(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
                s[c*4+3] = (unsigned char)(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
            }
        } else memcpy(s, t, 16);
    }
    memcpy(out, s, 16);
}

int wpa_aes_roundtrip(void) {
    unsigned char key[16], in[16], enc[16], dec[16], rk[176];
    unsigned char nist[16] = { 0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    for (int i = 0; i < 16; i++) { key[i] = (unsigned char)i; in[i] = (unsigned char)(i * 17); }
    aes_key(key, rk);
    aes_encrypt(rk, in, enc);
    aes_decrypt(rk, enc, dec);
    if (memcmp(in, dec, 16)) return 1;
    unsigned char plain[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    aes_encrypt(rk, plain, enc);
    if (memcmp(enc, nist, 16)) return 2;
    aes_decrypt(rk, nist, dec);
    return memcmp(dec, plain, 16) ? 3 : 0;
}

int wpa_aes_wrap(const unsigned char kek[16], const unsigned char *in, unsigned inlen, unsigned char *out) {
    /* У третьего сообщения свёртка часто 80 байт, а не 32. */
    if (!inlen || inlen % 8 || inlen > 128) return -1;
    unsigned n = inlen / 8;
    unsigned char rk[176], a[8], r[16][8], b[16];
    aes_key(kek, rk);
    memset(a, 0xa6, 8);
    for (unsigned i = 0; i < n; i++) memcpy(r[i], in + i * 8, 8);
    for (int j = 0; j <= 5; j++) {
        for (unsigned i = 1; i <= n; i++) {
            memcpy(b, a, 8);
            memcpy(b + 8, r[i - 1], 8);
            aes_encrypt(rk, b, b);
            unsigned t = n * (unsigned)j + i;
            memcpy(a, b, 8);
            a[7] ^= (unsigned char)t;
            a[6] ^= (unsigned char)(t >> 8);
            a[5] ^= (unsigned char)(t >> 16);
            a[4] ^= (unsigned char)(t >> 24);
            memcpy(r[i - 1], b + 8, 8);
        }
    }
    memcpy(out, a, 8);
    for (unsigned i = 0; i < n; i++) memcpy(out + 8 + i * 8, r[i], 8);
    return (int)(inlen + 8);
}

int wpa_aes_unwrap(const unsigned char kek[16], const unsigned char *in, unsigned inlen, unsigned char *out) {
    if (!in || inlen < 24 || (inlen % 8) || inlen > 136) return -1;
    unsigned n = inlen / 8 - 1;
    unsigned char rk[176], a[8], r[16][8], b[16];
    aes_key(kek, rk);
    memcpy(a, in, 8);
    for (unsigned i = 0; i < n; i++) memcpy(r[i], in + 8 + i * 8, 8);
    for (int j = 5; j >= 0; j--) {
        for (int i = (int)n; i >= 1; i--) {
            unsigned t = (unsigned)(n * (unsigned)j + (unsigned)i);
            a[7] ^= (unsigned char)t;
            a[6] ^= (unsigned char)(t >> 8);
            a[5] ^= (unsigned char)(t >> 16);
            a[4] ^= (unsigned char)(t >> 24);
            memcpy(b, a, 8);
            memcpy(b + 8, r[i - 1], 8);
            aes_decrypt(rk, b, b);
            memcpy(a, b, 8);
            memcpy(r[i - 1], b + 8, 8);
        }
    }
    for (int i = 0; i < 8; i++) if (a[i] != 0xa6) return -1;
    for (unsigned i = 0; i < n; i++) memcpy(out + i * 8, r[i], 8);
    return (int)(n * 8);
}

int wpa_take_m3(const unsigned char *frame, unsigned len, const unsigned char kck[16], const unsigned char kek[16],
                unsigned char gtk[16], unsigned char *m4, unsigned char *keyid) {
    if (!wpa_eapol_ok(kck, frame, len)) return -1;
    unsigned info = get16b(frame + 5);
    /* 0x1000 — зашифрованные данные ключа. 0x2000 — другое поле, у настоящего
     * третьего сообщения его нет, и проверка отбрасывала верную подпись. */
    if (!(info & 0x80) || !(info & 0x100) || !(info & 0x1000)) return -2;
    unsigned kd = get16b(frame + 97);
    unsigned char plain[256];
    if (99 + kd > len || kd < 8 || kd - 8 > sizeof(plain)) return -3;
    if (wpa_aes_unwrap(kek, frame + 99, kd, plain) < 0) return -3;
    const unsigned char *g = 0;
    unsigned char id = 0;
    for (unsigned i = 0; i + 8 + 16 <= kd - 8; i++) {
        if (plain[i] == 0xdd && plain[i+2] == 0x00 && plain[i+3] == 0x0f && plain[i+4] == 0xac && plain[i+5] == 1) {
            g = plain + i + 8;
            id = (unsigned char)(plain[i + 6] & 3);
            break;
        }
    }
    if (!g) return -3;
    if (keyid) *keyid = id;
    memcpy(gtk, g, 16);
    memset(m4, 0, 99);
    m4[0] = 1; m4[1] = 3; put16b(m4 + 2, 95); m4[4] = 2;
    put16b(m4 + 5, 0x030a); /* secure, MIC, pairwise, version 2 */
    memcpy(m4 + 9, frame + 9, 8);
    wpa_eapol_mic(kck, m4, 99);
    return 99;
}

static void copy_tok(char *dst, int cap, const char *s) {
    int i = 0;
    if (*s == '"') {
        s++;
        while (*s && *s != '"' && i + 1 < cap) dst[i++] = *s++;
    } else while (*s && *s != ' ' && *s != '\n' && *s != '\r' && i + 1 < cap) dst[i++] = *s++;
    dst[i] = 0;
}

int wpa_load(const char *text, struct wpa_net *out, int max) {
    int n = 0;
    struct wpa_net cur;
    int open = 0;
    memset(&cur, 0, sizeof(cur));
    if (!text) return 0;
    while (*text) {
        while (*text == ' ' || *text == '\t' || *text == '\r') text++;
        if (*text == '#' ) { while (*text && *text != '\n') text++; }
        else if (!strncmp(text, "network", 7)) { memset(&cur, 0, sizeof(cur)); open = 1; while (*text && *text != '\n') text++; }
        else if (*text == '}' && open) {
            if (n < max && cur.ssid[0]) out[n++] = cur;
            open = 0;
            text++;
        } else if (open && !strncmp(text, "ssid=", 5)) { copy_tok(cur.ssid, 33, text + 5); while (*text && *text != '\n') text++; }
        else if (open && !strncmp(text, "psk=", 4)) { copy_tok(cur.psk, 64, text + 4); cur.has_psk = cur.psk[0] != 0; while (*text && *text != '\n') text++; }
        else if (open && !strncmp(text, "priority=", 9)) { int v = 0, s = 1; const char *p = text + 9; if (*p == '-') { s = -1; p++; } while (*p >= '0' && *p <= '9') { v = v * 10 + *p - '0'; p++; } cur.priority = v * s; while (*text && *text != '\n') text++; }
        else { while (*text && *text != '\n') text++; }
        if (*text == '\n') text++;
    }
    return n;
}

int wpa_choose(const struct wpa_net *saved, int nsaved, const char ssids[][33], const int *secure, const int *rssi, int nvis) {
    int best = -1, prio = -1000000, sig = -999;
    for (int i = 0; i < nsaved; i++) {
        for (int v = 0; v < nvis; v++) {
            if (strcmp(saved[i].ssid, ssids[v])) continue;
            if (saved[i].has_psk && !secure[v]) continue;
            if (saved[i].priority > prio || (saved[i].priority == prio && rssi[v] > sig)) {
                best = i; prio = saved[i].priority; sig = rssi[v];
            }
        }
    }
    return best;
}

static int seen_before(const struct wpa_seen *a, const struct wpa_seen *b, const char *current) {
    int ca = current && current[0] && !strcmp(a->ssid, current);
    int cb = current && current[0] && !strcmp(b->ssid, current);
    if (ca != cb) return ca;
    if (a->saved != b->saved) return a->saved;
    if (a->bars != b->bars) return a->bars > b->bars;
    int c = strcmp(a->ssid, b->ssid);
    if (c) return c < 0;
    return memcmp(a->bssid, b->bssid, 6) < 0;
}

int wpa_order_seen(struct wpa_seen *v, int n, const char *current) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        int j = 0;
        while (j < m && strcmp(v[j].ssid, v[i].ssid)) j++;
        if (j == m) { if (m != i) v[m] = v[i]; m++; continue; }
        int secured = v[j].secured || v[i].secured, saved = v[j].saved || v[i].saved;
        if (v[i].rssi > v[j].rssi) v[j] = v[i];
        v[j].secured = secured;
        v[j].saved = saved;
    }
    for (int i = 1; i < m; i++) {
        struct wpa_seen t = v[i];
        int j = i;
        while (j > 0 && seen_before(&t, &v[j - 1], current)) { v[j] = v[j - 1]; j--; }
        v[j] = t;
    }
    return m;
}
