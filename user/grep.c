/* grep [-v] [-i] [-c] [-n] pattern [file...] — строки, содержащие pattern */
#include "myos.h"

static bool opt_v, opt_i, opt_c, opt_n;

static bool contains(const char *line, const char *pat) {
    size_t m = strlen(pat);
    for (const char *s = line; *s; s++) {
        size_t k = 0;
        while (k < m && s[k] && (opt_i ? toupper(s[k]) == toupper(pat[k]) : s[k] == pat[k])) k++;
        if (k == m) return true;
    }
    return m == 0;
}

static int search(int fd, const char *pat, const char *name) {
    char line[256];
    int count = 0, lineno = 0;
    while (readln(fd, line, sizeof(line)) >= 0) {
        lineno++;
        if (contains(line, pat) == opt_v) continue;
        count++;
        if (opt_c) continue;
        if (name) printf("%s:", name);
        if (opt_n) printf("%d:", lineno);
        printf("%s\n", line);
    }
    if (opt_c) { if (name) printf("%s:", name); printf("%d\n", count); }
    return count ? 0 : 1;                    /* как в Unix: 1 = ничего не нашли */
}

int main(int argc, char **argv) {
    int i = 1;
    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++)
        for (char *o = argv[i] + 1; *o; o++) {
            if (*o == 'v') opt_v = true;
            else if (*o == 'i') opt_i = true;
            else if (*o == 'c') opt_c = true;
            else if (*o == 'n') opt_n = true;
            else { dprintf(STDERR, "grep: unknown option -%c\n", *o); return 2; }
        }
    if (i >= argc) { dprintf(STDERR, "usage: grep [-vicn] pattern [file...]\n"); return 2; }
    const char *pat = argv[i++];
    if (i == argc) return search(STDIN, pat, NULL);
    bool names = argc - i > 1;              /* несколько файлов — подписываем имена */
    int rc = 1;
    for (; i < argc; i++) {
        int fd = open(argv[i], O_READ);
        if (fd < 0) { dprintf(STDERR, "grep: %s: %s\n", argv[i], strerror(-fd)); continue; }
        if (search(fd, pat, names ? argv[i] : NULL) == 0) rc = 0;
        close(fd);
    }
    return rc;
}
