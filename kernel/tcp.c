/* TCP: надёжный поток байт поверх ненадёжных IP-пакетов.
 *
 * Каждый байт имеет номер (sequence number). Отправитель хранит всё, что
 * отправил, пока получатель не подтвердит (ACK = «жду байт номер N»). Нет
 * подтверждения за rto — отправляем заново, начиная с первого неподтверждённого
 * (go-back-N). Получатель принимает только байты строго по порядку; всё, что
 * пришло не вовремя, выбрасывает и повторяет последний ACK — отправитель
 * перешлёт. Так из потерь и перестановок получается ровный поток.
 *
 * Соединение:  клиент SYN ->  <- SYN+ACK сервер  ACK ->   (три рукопожатия)
 * Закрытие:    FIN ->  <- ACK   ...   <- FIN   ACK ->      (каждая сторона отдельно)
 *
 * Упрощения: без опций окна/SACK, без медленного старта, одно окно 16 КБ. */
#include "net.h"
#include "string.h"
#include "timer.h"
#include "heap.h"
#include "syscall.h"

#define F_FIN 0x01
#define F_SYN 0x02
#define F_RST 0x04
#define F_PSH 0x08
#define F_ACK 0x10
#define MSS   1460
#define RTO_MIN  (TIMER_HZ / 3)         /* ~300 мс */
#define RTO_MAX  (TIMER_HZ * 4)
#define MAX_RETRIES 8

static inline bool seq_lt(u32 a, u32 b)  { return (i32)(a - b) < 0; }
static inline bool seq_le(u32 a, u32 b)  { return (i32)(a - b) <= 0; }

static u32 rx_free(struct sock *s) { return SOCK_RXBUF - s->rx_len; }

/* ------------------------------------------------------------ отправка сегмента */

struct pseudo { u32 src, dst; u8 zero, proto; u16 len; } __attribute__((packed));

static void send_segment(struct sock *s, u8 flags, u32 seq, const u8 *data, u32 len) {
    static u8 pkt[sizeof(struct tcp_hdr) + 4 + MSS];
    struct tcp_hdr *h = (struct tcp_hdr *)pkt;
    u32 hlen = sizeof(*h);
    memset(h, 0, sizeof(*h));
    h->sport = htons(s->lport);
    h->dport = htons(s->rport);
    h->seq = htonl(seq);
    h->ack = (flags & F_ACK) ? htonl(s->rcv_nxt) : 0;
    h->flags = flags;
    u32 win = rx_free(s);
    h->win = htons(win > 65535 ? 65535 : win);
    if (flags & F_SYN) {                         /* опция MSS: «присылай куски не больше 1460» */
        u8 *o = pkt + hlen;
        o[0] = 2; o[1] = 4; o[2] = MSS >> 8; o[3] = MSS & 0xFF;
        hlen += 4;
    }
    h->off = (hlen / 4) << 4;
    if (len) memcpy(pkt + hlen, data, len);

    /* контрольная сумма считается вместе с «псевдозаголовком» из IP-адресов */
    struct pseudo ph = { htonl(net_src_ip(s->rip)), htonl(s->rip), 0, PROTO_TCP, htons(hlen + len) };
    u32 sum = 0;
    const u8 *pp = (const u8 *)&ph;
    for (u32 i = 0; i < sizeof(ph); i += 2) sum += (pp[i] << 8) | pp[i + 1];
    h->csum = net_checksum(pkt, hlen + len, sum);
    ip_send(s->rip, PROTO_TCP, pkt, hlen + len);
}

static void send_ack(struct sock *s) { send_segment(s, F_ACK, s->snd_nxt, NULL, 0); }

static void send_rst(u32 dst, u16 sport, u16 dport, u32 seq, u32 ack, bool with_ack) {
    struct sock tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.rip = dst; tmp.lport = sport; tmp.rport = dport; tmp.rcv_nxt = ack;
    send_segment(&tmp, F_RST | (with_ack ? F_ACK : 0), seq, NULL, 0);
}

static void arm_timer(struct sock *s) {
    if (!s->rto) s->rto = RTO_MIN;
    s->rtx_deadline = timer_ticks() + s->rto;
}

/* Отправить всё, что можно: новые данные (сколько разрешает окно получателя),
 * затем FIN, если программа закрыла сокет и данные кончились. */
