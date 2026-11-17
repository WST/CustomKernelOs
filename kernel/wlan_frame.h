#ifndef WLAN_FRAME_H
#define WLAN_FRAME_H
#include "types.h"

/* Разбор и сборка 802.11 и программный AES-CCM. Без вызовов ядра: тот же файл
 * гоняется тестом на хосте. Кадры данных — три адреса, QoS допускается на приёме. */

struct wlan_parsed {
    char ssid[33];
    u8 bssid[6];
    u8 channel;
    u8 secured;
    u8 ssid_len;
};

int wlan_parse_beacon(const u8 *mpdu, u32 len, struct wlan_parsed *out);
int wlan_build_probe(u8 *out, u32 cap, const u8 sa[6], const u8 *ssid, u8 ssid_len);
int wlan_build_auth(u8 *out, u32 cap, const u8 bssid[6], const u8 sa[6]);
int wlan_build_assoc(u8 *out, u32 cap, const u8 bssid[6], const u8 sa[6], const u8 *ssid, u8 ssid_len);
int wlan_auth_ok(const u8 *mpdu, u32 len);
int wlan_assoc_ok(const u8 *mpdu, u32 len);
int wlan_is_eapol_eth(const u8 *eth, u32 len);
int wlan_eth_to_wifi(u8 *out, u32 cap, const u8 *eth, u32 elen, const u8 bssid[6], u16 seq);
int wlan_wifi_to_eth(u8 *out, u32 cap, const u8 *mpdu, u32 mlen);
int ccm_encrypt(const u8 key[16], const u8 nonce[13], const u8 *aad, u32 aad_len,
                const u8 *plain, u32 len, u8 *out, u8 mic[8]);
int ccm_decrypt(const u8 key[16], const u8 nonce[13], const u8 *aad, u32 aad_len,
                const u8 *crypt, u32 len, const u8 mic[8], u8 *out);
int wlan_ccmp_encrypt(u8 *out, u32 cap, const u8 *mpdu, u32 len, const u8 key[16], const u8 pn[6], u8 keyid);
int wlan_ccmp_decrypt(u8 *out, u32 cap, const u8 *mpdu, u32 len, const u8 key[16]);

#endif
