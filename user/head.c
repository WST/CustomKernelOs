/* head [-n N] [file] — первые N строк (по умолчанию 10) */
#include "myos.h"

int main(int argc, char **argv) {
    int n = 10, i = 1;
    if (i + 1 < argc && !strcmp(argv[i], "-n")) { n = atoi(argv[i + 1]); i += 2; }
    int fd = STDIN;
    if (i < argc && (fd = open(argv[i], O_READ)) < 0) {
        dprintf(STDERR, "head: %s: %s\n", argv[i], strerror(-fd));
        return 1;
    }
    char line[256];
    for (int k = 0; k < n && readln(fd, line, sizeof(line)) >= 0; k++) printf("%s\n", line);
    return 0;   /* выходим, не дочитав: пишущий в канал получит EPIPE */
}
