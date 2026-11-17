#include "wlan_frame.h"
#include "string.h"

static int fail;
static char msg[256];
static int outn;

static void put(const char *s) {
    while (*s && outn < (int)sizeof(msg) - 1) msg[outn++] = *s++;
}

static void expect(int cond, const char *what) {
    if (!cond) { put("FAIL "); put(what); put("\n"); fail++; }
}

static int same(const void *a, const void *b, unsigned n) { return memcmp(a, b, n) == 0; }

static void test_ccm(void) {
    u8 key[16], nonce[13], aad[8], plain[23], out[23], mic[8], back[23];
    for (int i = 0; i < 16; i++) key[i] = (u8)(0xc0 + i);
    u8 nsrc[13] = { 0,0,0,3,2,1,0,0xa0,0xa1,0xa2,0xa3,0xa4,0xa5 };
    memcpy(nonce, nsrc, 13);
    for (int i = 0; i < 8; i++) aad[i] = (u8)i;
    for (int i = 0; i < 23; i++) plain[i] = (u8)(8 + i);
    ccm_encrypt(key, nonce, aad, 8, plain, 23, out, mic);
    u8 want_c[23] = { 0x58,0x8c,0x97,0x9a,0x61,0xc6,0x63,0xd2,0xf0,0x66,0xd0,0xc2,0xc0,0xf9,0x89,0x80,0x6d,0x5f,0x6b,0x61,0xda,0xc3,0x84 };
    u8 want_m[8] = { 0x17,0xe8,0xd1,0x2c,0xfd,0xf9,0x26,0xe0 };
    expect(same(out, want_c, 23), "rfc3610 cipher");
    expect(same(mic, want_m, 8), "rfc3610 mic");
    expect(ccm_decrypt(key, nonce, aad, 8, out, 23, mic, back) == 0, "ccm decrypt");
    expect(same(back, plain, 23), "ccm plain");
    mic[0] ^= 1;
    expect(ccm_decrypt(key, nonce, aad, 8, out, 23, mic, back) != 0, "ccm bad mic");
}

static void test_frames(void) {
    u8 sa[6] = { 2,0,0,0,0,2 };
    u8 bssid[6] = { 2,0,0,0,0,1 };
    u8 buf[256], eth[64], back[128];
    int n = wlan_build_probe(buf, sizeof(buf), sa, (const u8 *)"MyOS", 4);
    expect(n > 24 && buf[0] == 0x40, "probe");
    u8 beacon[128];
    memset(beacon, 0, sizeof(beacon));
    beacon[0] = 0x80;
    memcpy(beacon + 16, bssid, 6);
    u8 *ie = beacon + 36;
    ie[0] = 0; ie[1] = 4; memcpy(ie + 2, "MyOS", 4);
    ie[6] = 3; ie[7] = 1; ie[8] = 6;
    ie[9] = 48; ie[10] = 2; ie[11] = 1; ie[12] = 0;
    struct wlan_parsed p;
    expect(wlan_parse_beacon(beacon, 36 + 13, &p) == 0, "parse");
    expect(p.channel == 6 && p.secured == 1 && strcmp(p.ssid, "MyOS") == 0, "beacon fields");
    n = wlan_build_auth(buf, sizeof(buf), bssid, sa);
    expect(n == 30 && buf[0] == 0xb0, "auth");
    buf[26] = 2;
    expect(wlan_auth_ok(buf, 30), "auth ok");
    n = wlan_build_assoc(buf, sizeof(buf), bssid, sa, (const u8 *)"MyOS", 4);
    expect(n > 28 && buf[0] == 0x00, "assoc");
    u8 dst[6] = { 0xff,0xff,0xff,0xff,0xff,0xff };
    memset(eth, 0, sizeof(eth));
    memcpy(eth, dst, 6);
    memcpy(eth + 6, sa, 6);
    eth[12] = 0x88; eth[13] = 0x8e; eth[14] = 1;
    expect(wlan_is_eapol_eth(eth, 15), "eapol");
    eth[12] = 0x08; eth[13] = 0x00;
    n = wlan_eth_to_wifi(buf, sizeof(buf), eth, 32, bssid, 1);
    expect(n == 50, "eth to wifi len");
    int m = wlan_wifi_to_eth(back, sizeof(back), buf, (u32)n);
    expect(m == 32 && same(back, eth, 32), "wifi to eth");
    u8 key[16], pn[6] = { 1,0,0,0,0,0 }, crypt[128], plain[128];
    for (int i = 0; i < 16; i++) key[i] = (u8)i;
    int c = wlan_ccmp_encrypt(crypt, sizeof(crypt), buf, (u32)n, key, pn, 0);
    expect(c == n + 16 && (crypt[1] & 0x40), "ccmp enc");
    int d = wlan_ccmp_decrypt(plain, sizeof(plain), crypt, (u32)c, key);
    expect(d == n && same(plain, buf, (unsigned)n), "ccmp dec");
    crypt[40] ^= 1;
    expect(wlan_ccmp_decrypt(plain, sizeof(plain), crypt, (u32)c, key) < 0, "ccmp tamper");
}

static void emit(void) {
#if defined(__x86_64__)
    long r;
    __asm__ volatile ("syscall" : "=a"(r) : "a"(1), "D"(1), "S"(msg), "d"(outn) : "rcx", "r11", "memory");
    (void)r;
#elif defined(__i386__)
    __asm__ volatile ("int $0x80" :: "a"(4), "b"(1), "c"(msg), "d"(outn));
#endif
}

int main(void) {
    test_ccm();
    test_frames();
    if (fail) put("wlan frames: FAIL\n");
    else put("wlan frames: PASS\n");
    emit();
    return fail ? 1 : 0;
}

#if defined(__x86_64__) && defined(WLAN_NOSTD)
void _start(void) {
    int rc = main();
    __asm__ volatile ("syscall" :: "a"(60), "D"(rc) : "rcx", "r11", "memory");
    for (;;) {}
}
#elif defined(__i386__) && defined(WLAN_NOSTD)
void _start(void) {
    int rc = main();
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(rc));
    for (;;) {}
}
#endif
