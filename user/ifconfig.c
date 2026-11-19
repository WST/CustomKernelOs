/* ifconfig — показать настройки сети; ifconfig dns 1.1.1.1 — сменить DNS-сервер */
#include "myos.h"

static void size_str(unsigned b, char *out) {
    if (b < 10240) snprintf(out, 16, "%u B", b);
    else if (b < 10 * 1024 * 1024) snprintf(out, 16, "%u KB", b / 1024);
    else snprintf(out, 16, "%u MB", b / (1024 * 1024));
}

int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "dns")) {
        unsigned ip;
        if (!ip_parse(argv[2], &ip)) { dprintf(STDERR, "ifconfig: bad address '%s'\n", argv[2]); return 2; }
        set_dns(ip);
        printf("DNS server is now %s\n", argv[2]);
        return 0;
    }
    if (argc != 1) { dprintf(STDERR, "usage: ifconfig            show settings\n       ifconfig dns IP     use another DNS server\n"); return 2; }

    struct if_info inf;
    int any = 0;
    char a[16], b[16], c[16], d[16];
    for (int i = 0; i < 4; i++) {
        if (ifctl(i, &inf) < 0) break;
        any = 1;
        const char *kind = inf.kind == 2 ? "Wi-Fi" : "Ethernet";
        printf(ANSI_BOLD "%s" ANSI_RESET "  %s   MAC %02x:%02x:%02x:%02x:%02x:%02x  link %s\n",
               inf.name, kind, inf.mac[0], inf.mac[1], inf.mac[2], inf.mac[3], inf.mac[4], inf.mac[5],
               inf.link ? "up" : "down");
        if (!inf.ip) { printf("      no address yet (DHCP in progress...)\n"); continue; }
        printf("      inet " ANSI_GREEN "%s" ANSI_RESET "  mask %s", ip_str(inf.ip, a), ip_str(inf.mask, b));
        if (inf.dhcp) printf("  (DHCP, lease %u s)\n", inf.lease); else printf("  (static)\n");
        printf("      gateway %s  dns %s\n", ip_str(inf.gw, c), ip_str(inf.dns, d));
        size_str(inf.rx_bytes, a); size_str(inf.tx_bytes, b);
        printf("      RX %u packets (%s)   TX %u packets (%s)\n", inf.rx_packets, a, inf.tx_packets, b);
    }
    if (any) return 0;
    struct net_info n;
    if (netinfo(&n) < 0 || !n.up) { printf("No supported network adapter. Check NET boot diagnostics.\n"); return 1; }
    printf(ANSI_BOLD "eth0" ANSI_RESET "  Intel Ethernet   MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    return 0;
}
