#ifndef WPA_H
#define WPA_H

/* WPA2-PSK: пароль остаётся в службе. В ядро уходят только PTK и GTK. */

struct wpa_net {
    char ssid[33];
    char psk[64];
    int priority;
    int has_psk;
};

void wpa_pmk(const char *pass, const char *ssid, unsigned char pmk[32]);
void wpa_ptk(const unsigned char pmk[32], const unsigned char aa[6], const unsigned char spa[6],
             const unsigned char anonce[32], const unsigned char snonce[32], unsigned char ptk[64]);
int wpa_eapol_mic(const unsigned char kck[16], unsigned char *frame, unsigned len);
int wpa_eapol_ok(const unsigned char kck[16], const unsigned char *frame, unsigned len);
int wpa_read_m1(const unsigned char *frame, unsigned len, unsigned char anonce[32], unsigned char replay[8]);
/* out — не меньше 121 байта: в Key Data лежит wpa_rsn_ie. */
int wpa_build_m2(unsigned char *out, const unsigned char replay[8], const unsigned char snonce[32], const unsigned char kck[16]);
extern const unsigned char wpa_rsn_ie[22];
/* 99 — успех. -1 подпись, -2 не то сообщение, -3 свёртка или нет GTK. keyid может быть NULL. */
int wpa_take_m3(const unsigned char *frame, unsigned len, const unsigned char kck[16], const unsigned char kek[16],
                unsigned char gtk[16], unsigned char *m4, unsigned char *keyid);
int wpa_aes_wrap(const unsigned char kek[16], const unsigned char *in, unsigned inlen, unsigned char *out);
int wpa_aes_unwrap(const unsigned char kek[16], const unsigned char *in, unsigned inlen, unsigned char *out);
int wpa_aes_roundtrip(void);
int wpa_load(const char *text, struct wpa_net *out, int max);
int wpa_choose(const struct wpa_net *saved, int nsaved, const char ssids[][33], const int *secure, const int *rssi, int nvis);

/* Строка списка сетей для интерфейса. */
struct wpa_seen {
    char ssid[33];
    unsigned char bssid[6];
    int rssi, bars, secured, channel, saved;
};
/* Одна строка на SSID с лучшим сигналом. Порядок не зависит от прихода маяков:
 * текущая сеть, сохранённые, деления сигнала, имя. Возвращает новое число строк. */
int wpa_order_seen(struct wpa_seen *v, int n, const char *current);

#endif
