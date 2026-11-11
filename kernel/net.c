#include "klog.h"
/* Сетевой стек: Ethernet, ARP, IPv4, ICMP, UDP, DHCP и сетевой поток ядра.
 *
 * Путь пакета наружу:  данные -> [UDP/TCP заголовок] -> ip_send -> [IP] -> [Ethernet] -> e1000
 * Путь пакета внутрь:  e1000 -> eth_input -> arp_input / ip_input -> icmp/udp/tcp_input -> сокет
 *
 * Приём делает поток ядра «net»: раз в 10 мс забирает кадры из кольца карты,
 * разбирает их, а заодно отвечает за таймеры TCP (повторная отправка). */
#include "net.h"
#include "e1000.h"
#include "string.h"
#include "timer.h"
#include "heap.h"
#include "vga.h"
#include "syscall.h"

struct net_config netcfg;
struct mutex net_lock;

static u8 frame[1600];                  /* собираем исходящий кадр (под net_lock) */
static u16 ip_id;

/* ------------------------------------------------------------ контрольная сумма
 * Сумма 16-битных слов с переносом, и в конце — побитовое НЕ. Одна и та же для
 * IP, ICMP, UDP и TCP. sum — начальное значение (для псевдозаголовка TCP/UDP). */
u16 net_checksum(const void *data, u32 len, u32 sum) {
    const u8 *p = data;
    for (; len > 1; len -= 2, p += 2) sum += (p[0] << 8) | p[1];
    if (len) sum += p[0] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return htons(~sum & 0xFFFF);
}

/* ------------------------------------------------------------ ARP
 * IP-адрес -> MAC-адрес. Спрашиваем широковещательно «у кого 10.0.2.2?»,
 * хозяин отвечает. Ответы (и вообще всех, кто нам пишет) запоминаем. */
#define ARP_N 16
static struct { u32 ip; u8 mac[6]; bool valid; } arp_cache[ARP_N];
static int arp_next;

struct arp_pkt {
    u16 htype, ptype;
    u8  hlen, plen;
    u16 op;
    u8  sha[6]; u32 spa;
    u8  tha[6]; u32 tpa;
} __attribute__((packed));

