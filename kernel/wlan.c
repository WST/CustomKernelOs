#include "wlan.h"
#include "wlan_frame.h"
#include "net.h"
#include "module.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "syscall.h"
#include "klog.h"

/* Одна карта. Кадры EAPOL не попадают в IP. Ключ CCMP применяется здесь, не в устройстве. */

#define BSS_MAX 64
#define EQ 8

/* Сколько сканов подряд точка может не ответить и остаться в списке. */
#define BSS_KEEP 2

struct bss {
    char ssid[33];
    u8 bssid[6];
    u8 channel, secured, miss;
    i32 rssi;
};

static struct {
    struct netif *iface;
    struct wlan_ops ops;
    void *dev, *mod;
    u8 mac[6], bssid[6], ptk[16], gtk[16], gtk_id, txpn[6];
    char ssid[33];
    int state, blocked, port, channel, max_ch, secure;
    int have_ptk, have_gtk, nlist, seq;
    int auth_ok, assoc_ok, auth_seen, assoc_seen;
    u16 auth_status, assoc_status, aid;
    i32 rssi;
    struct bss *list, *scan;
    int nscan;
    u8 *qmem;
    u16 eapol_len[4];
    int eapol_n, eapol_h;
    u16 ethlen[EQ];
    int ethn, ethh;
} W;

/* Очереди не в BSS: иначе образ наезжает на загрузочный стек 0x9b000. */
static u8 *eapol_at(int i) { return W.qmem + i * 512; }
static u8 *eth_at(int i) { return W.qmem + 4 * 512 + i * 1600; }

int wlan_present(void) { return W.iface != NULL; }

static int bars_of(i32 rssi) {
    if (rssi >= -50) return 4;
    if (rssi >= -60) return 3;
    if (rssi >= -70) return 2;
    if (rssi >= -80) return 1;
    return 0;
}

static void pn_inc(u8 pn[6]) {
    for (int i = 0; i < 6; i++) { pn[i]++; if (pn[i]) break; }
}

static int queue_eth(const u8 *eth, u32 len) {
    if (len > 1600 || W.ethn >= EQ) return -1;
    int i = (W.ethh + W.ethn) % EQ;
    memcpy(eth_at(i), eth, len);
    W.ethlen[i] = (u16)len;
    W.ethn++;
    return 0;
}

static int wlan_recv(struct netif *n, void *buf, u32 cap) {
    (void)n;
    if (!W.ethn) return 0;
    u32 nlen = W.ethlen[W.ethh];
    if (nlen > cap) nlen = cap;
    memcpy(buf, eth_at(W.ethh), nlen);
    W.ethh = (W.ethh + 1) % EQ;
    W.ethn--;
    return (int)nlen;
}

static bool wlan_linked(struct netif *n) {
    (void)n;
    return W.port && !W.blocked && W.state == WLAN_RUN;
}

static int wlan_send(struct netif *n, const void *frame, u32 len) {
    (void)n;
    const u8 *eth = frame;
    int eapol = wlan_is_eapol_eth(eth, len);
    if ((!W.port && !eapol) || !W.ops.tx || W.blocked) return -1;
    u8 mpdu[1800], crypt[1900];
    /* Рукопожатие остаётся без QoS: так точка его уже приняла. Остальные данные — QoS. */
    int m = wlan_eth_to_wifi(mpdu, sizeof(mpdu), frame, len, W.bssid, (u16)W.seq++, eapol ? 0 : 1);
    if (m < 0) return -1;
    /* Станция всегда шлёт точке (To DS), даже широковещание IP. Такой кадр
     * шифруется парным ключом. Групповой — только у кадров от точки. */
    if (W.have_ptk && !eapol) {
        pn_inc(W.txpn);
        int c = wlan_ccmp_encrypt(crypt, sizeof(crypt), mpdu, (u32)m, W.ptk, W.txpn, 0);
        if (c < 0) return -1;
        return W.ops.tx(W.dev, crypt, (u32)c) ? -1 : 0;
    }
    return W.ops.tx(W.dev, mpdu, (u32)m) ? -1 : 0;
}

/* Скан собирает новый список отдельно: видимый не пустеет на время обхода каналов.
 * Маяки между сканами список не трогают, иначе уровень у порога делений
 * переставляет строки несколько раз в секунду. */
static void remember(const struct wlan_parsed *p, i32 rssi) {
    if (!p->ssid_len && !p->ssid[0]) return;
    if (W.state != WLAN_SCANNING || !W.scan) return;
    for (int i = 0; i < W.nscan; i++) {
        if (!memcmp(W.scan[i].bssid, p->bssid, 6)) {
            if (rssi > W.scan[i].rssi) W.scan[i].rssi = rssi;
            W.scan[i].channel = p->channel;
            W.scan[i].secured = p->secured;
            return;
        }
    }
    if (W.nscan >= BSS_MAX) return;
    struct bss *b = &W.scan[W.nscan++];
    memset(b, 0, sizeof(*b));
    memcpy(b->ssid, p->ssid, 33);
    memcpy(b->bssid, p->bssid, 6);
    b->channel = p->channel;
    b->secured = p->secured;
    b->rssi = rssi;
}

