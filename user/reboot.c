/* reboot — перезагрузить компьютер */
#include "myos.h"

int main(int argc, char **argv) {
    (void)argv;
    if (argc > 1) { dprintf(STDERR, "usage: reboot\n"); return 2; }
    puts("Rebooting...");
    if (reboot(RB_REBOOT) < 0) {
        dprintf(STDERR, "reboot: %s\n", strerror(errno));
        return 1;
    }
    return 0;
}