static void arp_learn(u32 ip, const u8 *mac) {
    if (!ip) return;
    for (int i = 0; i < ARP_N; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { memcpy(arp_cache[i].mac, mac, 6); return; }
    arp_cache[arp_next].ip = ip;
    memcpy(arp_cache[arp_next].mac, mac, 6);
    arp_cache[arp_next].valid = true;
    arp_next = (arp_next + 1) % ARP_N;
}

static bool arp_lookup(u32 ip, u8 *mac) {
    for (int i = 0; i < ARP_N; i++)
        if (arp_cache[i].valid && arp_cache[i].ip == ip) { memcpy(mac, arp_cache[i].mac, 6); return true; }
    return false;
}

static int eth_send(const u8 *dst, u16 type, u32 payload_len) {
    struct eth_hdr *e = (struct eth_hdr *)frame;
    memcpy(e->dst, dst, 6);
    memcpy(e->src, netcfg.mac, 6);
    e->type = htons(type);
    return e1000_send(frame,sizeof(*e)+payload_len)<0?-EAGAIN:0;
}

static void arp_send(u16 op, const u8 *tha, u32 tpa) {
    static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    struct arp_pkt *a = (struct arp_pkt *)(frame + sizeof(struct eth_hdr));
    a->htype = htons(1);
    a->ptype = htons(ETH_IP);
    a->hlen = 6; a->plen = 4;
    a->op = htons(op);
    memcpy(a->sha, netcfg.mac, 6);
    a->spa = htonl(netcfg.ip);
    memcpy(a->tha, op == 1 ? (const u8 *)"\0\0\0\0\0\0" : tha, 6);
    a->tpa = htonl(tpa);
    eth_send(op == 1 ? bcast : tha, ETH_ARP, sizeof(*a));
}

static void arp_input(const u8 *p, u32 len) {
    if (len < sizeof(struct arp_pkt)) return;
    const struct arp_pkt *a = (const struct arp_pkt *)p;
    u32 spa = ntohl(a->spa), tpa = ntohl(a->tpa);
    arp_learn(spa, a->sha);
    if (ntohs(a->op) == 1 && netcfg.ip && tpa == netcfg.ip)   /* спрашивают нас — отвечаем */
        arp_send(2, a->sha, spa);
}

/* ------------------------------------------------------------ IPv4 */

/* в своей ли подсети адрес: тогда шлём напрямую, иначе — через шлюз */
static u32 next_hop(u32 dst) {
    if (dst == 0xFFFFFFFF || !netcfg.mask) return dst;
    return (dst & netcfg.mask) == (netcfg.ip & netcfg.mask) ? dst : netcfg.gw;
}

/* Петля (loopback): пакет самому себе (127.x.x.x или свой адрес) не идёт в карту,
 * а встаёт в очередь — сетевой поток разберёт его как пришедший. */
#define LOOP_MAX 64
static u8 *loop_q[LOOP_MAX];
static u32 loop_len[LOOP_MAX];
static int loop_h, loop_n;

static bool is_local(u32 ip) { return (ip >> 24) == 127 || (netcfg.ip && ip == netcfg.ip); }
u32 net_src_ip(u32 dst) { return (dst >> 24) == 127 ? dst : netcfg.ip; }

static void ip_input(const u8 *src_mac, const u8 *p, u32 len);

static void loop_drain(void) {
    for (int budget = 256; loop_n && budget; budget--) {
        u8 *p = loop_q[loop_h];
        u32 len = loop_len[loop_h];
        loop_h = (loop_h + 1) % LOOP_MAX;
        loop_n--;
        ip_input(NULL, p, len);          /* может положить в очередь ответ — разберём в этом же цикле */
        kfree(p);
    }
}

int ip_send(u32 dst, u8 proto, const void *payload, u32 len) {
    static const u8 bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    if (!netcfg.up) return -ENETDOWN;
    if (len > 1500 - sizeof(struct ip_hdr)) return -EINVAL;
    bool local = is_local(dst);
    u8 mac[6];
    u32 hop = next_hop(dst);
    if (dst == 0xFFFFFFFF) memcpy(mac, bcast, 6);
    else if (!local && !arp_lookup(hop, mac)) {
        arp_send(1, NULL, hop);          /* спросим — пакет пропадёт, TCP/ping повторят */
        return -EHOSTUNREACH;
    }
    struct ip_hdr *ip = (struct ip_hdr *)(frame + sizeof(struct eth_hdr));
    ip->ver_ihl = 0x45;                  /* IPv4, заголовок 5 слов = 20 байт */
    ip->tos = 0;
    ip->len = htons(sizeof(*ip) + len);
    ip->id = htons(ip_id++);
    ip->frag = htons(0x4000);            /* «не фрагментировать» */
    ip->ttl = 64;
    ip->proto = proto;
    ip->csum = 0;
    ip->src = htonl(net_src_ip(dst));
    ip->dst = htonl(dst);
    ip->csum = net_checksum(ip, sizeof(*ip), 0);
    memcpy((u8 *)ip + sizeof(*ip), payload, len);
    if (local) {
        if (loop_n == LOOP_MAX) return 0;                /* очередь полна — пакет «потерян», TCP повторит */
        u8 *copy = kmalloc(sizeof(*ip) + len);
        if (!copy) return 0;
        memcpy(copy, ip, sizeof(*ip) + len);
        loop_q[(loop_h + loop_n) % LOOP_MAX] = copy;
        loop_len[(loop_h + loop_n) % LOOP_MAX] = sizeof(*ip) + len;
        loop_n++;
        return 0;
    }
    return eth_send(mac,ETH_IP,sizeof(*ip)+len);
}

/* ------------------------------------------------------------ ICMP (ping) */

struct icmp_echo { u8 type, code; u16 csum, id, seq; } __attribute__((packed));

static struct { u16 id, seq; bool done; u32 sent; u32 rtt; } pinger;

static void icmp_input(u32 src, const u8 *p, u32 len) {
    if (len < sizeof(struct icmp_echo)) return;
    struct icmp_echo *h = (struct icmp_echo *)p;
    if (h->type == 8) {                              /* нас пингуют — ответим тем же */
        static u8 reply[1500];
        if (len > sizeof(reply)) return;
        memcpy(reply, p, len);
        struct icmp_echo *r = (struct icmp_echo *)reply;
        r->type = 0;
        r->csum = 0;
        r->csum = net_checksum(reply, len, 0);
        ip_send(src, PROTO_ICMP, reply, len);
    } else if (h->type == 0 && ntohs(h->id) == pinger.id && ntohs(h->seq) == pinger.seq) {
        pinger.rtt = (timer_ticks() - pinger.sent) * (1000 / TIMER_HZ);
        pinger.done = true;
        task_wakeup(&pinger);
    }
}

int net_ping(u32 ip, u16 seq, u32 timeout_ms) {
    u8 pkt[sizeof(struct icmp_echo) + 32];
    struct icmp_echo *h = (struct icmp_echo *)pkt;
    for (int attempt = 0; attempt < 2; attempt++) {    /* второй раз — если ARP ещё не знал адрес */
        mutex_lock(&net_lock);
        memset(pkt, 0, sizeof(pkt));
        h->type = 8;
        h->id = htons(0x4D4F);                         /* «MO» */
        h->seq = htons(seq);
        memcpy(pkt + sizeof(*h), "MyOS ping payload, 32 bytes!!!!", 32);
        h->csum = net_checksum(pkt, sizeof(pkt), 0);
        pinger.id = 0x4D4F; pinger.seq = seq; pinger.done = false;
        pinger.sent = timer_ticks();
        int r = ip_send(ip, PROTO_ICMP, pkt, sizeof(pkt));
        mutex_unlock(&net_lock);
        if (r == -EHOSTUNREACH && attempt == 0) { sleep_ms(200); continue; }
        if (r < 0) return r;

        u32 f = irq_save();
        u32 deadline = timer_ticks() + timeout_ms / (1000 / TIMER_HZ);
        while (!pinger.done && (i32)(timer_ticks() - deadline) < 0 && !task_current()->kill_pending)
            task_wait_timeout(&pinger, deadline - timer_ticks());
        irq_restore(f);
        if (task_current()->kill_pending) return -EINTR;
        return pinger.done ? (int)pinger.rtt : -ETIMEDOUT;
    }
    return -EHOSTUNREACH;
}

/* ------------------------------------------------------------ UDP */

int udp_send(u32 dst, u16 sport, u16 dport, const void *data, u32 len) {
    static u8 pkt[1500];
    if (len > sizeof(pkt) - sizeof(struct ip_hdr) - sizeof(struct udp_hdr)) return -EINVAL;
    struct udp_hdr *u = (struct udp_hdr *)pkt;
    u->sport = htons(sport);
    u->dport = htons(dport);
    u->len = htons(sizeof(*u) + len);
    u->csum = 0;                         /* у UDP поверх IPv4 сумма необязательна */
    memcpy(pkt + sizeof(*u), data, len);
    return ip_send(dst, PROTO_UDP, pkt, sizeof(*u) + len);
}

static void dhcp_input(const u8 *p, u32 len);

static void udp_input(u32 src, const u8 *p, u32 len) {
    if (len < sizeof(struct udp_hdr)) return;
    const struct udp_hdr *u = (const struct udp_hdr *)p;
    u32 ulen = ntohs(u->len);
    if (ulen < sizeof(*u) || ulen > len) return;
    u16 dport = ntohs(u->dport);
    if (dport == 68) { dhcp_input(p + sizeof(*u), ulen - sizeof(*u)); return; }
    udp_deliver(src, ntohs(u->sport), dport, p + sizeof(*u), ulen - sizeof(*u));
}

static void ip_input(const u8 *src_mac, const u8 *p, u32 len) {
    if (len < sizeof(struct ip_hdr)) return;
    const struct ip_hdr *ip = (const struct ip_hdr *)p;
    u32 hl = (ip->ver_ihl & 0x0F) * 4;
    u32 tl = ntohs(ip->len);
    if ((ip->ver_ihl >> 4) != 4 || hl < 20 || tl > len || tl < hl) return;
    if (net_checksum(ip, hl, 0) != 0) return;            /* битый заголовок */
    if (ntohs(ip->frag) & 0x3FFF) return;                 /* фрагменты не собираем */
    u32 src = ntohl(ip->src), dst = ntohl(ip->dst);
    if (netcfg.ip && dst != netcfg.ip && dst != 0xFFFFFFFF && (dst >> 24) != 127) return;
    if (src_mac && (src & netcfg.mask) == (netcfg.ip & netcfg.mask)) arp_learn(src, src_mac);
    const u8 *data = p + hl;
    u32 dlen = tl - hl;
    switch (ip->proto) {
    case PROTO_ICMP: icmp_input(src, data, dlen); break;
    case PROTO_UDP:  udp_input(src, data, dlen); break;
    case PROTO_TCP:  tcp_input(src, dst, data, dlen); break;
    }
}

static void eth_input(const u8 *p, u32 len) {
    if (len < sizeof(struct eth_hdr)) return;
    const struct eth_hdr *e = (const struct eth_hdr *)p;
    u16 type = ntohs(e->type);
    if (type == ETH_ARP) arp_input(p + sizeof(*e), len - sizeof(*e));
    else if (type == ETH_IP) ip_input(e->src, p + sizeof(*e), len - sizeof(*e));
}

static void net_poll(void) {
    static u8 buf[2048];
    int n;
    while ((n = e1000_receive(buf, sizeof(buf))) > 0) eth_input(buf, n);
    loop_drain();
}

/* ------------------------------------------------------------ DHCP
 * DISCOVER («есть тут DHCP-сервер?») -> OFFER («возьми 10.0.2.15»)
 * -> REQUEST («беру 10.0.2.15») -> ACK («твой; маска, шлюз, DNS вот такие») */

struct dhcp_msg {
    u8  op, htype, hlen, hops;
    u32 xid;
    u16 secs, flags;
    u32 ciaddr, yiaddr, siaddr, giaddr;
    u8  chaddr[16];
    u8  sname[64], file[128];
    u32 magic;
    u8  options[312];
} __attribute__((packed));

static struct {
    int state; /* 1 OFFER, 2 ACK, 3 configured, 4 NAK */
    u32 xid,offer,server,mask,gw,dns,lease,started,received,rejected;
} dh;

static void dhcp_send(int type) {
    static struct dhcp_msg m;
    memset(&m, 0, sizeof(m));
    m.op = 1; m.htype = 1; m.hlen = 6;
    m.xid = htonl(dh.xid);
    u32 elapsed=(timer_ticks()-dh.started)/TIMER_HZ;
    m.secs=htons(elapsed>65535?65535:elapsed);
    m.flags = htons(0x8000);                     /* ответ — широковещательно: у нас ещё нет адреса */
    memcpy(m.chaddr, netcfg.mac, 6);
    m.magic = htonl(0x63825363);
    u8 *o = m.options;
    *o++ = 53; *o++ = 1; *o++ = type;            /* тип сообщения */
    *o++=61;*o++=7;*o++=1;memcpy(o,netcfg.mac,6);o+=6; /* Ethernet client ID */
    if (type == 3) {
        *o++ = 50; *o++ = 4; u32 ip = htonl(dh.offer); memcpy(o, &ip, 4); o += 4;      /* хотим этот адрес */
        *o++ = 54; *o++ = 4; u32 sv = htonl(dh.server); memcpy(o, &sv, 4); o += 4;     /* у этого сервера */
    }
    *o++ = 55; *o++ = 3; *o++ = 1; *o++ = 3; *o++ = 6;   /* нужны: маска, шлюз, DNS */
    *o++ = 12; *o++ = 4; memcpy(o, "myos", 4); o += 4;    /* имя хоста */
    *o++ = 255;
    u32 bytes=sizeof(m)-sizeof(m.options)+(o-m.options);
    if (bytes<300)bytes=300; /* RFC 1542 BOOTP minimum; zero pad after END */
    int result=udp_send(0xFFFFFFFF,68,67,&m,bytes);
    klogf("NET: DHCP %s xid=%x bytes=%u result=%d\n",type==1?"DISCOVER":"REQUEST",dh.xid,bytes,result);
}

static void dhcp_input(const u8 *p, u32 len) {
    if (dh.state==0 || dh.state>=3)return;
    dh.received++;
    if (len < sizeof(struct dhcp_msg) - 312) { dh.rejected++;return; }
    const struct dhcp_msg *m = (const struct dhcp_msg *)p;
    if (m->op != 2 || m->htype!=1 || m->hlen!=6 || memcmp(m->chaddr,netcfg.mac,6) ||
        ntohl(m->xid) != dh.xid || ntohl(m->magic) != 0x63825363) { dh.rejected++;return; }
    int type = 0;
    u32 mask = 0, gw = 0, dns = 0, server = 0, lease = 0;
    const u8 *o = m->options, *end = p + len;
    while (o < end && *o != 255) {
        if (*o == 0) { o++; continue; }
        if (o + 2 > end || o + 2 + o[1] > end) { dh.rejected++;return; }
        u8 code = o[0], l = o[1];
        const u8 *v = o + 2;
        u32 val = l >= 4 ? (u32)v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3] : 0;
        if (code == 53 && l==1) type = v[0];
        if (code == 1) mask = val;
        if (code == 3) gw = val;
        if (code == 6) dns = val;
        if (code == 54) server = val;
        if (code == 51) lease = val;
        o += 2 + l;
    }
    if (dh.state == 1 && type == 2 && server && ntohl(m->yiaddr)) {
        dh.offer = ntohl(m->yiaddr);
        dh.server = server;
        dh.mask=mask;dh.gw=gw;dh.dns=dns;dh.lease=lease;
        klogf("NET: DHCP OFFER address=%x server=%x\n",dh.offer,server);
        dh.state = 2;
        dhcp_send(3);
    } else if (dh.state == 2 && type == 5 && (!server || server==dh.server) &&
               (!m->yiaddr || ntohl(m->yiaddr)==dh.offer)) {
        if(mask)dh.mask=mask;
        if(gw)dh.gw=gw;
        if(dns)dh.dns=dns;
        if(lease)dh.lease=lease;
        klogf("NET: DHCP ACK address=%x\n",dh.offer);
        dh.state = 3;
    } else if (dh.state==2 && type==6 && (!server || server==dh.server)) {
        klogf("NET: DHCP NAK from server=%x\n",server);
        dh.state=4;
    } else dh.rejected++;
}

