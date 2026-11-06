/* Сокеты: то, чем программы пользуются сетью.
 *
 * Сокет — объект ядра, а для программы — обычный файловый дескриптор
 * (struct file типа FILE_SOCKET). Поэтому read/write/close/dup2 работают как
 * с файлами, а sh может, например, перенаправить вывод программы в сокет.
 *
 * Ожидание («нет данных — спим») устроено как везде в ядре: проверка и
 * засыпание — с запрещёнными прерываниями, а сетевой поток будит task_wakeup(s). */
#include "net.h"
#include "heap.h"
#include "string.h"
#include "timer.h"
#include "syscall.h"

struct sock *socks[MAX_SOCKS];
static u16 next_port = 49152;
static struct sock *deferred[MAX_SOCKS];        /* закрытые из прерывания (Ctrl+C) */
static int n_deferred;

struct sock *sock_alloc(int type) {
    struct sock *s = kcalloc(1, sizeof(*s));
    if (!s) return NULL;
    s->type = type;
    if (type == SOCK_TCP) {
        s->rxbuf = kmalloc(SOCK_RXBUF);
        s->txbuf = kmalloc(SOCK_TXBUF);
        if (!s->rxbuf || !s->txbuf) { kfree(s->rxbuf); kfree(s->txbuf); kfree(s); return NULL; }
    }
    u32 f = irq_save();
    for (int i = 0; i < MAX_SOCKS; i++)
        if (!socks[i]) { socks[i] = s; irq_restore(f); return s; }
    irq_restore(f);
    kfree(s->rxbuf); kfree(s->txbuf); kfree(s);
    return NULL;
}

void sock_free(struct sock *s) {
    u32 f = irq_save();
    for (int i = 0; i < MAX_SOCKS; i++) if (socks[i] == s) socks[i] = NULL;
    irq_restore(f);
    for (int i = 0; i < s->qn; i++) kfree(s->q[(s->qh + i) % UDP_QUEUE]);
    kfree(s->rxbuf);
    kfree(s->txbuf);
    kfree(s);
}

static bool port_used(int type, u16 port) {
    for (int i = 0; i < MAX_SOCKS; i++)
        if (socks[i] && socks[i]->type == type && socks[i]->lport == port &&
            !(socks[i]->type == SOCK_TCP && socks[i]->parent)) return true;
    return false;
}

u16 sock_ephemeral_port(void) {
    for (int tries = 0; tries < 16384; tries++) {
        u16 p = next_port++;
        if (next_port < 49152) next_port = 49152;
        if (!port_used(SOCK_TCP, p) && !port_used(SOCK_UDP, p)) return p;
    }
    return 0;
}

/* сетевой поток: пришла UDP-датаграмма */
void udp_deliver(u32 src, u16 sport, u16 dport, const u8 *data, u32 len) {
    for (int i = 0; i < MAX_SOCKS; i++) {
        struct sock *s = socks[i];
        if (!s || s->type != SOCK_UDP || s->lport != dport || s->user_closed) continue;
        if (s->qn == UDP_QUEUE) return;                 /* очередь полна — выбрасываем, как в UDP и положено */
        struct dgram *d = kmalloc(sizeof(*d) + len);
        if (!d) return;
        d->ip = src; d->port = sport; d->len = len;
        memcpy(d->data, data, len);
        u32 f = irq_save();
        s->q[(s->qh + s->qn) % UDP_QUEUE] = d;
        s->qn++;
        irq_restore(f);
        task_wakeup(s);
        return;
    }
}

/* ------------------------------------------------------------ закрытие */

static void close_now(struct sock *s) {
    s->user_closed = true;
    s->close_deadline = timer_ticks() + 10 * TIMER_HZ;  /* дольше 10 с не ждём вежливого закрытия */
    if (s->type == SOCK_TCP) tcp_close(s);
}

void sock_close(struct sock *s) {
    if (irq_nesting) {                  /* из прерывания мьютекс брать нельзя — доделает сетевой поток */
        u32 f = irq_save();
        if (n_deferred < MAX_SOCKS) deferred[n_deferred++] = s;
        irq_restore(f);
        return;
    }
    mutex_lock(&net_lock);
    close_now(s);
    mutex_unlock(&net_lock);
}

