#include "module.h"

/* Загрузчик гоняется как i386-программа хоста: указатели модуля должны
 * помещаться в u32, поэтому тест нельзя собирать под x86_64. */

int kmod_ping(int v) { return v; }
const struct kexport kexports[] = { { "kmod_ping", (void *)kmod_ping } };
const int kexport_n = 1;

/* Куча ядра на MyOS исполняемая. У хоста NX включён, поэтому тест
 * берёт страницы через mmap с правом исполнения. */
static unsigned char *heapmem;
static unsigned used, heapcap;
static void *sys_mmap(unsigned n) {
    struct { unsigned addr, len, prot, flags, fd, offset; } a;
    a.addr = 0; a.len = n; a.prot = 7; a.flags = 0x22; a.fd = (unsigned)-1; a.offset = 0;
    unsigned r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(90), "b"(&a) : "memory");
    if (r > 0xfffff000u) return 0;
    return (void *)r;
}
void *kmalloc(size_t n) {
    if (!heapmem) { heapmem = sys_mmap(256 * 1024); heapcap = heapmem ? 256 * 1024 : 0; }
    n = (n + 15u) & ~15u;
    if (!heapmem || used + n > heapcap) return 0;
    void *p = heapmem + used;
    used += n;
    return p;
}
void kfree(void *p) { (void)p; }
void klogf(const char *fmt, ...) { (void)fmt; }

static char msg[128];
static int outn, fail;
static void put(const char *s) { while (*s && outn < (int)sizeof(msg) - 1) msg[outn++] = *s++; }
static void expect(int c, const char *w) { if (!c) { put("FAIL "); put(w); put("\n"); fail++; } }

static int sys_open(const char *path) {
    int fd;
    __asm__ volatile ("int $0x80" : "=a"(fd) : "a"(5), "b"(path), "c"(0));
    return fd;
}
static int sys_read(int fd, void *buf, unsigned n) {
    int r;
    __asm__ volatile ("int $0x80" : "=a"(r) : "a"(3), "b"(fd), "c"(buf), "d"(n));
    return r;
}
static void sys_close(int fd) { __asm__ volatile ("int $0x80" :: "a"(6), "b"(fd)); }

struct ehdr {
    unsigned char ident[16];
    unsigned short type, machine;
    unsigned version, entry, phoff, shoff, flags;
    unsigned short ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
};
struct shdr {
    unsigned name, type, flags, addr, offset, size, link, info, addralign, entsize;
};

static void spoil_reloc(unsigned char *buf, unsigned len) {
    if (len < sizeof(struct ehdr)) return;
    struct ehdr *eh = (struct ehdr *)buf;
    if (eh->shoff + eh->shnum * sizeof(struct shdr) > len) return;
    struct shdr *sh = (struct shdr *)(buf + eh->shoff);
    for (unsigned i = 0; i < eh->shnum; i++) {
        if (sh[i].type != 9 || sh[i].size < 8) continue;
        unsigned *info = (unsigned *)(buf + sh[i].offset + 4);
        *info = (*info & ~0xffu) | 99u;
        return;
    }
}

static int load_file(const char *path, unsigned char *buf, unsigned cap) {
    int fd = sys_open(path);
    if (fd < 0) return -1;
    int n = 0;
    while (n < (int)cap) {
        int r = sys_read(fd, buf + n, cap - (unsigned)n);
        if (r < 0) { sys_close(fd); return -1; }
        if (r == 0) break;
        n += r;
    }
    sys_close(fd);
    return n;
}

static void run(void) {
    static unsigned char image[65536], bad[65536];
    int n = load_file("build/kmod_sample.ko", image, sizeof(image));
    expect(n > 64, "read ko");
    if (n > 64) {
        for (int i = 0; i < n; i++) bad[i] = image[i];
        spoil_reloc(bad, (unsigned)n);
        expect(module_load_image(bad, (unsigned)n, "bad") == -8, "reject reloc");
        int id = module_load_image(image, (unsigned)n, "sample");
        expect(id >= 0, "load");
        int *mark = module_symbol(id, "kmod_mark");
        expect(mark && *mark == 7, "reloc value");
        expect(module_ctl(2, "sample", 0, 0) == 0, "unload");
    }
    if (fail) put("kmod: FAIL\n");
    else put("kmod: PASS\n");
    __asm__ volatile ("int $0x80" :: "a"(4), "b"(1), "c"(msg), "d"(outn));
}

void _start(void) {
    run();
    __asm__ volatile ("int $0x80" :: "a"(1), "b"(fail ? 1 : 0));
    for (;;) {}
}