static void output(struct sock *s) {
    if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT &&
        s->state != TCP_FIN_WAIT1 && s->state != TCP_LAST_ACK && s->state != TCP_CLOSING) return;
    for (;;) {
        u32 sent = s->snd_nxt - s->snd_una;            /* уже в полёте */
        if (s->fin_sent) break;
        if (sent >= s->tx_len) break;
        u32 wnd = s->snd_wnd ? s->snd_wnd : 1;
        if (sent >= wnd) break;
        u32 n = s->tx_len - sent;
        if (n > MSS) n = MSS;
        if (n > wnd - sent) n = wnd - sent;
        send_segment(s, F_ACK | F_PSH, s->snd_nxt, s->txbuf + sent, n);
        s->snd_nxt += n;
        if (!s->rtx_deadline) arm_timer(s);
    }
    if (s->fin_pending && !s->fin_sent && s->snd_nxt - s->snd_una == s->tx_len) {
        send_segment(s, F_FIN | F_ACK, s->snd_nxt, NULL, 0);
        s->snd_nxt++;
        s->fin_sent = true;
        if (s->state == TCP_ESTABLISHED) s->state = TCP_FIN_WAIT1;
        else if (s->state == TCP_CLOSE_WAIT) s->state = TCP_LAST_ACK;
        if (!s->rtx_deadline) arm_timer(s);
    }
}

/* ------------------------------------------------------------ приём сегмента */

static struct sock *find(u32 rip, u16 rport, u16 lport) {
    struct sock *listener = NULL;
    for (int i = 0; i < MAX_SOCKS; i++) {
        struct sock *s = socks[i];
        if (!s || s->type != SOCK_TCP || s->lport != lport) continue;
        if (s->state == TCP_LISTEN) { listener = s; continue; }
        if (s->rip == rip && s->rport == rport && s->state != TCP_CLOSED) return s;
    }
    return listener;
}

static void set_closed(struct sock *s, int err) {
    s->state = TCP_CLOSED;
    if (err) s->err = err;
    s->rtx_deadline = 0;
    task_wakeup(s);
    if (s->parent) task_wakeup(s->parent);
}

