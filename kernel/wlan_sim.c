#include "wlan.h"
#include "wlan_frame.h"
#include "net.h"
#include "fs.h"
#include "string.h"
#include "klog.h"

/* Учебная точка для QEMU: файла /etc/wifi/sim достаточно, карта не нужна.
 * Ответы на probe/auth/assoc отдаются сразу. DHCP и ARP кладутся в очередь
 * приёма, чтобы не пересобирать кадр, который стек ещё держит. */

static const u8 ap[6] = { 0x02, 0, 0, 0, 0, 1 };
static const u8 sta[6] = { 0x02, 0, 0, 0, 0, 2 };
static const u32 offer_ip = IP(10, 9, 9, 9);
static const u32 gw_ip = IP(10, 9, 9, 1);

static void put16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }

static int sim_up(void *dev) { (void)dev; return 0; }
static void sim_down(void *dev) { (void)dev; }
static int sim_ch(void *dev, int ch) { (void)dev; (void)ch; return 0; }
static int sim_radio(void *dev) { (void)dev; return 0; }

static void reply_mgmt(u8 fc, const u8 da[6], const u8 *body, u32 blen) {
    u8 frame[128];
    if (24 + blen > sizeof(frame)) return;
    memset(frame, 0, 24);
    frame[0] = fc;
    memcpy(frame + 4, da, 6);
    memcpy(frame + 10, ap, 6);
    memcpy(frame + 16, ap, 6);
    memcpy(frame + 24, body, blen);
    wlan_rx(frame, 24 + blen, -40);
}

static void push_eth(const u8 *eth, u32 len) {
    u8 mpdu[1600];
    if (len < 14 || len + 32 > sizeof(mpdu)) return;
    memset(mpdu, 0, 24);
    mpdu[0] = 0x08;
    mpdu[1] = 0x02;
    memcpy(mpdu + 4, eth, 6);
    memcpy(mpdu + 10, ap, 6);
    memcpy(mpdu + 16, eth + 6, 6);
    mpdu[24] = 0xaa; mpdu[25] = 0xaa; mpdu[26] = 0x03;
    memcpy(mpdu + 30, eth + 12, len - 12);
    wlan_rx(mpdu, len + 18, -40);
}

static void dhcp_reply(const u8 *bootp, u32 len, int kind) {
    if (len < 240) return;
    u8 pkt[590];
    memset(pkt, 0, sizeof(pkt));
    memcpy(pkt, "\xff\xff\xff\xff\xff\xff", 6);
    memcpy(pkt + 6, ap, 6);
    pkt[12] = 0x08; pkt[13] = 0x00;
    struct ip_hdr *ip = (struct ip_hdr *)(pkt + 14);
    ip->ver_ihl = 0x45;
    ip->ttl = 64;
    ip->proto = PROTO_UDP;
    ip->src = htonl(gw_ip);
    ip->dst = htonl(0xffffffffu);
    struct udp_hdr *u = (struct udp_hdr *)(ip + 1);
    u->sport = htons(67);
    u->dport = htons(68);
    u8 *m = (u8 *)(u + 1);
    m[0] = 2; m[1] = 1; m[2] = 6;
    memcpy(m + 4, bootp + 4, 4);
    u32 yi = htonl(offer_ip);
    memcpy(m + 16, &yi, 4);
    memcpy(m + 28, bootp + 28, 16);
    u32 magic = htonl(0x63825363);
    memcpy(m + 236, &magic, 4);
    u8 *o = m + 240;
    *o++ = 53; *o++ = 1; *o++ = (u8)kind;
    *o++ = 54; *o++ = 4; u32 sv = htonl(gw_ip); memcpy(o, &sv, 4); o += 4;
    *o++ = 1; *o++ = 4; u32 mask = htonl(IP(255, 255, 255, 0)); memcpy(o, &mask, 4); o += 4;
    *o++ = 3; *o++ = 4; memcpy(o, &sv, 4); o += 4;
    *o++ = 6; *o++ = 4; memcpy(o, &sv, 4); o += 4;
    *o++ = 51; *o++ = 4; u32 lease = htonl(3600); memcpy(o, &lease, 4); o += 4;
    *o++ = 255;
    u32 mlen = (u32)(o - m);
    if (mlen < 300) mlen = 300;
    u->len = htons(sizeof(*u) + mlen);
    ip->len = htons(sizeof(*ip) + sizeof(*u) + mlen);
    ip->csum = net_checksum(ip, sizeof(*ip), 0);
    push_eth(pkt, 14 + sizeof(*ip) + sizeof(*u) + mlen);
}