static bool dhcp_run(void) {
    dh.xid = 0x4D794F53 ^ timer_ticks();
    dh.started=timer_ticks();dh.received=dh.rejected=0;dh.state=1;
    for (int attempt = 0; attempt < 3 && dh.state != 3; attempt++) {
        if (!e1000_link())break;
        mutex_lock(&net_lock);
        dhcp_send(dh.state==2?3:1);
        mutex_unlock(&net_lock);
        u32 deadline=timer_ticks()+(4u<<attempt)*TIMER_HZ;
        int previous=dh.state;
        bool refused=false;
        while (dh.state!=3 && (i32)(timer_ticks()-deadline)<0 && e1000_link()) {
            mutex_lock(&net_lock);
            net_poll();
            mutex_unlock(&net_lock);
            if (dh.state==4) { dh.state=1;refused=true;break; }
            if (previous==1 && dh.state==2)deadline=timer_ticks()+4*TIMER_HZ;
            previous=dh.state;
            sleep_ms(10);
        }
        if(dh.state!=3 && !refused) {
            klogf("NET: DHCP timeout waiting for %s, attempt=%u replies=%u rejected=%u\n",
                dh.state==2?"ACK":"OFFER",attempt+1,dh.received,dh.rejected);
            e1000_diagnose();
        }
    }
    mutex_lock(&net_lock);
    bool configured=dh.state==3 && dh.offer!=0;
    if (configured) {
        netcfg.ip = dh.offer;
        netcfg.mask = dh.mask ? dh.mask : IP(255, 255, 255, 0);
        netcfg.gw = dh.gw;
        netcfg.dns = dh.dns;
        netcfg.lease = dh.lease;
        netcfg.dhcp = true;
        klogf("NET: DHCP configured %u.%u.%u.%u\n",netcfg.ip>>24,(netcfg.ip>>16)&255,(netcfg.ip>>8)&255,netcfg.ip&255);
        if (netcfg.gw)arp_send(1,NULL,netcfg.gw);
    } else {
        dh.state=0;
        klogf("NET: DHCP pending; retrying after link/server becomes available\n");
    }
    mutex_unlock(&net_lock);
    return configured;
}

