/* mkdir dir... — создать каталоги */
#include "myos.h"

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: mkdir dir...\n"); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = mkdir(argv[i]);
        if (r < 0) { dprintf(STDERR, "mkdir: %s: %s\n", argv[i], strerror(-r)); rc = 1; }
    }
    return rc;
}