void wlan_rx(const void *pkt, u32 len, i32 rssi) {
    const u8 *mpdu = pkt;
    if (!mpdu || len < 24 || !W.iface) return;
    struct wlan_parsed parsed;
    if (wlan_parse_beacon(mpdu, len, &parsed) == 0) { remember(&parsed, rssi); return; }
    if (len >= 30 && (mpdu[0] == 0xb0 || mpdu[0] == 0x10 || mpdu[0] == 0x30) && !memcmp(mpdu + 16, W.bssid, 6)) {
        u16 status = (u16)(mpdu[26] | (mpdu[27] << 8));
        if (mpdu[0] == 0xb0) {
            W.auth_status = status;
            W.auth_seen = 1;
            if (wlan_auth_ok(mpdu, len)) W.auth_ok = 1;
        } else {
            W.assoc_status = status;
            W.assoc_seen = 1;
            if (status == 0) {
                W.aid = (u16)((mpdu[28] | (mpdu[29] << 8)) & 0x3fff);
                W.assoc_ok = 1;
            }
        }
        return;
    }
    if ((mpdu[0] & 0x0c) != 0x08) return;
    u8 plain[1800], eth[1600];
    const u8 *use = mpdu;
    u32 ulen = len;
    if (mpdu[1] & 0x40) {
        int group = mpdu[4] & 1;
        const u8 *key = (group && W.have_gtk) ? W.gtk : (W.have_ptk ? W.ptk : NULL);
        if (!key) return;
        int d = wlan_ccmp_decrypt(plain, sizeof(plain), mpdu, len, key);
        if (d < 0) return;
        use = plain;
        ulen = (u32)d;
    }
    int e = wlan_wifi_to_eth(eth, sizeof(eth), use, ulen);
    if (e < 14) return;
    if (wlan_is_eapol_eth(eth, (u32)e)) {
        if (W.eapol_n >= 4 || (u32)e - 14 > 512) return;
        int i = (W.eapol_h + W.eapol_n) % 4;
        memcpy(eapol_at(i), eth + 14, (u32)e - 14);
        W.eapol_len[i] = (u16)(e - 14);
        W.eapol_n++;
        klogf("WLAN: eapol %u\n", (u32)e - 14);
        return;
    }
    queue_eth(eth, (u32)e);
    if (rssi) W.rssi = rssi;
}

void wlan_hw_block(int blocked) {
    W.blocked = blocked ? 1 : 0;
    if (W.blocked) { W.port = 0; W.state = WLAN_BLOCKED; }
    else if (W.state == WLAN_BLOCKED) W.state = WLAN_IDLE;
}

int wlan_attach(const char *name, const u8 mac[6], const struct wlan_ops *ops, void *dev, int max_channel) {
    if (W.iface || !ops || !mac) return -EBUSY;
    memset(&W, 0, sizeof(W));
    memcpy(W.mac, mac, 6);
    W.ops = *ops;
    W.dev = dev;
    W.max_ch = max_channel >= 1 && max_channel <= 13 ? max_channel : 11;
    W.channel = 1;
    W.state = WLAN_IDLE;
    W.qmem = kmalloc(4 * 512 + EQ * 1600 + 2 * BSS_MAX * sizeof(struct bss));
    if (!W.qmem) { memset(&W, 0, sizeof(W)); return -ENOMEM; }
    W.list = (struct bss *)(W.qmem + 4 * 512 + EQ * 1600);
    W.scan = W.list + BSS_MAX;
    W.mod = module_current();
    if (W.mod) module_hold(W.mod);
    struct netif proto;
    memset(&proto, 0, sizeof(proto));
    for (int i = 0; name && name[i] && i < 7; i++) proto.name[i] = name[i];
    if (!proto.name[0]) { proto.name[0] = 'w'; proto.name[1] = '0'; }
    memcpy(proto.mac, mac, 6);
    proto.kind = NETIF_WLAN;
    proto.prefer = 1;
    proto.send = wlan_send;
    proto.link = wlan_linked;
    proto.recv = wlan_recv;
    W.iface = netif_add(&proto);
    if (!W.iface) { if (W.mod) module_release(W.mod); kfree(W.qmem); memset(&W, 0, sizeof(W)); return -ENOMEM; }
    if (W.ops.up) W.ops.up(W.dev);
    return 0;
}

