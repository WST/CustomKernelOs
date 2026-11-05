/* wc [file] — строки, слова, байты. Без аргумента читает stdin (клавиатура: до Ctrl+D). */
#include "myos.h"

int main(int argc, char **argv) {
    int fd = STDIN;
    if (argc > 1 && (fd = open(argv[1], O_READ)) < 0) {
        dprintf(STDERR, "wc: cannot open %s\n", argv[1]);
        return 1;
    }

    char buf[256];
    unsigned lines = 0, words = 0, bytes = 0;
    bool in_word = false;
    int n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            bytes++;
            if (buf[i] == '\n') lines++;
            if (isspace(buf[i])) in_word = false;
            else if (!in_word) { in_word = true; words++; }
        }
    }
    if (fd != STDIN) close(fd);
    printf("%7u %7u %7u %s\n", lines, words, bytes, argc > 1 ? argv[1] : "");
    return 0;
}
