/* Копирует Lib/ в /ext/lib/python3.15, кроме test, tkinter и idlelib. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "dirent.h"
#include "sys/stat.h"
#include "fcntl.h"
#include "unistd.h"

static int skipped(const char *name) {
    return !strcmp(name, "test") || !strcmp(name, "tkinter") || !strcmp(name, "idlelib")
        || !strcmp(name, "__pycache__") || !strcmp(name, ".") || !strcmp(name, "..");
}

static int copy_file(const char *from, const char *to) {
    int in = open(from, O_RDONLY);
    int out, n;
    char buf[4096];
    if (in < 0) return -1;
    out = open(to, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    while ((n = (int)read(in, buf, sizeof buf)) > 0) {
        int off = 0;
        while (off < n) {
            int w = (int)write(out, buf + off, (size_t)(n - off));
            if (w < 0) { close(in); close(out); return -1; }
            off += w;
        }
    }
    close(in);
    close(out);
    return n < 0 ? -1 : 0;
}

static int copy_tree(const char *from, const char *to) {
    DIR *d;
    struct dirent *e;
    struct stat st;
    if (stat(from, &st) < 0) return -1;
    if (!S_ISDIR(st.st_mode)) return copy_file(from, to);
    mkdir(to, 0755);
    d = opendir(from);
    if (!d) return -1;
    while ((e = readdir(d))) {
        char src[512], dst[512];
        if (skipped(e->d_name)) continue;
        if (snprintf(src, sizeof src, "%s/%s", from, e->d_name) >= (int)sizeof src) continue;
        if (snprintf(dst, sizeof dst, "%s/%s", to, e->d_name) >= (int)sizeof dst) continue;
        if (copy_tree(src, dst) < 0) { closedir(d); return -1; }
    }
    closedir(d);
    return 0;
}

int main(void) {
    mkdir("/ext", 0755);
    mkdir("/ext/bin", 0755);
    mkdir("/ext/lib", 0755);
    if (copy_tree("Lib", "/ext/lib/python3.15") < 0) {
        fprintf(stderr, "pyinstall: cannot copy Lib\n");
        return 1;
    }
    return 0;
}
