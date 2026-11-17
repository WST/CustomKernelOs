#ifndef WIFI_SHM_H
#define WIFI_SHM_H
/* Страницы между wifid и столом. Пароль лежит только во второй и стирается после чтения. */
#define WIFI_MAGIC 0x57494631u
enum { WIFI_CONNECT = 1, WIFI_DISCONNECT = 2, WIFI_FORGET = 3, WIFI_RADIO = 4, WIFI_SCAN = 5 };
struct wifi_ap {
    char ssid[33];
    unsigned char bssid[6];
    unsigned char channel, bars, secured, saved;
    int rssi;
};
struct wifi_pub {
    unsigned magic, seq, state, bars, secured, blocked, count, channel;
    int rssi;
    char ssid[33];
    char error[48];
    unsigned char mac[6], bssid[6];
    struct wifi_ap ap[16];
};
struct wifi_req {
    unsigned seq, op, priority;
    char ssid[33];
    char pass[64];
};
#endif
