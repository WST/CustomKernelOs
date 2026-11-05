/* kill pid... — завершить процессы */
#include "myos.h"

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: kill pid...\n"); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = kill(atoi(argv[i]));
        if (r < 0) { dprintf(STDERR, "kill: %s: %s\n", argv[i], strerror(-r)); rc = 1; }
    }
    return rc;
}
