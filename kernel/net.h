#ifndef NET_H
#define NET_H
#include "types.h"
#include "task.h"

/* Сетевой стек MyOS: Ethernet, ARP, IPv4, ICMP, UDP, DHCP, TCP.
 * IP-адреса внутри ядра — u32 в обычном (хостовом) порядке: 10.0.2.15 = 0x0A00020F.
 * В пакетах — сетевой порядок байт (big endian), переводим через htons/htonl. */

#define IP(a, b, c, d) (((u32)(a) << 24) | ((u32)(b) << 16) | ((u32)(c) << 8) | (u32)(d))

static inline u16 htons(u16 v) { return (v >> 8) | (v << 8); }
static inline u16 ntohs(u16 v) { return htons(v); }
static inline u32 htonl(u32 v) { return (v >> 24) | ((v >> 8) & 0xFF00) | ((v << 8) & 0xFF0000) | (v << 24); }
static inline u32 ntohl(u32 v) { return htonl(v); }

#define ETH_ARP  0x0806
#define ETH_IP   0x0800
#define PROTO_ICMP 1
#define PROTO_TCP  6
#define PROTO_UDP  17

struct eth_hdr { u8 dst[6], src[6]; u16 type; } __attribute__((packed));
struct ip_hdr {
    u8  ver_ihl, tos;
    u16 len, id, frag;
    u8  ttl, proto;
    u16 csum;
    u32 src, dst;
} __attribute__((packed));
struct udp_hdr { u16 sport, dport, len, csum; } __attribute__((packed));
struct tcp_hdr {
    u16 sport, dport;
    u32 seq, ack;
    u8  off, flags;
    u16 win, csum, urg;
} __attribute__((packed));

struct net_config {
    u8   mac[6];
    u32  ip, mask, gw, dns;
    bool up;                        /* есть карта */
    bool dhcp;                      /* адрес получен по DHCP (иначе — запасной статический) */
    u32  lease;                     /* секунд */
};
extern struct net_config netcfg;
extern struct mutex net_lock;       /* весь стек — под одним мьютексом */

bool net_init(void);                /* найти карту и запустить сетевой поток */
u16  net_checksum(const void *data, u32 len, u32 sum);   /* интернет-контрольная сумма */
u32  net_src_ip(u32 dst);          /* с какого адреса слать: 127.x — с себя же */
int  ip_send(u32 dst, u8 proto, const void *payload, u32 len);
int  udp_send(u32 dst, u16 sport, u16 dport, const void *data, u32 len);
int  net_ping(u32 ip, u16 seq, u32 timeout_ms);          /* -> мс или -errno */

/* ------------------------------------------------------------ сокеты */

enum { SOCK_TCP = 1, SOCK_UDP = 2 };
enum tcp_state { TCP_CLOSED, TCP_LISTEN, TCP_SYN_SENT, TCP_SYN_RCVD, TCP_ESTABLISHED,
                 TCP_FIN_WAIT1, TCP_FIN_WAIT2, TCP_CLOSE_WAIT, TCP_CLOSING, TCP_LAST_ACK,
                 TCP_TIME_WAIT };

#define SOCK_RXBUF 32768              /* окно приёма: 22 сегмента — меньше кольца e1000 (32) */
#define SOCK_TXBUF 16384
#define UDP_QUEUE  8
#define ACCEPT_MAX 8

struct dgram { u32 ip; u16 port; u32 len; u8 data[]; };

struct sock {
    int  type;
    u16  lport, rport;
    u32  rip;
    bool user_closed;               /* программа закрыла дескриптор */
    int  err;                       /* отложенная ошибка (-errno) */

    /* UDP: очередь пришедших датаграмм */
    struct dgram *q[UDP_QUEUE];
    int  qh, qn;

    /* TCP */
    enum tcp_state state;
    u32  iss, snd_una, snd_nxt, snd_wnd, rcv_nxt;
    u8  *rxbuf; u32 rx_head, rx_len;       /* кольцо принятых, ещё не прочитанных байт */
    u8  *txbuf; u32 tx_len;                /* отправленные-но-не-подтверждённые + ещё не отправленные; txbuf[0] = snd_una */
    bool fin_pending, fin_sent, fin_acked, peer_fin;
    u32  rto, rtx_deadline, retries;       /* повторная отправка (в тиках) */
    u32  close_deadline;                   /* TIME_WAIT */
    struct sock *parent;                   /* для ещё не принятых accept() соединений */
    struct sock *aq[ACCEPT_MAX];
    int  aq_n, backlog;
    u32  rcv_timeout, conn_timeout;        /* мс, 0 — ждать сколько угодно (sockctl) */
};

#define MAX_SOCKS 32
extern struct sock *socks[MAX_SOCKS];

struct sock *sock_alloc(int type);
void sock_free(struct sock *s);
u16  sock_ephemeral_port(void);
void udp_deliver(u32 src, u16 sport, u16 dport, const u8 *data, u32 len);

/* tcp.c */
void tcp_input(u32 src, u32 dst, const u8 *seg, u32 len);
void tcp_timer(void);
int  tcp_connect(struct sock *s, u32 ip, u16 port);
int  tcp_read(struct sock *s, void *buf, u32 len);
int  tcp_write(struct sock *s, const void *buf, u32 len);
void tcp_close(struct sock *s);
int  tcp_listen(struct sock *s, int backlog);
struct sock *tcp_accept(struct sock *s, int *err);

/* socket.c: вход из файлового слоя */
int  sock_read(struct sock *s, void *buf, u32 len);
int  sock_write(struct sock *s, const void *buf, u32 len);
void sock_close(struct sock *s);
void sock_reap(void);               /* сетевой поток: доделать отложенное */
int  sock_connect(struct sock *s, u32 ip, u16 port);
int  sock_ctl(struct sock *s, int op, u32 arg);
int  sock_bind(struct sock *s, u16 port);
int  sock_listen(struct sock *s, int backlog);
struct sock *sock_accept(struct sock *s, int *err);
int  sock_sendto(struct sock *s, const void *buf, u32 len, u32 ip, u16 port);
int  sock_recvfrom(struct sock *s, void *buf, u32 len, u32 *ip, u16 *port, u32 timeout_ms);

#endif
