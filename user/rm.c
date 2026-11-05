/* rm path... — удалить файлы или пустые каталоги */
#include "myos.h"

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: rm path...\n"); return 2; }
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int r = unlink(argv[i]);
        if (r < 0) { dprintf(STDERR, "rm: %s: %s\n", argv[i], strerror(-r)); rc = 1; }
    }
    return rc;
}
