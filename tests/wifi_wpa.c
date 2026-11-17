#include "wpa.h"
#include <stdio.h>
#include <string.h>

static int fail;
static void expect(int c, const char *w) { if (!c) { printf("FAIL %s\n", w); fail++; } }

int main(void) {
    unsigned char pmk[32], want[32] = {
        0xf4,0x2c,0x6f,0xc5,0x2d,0xf0,0xeb,0xef,0x9e,0xbb,0x4b,0x90,0xb3,0x8a,0x5f,0x90,
        0x2e,0x83,0xfe,0x1b,0x13,0x5a,0x70,0xe2,0x3a,0xed,0x76,0x2e,0x97,0x10,0xa1,0x2e
    };
    expect(wpa_aes_roundtrip() == 0, "aes roundtrip");
    if (wpa_aes_roundtrip()) printf("aes code %d\n", wpa_aes_roundtrip());
    wpa_pmk("password", "IEEE", pmk);
    expect(memcmp(pmk, want, 32) == 0, "pbkdf2");
    unsigned char kek[16], plain[16], wrapped[24] = {
        0x1f,0xa6,0x8b,0x0a,0x81,0x12,0xb4,0x47,0xae,0xf3,0x4b,0xd8,0xfb,0x5a,0x7b,0x82,
        0x9d,0x3e,0x86,0x23,0x71,0xd2,0xcf,0xe5
    };
    for (int i = 0; i < 16; i++) kek[i] = (unsigned char)i;
    unsigned char expect_p[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    unsigned char mine[24];
    expect(wpa_aes_wrap(kek, expect_p, 16, mine) == 24, "wrap len");
    expect(memcmp(mine, wrapped, 24) == 0, "wrap vector");
    expect(wpa_aes_unwrap(kek, wrapped, 24, plain) == 16, "unwrap len");
    expect(memcmp(plain, expect_p, 16) == 0, "unwrap");
    if (memcmp(plain, expect_p, 16)) {
        printf(" got");
        for (int i = 0; i < 16; i++) printf(" %02x", plain[i]);
        printf("\n");
    }
    unsigned char aa[6] = { 2,0,0,0,0,1 }, spa[6] = { 2,0,0,0,0,2 }, anonce[32], snonce[32], ptk[64];
    for (int i = 0; i < 32; i++) { anonce[i] = (unsigned char)i; snonce[i] = (unsigned char)(32 + i); }
    wpa_ptk(pmk, aa, spa, anonce, snonce, ptk);
    unsigned char m1[99];
    memset(m1, 0, sizeof(m1));
    m1[0] = 1; m1[1] = 3; m1[2] = 0; m1[3] = 95; m1[4] = 2; m1[5] = 0x00; m1[6] = 0x8a;
    memcpy(m1 + 17, anonce, 32);
    unsigned char got_a[32], replay[8];
    expect(wpa_read_m1(m1, 99, got_a, replay) == 0, "m1");
    expect(memcmp(got_a, anonce, 32) == 0, "anonce");
    unsigned char m2[128];
    int n2 = wpa_build_m2(m2, replay, snonce, ptk);
    expect(n2 == 99 && wpa_eapol_ok(ptk, m2, 99), "m2 mic");
    m2[81] ^= 1;
    expect(!wpa_eapol_ok(ptk, m2, 99), "m2 bad mic");
    const char *text = "network {\nssid=Home\npsk=secret\npriority=1\n}\nnetwork {\nssid=Cafe\npriority=5\n}\n";
    struct wpa_net nets[4];
    int n = wpa_load(text, nets, 4);
    expect(n == 2 && nets[0].has_psk && !nets[1].has_psk && nets[1].priority == 5, "profiles");
    char ssids[2][33];
    memset(ssids, 0, sizeof(ssids));
    memcpy(ssids[0], "Home", 4); memcpy(ssids[1], "Cafe", 4);
    int sec[2] = { 1, 0 }, rssi[2] = { -40, -70 };
    expect(wpa_choose(nets, n, ssids, sec, rssi, 2) == 1, "priority");
    if (fail) { printf("%d failures\n", fail); return 1; }
    printf("wifi wpa: PASS\n");
    return 0;
}