void wlan_detach(void) {
    if (!W.iface) return;
    W.port = 0;
    if (W.ops.down) W.ops.down(W.dev);
    netif_del(W.iface);
    if (W.mod) module_release(W.mod);
    kfree(W.qmem);
    memset(&W, 0, sizeof(W));
}

static struct bss *find_ssid(const char *ssid, const u8 *bssid, int has) {
    struct bss *best = NULL;
    for (int i = 0; i < W.nlist; i++) {
        if (strcmp(W.list[i].ssid, ssid)) continue;
        if (has && memcmp(W.list[i].bssid, bssid, 6)) continue;
        if (!best || W.list[i].rssi > best->rssi) best = &W.list[i];
    }
    return best;
}

static int tx_mpdu(const u8 *mpdu, int n) {
    if (n < 0 || !W.ops.tx) return -ENODEV;
    return W.ops.tx(W.dev, mpdu, (u32)n);
}

static int do_scan(void) {
    if (!W.iface) return -ENODEV;
    if (W.blocked) return -EPERM;
    int prev = W.state;
    W.nscan = 0;
    W.state = WLAN_SCANNING;
    if (W.ops.scan) {
        W.ops.scan(W.dev);
    } else {
        u8 probe[128];
        for (int ch = 1; ch <= W.max_ch; ch++) {
            if (W.ops.set_channel && W.ops.set_channel(W.dev, ch)) continue;
            W.channel = ch;
            int n = wlan_build_probe(probe, sizeof(probe), W.mac, NULL, 0);
            tx_mpdu(probe, n);
            sleep_ms(20);
        }
    }
    W.state = prev == WLAN_RUN || prev == WLAN_EAPOL ? prev : WLAN_IDLE;
    for (int i = 0; i < W.nlist && W.nscan < BSS_MAX; i++) {
        int seen = 0;
        for (int j = 0; j < W.nscan && !seen; j++) seen = !memcmp(W.scan[j].bssid, W.list[i].bssid, 6);
        if (seen || W.list[i].miss >= BSS_KEEP) continue;
        W.scan[W.nscan] = W.list[i];
        W.scan[W.nscan++].miss++;
    }
    memcpy(W.list, W.scan, (u32)W.nscan * sizeof(struct bss));
    W.nlist = W.nscan;
    return W.nlist;
}

static int do_connect(struct wlan_connect_u *c) {
    if (!W.iface) return -ENODEV;
    if (W.blocked) return -EPERM;
    c->ssid[32] = 0;
    if (W.nlist == 0) do_scan();
    struct bss *b = find_ssid(c->ssid, c->bssid, c->has_bssid);
    if (!b) return -ENOENT;
    W.auth_ok = W.assoc_ok = W.auth_seen = W.assoc_seen = 0;
    W.eapol_n = W.eapol_h = 0;
    W.port = 0;
    W.have_ptk = W.have_gtk = 0;
    W.secure = b->secured || c->secure;
    int rsn = b->secured;
    memcpy(W.ssid, b->ssid, 33);
    memcpy(W.bssid, b->bssid, 6);
    W.channel = b->channel ? b->channel : 1;
    W.rssi = b->rssi;
    W.state = WLAN_ASSOC;
    if (W.ops.set_channel) W.ops.set_channel(W.dev, W.channel);
    u8 frame[256];
    /* Три попытки, как у обычных клиентов: точка может пропустить кадр, пока занята. */
    for (int t = 0; t < 3 && !W.auth_ok && !W.auth_seen; t++) {
        tx_mpdu(frame, wlan_build_auth(frame, sizeof(frame), W.bssid, W.mac));
        for (int i = 0; i < 30 && !W.auth_ok && !W.auth_seen; i++) sleep_ms(10);
    }
    if (!W.auth_ok) {
        if (W.auth_seen) klogf("WLAN: auth %s ch %d refused, status %u\n", W.ssid, W.channel, W.auth_status);
        else klogf("WLAN: auth %s ch %d: no answer\n", W.ssid, W.channel);
        W.state = WLAN_IDLE;
        return W.auth_seen ? -ECONNREFUSED : -EHOSTUNREACH;
    }
    for (int t = 0; t < 3 && !W.assoc_seen; t++) {
        tx_mpdu(frame, wlan_build_assoc(frame, sizeof(frame), W.bssid, W.mac, (const u8 *)W.ssid, (u8)strlen(W.ssid), rsn));
        for (int i = 0; i < 30 && !W.assoc_seen; i++) sleep_ms(10);
    }
    if (!W.assoc_ok) {
        if (W.assoc_seen) klogf("WLAN: assoc %s refused, status %u\n", W.ssid, W.assoc_status);
        else klogf("WLAN: assoc %s: no answer\n", W.ssid);
        W.state = WLAN_IDLE;
        return W.assoc_seen ? -ECONNREFUSED : -ETIMEDOUT;
    }
    klogf("WLAN: associated with %s ch %d aid %u\n", W.ssid, W.channel, W.aid);
    if (W.ops.joined && W.ops.joined(W.dev, W.bssid, W.aid)) {
        klogf("WLAN: card did not accept the association\n");
        W.state = WLAN_IDLE;
        return -EIO;
    }
    if (!W.secure) { W.port = 1; W.state = WLAN_RUN; }
    else W.state = WLAN_EAPOL;
    return 0;
}

