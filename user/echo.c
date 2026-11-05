/* echo [args...] — напечатать аргументы через пробел */
#include "myos.h"

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (i > 1) write(STDOUT, " ", 1);
        write(STDOUT, argv[i], strlen(argv[i]));
    }
    write(STDOUT, "\n", 1);
    return 0;
}