void tcp_input(u32 src, u32 dst, const u8 *seg, u32 len) {
    if (len < sizeof(struct tcp_hdr)) return;
    const struct tcp_hdr *h = (const struct tcp_hdr *)seg;
    u32 hlen = (h->off >> 4) * 4;
    if (hlen < 20 || hlen > len) return;
    struct pseudo ph = { htonl(src), htonl(dst), 0, PROTO_TCP, htons(len) };
    u32 sum = 0;
    const u8 *pp = (const u8 *)&ph;
    for (u32 i = 0; i < sizeof(ph); i += 2) sum += (pp[i] << 8) | pp[i + 1];
    if (net_checksum(seg, len, sum) != 0) return;          /* испорчен — пусть перешлют */

    u16 sport = ntohs(h->sport), dport = ntohs(h->dport);
    u32 seq = ntohl(h->seq), ack = ntohl(h->ack);
    u8 fl = h->flags;
    const u8 *data = seg + hlen;
    u32 dlen = len - hlen;

    struct sock *s = find(src, sport, dport);
    if (!s) {                                               /* никто не слушает этот порт */
        if (!(fl & F_RST)) {
            if (fl & F_ACK) send_rst(src, dport, sport, ack, 0, false);
            else send_rst(src, dport, sport, 0, seq + dlen + ((fl & F_SYN) ? 1 : 0), true);
        }
        return;
    }

    if (fl & F_RST) {
        if (s->state != TCP_LISTEN)
            set_closed(s, s->state == TCP_SYN_SENT ? -ECONNREFUSED : -ECONNRESET);
        return;
    }

    if (s->state == TCP_LISTEN) {                           /* новый клиент стучится */
        if (!(fl & F_SYN)) return;
        int pending = 0;
        for (int i = 0; i < MAX_SOCKS; i++)
            if (socks[i] && socks[i]->parent == s && socks[i]->state == TCP_SYN_RCVD) pending++;
        if (s->aq_n + pending >= s->backlog) return;        /* очередь полна — пусть повторит */
        struct sock *c = sock_alloc(SOCK_TCP);
        if (!c) return;
        c->lport = s->lport;
        c->rip = src;
        c->rport = sport;
        c->parent = s;
        c->rcv_nxt = seq + 1;
        c->iss = timer_ticks() * 64000 + 12345;
        c->snd_una = c->iss;
        c->snd_nxt = c->iss + 1;
        c->snd_wnd = ntohs(h->win);
        c->state = TCP_SYN_RCVD;
        send_segment(c, F_SYN | F_ACK, c->iss, NULL, 0);
        arm_timer(c);
        return;
    }

    if (s->state == TCP_SYN_SENT) {                         /* ждём SYN+ACK на наш SYN */
        if ((fl & F_ACK) && ack != s->iss + 1) { send_rst(src, dport, sport, ack, 0, false); return; }
        if ((fl & F_SYN) && (fl & F_ACK)) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_wnd = ntohs(h->win);
            s->state = TCP_ESTABLISHED;
            s->rtx_deadline = 0;
            s->retries = 0;
            send_ack(s);
            task_wakeup(s);
            output(s);
        }
        return;
    }

    /* ---- синхронизированные состояния ---- */
    if (fl & F_SYN) { send_ack(s); return; }               /* повтор SYN+ACK: наш ACK потерялся */

    if (fl & F_ACK) {
        if (s->state == TCP_SYN_RCVD && ack == s->snd_nxt) {   /* рукопожатие завершено */
            s->state = TCP_ESTABLISHED;
            s->snd_una = ack;
            s->rtx_deadline = 0;
            s->retries = 0;
            struct sock *p = s->parent;
            if (p && p->state == TCP_LISTEN && p->aq_n < ACCEPT_MAX) {
                p->aq[p->aq_n++] = s;
                task_wakeup(p);
            }
        }
        if (seq_lt(s->snd_una, ack) && seq_le(ack, s->snd_nxt)) {   /* подтверждены новые байты */
            u32 n = ack - s->snd_una;
            u32 data_acked = n > s->tx_len ? s->tx_len : n;
            memmove(s->txbuf, s->txbuf + data_acked, s->tx_len - data_acked);
            s->tx_len -= data_acked;
            s->snd_una = ack;
            if (s->fin_sent && ack == s->snd_nxt) s->fin_acked = true;
            s->rto = RTO_MIN;
            s->retries = 0;
            s->rtx_deadline = s->snd_una != s->snd_nxt ? timer_ticks() + s->rto : 0;
            task_wakeup(s);                                 /* в буфере отправки появилось место */
        }
        s->snd_wnd = ntohs(h->win);
        if (s->fin_acked) {
            if (s->state == TCP_FIN_WAIT1) s->state = TCP_FIN_WAIT2;
            else if (s->state == TCP_CLOSING) { s->state = TCP_TIME_WAIT; s->close_deadline = timer_ticks() + TIMER_HZ; }
            else if (s->state == TCP_LAST_ACK) set_closed(s, 0);
        }
    }

    bool can_recv = s->state == TCP_ESTABLISHED || s->state == TCP_FIN_WAIT1 || s->state == TCP_FIN_WAIT2;
    u32 taken = 0;
    if (dlen && can_recv) {
        if (seq == s->rcv_nxt) {                            /* ровно следующий кусок — берём */
            taken = dlen < rx_free(s) ? dlen : rx_free(s);
            for (u32 i = 0; i < taken; i++)
                s->rxbuf[(s->rx_head + s->rx_len + i) % SOCK_RXBUF] = data[i];
            s->rx_len += taken;
            s->rcv_nxt += taken;
            task_wakeup(s);
        }
        send_ack(s);                                        /* в любом случае скажем, чего ждём */
    }

    if ((fl & F_FIN) && s->peer_fin) {
        send_ack(s);                                        /* повтор FIN: наш ACK потерялся */
    } else if ((fl & F_FIN) && can_recv && taken == dlen && seq + dlen == s->rcv_nxt) {
        /* FIN принимаем, только если все данные перед ним уже взяты */
        s->rcv_nxt++;                                       /* FIN занимает один номер */
        s->peer_fin = true;
        send_ack(s);
        if (s->state == TCP_ESTABLISHED) s->state = TCP_CLOSE_WAIT;
        else if (s->state == TCP_FIN_WAIT1) s->state = s->fin_acked ? TCP_TIME_WAIT : TCP_CLOSING;
        else if (s->state == TCP_FIN_WAIT2) s->state = TCP_TIME_WAIT;
        if (s->state == TCP_TIME_WAIT) s->close_deadline = timer_ticks() + TIMER_HZ;
        task_wakeup(s);
    }
    output(s);
}

/* ------------------------------------------------------------ таймеры (каждые 10 мс) */

