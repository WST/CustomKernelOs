/* poweroff — выключить компьютер */
#include "myos.h"

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) { dprintf(STDERR, "usage: poweroff\n"); return 2; }
    puts("Powering off...");
    if (reboot(RB_POWEROFF) < 0) {
        dprintf(STDERR, "poweroff: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
