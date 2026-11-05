/* sleep N — подождать N секунд */
#include "myos.h"

int main(int argc, char **argv) {
    if (argc < 2) { dprintf(STDERR, "usage: sleep seconds\n"); return 2; }
    sleep_ms(atoi(argv[1]) * 1000);
    return 0;
}
