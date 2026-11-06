/* cat [file...] — вывести файлы; без аргументов копирует stdin в stdout */
#include "myos.h"

static int copy(int fd) {
    char buf[512];
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        if (write(STDOUT, buf, n) < 0) return 1;
    return n < 0;
}

int main(int argc, char **argv) {
    if (argc < 2) return copy(STDIN);
    int rc = 0;
    for (int i = 1; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) { dprintf(STDERR, "cat: %s: %s\n", argv[i], strerror(errno)); rc = 1; continue; }
        rc |= copy(fd);
        close(fd);
    }
    return rc;
}
