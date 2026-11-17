#include "module.h"
#ifdef HOST_TEST
void klogf(const char *fmt, ...);
void *kmalloc(size_t n);
void kfree(void *p);
#include "string.h"
#define ENOENT 2
#define EIO 5
#define ENOEXEC 8
#define ENOMEM 12
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22
#else
#include "klog.h"
#include "string.h"
#include "heap.h"
#include "fs.h"
#include "pci.h"
#include "syscall.h"
#endif

/* Образ модуля лежит в куче ядра. На этом i386 без PAE страницы кучи исполняемые,
 * отдельный NX-бит не включается. */

#define MOD_MAX 8
#define ELF_MAGIC 0x464C457Fu
#define ET_REL 1
#define EM_386 3
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_NOBITS 8
#define SHT_REL 9
#define SHF_ALLOC 2
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define R_386_32 1
#define R_386_PC32 2

struct elf32_ehdr {
    u8 e_ident[16];
    u16 e_type, e_machine;
    u32 e_version, e_entry, e_phoff, e_shoff, e_flags;
    u16 e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
};
struct elf32_shdr {
    u32 sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    u32 sh_link, sh_info, sh_addralign, sh_entsize;
};
struct elf32_sym { u32 st_name, st_value, st_size; u8 st_info, st_other; u16 st_shndx; };
struct elf32_rel { u32 r_offset, r_info; };
#define ELF_R_SYM(i) ((i) >> 8)
#define ELF_R_TYPE(i) ((u8)(i))

struct loaded {
    char name[16];
    u8 *base;
    u32 size;
    void (*fini)(void);
    int refs, alive;
    u32 shnum;
    u8 **sec;
    const struct elf32_sym *sym;
    u32 nsym;
    const char *str;
};

static struct loaded mods[MOD_MAX];
static struct loaded *loading;
extern const struct kexport kexports[];
extern const int kexport_n;

static struct loaded *by_id(int id) {
    if (id < 0 || id >= MOD_MAX || !mods[id].alive) return NULL;
    return &mods[id];
}

void *module_current(void) { return loading; }
void module_hold(void *mod) { struct loaded *m = mod; if (m && m->alive) m->refs++; }
void module_release(void *mod) { struct loaded *m = mod; if (m && m->alive && m->refs) m->refs--; }

static void *lookup(const char *name) {
    for (int i = 0; i < kexport_n; i++)
        if (kexports[i].name && !strcmp(kexports[i].name, name)) return kexports[i].addr;
    return NULL;
}

static int range_ok(const u8 *image, u32 len, u32 off, u32 n) {
    (void)image;
    return n <= len && off <= len - n;
}

