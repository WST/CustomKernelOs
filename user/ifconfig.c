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

    struct net_info n;
    if (netinfo(&n) < 0 || !n.up) { printf("No network card. Start QEMU with: -nic user,model=e1000\n"); return 1; }
    char a[16], b[16], c[16], d[16];
    printf(ANSI_BOLD "eth0" ANSI_RESET "  Intel e1000   MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
           n.mac[0], n.mac[1], n.mac[2], n.mac[3], n.mac[4], n.mac[5]);
    if (!n.ip) { printf("      no address yet (DHCP in progress...)\n"); return 0; }
    printf("      inet " ANSI_GREEN "%s" ANSI_RESET "  mask %s", ip_str(n.ip, a), ip_str(n.mask, b));
    if (n.dhcp) printf("  (DHCP, lease %u s)\n", n.lease); else printf("  (static)\n");
    printf("      gateway %s  dns %s\n", ip_str(n.gw, c), ip_str(n.dns, d));
    size_str(n.rx_bytes, a); size_str(n.tx_bytes, b);
    printf("      RX %u packets (%s)   TX %u packets (%s)\n", n.rx_packets, a, n.tx_packets, b);
    return 0;
}