static void on_data(const u8 *mpdu, u32 len) {
    u8 eth[1600];
    int n = wlan_wifi_to_eth(eth, sizeof(eth), mpdu, len);
    if (n < 14) return;
    u16 type = (u16)((eth[12] << 8) | eth[13]);
    if (type == ETH_ARP && n >= 14 + 28) {
        const u8 *a = eth + 14;
        u16 op = (u16)((a[6] << 8) | a[7]);
        u32 tpa;
        memcpy(&tpa, a + 24, 4);
        if (op == 1 && tpa == htonl(gw_ip)) {
            u8 reply[64];
            memset(reply, 0, sizeof(reply));
            memcpy(reply, eth + 6, 6);
            memcpy(reply + 6, ap, 6);
            reply[12] = 0x08; reply[13] = 0x06;
            put16(reply + 14, htons(1)); put16(reply + 16, htons(0x0800));
            reply[18] = 6; reply[19] = 4; put16(reply + 20, htons(2));
            memcpy(reply + 22, ap, 6);
            u32 spa = htonl(gw_ip);
            memcpy(reply + 28, &spa, 4);
            memcpy(reply + 32, a + 8, 6);
            memcpy(reply + 38, a + 14, 4);
            push_eth(reply, 42);
        }
        return;
    }
    if (type != ETH_IP || n < 14 + 20 + 8 + 240) return;
    const struct ip_hdr *ip = (const struct ip_hdr *)(eth + 14);
    if (ip->proto != PROTO_UDP) return;
    const struct udp_hdr *u = (const struct udp_hdr *)(ip + 1);
    if (ntohs(u->dport) != 67) return;
    const u8 *m = (const u8 *)(u + 1);
    u32 mlen = (u32)(n - (m - eth));
    int kind = 2;
    for (u32 i = 240; i + 2 < mlen && m[i] != 255; ) {
        if (m[i] == 0) { i++; continue; }
        if (m[i] == 53 && m[i + 1] == 1) kind = m[i + 2] == 3 ? 5 : 2;
        i += 2 + m[i + 1];
    }
    dhcp_reply(m, mlen, kind);
}

static int sim_tx(void *dev, const void *mpdu, u32 len) {
    (void)dev;
    const u8 *p = mpdu;
    if (!p || len < 24) return -1;
    if (p[0] == 0x40) {
        u8 body[48];
        memset(body, 0, 12);
        body[10] = 0x11; body[11] = 0x00;
        body[12] = 0; body[13] = 4; memcpy(body + 14, "MyOS", 4);
        body[18] = 3; body[19] = 1; body[20] = 6;
        reply_mgmt(0x50, sta, body, 21);
        return 0;
    }
    if (p[0] == 0xb0) {
        u8 body[6];
        put16(body, 0); put16(body + 2, 2); put16(body + 4, 0);
        reply_mgmt(0xb0, p + 10, body, 6);
        return 0;
    }
    if (p[0] == 0x00) {
        u8 body[6];
        put16(body, 0x0431); put16(body + 2, 0); put16(body + 4, 1);
        reply_mgmt(0x10, p + 10, body, 6);
        return 0;
    }
    if ((p[0] & 0x0c) == 0x08) on_data(p, len);
    return 0;
}

void wlan_sim_maybe(void) {
    if (wlan_present()) return;
    u32 ino;
    if (!fs_ready() || fs_resolve(FS_ROOT, "/etc/wifi/sim", &ino)) return;
    static const struct wlan_ops ops = { sim_up, sim_down, sim_ch, sim_tx, sim_radio };
    if (wlan_attach("wlan0", sta, &ops, NULL, 11)) return;
    klogf("WLAN: simulated open network MyOS on wlan0\n");
}