static int do_disconnect(void) {
    W.port = 0;
    W.have_ptk = W.have_gtk = 0;
    W.ssid[0] = 0;
    if (W.state != WLAN_BLOCKED) W.state = W.iface ? WLAN_IDLE : WLAN_DOWN;
    return 0;
}

int wlan_ctl(int op, void *buf, u32 len) {
    if (op == WLAN_COUNT) return W.nlist;
    if (op == WLAN_STATUS) {
        if (!buf || len < sizeof(struct wlan_status_u)) return -EINVAL;
        struct wlan_status_u *s = buf;
        memset(s, 0, sizeof(*s));
        s->state = W.iface ? (u32)W.state : WLAN_DOWN;
        s->bars = (u32)bars_of(W.rssi);
        s->secured = (u32)W.secure;
        s->blocked = (u32)W.blocked;
        s->channel = (u32)W.channel;
        s->rssi = W.rssi;
        memcpy(s->mac, W.mac, 6);
        memcpy(s->bssid, W.bssid, 6);
        memcpy(s->ssid, W.ssid, 33);
        return 0;
    }
    if (!W.iface && op != WLAN_STATUS) return -ENODEV;
    if (op == WLAN_SCAN) return do_scan();
    if (op == WLAN_RESULT) {
        if (!buf || len < sizeof(struct wlan_bss_u)) return -EINVAL;
        struct wlan_bss_u *b = buf;
        if (b->index >= (u32)W.nlist) return -ENOENT;
        struct bss *s = &W.list[b->index];
        u32 index = b->index;
        memset(b, 0, sizeof(*b));
        b->index = index;
        b->rssi = s->rssi;
        b->channel = s->channel;
        b->secured = s->secured;
        b->bars = (u32)bars_of(s->rssi);
        memcpy(b->bssid, s->bssid, 6);
        memcpy(b->ssid, s->ssid, 33);
        return 0;
    }
    if (op == WLAN_CONNECT) {
        if (!buf || len < sizeof(struct wlan_connect_u)) return -EINVAL;
        return do_connect(buf);
    }
    if (op == WLAN_DISCONNECT) return do_disconnect();
    if (op == WLAN_EAPOL_TX) {
        if (!buf || len < 4 || len > 512) return -EINVAL;
        u8 eth[14 + 512];
        memset(eth, 0, 14);
        memcpy(eth, W.bssid, 6);
        memcpy(eth + 6, W.mac, 6);
        eth[12] = 0x88; eth[13] = 0x8e;
        memcpy(eth + 14, buf, len);
        return wlan_send(W.iface, eth, 14 + len);
    }
    if (op == WLAN_EAPOL_RX) {
        if (!buf || len < sizeof(struct wlan_eapol_u)) return -EINVAL;
        struct wlan_eapol_u *e = buf;
        if (!W.eapol_n) { e->len = 0; return 0; }
        u16 n = W.eapol_len[W.eapol_h];
        if (n > 512) n = 512;
        memcpy(e->data, eapol_at(W.eapol_h), n);
        e->len = n;
        W.eapol_h = (W.eapol_h + 1) % 4;
        W.eapol_n--;
        return n;
    }
    if (op == WLAN_SET_KEY) {
        if (!buf || len < sizeof(struct wlan_key_u)) return -EINVAL;
        struct wlan_key_u *k = buf;
        if (k->kind == WLAN_KEY_PTK) { memcpy(W.ptk, k->key, 16); W.have_ptk = 1; }
        else if (k->kind == WLAN_KEY_GTK) { memcpy(W.gtk, k->key, 16); W.gtk_id = k->id & 3; W.have_gtk = 1; }
        else return -EINVAL;
        return 0;
    }
    if (op == WLAN_PORT) {
        W.port = len ? 1 : 0;
        if (W.port) W.state = WLAN_RUN;
        else if (W.state == WLAN_RUN) W.state = WLAN_EAPOL;
        return 0;
    }
    if (op == WLAN_RADIO) {
        if (W.ops.radio && W.ops.radio(W.dev)) { wlan_hw_block(1); return -EPERM; }
        wlan_hw_block(len ? 0 : 1);
        return 0;
    }
    return -EINVAL;
}
