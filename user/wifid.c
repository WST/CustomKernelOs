#include "myos.h"
#include "sys/stat.h"
#include "wifi_shm.h"
#include "wpa.c"

/* Служба ассоциации. Пароль не остаётся в ядре: сюда приходит фраза, отсюда уходят PTK и GTK. */

static struct wpa_net nets[8];
static int nnets;
static struct wifi_pub *pub;
static struct wifi_req *req;
static int pub_id, req_id;
static unsigned seen_req;
static char last_err[48];

static void load_nets(void) {
    nnets = 0;
    int fd = open("/etc/wifi/networks", O_RDONLY);
    if (fd < 0) return;
    char text[2048];
    int n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n < 0) n = 0;
    text[n] = 0;
    nnets = wpa_load(text, nets, 8);
}

static void save_nets(void) {
    mkdir("/etc");
    mkdir("/etc/wifi");
    chmod("/etc/wifi", 0700);
    int fd = open("/etc/wifi/networks", O_WRITE);
    if (fd < 0) return;
    char buf[2048];
    int n = 0;
    for (int i = 0; i < nnets; i++) {
        int w = snprintf(buf + n, sizeof(buf) - (unsigned)n, "network {\nssid=\"%s\"\npriority=%d\n", nets[i].ssid, nets[i].priority);
        if (w < 0) break;
        n += w;
        if (nets[i].has_psk) {
            w = snprintf(buf + n, sizeof(buf) - (unsigned)n, "psk=\"%s\"\n", nets[i].psk);
            if (w < 0) break;
            n += w;
        }
        if (n + 3 >= (int)sizeof(buf)) break;
        buf[n++] = '}'; buf[n++] = '\n';
    }
    write(fd, buf, (unsigned)n);
    close(fd);
    chmod("/etc/wifi/networks", 0600);
}

static void remember(const char *ssid, const char *pass) {
    for (int i = 0; i < nnets; i++) if (!strcmp(nets[i].ssid, ssid)) {
        if (pass && pass[0]) { snprintf(nets[i].psk, sizeof(nets[i].psk), "%s", pass); nets[i].has_psk = 1; }
        save_nets();
        return;
    }
    if (nnets >= 8) return;
    memset(&nets[nnets], 0, sizeof(nets[nnets]));
    snprintf(nets[nnets].ssid, sizeof(nets[nnets].ssid), "%s", ssid);
    if (pass && pass[0]) { snprintf(nets[nnets].psk, sizeof(nets[nnets].psk), "%s", pass); nets[nnets].has_psk = 1; }
    nets[nnets].priority = nnets;
    nnets++;
    save_nets();
}

static void forget(const char *ssid) {
    for (int i = 0; i < nnets; i++) if (!strcmp(nets[i].ssid, ssid)) {
        memmove(&nets[i], &nets[i + 1], (unsigned)(nnets - i - 1) * sizeof(nets[0]));
        nnets--;
        save_nets();
        return;
    }
}

/* Первое сообщение может прийти раньше, чем карта начнёт пропускать данные точки;
 * точка повторяет его до четырёх раз с паузой до секунды. */
static int eapol_wait(struct wlan_eapol_u *e, int ms) {
    for (int i = 0; i < ms / 20; i++) {
        int n = wlanctl(WLAN_EAPOL_RX, e, sizeof(*e));
        if (n > 0) return n;
        sleep_ms(20);
    }
    return 0;
}

static int handshake(const char *ssid, const char *pass) {
    unsigned char pmk[32], ptk[64], anonce[32], snonce[32], replay[8], gtk[16];
    struct wlan_eapol_u e;
    wpa_pmk(pass, ssid, pmk);
    unsigned t = ticks();
    for (int i = 0; i < 32; i++) snonce[i] = (unsigned char)(t * 17 + i * 13);
    if (!eapol_wait(&e, 4000) || wpa_read_m1(e.data, e.len, anonce, replay)) { snprintf(last_err, sizeof(last_err), "нет первого сообщения"); return -1; }
    /* Отвечать надо на последний повтор: у него больше счётчик. */
    struct wlan_eapol_u later;
    while (wlanctl(WLAN_EAPOL_RX, &later, sizeof(later)) > 0)
        wpa_read_m1(later.data, later.len, anonce, replay);
    struct wlan_status_u st;
    if (wlanctl(WLAN_STATUS, &st, sizeof(st)) < 0) return -1;
    wpa_ptk(pmk, st.bssid, st.mac, anonce, snonce, ptk);
    unsigned char m2[128];
    int n2 = wpa_build_m2(m2, replay, snonce, ptk);
    if (wlanctl(WLAN_EAPOL_TX, m2, (unsigned)n2) < 0) return -1;
    struct wlan_key_u key;
    memset(&key, 0, sizeof(key));
    key.kind = WLAN_KEY_PTK;
    memcpy(key.key, ptk + 32, 16);
    if (wlanctl(WLAN_SET_KEY, &key, sizeof(key)) < 0) return -1;
    if (!eapol_wait(&e, 2000)) { snprintf(last_err, sizeof(last_err), "нет третьего сообщения"); return -1; }
    unsigned char m4[128], kid = 0;
    int hm = wpa_take_m3(e.data, e.len, ptk, ptk + 16, gtk, m4, &kid);
    if (hm < 0) {
        snprintf(last_err, sizeof(last_err), "%s",
                 hm == -3 ? "ключ не раскрылся" : hm == -2 ? "не то третье сообщение" : "подпись не сошлась");
        return -1;
    }
    key.kind = WLAN_KEY_GTK;
    key.id = kid;
    memcpy(key.key, gtk, 16);
    wlanctl(WLAN_SET_KEY, &key, sizeof(key));
    wlanctl(WLAN_EAPOL_TX, m4, 99);
    wlanctl(WLAN_PORT, 0, 1);
    last_err[0] = 0;
    return 0;
}