int module_load_image(const void *image, u32 len, const char *name) {
    if (!image || len < sizeof(struct elf32_ehdr)) return -EINVAL;
    const struct elf32_ehdr *eh = image;
    if (*(const u32 *)eh->e_ident != ELF_MAGIC || eh->e_ident[4] != 1) return -ENOEXEC;
    if (eh->e_type != ET_REL || eh->e_machine != EM_386) return -ENOEXEC;
    if (!eh->e_shnum || eh->e_shentsize != sizeof(struct elf32_shdr)) return -ENOEXEC;
    if (!range_ok(image, len, eh->e_shoff, eh->e_shnum * sizeof(struct elf32_shdr))) return -ENOEXEC;
    const struct elf32_shdr *sh = (const struct elf32_shdr *)((const u8 *)image + eh->e_shoff);
    int slot = -1;
    for (int i = 0; i < MOD_MAX; i++) if (!mods[i].alive) { slot = i; break; }
    if (slot < 0) return -ENOMEM;
    u32 total = 16;
    for (u16 i = 0; i < eh->e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        u32 al = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
        if (al > 16) return -ENOEXEC;
        total = (total + al - 1) & ~(al - 1);
        if (sh[i].sh_size > 1024 * 1024 || total + sh[i].sh_size < total) return -ENOMEM;
        total += sh[i].sh_size;
    }
    u8 *base = kmalloc(total + 16);
    u8 **sec = kmalloc(eh->e_shnum * sizeof(u8 *));
    if (!base || !sec) { kfree(base); kfree(sec); return -ENOMEM; }
    memset(base, 0, total + 16);
    memset(sec, 0, eh->e_shnum * sizeof(u8 *));
    u32 cursor = 16;
    for (u16 i = 0; i < eh->e_shnum; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        u32 al = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
        cursor = (cursor + al - 1) & ~(al - 1);
        sec[i] = base + cursor;
        if (sh[i].sh_type == SHT_PROGBITS) {
            if (!range_ok(image, len, sh[i].sh_offset, sh[i].sh_size)) { kfree(base); kfree(sec); return -ENOEXEC; }
            memcpy(sec[i], (const u8 *)image + sh[i].sh_offset, sh[i].sh_size);
        } else if (sh[i].sh_type == SHT_NOBITS) {
            memset(sec[i], 0, sh[i].sh_size);
        }
        cursor += sh[i].sh_size;
    }
    const struct elf32_sym *syms = NULL;
    const char *str = NULL;
    u32 nsym = 0, strsz = 0;
    for (u16 i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type != SHT_REL) continue;
        if (sh[i].sh_link >= eh->e_shnum || sh[sh[i].sh_link].sh_type != SHT_SYMTAB) { kfree(base); kfree(sec); return -ENOEXEC; }
        const struct elf32_shdr *sy = &sh[sh[i].sh_link];
        if (sy->sh_link >= eh->e_shnum || !range_ok(image, len, sy->sh_offset, sy->sh_size)) { kfree(base); kfree(sec); return -ENOEXEC; }
        if (!syms) {
            syms = (const struct elf32_sym *)((const u8 *)image + sy->sh_offset);
            nsym = sy->sh_entsize ? sy->sh_size / sy->sh_entsize : 0;
            const struct elf32_shdr *st = &sh[sy->sh_link];
            if (!range_ok(image, len, st->sh_offset, st->sh_size)) { kfree(base); kfree(sec); return -ENOEXEC; }
            str = (const char *)image + st->sh_offset;
            strsz = st->sh_size;
        }
        const struct elf32_shdr *st = &sh[sy->sh_link];
        if (!range_ok(image, len, sh[i].sh_offset, sh[i].sh_size) || sh[i].sh_info >= eh->e_shnum) { kfree(base); kfree(sec); return -ENOEXEC; }
        u8 *dest = sec[sh[i].sh_info];
        if (!dest) continue; /* .eh_frame и прочие секции без SHF_ALLOC */
        u32 nrel = sh[i].sh_entsize ? sh[i].sh_size / sh[i].sh_entsize : 0;
        const struct elf32_rel *rel = (const struct elf32_rel *)((const u8 *)image + sh[i].sh_offset);
        for (u32 r = 0; r < nrel; r++) {
            u32 symi = ELF_R_SYM(rel[r].r_info);
            u8 type = ELF_R_TYPE(rel[r].r_info);
            if (symi >= nsym || rel[r].r_offset + 4 > sh[sh[i].sh_info].sh_size) { kfree(base); kfree(sec); return -ENOEXEC; }
            const struct elf32_sym *syment = &syms[symi];
            u32 value = 0;
            if (syment->st_shndx == SHN_UNDEF) {
                if (syment->st_name >= st->sh_size) { kfree(base); kfree(sec); return -ENOEXEC; }
                void *ext = lookup(str + syment->st_name);
                if (!ext) {
                    klogf("KMOD: unresolved %s\n", str + syment->st_name);
                    kfree(base); kfree(sec); return -ENOENT;
                }
                value = (u32)(uptr)ext;
            } else if (syment->st_shndx == SHN_ABS) {
                value = syment->st_value;
            } else if (syment->st_shndx < eh->e_shnum && sec[syment->st_shndx]) {
                value = (u32)(uptr)sec[syment->st_shndx] + syment->st_value;
            } else { kfree(base); kfree(sec); return -ENOEXEC; }
            u8 *at = dest + rel[r].r_offset;
            u32 addend = *(u32 *)at;
            if (type == R_386_32) *(u32 *)at = addend + value;
            else if (type == R_386_PC32) *(u32 *)at = addend + value - (u32)(uptr)at;
            else { kfree(base); kfree(sec); return -ENOEXEC; }
        }
    }
    const struct myos_module *desc = NULL;
    for (u32 i = 0; syms && i < nsym; i++) {
        if (!syms[i].st_name || !str || strcmp(str + syms[i].st_name, "myos_module")) continue;
        if (syms[i].st_shndx >= eh->e_shnum || !sec[syms[i].st_shndx]) break;
        desc = (const struct myos_module *)(sec[syms[i].st_shndx] + syms[i].st_value);
        break;
    }
    if (!desc || desc->magic != MOD_MAGIC || !desc->init || !desc->name) { kfree(base); kfree(sec); return -ENOEXEC; }
    struct elf32_sym *owned_sym = NULL;
    char *owned_str = NULL;
    if (syms && nsym && str && strsz) {
        owned_sym = kmalloc(nsym * sizeof(*syms));
        owned_str = kmalloc(strsz + 1);
        if (!owned_sym || !owned_str) { kfree(owned_sym); kfree(owned_str); kfree(base); kfree(sec); return -ENOMEM; }
        memcpy(owned_sym, syms, nsym * sizeof(*syms));
        memcpy(owned_str, str, strsz);
        owned_str[strsz] = 0;
        syms = owned_sym;
        str = owned_str;
    }
    struct loaded *m = &mods[slot];
    memset(m, 0, sizeof(*m));
    for (int i = 0; desc->name[i] && i < 15; i++) m->name[i] = desc->name[i];
    if (name && name[0]) {
        m->name[0] = 0;
        for (int i = 0; name[i] && i < 15; i++) m->name[i] = name[i];
    }
    m->base = base; m->size = total; m->fini = desc->fini; m->alive = 1;
    m->sec = sec; m->shnum = eh->e_shnum; m->sym = syms; m->nsym = nsym; m->str = str;
    struct loaded *prev = loading;
    loading = m;
    int rc = desc->init();
    loading = prev;
    if (rc) {
        if (m->fini && !m->refs) m->fini();
        kfree(owned_sym); kfree(owned_str);
        kfree(sec); kfree(base);
        memset(m, 0, sizeof(*m));
        return rc;
    }
    klogf("KMOD: loaded %s\n", m->name);
    return slot;
}

