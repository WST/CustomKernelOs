/* ext — минимальные порты.
 *
 *   ext list
 *   ext install <имя>
 *
 * Декларация /ext/instructions/<имя>.inst, одна команда на строку:
 *   name <id>
 *   descr <текст>
 *   workdir <путь>          создать каталог и перейти в него
 *   mkdir <путь>            создать каталог, в том числе вложенный
 *   fetch [-r N] <url> [куда]
 *   cc <аргументы cc>
 * # до конца строки — комментарий.
 * fetch вызывает /bin/download. Без -r повторов два.
 * tar, git и svn в этом языке задуманы, но пока это неизвестная команда.
 */
#include "myos.h"
#include "stdio.h"
#include "dirent.h"

#define INST_DIR "/ext/instructions"
#define MAX_ARG  64
#define MAX_PKG  64

static int split_line(char *s, char **av) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        if (n == MAX_ARG) return -1;
        if (*s == '"') {
            av[n++] = ++s;
            while (*s && *s != '"') s++;
            if (*s == '"') *s++ = 0;
        } else {
            av[n++] = s;
            while (*s && *s != ' ' && *s != '\t' && *s != '#') s++;
            if (*s == '#') { *s = 0; break; }
            if (*s) *s++ = 0;
        }
    }
    av[n] = NULL;
    return n;
}

static int mkdir_p(const char *path) {
    char buf[128];
    int n, i;
    if (!path || !path[0]) return -EINVAL;
    n = (int)strlen(path);
    if (n >= (int)sizeof buf) return -ENAMETOOLONG;
    memcpy(buf, path, (size_t)n + 1);
    for (i = 1; i <= n; i++) {
        int r;
        char save;
        if (buf[i] != '/' && buf[i] != 0) continue;
        save = buf[i];
        buf[i] = 0;
        if (!(buf[0] == '/' && buf[1] == 0)) {
            r = mkdir(buf);
            if (r < 0 && r != -EEXIST) return r;
        }
        buf[i] = save;
    }
    return 0;
}

static int run_prog(const char *path, char **argv) {
    int pid, st = 0;
    pid = fork();
    if (pid < 0) { dprintf(STDERR, "ext: fork: %s\n", strerror(-pid)); return 1; }
    if (pid == 0) {
        int r = exec(path, argv);
        dprintf(STDERR, "ext: %s: %s\n", path, strerror(-r));
        exit(127);
    }
    if (waitpid(pid, &st, 0) < 0) return 1;
    return st;
}

static int cmd_fetch(int argc, char **argv) {
    char *av[8];
    const char *retries = "2";
    const char *url, *dest = NULL;
    int i = 1, n = 0;
    while (i < argc && argv[i][0] == '-' && argv[i][1]) {
        if (!strcmp(argv[i], "-r") && i + 1 < argc) { retries = argv[++i]; i++; }
        else { dprintf(STDERR, "ext: fetch: bad option %s\n", argv[i]); return 1; }
    }
    if (i >= argc) { dprintf(STDERR, "ext: fetch: url required\n"); return 1; }
    url = argv[i++];
    if (i < argc) dest = argv[i++];
    if (i != argc) { dprintf(STDERR, "ext: fetch: extra arguments\n"); return 1; }
    av[n++] = "download";
    av[n++] = "-r";
    av[n++] = (char *)retries;
    if (dest) { av[n++] = "-o"; av[n++] = (char *)dest; }
    av[n++] = (char *)url;
    av[n] = NULL;
    printf("fetch %s\n", url);
    return run_prog("/bin/download", av);
}

static int cmd_cc(int argc, char **argv) {
    int i;
    if (argc < 2) { dprintf(STDERR, "ext: cc: arguments required\n"); return 1; }
    argv[0] = "cc";
    printf("cc");
    for (i = 1; i < argc; i++) printf(" %s", argv[i]);
    printf("\n");
    return run_prog("/bin/cc", argv);
}