static void publish(void);

static int connect_ssid(const char *ssid, const char *pass, int secure) {
    struct wlan_connect_u c;
    memset(&c, 0, sizeof(c));
    snprintf(c.ssid, sizeof(c.ssid), "%s", ssid);
    c.secure = (unsigned char)(secure || (pass && pass[0]));
    snprintf(last_err, sizeof(last_err), "подключение…");
    publish();
    int rc = wlanctl(WLAN_CONNECT, &c, sizeof(c));
    if (rc < 0) {
        const char *why = rc == -2 ? "сеть не найдена, обновите список" :
                          rc == -113 ? "точка не отвечает" :
                          rc == -111 ? "точка отказала в подключении" :
                          rc == -110 ? "точка не ответила на подключение" : "ошибка подключения";
        snprintf(last_err, sizeof(last_err), "%s", why);
        return rc;
    }
    struct wlan_status_u st;
    wlanctl(WLAN_STATUS, &st, sizeof(st));
    if (st.state == WLAN_EAPOL) {
        if (!pass || !pass[0]) { snprintf(last_err, sizeof(last_err), "нужен пароль"); return -1; }
        if (handshake(ssid, pass)) return -1;
    }
    last_err[0] = 0;
    remember(ssid, pass);
    return 0;
}

static void autoconnect(void) {
    struct wlan_status_u st;
    if (wlanctl(WLAN_STATUS, &st, sizeof(st)) < 0 || st.state == WLAN_DOWN || st.state == WLAN_BLOCKED || st.state == WLAN_RUN) return;
    int n = wlanctl(WLAN_SCAN, 0, 0);
    if (n <= 0) return;
    char ssids[16][33];
    int secure[16], rssi[16], vis = n > 16 ? 16 : n;
    for (int i = 0; i < vis; i++) {
        struct wlan_bss_u b; memset(&b, 0, sizeof(b)); b.index = (unsigned)i;
        if (wlanctl(WLAN_RESULT, &b, sizeof(b)) < 0) { vis = i; break; }
        memcpy(ssids[i], b.ssid, 33);
        secure[i] = (int)b.secured;
        rssi[i] = b.rssi;
    }
    int pick = wpa_choose(nets, nnets, ssids, secure, rssi, vis);
    if (pick >= 0) connect_ssid(nets[pick].ssid, nets[pick].has_psk ? nets[pick].psk : "", nets[pick].has_psk);
}

