#ifndef WLAN_H
#define WLAN_H
#include "types.h"

/* Управление из кольца 3. Номера совпадают с user/lib/myos.h. */
enum {
    WLAN_STATUS = 1, WLAN_SCAN = 2, WLAN_RESULT = 3, WLAN_CONNECT = 4,
    WLAN_DISCONNECT = 5, WLAN_EAPOL_TX = 6, WLAN_EAPOL_RX = 7,
    WLAN_SET_KEY = 8, WLAN_PORT = 9, WLAN_RADIO = 10, WLAN_COUNT = 11
};
enum { WLAN_DOWN = 0, WLAN_IDLE = 1, WLAN_SCANNING = 2, WLAN_ASSOC = 3, WLAN_EAPOL = 4, WLAN_RUN = 5, WLAN_BLOCKED = 6 };
enum { WLAN_KEY_PTK = 1, WLAN_KEY_GTK = 2 };

struct wlan_status_u {
    u32 state, bars, secured, blocked, channel;
    i32 rssi;
    u8 mac[6], bssid[6];
    char ssid[33];
    u8 pad;
};
struct wlan_bss_u {
    u32 index;
    i32 rssi;
    u32 channel, secured, bars;
    u8 bssid[6];
    char ssid[33];
    u8 pad[3];
};
struct wlan_connect_u {
    char ssid[33];
    u8 bssid[6];
    u8 has_bssid, secure;
};
struct wlan_eapol_u { u16 len; u8 data[512]; };
struct wlan_key_u { u8 kind, id; u8 key[16]; };

struct wlan_ops {
    int (*up)(void *dev);
    void (*down)(void *dev);
    int (*set_channel)(void *dev, int channel);
    int (*tx)(void *dev, const void *mpdu, u32 len);
    int (*radio)(void *dev); /* 1 радио выключено аппаратно */
};

int wlan_attach(const char *name, const u8 mac[6], const struct wlan_ops *ops, void *dev, int max_channel);
void wlan_detach(void);
void wlan_rx(const void *mpdu, u32 len, i32 rssi);
void wlan_hw_block(int blocked);
int wlan_ctl(int op, void *buf, u32 len);
int wlan_present(void);

#endif