static int exec_args(int argc, char **argv) {
    int r;
    if (argc < 1) return 0;
    if (!strcmp(argv[0], "name") || !strcmp(argv[0], "descr")) return 0;
    if (!strcmp(argv[0], "workdir")) {
        if (argc != 2) { dprintf(STDERR, "ext: workdir needs one path\n"); return 1; }
        r = mkdir_p(argv[1]);
        if (r < 0) { dprintf(STDERR, "ext: %s: %s\n", argv[1], strerror(-r)); return 1; }
        r = chdir(argv[1]);
        if (r < 0) { dprintf(STDERR, "ext: %s: %s\n", argv[1], strerror(-r)); return 1; }
        return 0;
    }
    if (!strcmp(argv[0], "mkdir")) {
        if (argc != 2) { dprintf(STDERR, "ext: mkdir needs one path\n"); return 1; }
        r = mkdir_p(argv[1]);
        if (r < 0) { dprintf(STDERR, "ext: %s: %s\n", argv[1], strerror(-r)); return 1; }
        return 0;
    }
    if (!strcmp(argv[0], "fetch")) return cmd_fetch(argc, argv);
    if (!strcmp(argv[0], "cc")) return cmd_cc(argc, argv);
    dprintf(STDERR, "ext: unknown command '%s'\n", argv[0]);
    return 1;
}

static int read_line(FILE *f, char *buf, int max) {
    int i = 0, c, overflow = 0;
    while ((c = fgetc(f)) != EOF && c != '\n') {
        if (c == '\r') continue;
        if (i < max - 1) buf[i++] = (char)c;
        else overflow = 1;
    }
    buf[i] = 0;
    if (overflow) return -1;
    if (c == EOF && i == 0) return 0;
    return 1;
}

static int run_file(const char *path) {
    FILE *f = fopen(path, "r");
    char buf[1024];
    int line = 0, rc;
    if (!f) { dprintf(STDERR, "ext: %s: %s\n", path, strerror(errno)); return 1; }
    while ((rc = read_line(f, buf, (int)sizeof buf)) != 0) {
        char *av[MAX_ARG + 1];
        int n;
        line++;
        if (rc < 0) { dprintf(STDERR, "ext: %s:%d: line too long\n", path, line); fclose(f); return 1; }
        n = split_line(buf, av);
        if (n < 0) { dprintf(STDERR, "ext: %s:%d: too many words\n", path, line); fclose(f); return 1; }
        if (exec_args(n, av) != 0) { fclose(f); return 1; }
    }
    fclose(f);
    return 0;
}

static void copy_field(char *dst, int max, int argc, char **argv) {
    int i, pos = 0;
    dst[0] = 0;
    for (i = 1; i < argc && pos < max - 1; i++) {
        int k = 0;
        if (pos) dst[pos++] = ' ';
        while (argv[i][k] && pos < max - 1) dst[pos++] = argv[i][k++];
    }
    dst[pos] = 0;
}

static int read_meta(const char *path, char *name, int nmax, char *descr, int dmax) {
    FILE *f = fopen(path, "r");
    char buf[1024];
    int rc;
    name[0] = descr[0] = 0;
    if (!f) return -1;
    while ((rc = read_line(f, buf, (int)sizeof buf)) != 0) {
        char *av[MAX_ARG + 1];
        int n;
        if (rc < 0) break;
        n = split_line(buf, av);
        if (n < 1) continue;
        if (!strcmp(av[0], "name") && n >= 2 && !name[0]) copy_field(name, nmax, 2, av);
        if (!strcmp(av[0], "descr") && n >= 2 && !descr[0]) copy_field(descr, dmax, n, av);
    }
    fclose(f);
    return name[0] ? 0 : -1;
}

struct pkg {
    char name[32];
    char descr[80];
};

static int ends_inst(const char *s) {
    int n = (int)strlen(s);
    return n > 5 && !strcmp(s + n - 5, ".inst");
}

static int list_dir(const char *dir) {
    DIR *d = opendir(dir);
    struct pkg pk[MAX_PKG];
    int n = 0, i, j;
    if (!d) { dprintf(STDERR, "ext: %s: %s\n", dir, strerror(errno)); return 1; }
    for (;;) {
        struct dirent *e = readdir(d);
        char path[160];
        if (!e) break;
        if (!ends_inst(e->d_name) || n == MAX_PKG) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (read_meta(path, pk[n].name, (int)sizeof pk[n].name, pk[n].descr, (int)sizeof pk[n].descr) == 0)
            n++;
    }
    closedir(d);
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            if (strcmp(pk[j].name, pk[i].name) < 0) {
                struct pkg t = pk[i];
                pk[i] = pk[j];
                pk[j] = t;
            }
    for (i = 0; i < n; i++) {
        if (pk[i].descr[0]) printf("%s\t%s\n", pk[i].name, pk[i].descr);
        else printf("%s\n", pk[i].name);
    }
    return 0;
}