void tcp_timer(void) {
    u32 now = timer_ticks();
    for (int i = 0; i < MAX_SOCKS; i++) {
        struct sock *s = socks[i];
        if (!s || s->type != SOCK_TCP) continue;
        if (s->state == TCP_TIME_WAIT && (i32)(now - s->close_deadline) >= 0) set_closed(s, 0);
        if (!s->rtx_deadline || (i32)(now - s->rtx_deadline) < 0) continue;

        if (++s->retries > MAX_RETRIES) { set_closed(s, -ETIMEDOUT); continue; }
        s->rto = s->rto * 2 > RTO_MAX ? RTO_MAX : s->rto * 2;     /* каждый раз ждём вдвое дольше */
        if (s->state == TCP_SYN_SENT) {
            send_segment(s, F_SYN, s->iss, NULL, 0);
        } else if (s->state == TCP_SYN_RCVD) {
            send_segment(s, F_SYN | F_ACK, s->iss, NULL, 0);
        } else {
            s->snd_nxt = s->snd_una;                         /* go-back-N: всё с первого неподтверждённого */
            if (s->fin_sent && !s->fin_acked) s->fin_sent = false;
            if (s->state == TCP_FIN_WAIT1 && !s->fin_sent) s->state = TCP_ESTABLISHED;
            if (s->state == TCP_LAST_ACK && !s->fin_sent) s->state = TCP_CLOSE_WAIT;
            s->rtx_deadline = 0;
            output(s);
        }
        if (s->state == TCP_SYN_SENT || s->state == TCP_SYN_RCVD) s->rtx_deadline = now + s->rto;
        else if (s->snd_una != s->snd_nxt) s->rtx_deadline = now + s->rto;
    }
}

/* ------------------------------------------------------------ для сокетов (под net_lock) */

int tcp_connect(struct sock *s, u32 ip, u16 port) {
    if (s->state != TCP_CLOSED) return -EISCONN;
    if (!s->lport) s->lport = sock_ephemeral_port();
    s->rip = ip;
    s->rport = port;
    s->iss = timer_ticks() * 64000 + 777;
    s->snd_una = s->iss;
    s->snd_nxt = s->iss + 1;
    s->state = TCP_SYN_SENT;
    s->rto = RTO_MIN;
    send_segment(s, F_SYN, s->iss, NULL, 0);
    arm_timer(s);
    return 0;
}

int tcp_listen(struct sock *s, int backlog) {
    if (!s->lport) return -EINVAL;
    s->state = TCP_LISTEN;
    s->backlog = backlog < 1 ? 1 : backlog > ACCEPT_MAX ? ACCEPT_MAX : backlog;
    return 0;
}

struct sock *tcp_accept(struct sock *s, int *err) {
    if (s->state != TCP_LISTEN) { *err = -EINVAL; return NULL; }
    if (!s->aq_n) { *err = 0; return NULL; }
    struct sock *c = s->aq[0];
    memmove(s->aq, s->aq + 1, (--s->aq_n) * sizeof(s->aq[0]));
    c->parent = NULL;
    return c;
}

int tcp_read(struct sock *s, void *buf, u32 len) {
    if (s->rx_len == 0) return 0;
    bool was_small = rx_free(s) < MSS;
    u32 n = len < s->rx_len ? len : s->rx_len;
    u8 *out = buf;
    for (u32 i = 0; i < n; i++) out[i] = s->rxbuf[(s->rx_head + i) % SOCK_RXBUF];
    s->rx_head = (s->rx_head + n) % SOCK_RXBUF;
    s->rx_len -= n;
    if (was_small && s->state != TCP_CLOSED) send_ack(s);   /* окно снова открылось — скажем */
    return n;
}

int tcp_write(struct sock *s, const void *buf, u32 len) {
    u32 room = SOCK_TXBUF - s->tx_len;
    u32 n = len < room ? len : room;
    memcpy(s->txbuf + s->tx_len, buf, n);
    s->tx_len += n;
    output(s);
    return n;
}

void tcp_close(struct sock *s) {
    switch (s->state) {
    case TCP_ESTABLISHED: case TCP_CLOSE_WAIT: case TCP_SYN_RCVD:
        if (s->state == TCP_SYN_RCVD) s->state = TCP_ESTABLISHED;
        s->fin_pending = true;
        output(s);
        break;
    case TCP_LISTEN:                                        /* непринятых клиентов — сбросить */
        for (int i = 0; i < MAX_SOCKS; i++) {
            struct sock *c = socks[i];
            if (c && c->parent == s) {
                send_segment(c, F_RST | F_ACK, c->snd_nxt, NULL, 0);
                c->parent = NULL;
                c->user_closed = true;
                c->state = TCP_CLOSED;
            }
        }
        s->state = TCP_CLOSED;
        break;
    case TCP_SYN_SENT:
        s->state = TCP_CLOSED;
        break;
    default:
        break;
    }
}