/* под net_lock, из сетевого потока */
void sock_reap(void) {
    u32 f = irq_save();
    while (n_deferred) {
        struct sock *s = deferred[--n_deferred];
        irq_restore(f);
        close_now(s);
        f = irq_save();
    }
    irq_restore(f);
    u32 now = timer_ticks();
    for (int i = 0; i < MAX_SOCKS; i++) {
        struct sock *s = socks[i];
        if (!s || !s->user_closed) continue;
        bool done = s->type == SOCK_UDP || s->state == TCP_CLOSED ||
                    (i32)(now - s->close_deadline) >= 0;
        if (done) sock_free(s);
    }
}

/* ------------------------------------------------------------ чтение и запись */

int sock_read(struct sock *s, void *buf, u32 len) {
    u32 deadline = s->rcv_timeout ? timer_ticks() + (s->rcv_timeout + 9) / 10 : 0;
    for (;;) {
        mutex_lock(&net_lock);
        if (s->type == SOCK_UDP) {
            if (s->qn) {
                struct dgram *d = s->q[s->qh];
                u32 n = len < d->len ? len : d->len;
                memcpy(buf, d->data, n);
                u32 f = irq_save();
                s->qh = (s->qh + 1) % UDP_QUEUE;
                s->qn--;
                irq_restore(f);
                kfree(d);
                mutex_unlock(&net_lock);
                return n;
            }
        } else {
            if (s->rx_len) { int n = tcp_read(s, buf, len); mutex_unlock(&net_lock); return n; }
            if (s->peer_fin || s->state == TCP_CLOSED) {
                int r = s->state == TCP_CLOSED && s->err ? s->err : 0;   /* 0 = конец потока */
                mutex_unlock(&net_lock);
                return r;
            }
        }
        mutex_unlock(&net_lock);
        u32 f = irq_save();
        if (s->type == SOCK_UDP ? !s->qn : (!s->rx_len && !s->peer_fin && s->state != TCP_CLOSED)) {
            if (!deadline) task_wait(s);
            else if ((i32)(timer_ticks() - deadline) >= 0) { irq_restore(f); return -ETIMEDOUT; }
            else task_wait_timeout(s, deadline - timer_ticks());
        }
        irq_restore(f);
        if (task_current()->kill_pending) return -EINTR;
    }
}

int sock_write(struct sock *s, const void *buf, u32 len) {
    if (s->type == SOCK_UDP) {
        if (!s->rport) return -ENOTCONN;
        mutex_lock(&net_lock);
        if (!s->lport) s->lport = sock_ephemeral_port();
        int r = udp_send(s->rip, s->lport, s->rport, buf, len);
        mutex_unlock(&net_lock);
        return r < 0 ? r : (int)len;
    }
    u32 total = 0;
    const u8 *p = buf;
    while (total < len) {
        mutex_lock(&net_lock);
        if (s->state != TCP_ESTABLISHED && s->state != TCP_CLOSE_WAIT) {
            int err = s->err ? s->err : -EPIPE;
            mutex_unlock(&net_lock);
            return total ? (int)total : err;
        }
        u32 n = tcp_write(s, p + total, len - total);
        mutex_unlock(&net_lock);
        total += n;
        if (n) continue;
        u32 f = irq_save();                              /* буфер полон — ждём подтверждений */
        if (s->tx_len == SOCK_TXBUF && (s->state == TCP_ESTABLISHED || s->state == TCP_CLOSE_WAIT))
            task_wait(s);
        irq_restore(f);
        if (task_current()->kill_pending) return total ? (int)total : -EINTR;
    }
    return total;
}

/* ------------------------------------------------------------ для системных вызовов */

