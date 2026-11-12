/* cp [-rvf] source... dest — копировать файлы и, с -r, каталоги.
 * Несколько источников можно класть только в существующий каталог.
 * Файл сначала читается целиком, потом пишется: копирование на самого себя
 * не затирает данные. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "dirent.h"

#define COPY_MAX (9 * 1024 * 1024)

static int opt_recur, opt_force, opt_verbose;

static void die_path(const char *path, const char *msg) {
    fprintf(stderr, "cp: %s: %s\n", path, msg);
}

static const char *base_of(const char *path) {
    const char *s = path, *p;
    for (p = path; *p; p++) if (*p == '/') s = p + 1;
    return s[0] ? s : path;
}

static int join_path(char *dst, int cap, const char *dir, const char *name) {
    int nd = (int)strlen(dir), nn = (int)strlen(name), slash;
    slash = !(nd == 1 && dir[0] == '/') && nd > 0 && dir[nd - 1] != '/';
    if (nd + slash + nn + 1 > cap) return -1;
    memcpy(dst, dir, (size_t)nd);
    if (slash) dst[nd++] = '/';
    memcpy(dst + nd, name, (size_t)nn + 1);
    return 0;
}

/* path лежит внутри dir или совпадает с ним. */
static int path_inside(const char *dir, const char *path) {
    int n = (int)strlen(dir);
    if (n == 1 && dir[0] == '/') return path[0] == '/';
    if (strncmp(path, dir, (size_t)n) != 0) return 0;
    return path[n] == 0 || path[n] == '/';
}

static int read_file(const char *path, unsigned char **out, int *out_n) {
    struct stat st;
    int fd, off = 0;
    unsigned char *buf;
    if (stat(path, &st) < 0) { die_path(path, strerror(errno)); return -1; }
    if (!S_ISREG(st.st_mode)) { die_path(path, "not a regular file"); return -1; }
    if (st.st_size > COPY_MAX) { die_path(path, "file too large"); return -1; }
    fd = open(path, O_RDONLY);
    if (fd < 0) { die_path(path, strerror(errno)); return -1; }
    buf = malloc(st.st_size ? st.st_size : 1);
    if (!buf) { close(fd); die_path(path, "out of memory"); return -1; }
    while (off < (int)st.st_size) {
        int r = read(fd, buf + off, (size_t)((int)st.st_size - off));
        if (r < 0) { close(fd); free(buf); die_path(path, strerror(errno)); return -1; }
        if (r == 0) break;
        off += r;
    }
    close(fd);
    *out = buf;
    *out_n = off;
    return 0;
}

static int write_file(const char *path, const unsigned char *data, int n,unsigned mode) {
    int fd, off = 0;
    if (opt_force) remove(path);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC,mode&0777);
    if (fd < 0) { die_path(path, strerror(errno)); return -1; }
    while (off < n) {
        int r = write(fd, data + off, (size_t)(n - off));
        if (r <= 0) {
            int e = errno;
            close(fd);
            errno = e;
            die_path(path, strerror(errno));
            return -1;
        }
        off += r;
    }
    if (close(fd) < 0) { die_path(path, strerror(errno)); return -1; }
    return 0;
}

static int copy_path(const char *src, const char *dst);

static int copy_dir(const char *src, const char *dst,unsigned mode) {
    DIR *d;
    struct dirent *ent;
    char **names = NULL;
    int n = 0, cap = 0, i, rc = 0, mr;
    if (path_inside(src, dst)) { die_path(dst, "cannot copy a directory into itself"); return -1; }
    unsigned mask=umask(0);umask(mask);
    mr = mkdir(dst,(mode&0777)|0700);
    if(mr==-1)mr=-errno;
    if (mr < 0 && mr != -EEXIST) { die_path(dst, strerror(-mr)); return -1; }
    d = opendir(src);
    if (!d) { die_path(src, strerror(errno)); return -1; }
    while ((ent = readdir(d))) {
        char *copy;
        if (n >= cap) {
            int nc = cap ? cap * 2 : 16;
            char **nn = realloc(names, (size_t)nc * sizeof *nn);
            if (!nn) { closedir(d); free(names); die_path(src, "out of memory"); return -1; }
            names = nn;
            cap = nc;
        }
        copy = malloc(strlen(ent->d_name) + 1);
        if (!copy) {
            closedir(d);
            for (i = 0; i < n; i++) free(names[i]);
            free(names);
            die_path(src, "out of memory");
            return -1;
        }
        memcpy(copy, ent->d_name, strlen(ent->d_name) + 1);
        names[n++] = copy;
    }
    closedir(d);
    for (i = 0; i < n; i++) {
        char from[128], to[128];
        if (join_path(from, (int)sizeof from, src, names[i]) < 0 ||
            join_path(to, (int)sizeof to, dst, names[i]) < 0) {
            die_path(src, "file name too long");
            rc = -1;
        } else if (copy_path(from, to) < 0) rc = -1;
        free(names[i]);
    }
    free(names);
    if(mr==0 && chmod(dst,mode&0777&~mask)<0){die_path(dst,strerror(errno));rc=-1;}
    return rc;
}

static int copy_path(const char *src, const char *dst) {
    struct stat st;
    unsigned char *data = NULL;
    int n = 0;
    if (stat(src, &st) < 0) { die_path(src, strerror(errno)); return -1; }
    if (S_ISDIR(st.st_mode)) {
        if (!opt_recur) { die_path(src, "is a directory"); return -1; }
        if (opt_verbose) fprintf(stderr, "%s -> %s\n", src, dst);
        return copy_dir(src,dst,st.st_mode);
    }
    if (read_file(src, &data, &n) < 0) return -1;
    if (opt_verbose) fprintf(stderr, "%s -> %s\n", src, dst);
    if (write_file(dst,data,n,st.st_mode) < 0) { free(data); return -1; }
    free(data);
    return 0;
}

static int copy_to(const char *src, const char *dest, int dest_dir) {
    char target[128];
    if (dest_dir) {
        if (join_path(target, (int)sizeof target, dest, base_of(src)) < 0) {
            die_path(dest, "file name too long");
            return -1;
        }
    } else {
        if ((int)strlen(dest) >= (int)sizeof target) { die_path(dest, "file name too long"); return -1; }
        memcpy(target, dest, strlen(dest) + 1);
    }
    return copy_path(src, target);
}

static void usage(void) {
    fprintf(stderr, "usage: cp [-rvf] source dest\n       cp [-rvf] source... directory\n");
}

int main(int argc, char **argv) {
    int i, rc = 0, optend = 1, ns, dest_dir = 0;
    struct stat st;
    const char *dest;
    for (i = 1; i < argc; i++) {
        char *a = argv[i], *p;
        if (a[0] != '-' || a[1] == 0) break;
        if (a[1] == '-' && a[2] == 0) { optend = i + 1; break; }
        for (p = a + 1; *p; p++) {
            if (*p == 'r' || *p == 'R') opt_recur = 1;
            else if (*p == 'f') opt_force = 1;
            else if (*p == 'v') opt_verbose = 1;
            else { fprintf(stderr, "cp: unknown option '%c'\n", *p); usage(); return 1; }
        }
        optend = i + 1;
    }
    ns = argc - optend;
    if (ns < 2) { usage(); return 1; }
    dest = argv[argc - 1];
    if (stat(dest, &st) == 0 && S_ISDIR(st.st_mode)) dest_dir = 1;
    else if (ns > 2) { die_path(dest, "not a directory"); return 1; }
    for (i = optend; i < argc - 1; i++)
        if (copy_to(argv[i], dest, dest_dir) < 0) rc = 1;
    return rc;
}
