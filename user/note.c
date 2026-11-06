/* note [text...] — дописать строку в /notes.txt; без аргументов — показать заметки. */
#include "myos.h"

int main(int argc, char **argv) {
    if (argc == 1) {
        int fd = open("/notes.txt", O_READ);
        if (fd < 0) { printf("no notes yet. Add one: note buy milk\n"); return 0; }
        char buf[256];
        int n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) write(STDOUT, buf, n);
        close(fd);
        return 0;
    }
    int fd = open("/notes.txt", O_WRONLY | O_CREAT | O_APPEND);
    if (fd < 0) { dprintf(STDERR, "note: cannot open /notes.txt\n"); return 1; }
    char line[256];
    line[0] = 0;
    for (int i = 1; i < argc; i++) {
        if (strlen(line) + strlen(argv[i]) + 2 >= sizeof(line)) break;
        if (i > 1) strcat(line, " ");
        strcat(line, argv[i]);
    }
    strcat(line, "\n");
    write(fd, line, strlen(line));
    close(fd);
    printf("saved.\n");
    return 0;
}
