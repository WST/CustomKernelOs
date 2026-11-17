#include "myos.h"
int main(int argc, char **argv) {
    if (argc != 2) { dprintf(2, "usage: kldload /boot/modules/name.ko\n"); return 1; }
    int id = modctl(1, (int)argv[1], 0);
    if (id < 0) { dprintf(2, "kldload: %d\n", id); return 1; }
    printf("loaded %s (%d)\n", argv[1], id);
    return 0;
}