/* ------------------------------------------------------------ поток */

static void net_thread(void *arg) {
    (void)arg;
    u32 retry_at=timer_ticks();bool linked_before=false;
    for (;;) {
        bool linked=e1000_link();
        if (linked && !linked_before && netcfg.dhcp) {
            mutex_lock(&net_lock);
            netcfg.ip=netcfg.mask=netcfg.gw=netcfg.dns=netcfg.lease=0;
            netcfg.dhcp=false;dh.state=0;memset(arp_cache,0,sizeof(arp_cache));
            mutex_unlock(&net_lock);
            retry_at=timer_ticks();
        }
        if (linked && !netcfg.ip && (i32)(timer_ticks()-retry_at)>=0) {
            dhcp_run();retry_at=timer_ticks()+5*TIMER_HZ;
        }
        linked_before=linked;
        mutex_lock(&net_lock);
        net_poll();
        tcp_timer();
        sock_reap();
        mutex_unlock(&net_lock);
        sleep_ms(10);
    }
}

bool net_init(void) {
    if (!e1000_init()) return false;
    e1000_mac(netcfg.mac);
    netcfg.up = true;
    ip_id = timer_ticks();
    if (task_create("net", net_thread, NULL)<0) { netcfg.up=false;return false; }
    return true;
}