int sock_connect(struct sock *s, u32 ip, u16 port) {
    if (!netcfg.up) return -ENETDOWN;
    if (s->type == SOCK_UDP) {
        mutex_lock(&net_lock);
        if (!s->lport) s->lport = sock_ephemeral_port();
        s->rip = ip; s->rport = port;
        mutex_unlock(&net_lock);
        return 0;
    }
    mutex_lock(&net_lock);
    int r = tcp_connect(s, ip, port);
    mutex_unlock(&net_lock);
    if (r) return r;
    u32 deadline = s->conn_timeout ? timer_ticks() + (s->conn_timeout + 9) / 10 : 0;
    for (;;) {                                           /* ждём SYN+ACK (или отказа, или тайм-аута) */
        u32 f = irq_save();
        if (s->state == TCP_SYN_SENT) {
            if (!deadline) task_wait(s);
            else if ((i32)(timer_ticks() - deadline) < 0) task_wait_timeout(s, deadline - timer_ticks());
        }
        irq_restore(f);
        if (task_current()->kill_pending) return -EINTR;
        if (s->state != TCP_SYN_SENT) break;
        if (deadline && (i32)(timer_ticks() - deadline) >= 0) {   /* сдаёмся: сокет снова «закрыт» */
            mutex_lock(&net_lock);
            if (s->state == TCP_SYN_SENT) { s->state = TCP_CLOSED; s->rtx_deadline = 0; }
            mutex_unlock(&net_lock);
            return -ETIMEDOUT;
        }
    }
    return s->state == TCP_ESTABLISHED ? 0 : (s->err ? s->err : -ECONNREFUSED);
}

/* настройки сокета: SOCK_RCVTIMEO (1) — сколько мс ждать данных в read (0 — вечно),
 * SOCK_CONNTIMEO (2) — сколько мс ждать ответа на connect */
int sock_ctl(struct sock *s, int op, u32 arg) {
    switch (op) {
    case 1: s->rcv_timeout = arg; return 0;
    case 2: s->conn_timeout = arg; return 0;
    case 3: return s->type == SOCK_TCP ? (int)s->rx_len : s->qn;   /* сколько можно прочитать без ожидания */
    }
    return -EINVAL;
}

int sock_bind(struct sock *s, u16 port) {
    mutex_lock(&net_lock);
    int r = 0;
    if (port_used(s->type, port)) r = -EADDRINUSE;
    else s->lport = port;
    mutex_unlock(&net_lock);
    return r;
}

int sock_listen(struct sock *s, int backlog) {
    if (s->type != SOCK_TCP) return -EINVAL;
    mutex_lock(&net_lock);
    int r = tcp_listen(s, backlog);
    mutex_unlock(&net_lock);
    return r;
}

struct sock *sock_accept(struct sock *s, int *err) {
    for (;;) {
        mutex_lock(&net_lock);
        struct sock *c = tcp_accept(s, err);
        mutex_unlock(&net_lock);
        if (c || *err) return c;
        u32 f = irq_save();
        if (!s->aq_n && s->state == TCP_LISTEN) task_wait(s);
        irq_restore(f);
        if (task_current()->kill_pending) { *err = -EINTR; return NULL; }
    }
}

int sock_sendto(struct sock *s, const void *buf, u32 len, u32 ip, u16 port) {
    if (s->type != SOCK_UDP) return -EINVAL;
    mutex_lock(&net_lock);
    if (!s->lport) s->lport = sock_ephemeral_port();
    int r = udp_send(ip, s->lport, port, buf, len);
    mutex_unlock(&net_lock);
    return r < 0 ? r : (int)len;
}

/* timeout_ms = 0 — ждать вечно */
int sock_recvfrom(struct sock *s, void *buf, u32 len, u32 *ip, u16 *port, u32 timeout_ms) {
    if (s->type != SOCK_UDP) return -EINVAL;
    u32 deadline = timer_ticks() + (timeout_ms + 9) / 10;
    for (;;) {
        mutex_lock(&net_lock);
        if (s->qn) {
            struct dgram *d = s->q[s->qh];
            u32 n = len < d->len ? len : d->len;
            memcpy(buf, d->data, n);
            *ip = d->ip; *port = d->port;
            u32 f = irq_save();
            s->qh = (s->qh + 1) % UDP_QUEUE;
            s->qn--;
            irq_restore(f);
            kfree(d);
            mutex_unlock(&net_lock);
            return n;
        }
        mutex_unlock(&net_lock);
        if (timeout_ms && (i32)(timer_ticks() - deadline) >= 0) return -ETIMEDOUT;
        u32 f = irq_save();
        if (!s->qn) {
            if (timeout_ms) task_wait_timeout(s, deadline - timer_ticks());
            else task_wait(s);
        }
        irq_restore(f);
        if (task_current()->kill_pending) return -EINTR;
    }
}
