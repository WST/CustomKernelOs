/* ping [-c N] host — отправить эхо-запросы ICMP (Ctrl+C — остановить) */
#include "myos.h"

static bool stop_requested(void) {          /* Ctrl+C в режиме NOSIG приходит как код 3 */
    int k;
    while ((k = readkey()) > 0) if (k == 3) return true;
    return false;
}

int main(int argc, char **argv) {
    int count = 4, ai = 1;
    if (argc > 2 && !strcmp(argv[1], "-c")) { count = atoi(argv[2]); ai = 3; }
    if (ai != argc - 1) { dprintf(STDERR, "usage: ping [-c count] host   (count 0 = until Ctrl+C)\n"); return 2; }
    const char *host = argv[ai];
    unsigned ip;
    int r = resolve(host, &ip);
    if (r < 0) { dprintf(STDERR, "ping: %s: %s\n", host, r == -ENOENT ? "host not found" : strerror(-r)); return 1; }
    char ips[16];
    ip_str(ip, ips);
    printf("PING %s (%s): 32 data bytes\n", host, ips);

    int old = ttymode(TTY_RAW | TTY_NONBLOCK | TTY_NOSIG);
    int sent = 0, got = 0, tmin = 1 << 30, tmax = 0, tsum = 0;
    bool stop = false;
    for (int seq = 1; (count == 0 || seq <= count) && !stop; seq++) {
        unsigned start = ticks();
        r = ping(ip, seq, 1000);
        sent++;
        if (r >= 0) {
            got++; tsum += r;
            if (r < tmin) tmin = r;
            if (r > tmax) tmax = r;
            printf("reply from %s: seq=%d time=%s%d ms\n", ips, seq, r < 10 ? "<" : "", r < 10 ? 10 : r);
        } else if (r == -EINTR) {
            break;
        } else {
            printf("seq=%d: %s\n", seq, strerror(-r));
        }
        if (stop_requested()) break;
        if (count && seq == count) break;
        while (ticks() - start < 1000 && !(stop = stop_requested())) sleep_ms(50);   /* раз в секунду */
    }
    ttymode(old);
    printf("--- %s ping statistics ---\n", host);
    printf("%d sent, %d received, %d%% loss", sent, got, sent ? (sent - got) * 100 / sent : 0);
    if (got) printf(", time min/avg/max = %d/%d/%d ms", tmin, tsum / got, tmax);
    printf("\n");
    return got ? 0 : 1;
}
