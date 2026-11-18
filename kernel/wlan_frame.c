#include "wlan_frame.h"
#include "string.h"

/* AES-CCM с L=2 и MIC 8 байт: так у CCMP. Маска AAD — по 802.11: из FC убираются
 * retry, power и more-data, бит защиты ставится в 1, номер последовательности обнуляется. */

static const u8 sbox[256] = {
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

static u8 xt(u8 a) { return (u8)((a << 1) ^ ((a & 0x80) ? 0x1b : 0)); }

static void aes_encrypt(const u8 *rk, const u8 in[16], u8 out[16]) {
    u8 s[16];
    memcpy(s, in, 16);
    for (int i = 0; i < 16; i++) s[i] ^= rk[i];
    for (int round = 1; round <= 10; round++) {
        u8 t[16];
        for (int i = 0; i < 16; i++) t[i] = sbox[s[i]];
        u8 r1 = t[1], r2 = t[5], r3 = t[9], r4 = t[13];
        t[1] = t[5]; t[5] = t[9]; t[9] = t[13]; t[13] = r1;
        r1 = t[2]; t[2] = t[10]; t[10] = r1; r1 = t[6]; t[6] = t[14]; t[14] = r1;
        r1 = t[15]; t[15] = t[11]; t[11] = t[7]; t[7] = t[3]; t[3] = r1;
        (void)r2; (void)r3; (void)r4;
        if (round < 10) {
            for (int c = 0; c < 4; c++) {
                u8 a0 = t[c*4], a1 = t[c*4+1], a2 = t[c*4+2], a3 = t[c*4+3];
                t[c*4]   = (u8)(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
                t[c*4+1] = (u8)(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
                t[c*4+2] = (u8)(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
                t[c*4+3] = (u8)(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
            }
        }
        for (int i = 0; i < 16; i++) s[i] = t[i] ^ rk[round * 16 + i];
    }
    memcpy(out, s, 16);
}

static void aes_setup(const u8 key[16], u8 rk[176]) {
    static const u8 rcon[10] = { 0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };
    memcpy(rk, key, 16);
    for (int i = 4; i < 44; i++) {
        u8 t[4];
        memcpy(t, rk + (i - 1) * 4, 4);
        if ((i % 4) == 0) {
            u8 x = t[0];
            t[0] = (u8)(sbox[t[1]] ^ rcon[i / 4 - 1]);
            t[1] = sbox[t[2]];
            t[2] = sbox[t[3]];
            t[3] = sbox[x];
        }
        for (int j = 0; j < 4; j++) rk[i * 4 + j] = (u8)(rk[(i - 4) * 4 + j] ^ t[j]);
    }
}

static void xor16(u8 *d, const u8 *s) { for (int i = 0; i < 16; i++) d[i] ^= s[i]; }

static void ccm_blocks(const u8 key[16], const u8 nonce[13], const u8 *aad, u32 aad_len,
                       const u8 *in, u32 len, u8 *out, u8 mic[8], int enc) {
    u8 rk[176], b[16], s[16], ctr[16];
    aes_setup(key, rk);
    memset(b, 0, 16);
    b[0] = (u8)(0x40 | (3 << 3) | 1); /* Adata, M=8, L=2 */
    if (!aad_len) b[0] = (u8)((3 << 3) | 1);
    memcpy(b + 1, nonce, 13);
    b[14] = (u8)(len >> 8);
    b[15] = (u8)len;
    aes_encrypt(rk, b, s);
    if (aad_len) {
        u8 block[16];
        memset(block, 0, 16);
        block[0] = (u8)(aad_len >> 8);
        block[1] = (u8)aad_len;
        u32 n = aad_len > 14 ? 14 : aad_len;
        memcpy(block + 2, aad, n);
        xor16(s, block);
        aes_encrypt(rk, s, s);
        u32 off = n;
        while (off < aad_len) {
            memset(block, 0, 16);
            u32 c = aad_len - off > 16 ? 16 : aad_len - off;
            memcpy(block, aad + off, c);
            xor16(s, block);
            aes_encrypt(rk, s, s);
            off += c;
        }
    }
    memset(ctr, 0, 16);
    ctr[0] = 1;
    memcpy(ctr + 1, nonce, 13);
    u8 s0[16];
    aes_encrypt(rk, ctr, s0);
    for (u32 off = 0; off < len; off += 16) {
        u32 c = len - off > 16 ? 16 : len - off;
        u16 n = (u16)(off / 16 + 1);
        ctr[14] = (u8)(n >> 8);
        ctr[15] = (u8)n;
        u8 ks[16], block[16];
        aes_encrypt(rk, ctr, ks);
        memset(block, 0, 16);
        if (enc) {
            memcpy(block, in + off, c);
            xor16(s, block);
            aes_encrypt(rk, s, s);
            for (u32 i = 0; i < c; i++) out[off + i] = (u8)(in[off + i] ^ ks[i]);
        } else {
            for (u32 i = 0; i < c; i++) {
                out[off + i] = (u8)(in[off + i] ^ ks[i]);
                block[i] = out[off + i];
            }
            xor16(s, block);
            aes_encrypt(rk, s, s);
        }
    }
    for (int i = 0; i < 8; i++) mic[i] = (u8)(s[i] ^ s0[i]);
}

int ccm_encrypt(const u8 key[16], const u8 nonce[13], const u8 *aad, u32 aad_len,
                const u8 *plain, u32 len, u8 *out, u8 mic[8]) {
    ccm_blocks(key, nonce, aad, aad_len, plain, len, out, mic, 1);
    return 0;
}

int ccm_decrypt(const u8 key[16], const u8 nonce[13], const u8 *aad, u32 aad_len,
                const u8 *crypt, u32 len, const u8 mic[8], u8 *out) {
    u8 got[8];
    ccm_blocks(key, nonce, aad, aad_len, crypt, len, out, got, 0);
    u8 diff = 0;
    for (int i = 0; i < 8; i++) diff |= (u8)(got[i] ^ mic[i]);
    return diff ? -1 : 0;
}

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static u16 get16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }

static int hdr_len(const u8 *fc) {
    if ((fc[0] & 0x0c) != 0x08) return 24;
    int qos = (fc[0] & 0x80) != 0;
    int a4 = (fc[1] & 0x03) == 0x03;
    /* У QoS-кадра бит Order — это 4 байта HT Control перед телом, не «строгий порядок». */
    int htc = qos && (fc[1] & 0x80);
    return 24 + (a4 ? 6 : 0) + (qos ? 2 : 0) + (htc ? 4 : 0);
}

static void ccmp_aad(const u8 *hdr, u8 aad[24], u32 *alen) {
    int qos = (hdr[0] & 0x0c) == 0x08 && (hdr[0] & 0x80);
    aad[0] = (u8)(hdr[0] & 0x8f);
    /* Бит защиты в подписи кадра обязан быть 1. Ноль не сходится с точкой,
     * и она выбрасывает DHCP, хотя кадр по радио принят. */
    aad[1] = (u8)((hdr[1] & 0xc7) | 0x40);
    if (qos) aad[1] = (u8)(aad[1] & ~0x80);
    memcpy(aad + 2, hdr + 4, 18);
    aad[20] = (u8)(hdr[22] & 0x0f);
    aad[21] = 0;
    *alen = 22;
    if (qos) {
        aad[22] = (u8)(hdr[24] & 0x0f);
        aad[23] = 0;
        *alen = 24;
    }
}

static void ccmp_nonce(const u8 *hdr, const u8 pn[6], u8 nonce[13]) {
    int qos = (hdr[0] & 0x0c) == 0x08 && (hdr[0] & 0x80);
    nonce[0] = qos ? (u8)(hdr[24] & 0x0f) : 0;
    memcpy(nonce + 1, hdr + 10, 6);
    for (int i = 0; i < 6; i++) nonce[7 + i] = pn[5 - i];
}

int wlan_ccmp_encrypt(u8 *out, u32 cap, const u8 *mpdu, u32 len, const u8 key[16], const u8 pn[6], u8 keyid) {
    if (!mpdu || len < 24 || (mpdu[0] & 0x0c) != 0x08) return -1;
    u32 h = (u32)hdr_len(mpdu);
    if (len < h || cap < len + 16) return -1;
    memcpy(out, mpdu, h);
    out[1] = (u8)(out[1] | 0x40);
    u8 *ccmp = out + h;
    ccmp[0] = pn[0]; ccmp[1] = pn[1]; ccmp[2] = 0; ccmp[3] = (u8)(0x20 | ((keyid & 3) << 6));
    ccmp[4] = pn[2]; ccmp[5] = pn[3]; ccmp[6] = pn[4]; ccmp[7] = pn[5];
    u8 aad[24], nonce[13], mic[8];
    u32 alen = 0;
    ccmp_aad(out, aad, &alen);
    ccmp_nonce(out, pn, nonce);
    ccm_encrypt(key, nonce, aad, alen, mpdu + h, len - h, ccmp + 8, mic);
    memcpy(ccmp + 8 + (len - h), mic, 8);
    return (int)(len + 16);
}

int wlan_ccmp_aad_sample(void) {
    u8 hdr[24] = {
        0x08, 0x48, 0xc3, 0x2c, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30,
        0xf1, 0x84, 0x44, 0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x80, 0x33
    };
    u8 want[22] = {
        0x08, 0x40, 0x0f, 0xd2, 0xe1, 0x28, 0xa5, 0x7c, 0x50, 0x30, 0xf1, 0x84,
        0x44, 0x08, 0xab, 0xae, 0xa5, 0xb8, 0xfc, 0xba, 0x00, 0x00
    };
    u8 aad[24];
    u32 alen = 0;
    ccmp_aad(hdr, aad, &alen);
    return alen == 22 && memcmp(aad, want, 22) == 0;
}

int wlan_ccmp_decrypt(u8 *out, u32 cap, const u8 *mpdu, u32 len, const u8 key[16]) {
    if (!mpdu || len < 24 + 16 || (mpdu[0] & 0x0c) != 0x08 || !(mpdu[1] & 0x40)) return -1;
    u32 h = (u32)hdr_len(mpdu);
    if (len < h + 16 || cap < len - 16) return -1;
    const u8 *ccmp = mpdu + h;
    if (!(ccmp[3] & 0x20)) return -1;
    u8 pn[6] = { ccmp[0], ccmp[1], ccmp[4], ccmp[5], ccmp[6], ccmp[7] };
    u32 blen = len - h - 16;
    u8 aad[24], nonce[13];
    u32 alen = 0;
    u8 hdr[32];
    memcpy(hdr, mpdu, h > 32 ? 32 : h);
    hdr[1] = (u8)(hdr[1] & ~0x40);
    ccmp_aad(hdr, aad, &alen);
    ccmp_nonce(hdr, pn, nonce);
    if (ccm_decrypt(key, nonce, aad, alen, ccmp + 8, blen, ccmp + 8 + blen, out + h)) return -1;
    memcpy(out, hdr, h);
    return (int)(h + blen);
}

static void mgmt_hdr(u8 *o, u8 fc0, const u8 da[6], const u8 sa[6], const u8 bssid[6]) {
    o[0] = fc0; o[1] = 0;
    put16(o + 2, 0);
    memcpy(o + 4, da, 6);
    memcpy(o + 10, sa, 6);
    memcpy(o + 16, bssid, 6);
    put16(o + 22, 0);
}

int wlan_build_probe(u8 *out, u32 cap, const u8 sa[6], const u8 *ssid, u8 ssid_len) {
    u8 bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
    if (cap < (u32)(24 + 2 + ssid_len + 10) || ssid_len > 32) return -1;
    mgmt_hdr(out, 0x40, bcast, sa, bcast);
    u8 *p = out + 24;
    *p++ = 0; *p++ = ssid_len;
    if (ssid_len) memcpy(p, ssid, ssid_len);
    p += ssid_len;
    *p++ = 1; *p++ = 8;
    memcpy(p, "\x82\x84\x8b\x96\x0c\x12\x18\x24", 8);
    p += 8;
    return (int)(p - out);
}

int wlan_parse_beacon(const u8 *mpdu, u32 len, struct wlan_parsed *out) {
    if (!mpdu || !out || len < 36) return -1;
    u8 fc = mpdu[0];
    if (fc != 0x80 && fc != 0x50) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->bssid, mpdu + 16, 6);
    const u8 *p = mpdu + 36;
    const u8 *end = mpdu + len;
    while (p + 2 <= end && p + 2 + p[1] <= end) {
        u8 id = p[0], n = p[1];
        const u8 *v = p + 2;
        if (id == 0 && n <= 32) { memcpy(out->ssid, v, n); out->ssid[n] = 0; out->ssid_len = n; }
        if (id == 3 && n == 1) out->channel = v[0];
        if (id == 48 && n >= 2) out->secured = 1;
        p += 2 + n;
    }
    return 0;
}

int wlan_build_auth(u8 *out, u32 cap, const u8 bssid[6], const u8 sa[6]) {
    if (cap < 30) return -1;
    mgmt_hdr(out, 0xb0, bssid, sa, bssid);
    put16(out + 24, 0);
    put16(out + 26, 1);
    put16(out + 28, 0);
    return 30;
}

/* Тот же RSN, что во втором сообщении рукопожатия: точка сверяет их побайтно. */
const u8 wlan_rsn_ie[22] = {
    48, 20, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 4, 1, 0, 0x00, 0x0f, 0xac, 2, 0, 0
};

int wlan_build_assoc(u8 *out, u32 cap, const u8 bssid[6], const u8 sa[6], const u8 *ssid, u8 ssid_len, int secure) {
    if (ssid_len > 32 || cap < (u32)(24 + 4 + 2 + ssid_len + 10 + 6 + 9 + 22)) return -1;
    mgmt_hdr(out, 0x00, bssid, sa, bssid);
    put16(out + 24, secure ? 0x0431 : 0x0421);
    put16(out + 26, 10);
    u8 *p = out + 28;
    *p++ = 0; *p++ = ssid_len;
    if (ssid_len) memcpy(p, ssid, ssid_len);
    p += ssid_len;
    *p++ = 1; *p++ = 8;
    memcpy(p, "\x82\x84\x8b\x96\x0c\x12\x18\x24", 8);
    p += 8;
    *p++ = 50; *p++ = 4;
    memcpy(p, "\x30\x48\x60\x6c", 4);
    p += 4;
    /* WMM: без этого элемента точка не ждёт QoS и отбрасывает обычные данные. */
    static const u8 wmm[9] = { 0xdd, 7, 0x00, 0x50, 0xf2, 2, 0, 1, 0 };
    memcpy(p, wmm, sizeof(wmm));
    p += sizeof(wmm);
    if (secure) { memcpy(p, wlan_rsn_ie, sizeof(wlan_rsn_ie)); p += sizeof(wlan_rsn_ie); }
    return (int)(p - out);
}

int wlan_auth_ok(const u8 *mpdu, u32 len) {
    return mpdu && len >= 30 && mpdu[0] == 0xb0 && get16(mpdu + 24) == 0 && get16(mpdu + 26) == 2 && get16(mpdu + 28) == 0;
}

int wlan_assoc_ok(const u8 *mpdu, u32 len) {
    return mpdu && len >= 30 && mpdu[0] == 0x10 && get16(mpdu + 26) == 0;
}

int wlan_is_eapol_eth(const u8 *eth, u32 len) {
    return eth && len >= 14 && eth[12] == 0x88 && eth[13] == 0x8e;
}

int wlan_eth_to_wifi(u8 *out, u32 cap, const u8 *eth, u32 elen, const u8 bssid[6], u16 seq, int qos) {
    u32 extra = qos ? 2u : 0u;
    if (!eth || elen < 14 || cap < elen + 18 + extra) return -1;
    memset(out, 0, 24 + extra);
    out[0] = qos ? 0x88 : 0x08;
    out[1] = 0x01;
    memcpy(out + 4, bssid, 6);
    memcpy(out + 10, eth + 6, 6);
    memcpy(out + 16, eth, 6);
    put16(out + 22, (u16)((seq & 0x0fff) << 4));
    u8 *p = out + 24;
    if (qos) { p[0] = p[1] = 0; p += 2; }
    p[0] = 0xaa; p[1] = 0xaa; p[2] = 0x03;
    p[3] = p[4] = p[5] = 0;
    memcpy(p + 6, eth + 12, elen - 12);
    return (int)(elen + 18 + extra);
}

int wlan_wifi_to_eth(u8 *out, u32 cap, const u8 *mpdu, u32 mlen) {
    if (!mpdu || (mpdu[0] & 0x0c) != 0x08) return -1;
    if (mpdu[1] & 0x40) return -1;
    u32 h = (u32)hdr_len(mpdu);
    if (mlen < h + 8 || cap < mlen - h + 6) return -1;
    const u8 *llc = mpdu + h;
    if (llc[0] != 0xaa || llc[1] != 0xaa || llc[2] != 0x03) return -1;
    int from = mpdu[1] & 0x02;
    const u8 *dst = from ? mpdu + 4 : mpdu + 16;
    const u8 *src = from ? mpdu + 16 : mpdu + 10;
    memcpy(out, dst, 6);
    memcpy(out + 6, src, 6);
    memcpy(out + 12, llc + 6, mlen - h - 6);
    return (int)(mlen - h + 6);
}
