/* yes [text] — печатать text (или y) бесконечно. Попробуй: yes | head -n 3 */
#include "myos.h"

int main(int argc, char **argv) {
    const char *s = argc > 1 ? argv[1] : "y";
    char line[128];
    int n = snprintf(line, sizeof(line), "%s\n", s);
    for (;;) write(STDOUT, line, n);   /* когда читатель уйдёт, ядро завершит нас (EPIPE) */
}
