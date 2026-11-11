/* ar — архивы объектных файлов (статические библиотеки .a), формат GNU / System V.
 *
 *   ar rcs libfoo.a a.o b.o   добавить или заменить члены и записать индекс символов
 *   ar q libfoo.a c.o         дописать в конец, не ища одноимённых
 *   ar t libfoo.a             список членов (v — с размерами)
 *   ar x libfoo.a [a.o]       извлечь все или названные члены в текущую папку
 *   ar p libfoo.a a.o         вывести член в стандартный вывод
 *   ar d libfoo.a a.o         удалить члены
 *   ar s libfoo.a             только пересчитать индекс; ranlib — тот же файл под вторым именем
 *
 * Индекс (член "/") — какие глобальные символы определены в каком члене-ELF: по нему
 * cc решает, что взять из библиотеки. Имена длиннее 15 символов — в таблице "//".
 * Время, владелец и права пишутся нулями (как ar D): из одних входов — один архив. */
#include "myos.h"
#include "stdio.h"
#include "stdlib.h"

struct member {
    char *name;
    unsigned char *data;
    int size;
};

static struct member *mem;
static int n_mem, cap_mem;
static const char *me = "ar";
static int opt_c, opt_v, opt_noindex;

static void die(const char *fmt, const char *arg) {
    fprintf(stderr, "%s: ", me);
    fprintf(stderr, fmt, arg);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(int n) {
    void *p = malloc(n > 0 ? n : 1);
    if (!p) die("%s", "out of memory");
    return p;
}

static const char *base_of(const char *path) {
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

/* весь файл в память: 0 — нет такого файла */
static unsigned char *read_all(const char *path, int *len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    int cap = 4096, n = 0;
    unsigned char *buf = xmalloc(cap);
    for (;;) {
        if (n == cap) {
            unsigned char *nb = xmalloc(cap * 2);
            memcpy(nb, buf, n);
            free(buf);
            buf = nb;
            cap *= 2;
        }
        int r = read(fd, buf + n, cap - n);
        if (r < 0) { close(fd); die("cannot read %s", path); }
        if (r == 0) break;
        n += r;
    }
    close(fd);
    *len = n;
    return buf;
}

static void write_all(const char *path, const unsigned char *p, int n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) die("cannot write %s", path);
    while (n > 0) {
        int w = write(fd, p, n);
        if (w <= 0) { close(fd); die("write error: %s", path); }
        p += w;
        n -= w;
    }
    close(fd);
}

static int find_member(const char *name) {
    for (int i = 0; i < n_mem; i++) if (!strcmp(mem[i].name, name)) return i;
    return -1;
}

static void add_member(char *name, unsigned char *data, int size) {
    if (n_mem == cap_mem) {
        cap_mem = cap_mem ? cap_mem * 2 : 16;
        struct member *p = xmalloc(sizeof(struct member) * cap_mem);
        if (mem) memcpy(p, mem, sizeof(struct member) * n_mem);
        free(mem);
        mem = p;
    }
    mem[n_mem].name = name;
    mem[n_mem].data = data;
    mem[n_mem].size = size;
    n_mem++;
}

/* ------------------------------------------------------------ чтение архива */

static int field_num(const unsigned char *p, int n) {
    int v = 0;
    for (int i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10 + (p[i] - '0');
    return v;
}

/* 0 — архива нет; иначе члены в mem[] */
static int load_archive(const char *path) {
    int len;
    unsigned char *a = read_all(path, &len);
    if (!a) return 0;
    if (len < 8 || memcmp(a, "!<arch>\n", 8)) die("%s: not an archive", path);
    const char *longnames = NULL;
    int longnames_len = 0;
    int pos = 8;
    while (pos + 60 <= len) {
        unsigned char *h = a + pos;
        if (h[58] != '`' || h[59] != '\n') die("%s: broken member header", path);
        int size = field_num(h + 48, 10);
        unsigned char *data = h + 60;
        if (pos + 60 + size > len) die("%s: truncated member", path);
        pos += 60 + size + (size & 1);
        char *name;
        if (h[0] == '/' && (h[1] == ' ' || !memcmp(h, "/SYM64/", 7))) continue;     /* индекс */
        if (h[0] == '/' && h[1] == '/') { longnames = (const char *)data; longnames_len = size; continue; }
        if (h[0] == '/' && h[1] >= '0' && h[1] <= '9') {
            int off = field_num(h + 1, 15), e = off;
            if (!longnames || off >= longnames_len) die("%s: bad long member name", path);
            while (e < longnames_len && longnames[e] != '/' && longnames[e] != '\n') e++;
            name = xmalloc(e - off + 1);
            memcpy(name, longnames + off, e - off);
            name[e - off] = 0;
        } else if (!memcmp(h, "#1/", 3)) {                  /* BSD: имя в начале данных */
            int n = field_num(h + 3, 13);
            if (n > size) die("%s: bad BSD member name", path);
            name = xmalloc(n + 1);
            memcpy(name, data, n);
            name[n] = 0;
            data += n;
            size -= n;
        } else {
            int n = 0;
            while (n < 16 && h[n] != '/' && h[n] != ' ') n++;
            name = xmalloc(n + 1);
            memcpy(name, h, n);
            name[n] = 0;
        }
        add_member(name, data, size);
    }
    return 1;
}

/* ------------------------------------------------------------ индекс символов */

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}

struct isym { const char *name; int member; };
static struct isym *isyms;
static int n_isyms, cap_isyms;

/* глобальные определённые символы объектного файла ELF32 (не ELF — пропустить) */
static void scan_symbols(int m) {
    const unsigned char *d = mem[m].data;
    unsigned len = mem[m].size;
    if (len < 52 || memcmp(d, "\x7f" "ELF", 4) || d[4] != 1 || d[5] != 1 || rd16(d + 16) != 1) return;
    unsigned shoff = rd32(d + 32), shnum = rd16(d + 48);
    if (rd16(d + 46) != 40 || shoff + shnum * 40 > len) return;
    for (unsigned i = 0; i < shnum; i++) {
        const unsigned char *sh = d + shoff + i * 40;
        if (rd32(sh + 4) != 2) continue;                            /* SHT_SYMTAB */
        unsigned off = rd32(sh + 16), size = rd32(sh + 20), link = rd32(sh + 24);
        if (link >= shnum || off + size > len) return;
        const unsigned char *str = d + shoff + link * 40;
        unsigned stroff = rd32(str + 16), strsize = rd32(str + 20);
        if (stroff + strsize > len) return;
        for (unsigned j = 1; j < size / 16; j++) {
            const unsigned char *s = d + off + j * 16;
            unsigned bind = s[12] >> 4, shndx = rd16(s + 14), name = rd32(s);
            if ((bind != 1 && bind != 2 && bind != 10) || shndx == 0 || name >= strsize) continue;
            if (n_isyms == cap_isyms) {
                cap_isyms = cap_isyms ? cap_isyms * 2 : 256;
                struct isym *p = xmalloc(sizeof(struct isym) * cap_isyms);
                if (isyms) memcpy(p, isyms, sizeof(struct isym) * n_isyms);
                free(isyms);
                isyms = p;
            }
            isyms[n_isyms].name = (const char *)d + stroff + name;
            isyms[n_isyms].member = m;
            n_isyms++;
        }
        return;
    }
}

/* ------------------------------------------------------------ запись архива */

struct out { unsigned char *p; int n, cap; };

static void put(struct out *o, const void *src, int n) {
    if (o->n + n > o->cap) {
        int cap = o->cap ? o->cap * 2 : 65536;
        while (cap < o->n + n) cap *= 2;
        unsigned char *np = xmalloc(cap);
        if (o->p) memcpy(np, o->p, o->n);
        free(o->p);
        o->p = np;
        o->cap = cap;
    }
    memcpy(o->p + o->n, src, n);
    o->n += n;
}

static void put_be32(struct out *o, unsigned v) {
    unsigned char b[4];
    b[0] = v >> 24; b[1] = v >> 16; b[2] = v >> 8; b[3] = v;
    put(o, b, 4);
}

/* поле заголовка: текст, дополненный пробелами до width */
static void field(unsigned char *h, int width, const char *s) {
    int i = 0;
    for (; s[i] && i < width; i++) h[i] = s[i];
    for (; i < width; i++) h[i] = ' ';
}

/* zero — значение полей даты, uid и gid: "0" или "" (у таблицы длинных имён) */
static void header(struct out *o, const char *name, const char *zero, const char *mode, int size) {
    unsigned char h[60];
    char num[16];
    field(h, 16, name);
    field(h + 16, 12, zero);
    field(h + 28, 6, zero);
    field(h + 34, 6, zero);
    field(h + 40, 8, mode);
    snprintf(num, sizeof(num), "%d", size);
    field(h + 48, 10, num);
    h[58] = '`';
    h[59] = '\n';
    put(o, h, 60);
}

static void pad(struct out *o, int size) { if (size & 1) put(o, "\n", 1); }

static void save_archive(const char *path) {
    n_isyms = 0;
    if (!opt_noindex) for (int i = 0; i < n_mem; i++) scan_symbols(i);

    /* длинные имена: "имя/\n" подряд, в заголовке члена — "/смещение" */
    struct out ln = {0};
    int *ln_off = xmalloc(sizeof(int) * (n_mem + 1));
    for (int i = 0; i < n_mem; i++) {
        ln_off[i] = -1;
        if (strlen(mem[i].name) <= 15) continue;
        ln_off[i] = ln.n;
        put(&ln, mem[i].name, strlen(mem[i].name));
        put(&ln, "/\n", 2);
    }
    if (ln.n & 1) put(&ln, "\n", 1);       /* как у GNU ar: выравнивание входит в размер */

    int idx_size = 0;
    if (n_isyms) {
        idx_size = 4 + 4 * n_isyms;
        for (int i = 0; i < n_isyms; i++) idx_size += strlen(isyms[i].name) + 1;
    }
    int idx_pad = idx_size & 1;             /* как у GNU ar: нулевой байт внутри индекса */
    idx_size += idx_pad;
    /* смещения заголовков членов — для индекса */
    int *moff = xmalloc(sizeof(int) * (n_mem + 1));
    int pos = 8;
    if (n_isyms) pos += 60 + idx_size;
    if (ln.n) pos += 60 + ln.n + (ln.n & 1);
    for (int i = 0; i < n_mem; i++) {
        moff[i] = pos;
        pos += 60 + mem[i].size + (mem[i].size & 1);
    }

    struct out o = {0};
    put(&o, "!<arch>\n", 8);
    if (n_isyms) {
        header(&o, "/", "0", "0", idx_size);
        put_be32(&o, n_isyms);
        for (int i = 0; i < n_isyms; i++) put_be32(&o, moff[isyms[i].member]);
        for (int i = 0; i < n_isyms; i++) put(&o, isyms[i].name, strlen(isyms[i].name) + 1);
        if (idx_pad) put(&o, "", 1);
    }
    if (ln.n) {
        header(&o, "//", "", "", ln.n);
        put(&o, ln.p, ln.n);
        pad(&o, ln.n);
    }
    for (int i = 0; i < n_mem; i++) {
        char name[32];
        if (ln_off[i] >= 0) snprintf(name, sizeof(name), "/%d", ln_off[i]);
        else snprintf(name, sizeof(name), "%s/", mem[i].name);
        header(&o, name, "0", "644", mem[i].size);
        put(&o, mem[i].data, mem[i].size);
        pad(&o, mem[i].size);
    }
    write_all(path, o.p, o.n);
}

/* ------------------------------------------------------------ команды */

static void usage(void) {
    fprintf(stderr,
        "usage: ar [-]{r|q|t|x|p|d|s}[cvsS] archive [file...]\n"
        "  r  insert files, replacing members with the same name   q  append files\n"
        "  t  list members        x  extract members      p  print members to stdout\n"
        "  d  delete members      s  rebuild the symbol index (same as ranlib)\n"
        "  c  do not say that the archive is created   v  verbose   S  no symbol index\n"
        "       ranlib archive...  rebuild the symbol index\n");
    exit(2);
}

static int named(const char *name, char **files, int n) {
    if (!n) return 1;
    for (int i = 0; i < n; i++) if (!strcmp(base_of(files[i]), name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    me = __progname ? __progname : (argc > 0 ? base_of(argv[0]) : "ar");
    if (!strcmp(me, "ranlib")) {
        if (argc < 2) { fprintf(stderr, "usage: ranlib archive...\n"); return 2; }
        for (int i = 1; i < argc; i++) {
            n_mem = 0;
            if (!load_archive(argv[i])) die("%s: no such file", argv[i]);
            save_archive(argv[i]);
        }
        return 0;
    }
    if (argc < 3) usage();
    int op = 0;
    for (const char *k = argv[1][0] == '-' ? argv[1] + 1 : argv[1]; *k; k++) {
        switch (*k) {
        case 'r': case 'q': case 't': case 'x': case 'p': case 'd':
            if (op && op != 's') usage();
            op = *k;
            break;
        case 's': if (!op) op = 's'; break;
        case 'c': opt_c = 1; break;
        case 'v': opt_v = 1; break;
        case 'S': opt_noindex = 1; break;
        case 'u': case 'D': case 'U': break;
        default: usage();
        }
    }
    if (!op) usage();
    const char *archive = argv[2];
    char **files = argv + 3;
    int n_files = argc - 3, rc = 0;
    int exists = load_archive(archive);

    if (op == 'r' || op == 'q') {
        if (!exists && !opt_c) fprintf(stderr, "%s: creating %s\n", me, archive);
        for (int i = 0; i < n_files; i++) {
            int len;
            unsigned char *data = read_all(files[i], &len);
            if (!data) die("%s: no such file", files[i]);
            const char *b = base_of(files[i]);
            char *name = xmalloc(strlen(b) + 1);
            strcpy(name, b);
            int m = op == 'r' ? find_member(name) : -1;
            if (m >= 0) {
                mem[m].data = data;
                mem[m].size = len;
                if (opt_v) printf("r - %s\n", name);
            } else {
                add_member(name, data, len);
                if (opt_v) printf("a - %s\n", name);
            }
        }
        save_archive(archive);
        return 0;
    }
    if (!exists) die("%s: no such file", archive);
    if (op == 's') { save_archive(archive); return 0; }
    if (op == 'd') {
        for (int i = 0; i < n_files; i++) {
            int m = find_member(base_of(files[i]));
            if (m < 0) { fprintf(stderr, "%s: no entry %s in archive\n", me, files[i]); rc = 1; continue; }
            if (opt_v) printf("d - %s\n", mem[m].name);
            for (int j = m; j + 1 < n_mem; j++) mem[j] = mem[j + 1];
            n_mem--;
        }
        save_archive(archive);
        return rc;
    }
    for (int i = 0; i < n_files; i++)
        if (find_member(base_of(files[i])) < 0) {
            fprintf(stderr, "%s: no entry %s in archive\n", me, files[i]);
            rc = 1;
        }
    for (int i = 0; i < n_mem; i++) {
        if (!named(mem[i].name, files, n_files)) continue;
        if (op == 't') {
            if (opt_v) printf("rw-r--r-- 0/0 %8d %s\n", mem[i].size, mem[i].name);
            else printf("%s\n", mem[i].name);
        } else if (op == 'p') {
            fflush(stdout);
            write(STDOUT, mem[i].data, mem[i].size);
        } else {
            if (opt_v) printf("x - %s\n", mem[i].name);
            write_all(mem[i].name, mem[i].data, mem[i].size);
        }
    }
    return rc;
}
