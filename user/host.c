/* host name — узнать IP-адрес по имени (запрос к DNS-серверу) */
#include "myos.h"

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: host name...\n"); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        unsigned ip;
        int r = resolve(argv[i], &ip);
        char s[16];
        if (r == 0) printf("%s has address %s\n", argv[i], ip_str(ip, s));
        else { printf("%s: %s\n", argv[i], r == -ENOENT ? "not found" : strerror(-r)); rc = 1; }
    }
    return rc;
}
