/* rm [-rf] path... — удалить файлы и каталоги.
 * Короткие флаги склеиваются, как в GNU rm: rm -rf /tmp/ext.
 * -r/-R рекурсивно (ссылки на каталог не раскрываются), -f молчит,
 * если пути нет, и без операндов завершается успешно.
 * Пустой каталог удаляется и без -r. «.» , «..» и рекурсивный «/» не трогаем. */
#include "myos.h"
#include "sys/stat.h"

static int opt_recur, opt_force, opt_nopreserve;

static int join_path(char *dst, int cap, const char *dir, const char *name) {
    int nd = (int)strlen(dir), nn = (int)strlen(name), slash;
    slash = !(nd == 1 && dir[0] == '/') && nd > 0 && dir[nd - 1] != '/';
    if (nd + slash + nn + 1 > cap) return -1;
    memcpy(dst, dir, (size_t)nd);
    if (slash) dst[nd++] = '/';
    memcpy(dst + nd, name, (size_t)nn + 1);
    return 0;
}

static int dot_operand(const char *path) {
    const char *s, *end, *base;
    int n = (int)strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    end = path + n;
    base = path;
    for (s = path; s < end; s++) if (*s == '/') base = s + 1;
    n = (int)(end - base);
    if (n == 1 && base[0] == '.') return 1;
    return n == 2 && base[0] == '.' && base[1] == '.';
}

static int is_root(const char *path) {
    int n = (int)strlen(path);
    while (n > 1 && path[n - 1] == '/') n--;
    return n == 1 && path[0] == '/';
}

static int remove_path(const char *path);

static int remove_children(const char *path) {
    struct dirent_info e;
    char **names = NULL;
    int n = 0, cap = 0, status = 0, i, rc = 0;
    for (i = 0; (status = dirread(path, i, &e)) == 1; i++) {
        char *copy;
        int len = (int)strlen(e.name);
        if (!strcmp(e.name, ".") || !strcmp(e.name, "..")) continue;
        if (n >= cap) {
            int nc = cap ? cap * 2 : 16;
            char **nn = realloc(names, (size_t)nc * sizeof *nn);
            if (!nn) {
                dprintf(STDERR, "rm: %s: out of memory\n", path);
                rc = -1;
                break;
            }
            names = nn;
            cap = nc;
        }
        copy = malloc((size_t)len + 1);
        if (!copy) {
            dprintf(STDERR, "rm: %s: out of memory\n", path);
            rc = -1;
            break;
        }
        memcpy(copy, e.name, (size_t)len + 1);
        names[n++] = copy;
    }
    if (status < 0) {
        dprintf(STDERR, "rm: %s: %s\n", path, strerror(-status));
        rc = -1;
    }
    for (i = 0; i < n; i++) {
        char child[1024];
        if (join_path(child, (int)sizeof child, path, names[i]) < 0) {
            dprintf(STDERR, "rm: %s: file name too long\n", path);
            rc = -1;
        } else if (remove_path(child) < 0) rc = -1;
        free(names[i]);
    }
    free(names);
    return rc;
}

static int remove_path(const char *path) {
    struct stat st;
    int child = 0, r;
    if (lstat(path, &st) < 0) {
        if (opt_force && errno == ENOENT) return 0;
        dprintf(STDERR, "rm: %s: %s\n", path, strerror(errno));
        return -1;
    }
    /* Каталог-ссылку снимаем саму, не заходя внутрь. */
    if (S_ISDIR(st.st_mode) && opt_recur) child = remove_children(path);
    r = unlink(path);
    if (r < 0) {
        if (opt_force && r == -ENOENT) return child;
        dprintf(STDERR, "rm: %s: %s\n", path, strerror(-r));
        return -1;
    }
    return child;
}

static void usage(void) {
    dprintf(STDERR, "usage: rm [-rf] path...\n");
}

/* 0 — это уже операнд, 1 — флаг съеден, 2 — «--», -1 — ошибка. */
static int take_opt(const char *a) {
    const char *p;
    if (a[0] != '-' || a[1] == 0) return 0;
    if (a[1] == '-' && a[2] == 0) return 2;
    if (a[1] == '-') {
        if (!strcmp(a, "--recursive")) opt_recur = 1;
        else if (!strcmp(a, "--force")) opt_force = 1;
        else if (!strcmp(a, "--no-preserve-root")) opt_nopreserve = 1;
        else {
            dprintf(STDERR, "rm: unknown option '%s'\n", a);
            usage();
            return -1;
        }
        return 1;
    }
    for (p = a + 1; *p; p++) {
        if (*p == 'r' || *p == 'R') opt_recur = 1;
        else if (*p == 'f') opt_force = 1;
        else {
            dprintf(STDERR, "rm: unknown option '%c'\n", *p);
            usage();
            return -1;
        }
    }
    return 1;
}

int main(int argc, char **argv) {
    int i, rc = 0, optend = 1;
    for (i = 1; i < argc; i++) {
        int k = take_opt(argv[i]);
        if (k == 0) break;
        if (k < 0) return 1;
        optend = i + 1;
        if (k == 2) break;
    }
    if (optend >= argc) {
        if (opt_force) return 0;
        usage();
        return 1;
    }
    for (i = optend; i < argc; i++) {
        char path[1024];
        int len = (int)strlen(argv[i]);
        if (len >= (int)sizeof(path)) {
            dprintf(STDERR, "rm: %s: file name too long\n", argv[i]);
            rc = 1;
            continue;
        }
        memcpy(path, argv[i], (size_t)len + 1);
        /* A terminal slash makes lstat follow a directory symlink on POSIX.
         * Remove it before inspection so recursive removal never follows it. */
        while (len > 1 && path[len - 1] == '/') path[--len] = 0;
        if (dot_operand(path)) {
            dprintf(STDERR, "rm: refusing to remove '.' or '..' directory: skipping '%s'\n", argv[i]);
            rc = 1;
            continue;
        }
        if (opt_recur && is_root(path) && !opt_nopreserve) {
            dprintf(STDERR, "rm: it is dangerous to operate recursively on '/'\n");
            rc = 1;
            continue;
        }
        if (remove_path(path) < 0) rc = 1;
    }
    return rc;
}