void *module_symbol(int id, const char *name) {
    struct loaded *m = by_id(id);
    if (!m || !name || !m->sym || !m->str) return NULL;
    for (u32 i = 0; i < m->nsym; i++) {
        if (!m->sym[i].st_name || strcmp(m->str + m->sym[i].st_name, name)) continue;
        if (m->sym[i].st_shndx >= m->shnum || !m->sec[m->sym[i].st_shndx]) return NULL;
        return m->sec[m->sym[i].st_shndx] + m->sym[i].st_value;
    }
    return NULL;
}

static int unload_slot(int id) {
    struct loaded *m = by_id(id);
    if (!m) return -ENOENT;
    if (m->refs) return -EBUSY;
    if (m->fini) m->fini();
    kfree((void *)m->sym); kfree((void *)m->str);
    kfree(m->sec); kfree(m->base);
    klogf("KMOD: unloaded %s\n", m->name);
    memset(m, 0, sizeof(*m));
    return 0;
}

#ifndef HOST_TEST
static int load_path(const char *path) {
    void *buf = NULL; u32 n = 0;
    int r = firmware_read(path, &buf, &n);
    if (r < 0) return r;
    const char *base = path;
    for (const char *p = path; *p; p++) if (*p == '/') base = p + 1;
    char name[16];
    memset(name, 0, sizeof(name));
    for (int i = 0; base[i] && base[i] != '.' && i < 15; i++) name[i] = base[i];
    for (int i = 0; i < MOD_MAX; i++) if (mods[i].alive && !strcmp(mods[i].name, name)) { kfree(buf); return i; }
    r = module_load_image(buf, n, name);
    kfree(buf);
    return r;
}