static int inst_path(const char *dir, const char *name, char *out, int max) {
    DIR *d;
    FILE *f;
    snprintf(out, (size_t)max, "%s/%s.inst", dir, name);
    f = fopen(out, "r");
    if (f) { fclose(f); return 0; }
    d = opendir(dir);
    if (!d) return -1;
    for (;;) {
        struct dirent *e = readdir(d);
        char path[160], got[32], descr[8];
        if (!e) break;
        if (!ends_inst(e->d_name)) continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        if (read_meta(path, got, (int)sizeof got, descr, (int)sizeof descr) == 0 && !strcmp(got, name)) {
            snprintf(out, (size_t)max, "%s", path);
            closedir(d);
            return 0;
        }
    }
    closedir(d);
    return -1;
}

static int install_name(const char *dir, const char *name) {
    char path[160];
    if (inst_path(dir, name, path, (int)sizeof path) < 0) {
        dprintf(STDERR, "ext: %s: no instruction in %s\n", name, dir);
        return 1;
    }
    if (run_file(path) != 0) return 1;
    printf("installed %s\n", name);
    return 0;
}

static void usage(void) {
    dprintf(STDERR, "usage: ext list | ext install <name>\n");
}

#if defined(__LINUX_TEST__)
static int selftest(void) {
    int fails = 0;
    char *av[8];
    char line[128];
    char name[32], descr[80];
    struct dirent_info st;
    FILE *f;
    snprintf(line, sizeof line, "fetch -r 1 https://example.com/a.c a.c");
    if (split_line(line, av) != 5) { printf("FAIL split fetch\n"); fails++; }
    snprintf(line, sizeof line, "descr OpenBSD wc");
    if (split_line(line, av) != 3) { printf("FAIL split descr\n"); fails++; }
    snprintf(line, sizeof line, "  # only a comment");
    if (split_line(line, av) != 0) { printf("FAIL comment\n"); fails++; }
    if (mkdir_p("/tmp/myos-ext-test/work/sub") < 0) { printf("FAIL mkdir_p\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/work/sub", &st) != 0 || st.type != 2) { printf("FAIL dir type\n"); fails++; }
    mkdir_p("/tmp/myos-ext-test/instructions");
    f = fopen("/tmp/myos-ext-test/instructions/demo.inst", "w");
    if (!f) { printf("FAIL write inst\n"); return 1; }
    fputs("name demo\ndescr hello there\nworkdir /tmp/myos-ext-test/demo\nmkdir nested\n", f);
    fclose(f);
    if (read_meta("/tmp/myos-ext-test/instructions/demo.inst", name, 32, descr, 80) != 0
        || strcmp(name, "demo") || strcmp(descr, "hello there")) {
        printf("FAIL meta '%s' '%s'\n", name, descr); fails++;
    }
    if (install_name("/tmp/myos-ext-test/instructions", "demo") != 0) { printf("FAIL install\n"); fails++; }
    if (dirstat("/tmp/myos-ext-test/demo/nested", &st) != 0 || st.type != 2) { printf("FAIL nested\n"); fails++; }
    f = fopen("/tmp/myos-ext-test/instructions/bad.inst", "w");
    if (f) { fputs("name bad\ndescr x\ntar xf a.tgz\n", f); fclose(f); }
    if (install_name("/tmp/myos-ext-test/instructions", "bad") == 0) { printf("FAIL unknown accepted\n"); fails++; }
    if (install_name("/tmp/myos-ext-test/instructions", "missing") == 0) { printf("FAIL missing accepted\n"); fails++; }
    return fails != 0;
}
#endif

int main(int argc, char **argv) {
#if defined(__LINUX_TEST__)
    if (argc == 2 && !strcmp(argv[1], "--selftest")) return selftest();
#endif
    if (argc == 2 && !strcmp(argv[1], "list")) return list_dir(INST_DIR);
    if (argc == 3 && !strcmp(argv[1], "install")) return install_name(INST_DIR, argv[2]);
    usage();
    return 2;
}