static void publish(void) {
    if (!pub) return;
    struct wlan_status_u st; memset(&st, 0, sizeof(st));
    int have = wlanctl(WLAN_STATUS, &st, sizeof(st)) == 0;
    pub->magic = WIFI_MAGIC;
    pub->state = have ? st.state : WLAN_DOWN;
    pub->bars = st.bars; pub->secured = st.secured; pub->blocked = st.blocked;
    pub->channel = st.channel; pub->rssi = st.rssi;
    memcpy(pub->ssid, st.ssid, 33);
    memcpy(pub->mac, st.mac, 6);
    memcpy(pub->bssid, st.bssid, 6);
    snprintf(pub->error, sizeof(pub->error), "%s", last_err);
    static struct wpa_seen seen[64];
    int n = 0, total = 0;
    if (have && st.state != WLAN_DOWN) total = wlanctl(WLAN_COUNT, 0, 0);
    if (total > 64) total = 64;
    for (int i = 0; i < total; i++) {
        struct wlan_bss_u b; memset(&b, 0, sizeof(b)); b.index = (unsigned)i;
        if (wlanctl(WLAN_RESULT, &b, sizeof(b)) < 0) break;
        struct wpa_seen *s = &seen[n++];
        memset(s, 0, sizeof(*s));
        memcpy(s->ssid, b.ssid, 33);
        s->ssid[32] = 0;
        memcpy(s->bssid, b.bssid, 6);
        s->channel = (int)b.channel;
        s->bars = (int)b.bars;
        s->secured = (int)b.secured;
        s->rssi = b.rssi;
        for (int k = 0; k < nnets; k++) if (!strcmp(nets[k].ssid, s->ssid)) s->saved = 1;
    }
    n = wpa_order_seen(seen, n, st.state == WLAN_RUN || st.state == WLAN_EAPOL ? st.ssid : "");
    if (n > WIFI_AP_MAX) n = WIFI_AP_MAX;
    for (int i = 0; i < n; i++) {
        memset(&pub->ap[i], 0, sizeof(pub->ap[i]));
        memcpy(pub->ap[i].ssid, seen[i].ssid, 33);
        memcpy(pub->ap[i].bssid, seen[i].bssid, 6);
        pub->ap[i].channel = (unsigned char)seen[i].channel;
        pub->ap[i].bars = (unsigned char)seen[i].bars;
        pub->ap[i].secured = (unsigned char)seen[i].secured;
        pub->ap[i].saved = (unsigned char)seen[i].saved;
        pub->ap[i].rssi = seen[i].rssi;
    }
    pub->count = (unsigned)n;
    pub->seq++;
}

static void handle_req(void) {
    if (!req || req->seq == seen_req) return;
    seen_req = req->seq;
    char ssid[33], pass[64];
    memcpy(ssid, req->ssid, 33); ssid[32] = 0;
    memcpy(pass, req->pass, 64); pass[63] = 0;
    memset(req->pass, 0, sizeof(req->pass));
    if (req->op == WIFI_SCAN) wlanctl(WLAN_SCAN, 0, 0);
    else if (req->op == WIFI_DISCONNECT) { wlanctl(WLAN_DISCONNECT, 0, 0); last_err[0] = 0; }
    else if (req->op == WIFI_FORGET) forget(ssid);
    else if (req->op == WIFI_RADIO) wlanctl(WLAN_RADIO, 0, req->priority ? 1u : 0u);
    else if (req->op == WIFI_CONNECT) connect_ssid(ssid, pass, pass[0] != 0);
    memset(pass, 0, sizeof(pass));
}

static void share(void) {
    if (!pub_id) {
        pub_id = shm_alloc(4096);
        req_id = shm_alloc(4096);
        pub = shm_map(pub_id);
        req = shm_map(req_id);
        if (!pub || !req) return;
        memset(pub, 0, 4096);
        memset(req, 0, 4096);
        pub->magic = WIFI_MAGIC;
    }
    struct proc_info list[64];
    int n = ps(list, 64);
    for (int i = 0; i < n; i++) {
        if (strcmp(list[i].name, "desktop") && strcmp(list[i].name, "settings")) continue;
        msg_allow((int)list[i].pid, 1);
        shm_grant(pub_id, (int)list[i].pid, SHM_READ | SHM_WRITE);
        shm_grant(req_id, (int)list[i].pid, SHM_READ | SHM_WRITE);
    }
    char ids[32];
    int len = snprintf(ids, sizeof(ids), "%d %d\n", pub_id, req_id);
    /* Один и тот же номер страниц не переписываем: иначе кэш диска не успевает сброситься. */
    static char ids_written[32];
    if (strcmp(ids, ids_written)) {
        mkdir("/tmp");
        int fd = open("/tmp/wifi.ids", O_WRITE);
        if (fd >= 0) { write(fd, ids, (unsigned)len); close(fd); snprintf(ids_written, sizeof(ids_written), "%s", ids); }
    }
    struct msg m;
    if (msg_recv(&m, 0) == 0 && m.type == 90) {
        msg_allow(m.from, 1);
        shm_grant(pub_id, m.from, SHM_READ | SHM_WRITE);
        shm_grant(req_id, m.from, SHM_READ | SHM_WRITE);
        struct msg r; memset(&r, 0, sizeof(r));
        r.type = 91; r.a = pub_id; r.b = req_id;
        msg_send(m.from, &r);
    }
}

int main(void) {
    service_register("wifid");
    load_nets();
    unsigned next = 0;
    for (;;) {
        share();
        struct wlan_status_u st;
        int have = wlanctl(WLAN_STATUS, &st, sizeof(st)) == 0 && st.state != WLAN_DOWN;
        /* Скан занимает радио на полторы секунды; без сохранённых сетей он нужен только списку. */
        if (have && ticks() >= next && st.state != WLAN_RUN && st.state != WLAN_BLOCKED && st.state != WLAN_EAPOL) {
            autoconnect();
            next = ticks() + (nnets ? 30000u : 60000u);
        }
        handle_req();
        publish();
        sleep_ms(200);
    }
}