int firmware_read(const char *path, void **out, u32 *len) {
    if (!path || !out || !len) return -EINVAL;
    if (!fs_ready()) return -ENODEV;
    u32 ino; struct fs_stat st;
    if (fs_resolve(FS_ROOT, path, &ino)) return -ENOENT;
    if (fs_stat(ino, &st) || st.type != FS_TYPE_FILE || !st.size || st.size > 1024 * 1024) return -EINVAL;
    u8 *buf = kmalloc(st.size + 1);
    if (!buf) return -ENOMEM;
    int n = fs_read(ino, 0, buf, st.size);
    if (n < 0 || (u32)n != st.size) { kfree(buf); return -EIO; }
    buf[st.size] = 0;
    *out = buf; *len = st.size;
    return 0;
}

static int hex_byte(const char *s, u32 *v) {
    u32 n = 0;
    for (int i = 0; i < 4; i++) {
        char c = s[i];
        u32 d;
        if (c >= '0' && c <= '9') d = (u32)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (u32)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (u32)(c - 'A' + 10);
        else return 0;
        n = (n << 4) | d;
    }
    *v = n;
    return 1;
}

void modules_autoload(void) {
    void *text = NULL; u32 len = 0;
    if (firmware_read("/boot/modules/aliases", &text, &len) == 0) {
        struct pci_dev devs[48];
        int nd = pci_list(devs, 48);
        char *p = text;
        while (*p) {
            char *line = p;
            while (*p && *p != '\n') p++;
            if (*p) *p++ = 0;
            if (line[0] == '#' || line[0] == 0) continue;
            u32 vendor = 0, device = 0;
            if (!hex_byte(line, &vendor) || line[4] != ' ' || !hex_byte(line + 5, &device) || line[9] != ' ') continue;
            const char *file = line + 10;
            int hit = 0;
            for (int i = 0; i < nd; i++) if (devs[i].vendor == vendor && devs[i].device == device) hit = 1;
            if (!hit) continue;
            char path[80];
            if (strlen(file) > 40) continue;
            strcpy(path, "/boot/modules/");
            strcpy(path + 14, file);
            int rc = load_path(path);
            if (rc < 0) klogf("KMOD: %s did not attach (%d)\n", file, rc);
        }
        kfree(text);
    }
    extern void wlan_sim_maybe(void);
    wlan_sim_maybe();
}
#endif

int module_ctl(int op, const char *path, int index, struct mod_info *out) {
    if (op == MOD_COUNT) {
        int n = 0;
        for (int i = 0; i < MOD_MAX; i++) if (mods[i].alive) n++;
        return n;
    }
    if (op == MOD_STAT) {
        int seen = 0;
        for (int i = 0; i < MOD_MAX; i++) if (mods[i].alive) {
            if (seen == index) {
                if (out) {
                    memset(out, 0, sizeof(*out));
                    memcpy(out->name, mods[i].name, 16);
                    out->index = (u32)i;
                    out->refs = (u32)mods[i].refs;
                }
                return 0;
            }
            seen++;
        }
        return -ENOENT;
    }
#ifndef HOST_TEST
    if (op == MOD_LOAD) return path && path[0] ? load_path(path) : -EINVAL;
    if (op == MOD_RESCAN) { modules_autoload(); return 0; }
#endif
    if (op == MOD_UNLOAD) {
        if (!path) return -EINVAL;
        for (int i = 0; i < MOD_MAX; i++) if (mods[i].alive && !strcmp(mods[i].name, path)) return unload_slot(i);
        return -ENOENT;
    }
    return -EINVAL;
}
