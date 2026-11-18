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
    expect(n2 == 121 && wpa_eapol_ok(ptk, m2, 121), "m2 mic");
    expect(m2[2] == 0 && m2[3] == 117 && m2[97] == 0 && m2[98] == 22 && !memcmp(m2 + 99, wpa_rsn_ie, 22), "m2 rsn");
    m2[81] ^= 1;
    expect(!wpa_eapol_ok(ptk, m2, 121), "m2 bad mic");
    unsigned char gtk_in[16], kde[24], gtk_wrap[32], m3[160], gtk_out[16], m4[128];
    for (int i = 0; i < 16; i++) gtk_in[i] = (unsigned char)(0x40 + i);
    kde[0] = 0xdd; kde[1] = 22; kde[2] = 0x00; kde[3] = 0x0f; kde[4] = 0xac; kde[5] = 1;
    kde[6] = 1; kde[7] = 0;
    memcpy(kde + 8, gtk_in, 16);
    expect(wpa_aes_wrap(ptk + 16, kde, 24, gtk_wrap) == 32, "gtk wrap");
    memset(m3, 0, sizeof(m3));
    m3[0] = 1; m3[1] = 3; m3[2] = 0; m3[3] = 127; m3[4] = 2;
    m3[5] = 0x13; m3[6] = 0xca;
    memcpy(m3 + 17, anonce, 32);
    m3[97] = 0; m3[98] = 32;
    memcpy(m3 + 99, gtk_wrap, 32);
    wpa_eapol_mic(ptk, m3, 131);
    unsigned char kid = 9;
    expect(wpa_take_m3(m3, 131, ptk, ptk + 16, gtk_out, m4, &kid) == 99, "m3");
    /* Длиннее 128 байт MIC считается другим путём: его сверяем с эталоном HMAC-SHA1. */
    unsigned char kck[16], longf[200], want179[16] = {
        0xdf,0x8e,0x3c,0x80,0x2b,0xcb,0x60,0x1a,0x78,0xc4,0x51,0x38,0x3e,0x10,0xea,0xbb
    };
    for (int i = 0; i < 16; i++) kck[i] = (unsigned char)i;
    memset(longf, 0x5a, 179);
    wpa_eapol_mic(kck, longf, 179);
    expect(memcmp(longf + 81, want179, 16) == 0, "mic 179");
    expect(memcmp(gtk_out, gtk_in, 16) == 0 && kid == 1 && wpa_eapol_ok(ptk, m4, 99), "gtk and m4");
    unsigned char wide[72], wide_wrap[80], m3b[200];
    memset(wide, 0x11, sizeof(wide));
    wide[0] = 0xdd; wide[1] = 22; wide[2] = 0x00; wide[3] = 0x0f; wide[4] = 0xac; wide[5] = 1;
    wide[6] = 2; wide[7] = 0;
    memcpy(wide + 8, gtk_in, 16);
    expect(wpa_aes_wrap(ptk + 16, wide, 72, wide_wrap) == 80, "wide wrap");
    memset(m3b, 0, sizeof(m3b));
    m3b[0] = 1; m3b[1] = 3; m3b[2] = 0; m3b[3] = 175; m3b[4] = 2;
    m3b[5] = 0x13; m3b[6] = 0xca;
    memcpy(m3b + 17, anonce, 32);
    m3b[97] = 0; m3b[98] = 80;
    memcpy(m3b + 99, wide_wrap, 80);
    wpa_eapol_mic(ptk, m3b, 179);
    expect(wpa_take_m3(m3b, 179, ptk, ptk + 16, gtk_out, m4, &kid) == 99, "m3 179");
    expect(memcmp(gtk_out, gtk_in, 16) == 0 && kid == 2, "gtk from 80");
    const char *text = "network {\nssid=Home\npsk=secret\npriority=1\n}\nnetwork {\nssid=Cafe\npriority=5\n}\n";
    struct wpa_net nets[4];
    int n = wpa_load(text, nets, 4);
    expect(n == 2 && nets[0].has_psk && !nets[1].has_psk && nets[1].priority == 5, "profiles");
    char ssids[2][33];
    memset(ssids, 0, sizeof(ssids));
    memcpy(ssids[0], "Home", 4); memcpy(ssids[1], "Cafe", 4);
    int sec[2] = { 1, 0 }, rssi[2] = { -40, -70 };
    expect(wpa_choose(nets, n, ssids, sec, rssi, 2) == 1, "priority");
    struct wpa_seen seen[6];
    memset(seen, 0, sizeof(seen));
    const char *names[6] = { "Zeta", "Alpha", "Home", "Alpha", "Beta", "Cafe" };
    int bars[6] = { 3, 2, 1, 4, 3, 0 }, levels[6] = { -55, -65, -75, -45, -58, -90 };
    for (int i = 0; i < 6; i++) {
        strcpy(seen[i].ssid, names[i]);
        seen[i].bssid[5] = (unsigned char)i;
        seen[i].bars = bars[i];
        seen[i].rssi = levels[i];
    }
    seen[2].saved = 1;
    seen[1].secured = 1;
    int m = wpa_order_seen(seen, 6, "Cafe");
    expect(m == 5, "one row per ssid");
    expect(!strcmp(seen[0].ssid, "Cafe") && !strcmp(seen[1].ssid, "Home"), "current then saved");
    expect(!strcmp(seen[2].ssid, "Alpha") && seen[2].bars == 4 && seen[2].secured, "strongest bssid keeps lock");
    expect(!strcmp(seen[3].ssid, "Beta") && !strcmp(seen[4].ssid, "Zeta"), "equal bars by name");
    struct wpa_seen again[6];
    memcpy(again, seen, sizeof(again));
    struct wpa_seen t = again[3]; again[3] = again[4]; again[4] = t;
    wpa_order_seen(again, 5, "Cafe");
    expect(!memcmp(again, seen, 5 * sizeof(seen[0])), "order ignores arrival");
    if (fail) { printf("%d failures\n", fail); return 1; }
    printf("wifi wpa: PASS\n");
    return 0;
}
