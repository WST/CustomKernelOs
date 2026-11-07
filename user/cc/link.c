/* Объектные файлы ELF (ET_REL) и компоновщик.
 *
 * Объектный файл в памяти (ObjFile, cc.h) один и тот же, откуда бы он ни взялся:
 * из генератора (gen.c), из своего .o или из .o, собранного gcc (-m32 -fno-pic).
 * obj_write пишет его как ELF ET_REL — такие файлы понимают и binutils (ld, objdump).
 * link_program собирает из нескольких файлов программу ET_EXEC:
 *   1. общие символы: одно определение на имя (слабое уступает обычному, COMMON — любому);
 *   2. отсев: остаются только секции, до которых можно дойти от точки входа;
 *   3. раскладка: код и константы с TEXT_BASE, данные и bss — с новой страницы;
 *   4. перемещения: R_386_32 = S + A, R_386_PC32 = S + A - P (A лежит в самих байтах). */
#include "cc.h"

#define SHT_PROGBITS      1
#define SHT_SYMTAB        2
#define SHT_STRTAB        3
#define SHT_RELA          4
#define SHT_NOBITS        8
#define SHT_REL           9
#define SHT_INIT_ARRAY    14
#define SHT_FINI_ARRAY    15
#define SHT_PREINIT_ARRAY 16
#define SHT_GROUP         17
#define SHF_WRITE         1
#define SHF_ALLOC         2
#define SHF_EXECINSTR     4
#define SHF_INFO_LINK     0x40
#define SHN_ABS           0xFFF1
#define SHN_COMMON        0xFFF2
#define GRP_COMDAT        1

static unsigned rd16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static unsigned rd32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((unsigned)p[3] << 24);
}
static void wr32(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void buf_u16(Buf *b, unsigned v) { buf_byte(b, v); buf_byte(b, v >> 8); }
static unsigned align_up(unsigned n, unsigned a) { return (n + a - 1) / a * a; }

void write_file(const char *path, Buf *b) {
#ifdef CC_HOST
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
#else
    int fd = open(path, O_WRITE);
#endif
    if (fd < 0) error("cannot write %s", path);
    int off = 0;
    while (off < b->len) {
        int n = write(fd, b->data + off, b->len - off);
        if (n <= 0) error("write error: %s", path);
        off += n;
    }
    close(fd);
}

/* ------------------------------------------------------------ ObjFile */

ObjFile *obj_new(const char *name) {
    ObjFile *f = xcalloc(sizeof(ObjFile));
    f->name = strndup2(name, strlen(name));
    return f;
}

int obj_add_section(ObjFile *f, char *name, int kind, int align, unsigned char *data, int size) {
    if (f->n_secs == f->cap_secs) {
        f->cap_secs = f->cap_secs ? f->cap_secs * 2 : 64;
        Section **p = xcalloc(sizeof(Section *) * f->cap_secs);
        if (f->secs) memcpy(p, f->secs, sizeof(Section *) * f->n_secs);
        free(f->secs);
        f->secs = p;
    }
    Section *s = xcalloc(sizeof(Section));
    s->name = name;
    s->kind = kind;
    s->align = align < 1 ? 1 : align;
    s->size = size;
    s->file = f;
    if (kind != SEC_BSS) {
        s->data = xcalloc(size);
        if (data && size) memcpy(s->data, data, size);
    }
    f->secs[f->n_secs] = s;
    return f->n_secs++;
}

int obj_add_sym(ObjFile *f, char *name, int bind, int type, int sec, unsigned value, unsigned size) {
    if (f->n_syms == f->cap_syms) {
        f->cap_syms = f->cap_syms ? f->cap_syms * 2 : 64;
        LinkSym *p = xcalloc(sizeof(LinkSym) * f->cap_syms);
        if (f->syms) memcpy(p, f->syms, sizeof(LinkSym) * f->n_syms);
        free(f->syms);
        f->syms = p;
    }
    LinkSym *s = &f->syms[f->n_syms];
    s->name = name;
    s->bind = bind;
    s->type = type;
    s->sec = sec;
    s->value = value;
    s->size = size;
    return f->n_syms++;
}

void obj_add_reloc(Section *s, int offset, int type, int sym) {
    if (s->n_rels == s->cap_rels) {
        s->cap_rels = s->cap_rels ? s->cap_rels * 2 : 16;
        LinkReloc *p = xcalloc(sizeof(LinkReloc) * s->cap_rels);
        if (s->rels) memcpy(p, s->rels, sizeof(LinkReloc) * s->n_rels);
        free(s->rels);
        s->rels = p;
    }
    s->rels[s->n_rels].offset = offset;
    s->rels[s->n_rels].type = type;
    s->rels[s->n_rels].sym = sym;
    s->n_rels++;
}

/* ------------------------------------------------------------ запись .o */

typedef struct Shdr { unsigned name, type, flags, addr, offset, size, link, info, align, entsize; } Shdr;

static int str_add(Buf *b, const char *s) {
    int off = b->len;
    buf_put(b, s, strlen(s) + 1);
    return off;
}

/* body начинается сразу за заголовком ELF (52 байта): выровнять смещение в файле */
static unsigned body_align(Buf *body, int align) {
    while ((52 + body->len) % align) buf_byte(body, 0);
    return 52 + body->len;
}

void obj_write(ObjFile *f, const char *path) {
    int n_rel = 0;
    for (int i = 0; i < f->n_secs; i++) if (f->secs[i]->n_rels) n_rel++;
    /* [0] пустая, [1..n] секции, потом .rel.*, .note.GNU-stack, .symtab, .strtab, .shstrtab */
    int note_idx = f->n_secs + n_rel + 1;
    int symtab_idx = note_idx + 1, strtab_idx = note_idx + 2, shstr_idx = note_idx + 3;
    int shnum = shstr_idx + 1;
    Shdr *sh = xcalloc(sizeof(Shdr) * shnum);
    Buf body = {0}, shstr = {0}, str = {0}, syms = {0};
    buf_byte(&shstr, 0);
    buf_byte(&str, 0);

    /* символы: сначала локальные (так требует ELF), потом остальные */
    int *map = xcalloc(sizeof(int) * (f->n_syms + 1));
    for (int i = 0; i < 16; i++) buf_byte(&syms, 0);
    int k = 1, first_global = 1;
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1) first_global = k;
        for (int i = 0; i < f->n_syms; i++) {
            LinkSym *s = &f->syms[i];
            if ((s->bind == BIND_LOCAL) != (pass == 0)) continue;
            map[i] = k++;
            buf_u32(&syms, s->name && *s->name ? str_add(&str, s->name) : 0);
            buf_u32(&syms, s->value);
            buf_u32(&syms, s->size);
            buf_byte(&syms, (s->bind << 4) | s->type);
            buf_byte(&syms, 0);
            if (s->sec >= 0) buf_u16(&syms, s->sec + 1);
            else if (s->sec == SYM_COMMON) buf_u16(&syms, SHN_COMMON);
            else if (s->sec == SYM_ABS) buf_u16(&syms, SHN_ABS);
            else buf_u16(&syms, 0);
        }
    }

    for (int i = 0; i < f->n_secs; i++) {
        Section *s = f->secs[i];
        Shdr *h = &sh[i + 1];
        h->name = str_add(&shstr, s->name);
        h->type = s->kind == SEC_BSS ? SHT_NOBITS : SHT_PROGBITS;
        h->flags = SHF_ALLOC | (s->kind == SEC_TEXT ? SHF_EXECINSTR : 0) |
                   (s->kind == SEC_DATA || s->kind == SEC_BSS ? SHF_WRITE : 0);
        h->offset = body_align(&body, s->align);
        h->size = s->size;
        h->align = s->align;
        if (s->kind != SEC_BSS) buf_put(&body, s->data, s->size);
    }
    int r = f->n_secs + 1;
    for (int i = 0; i < f->n_secs; i++) {
        Section *s = f->secs[i];
        if (!s->n_rels) continue;
        Shdr *h = &sh[r++];
        h->name = str_add(&shstr, format(".rel%s", s->name));
        h->type = SHT_REL;
        h->flags = SHF_INFO_LINK;
        h->offset = body_align(&body, 4);
        h->size = s->n_rels * 8;
        h->link = symtab_idx;
        h->info = i + 1;
        h->align = 4;
        h->entsize = 8;
        for (int j = 0; j < s->n_rels; j++) {
            buf_u32(&body, s->rels[j].offset);
            buf_u32(&body, (map[s->rels[j].sym] << 8) | s->rels[j].type);
        }
    }
    /* без этой пустой секции ld считает, что стеку нужно право на выполнение */
    sh[note_idx].name = str_add(&shstr, ".note.GNU-stack");
    sh[note_idx].type = SHT_PROGBITS;
    sh[note_idx].offset = body_align(&body, 1);
    sh[note_idx].align = 1;

    sh[symtab_idx].name = str_add(&shstr, ".symtab");
    sh[symtab_idx].type = SHT_SYMTAB;
    sh[symtab_idx].offset = body_align(&body, 4);
    sh[symtab_idx].size = syms.len;
    sh[symtab_idx].link = strtab_idx;
    sh[symtab_idx].info = first_global;
    sh[symtab_idx].align = 4;
    sh[symtab_idx].entsize = 16;
    buf_put(&body, syms.data, syms.len);

    sh[strtab_idx].name = str_add(&shstr, ".strtab");
    sh[strtab_idx].type = SHT_STRTAB;
    sh[strtab_idx].offset = body_align(&body, 1);
    sh[strtab_idx].size = str.len;
    sh[strtab_idx].align = 1;
    buf_put(&body, str.data, str.len);

    sh[shstr_idx].name = str_add(&shstr, ".shstrtab");
    sh[shstr_idx].type = SHT_STRTAB;
    sh[shstr_idx].offset = body_align(&body, 1);
    sh[shstr_idx].size = shstr.len;
    sh[shstr_idx].align = 1;
    buf_put(&body, shstr.data, shstr.len);

    unsigned shoff = body_align(&body, 4);
    Buf out = {0};
    buf_put(&out, "\x7f" "ELF\x01\x01\x01\0\0\0\0\0\0\0\0\0", 16);
    buf_u16(&out, 1);                                           /* e_type = REL */
    buf_u16(&out, 3);                                           /* e_machine = 386 */
    buf_u32(&out, 1);                                           /* e_version */
    buf_u32(&out, 0);                                           /* e_entry */
    buf_u32(&out, 0);                                           /* e_phoff */
    buf_u32(&out, shoff);
    buf_u32(&out, 0);                                           /* e_flags */
    buf_u16(&out, 52);                                          /* e_ehsize */
    buf_u16(&out, 0); buf_u16(&out, 0);                         /* e_phentsize, e_phnum */
    buf_u16(&out, 40);                                          /* e_shentsize */
    buf_u16(&out, shnum);
    buf_u16(&out, shstr_idx);
    buf_put(&out, body.data, body.len);
    for (int i = 0; i < shnum; i++) {
        Shdr *h = &sh[i];
        buf_u32(&out, h->name); buf_u32(&out, h->type); buf_u32(&out, h->flags); buf_u32(&out, h->addr);
        buf_u32(&out, h->offset); buf_u32(&out, h->size); buf_u32(&out, h->link); buf_u32(&out, h->info);
        buf_u32(&out, h->align); buf_u32(&out, h->entsize);
    }
    write_file(path, &out);
}

/* ------------------------------------------------------------ чтение .o */

/* поля Elf32_Shdr по порядку (по 4 байта) */
enum { F_NAME, F_TYPE, F_FLAGS, F_ADDR, F_OFFSET, F_SIZE, F_LINK, F_INFO, F_ALIGN, F_ENTSIZE };

/* группы COMDAT (gcc кладёт в них, например, __x86.get_pc_thunk.*): из одинаковых берём первую */
static char **comdat_seen;
static int n_comdat, cap_comdat;

static bool comdat_add(char *sig) {
    for (int i = 0; i < n_comdat; i++) if (!strcmp(comdat_seen[i], sig)) return false;
    if (n_comdat == cap_comdat) {
        cap_comdat = cap_comdat ? cap_comdat * 2 : 16;
        char **p = xcalloc(sizeof(char *) * cap_comdat);
        if (comdat_seen) memcpy(p, comdat_seen, sizeof(char *) * n_comdat);
        comdat_seen = p;
    }
    comdat_seen[n_comdat++] = sig;
    return true;
}

ObjFile *obj_read(const char *path) {
    int len;
    unsigned char *d = (unsigned char *)read_file(path, &len);
    if (!d) return NULL;
    if (len < 52 || memcmp(d, "\x7f" "ELF", 4) || d[4] != 1 || d[5] != 1)
        error("%s: not an ELF32 little-endian file", path);
    if (rd16(d + 16) != 1) error("%s: not a relocatable object file (ET_REL)", path);
    if (rd16(d + 18) != 3) error("%s: not an i386 object file", path);
    unsigned shoff = rd32(d + 32);
    int shnum = rd16(d + 48), shstrndx = rd16(d + 50);
    if (rd16(d + 46) != 40 || shnum == 0 || shoff + (unsigned)shnum * 40 > (unsigned)len || shstrndx >= shnum)
        error("%s: broken section header table", path);
    #define SH(i, field) rd32(d + shoff + (i) * 40 + (field) * 4)
    for (int i = 1; i < shnum; i++)
        if (SH(i, F_TYPE) != SHT_NOBITS && SH(i, F_OFFSET) + SH(i, F_SIZE) > (unsigned)len)
            error("%s: section %d is out of the file", path, i);
    char *shstr = (char *)d + SH(shstrndx, F_OFFSET);

    int symtab = 0;
    for (int i = 1; i < shnum; i++) if (SH(i, F_TYPE) == SHT_SYMTAB) symtab = i;
    unsigned char *symd = symtab ? d + SH(symtab, F_OFFSET) : NULL;
    int n_syms = symtab ? SH(symtab, F_SIZE) / 16 : 0;
    char *strtab = symtab ? (char *)d + SH(SH(symtab, F_LINK), F_OFFSET) : NULL;

    int *secmap = xcalloc(sizeof(int) * shnum);
    bool *dropped = xcalloc(sizeof(bool) * shnum);
    for (int i = 0; i < shnum; i++) secmap[i] = -1;

    for (int i = 1; i < shnum; i++) {
        if (SH(i, F_TYPE) != SHT_GROUP || !symd) continue;
        unsigned char *g = d + SH(i, F_OFFSET);
        int n = SH(i, F_SIZE) / 4;
        if (n < 1 || !(rd32(g) & GRP_COMDAT)) continue;
        char *sig = strtab + rd32(symd + SH(i, F_INFO) * 16);
        if (comdat_add(sig)) continue;
        for (int j = 1; j < n; j++) {
            unsigned m = rd32(g + j * 4);
            if (m < (unsigned)shnum) dropped[m] = true;
        }
    }

    ObjFile *f = obj_new(path);
    for (int i = 1; i < shnum; i++) {
        unsigned type = SH(i, F_TYPE), flags = SH(i, F_FLAGS), size = SH(i, F_SIZE);
        char *name = shstr + SH(i, F_NAME);
        if (!(flags & SHF_ALLOC) || dropped[i]) continue;
        if (type == SHT_INIT_ARRAY || type == SHT_FINI_ARRAY || type == SHT_PREINIT_ARRAY ||
            !strcmp(name, ".ctors") || !strcmp(name, ".dtors")) {
            if (size) error("%s: %s: constructors and destructors are not supported", path, name);
            continue;
        }
        if (type != SHT_PROGBITS && type != SHT_NOBITS) continue;     /* .note.gnu.property и т.п. */
        int kind = (flags & SHF_EXECINSTR) ? SEC_TEXT : type == SHT_NOBITS ? SEC_BSS :
                   (flags & SHF_WRITE) ? SEC_DATA : SEC_RODATA;
        secmap[i] = obj_add_section(f, strndup2(name, strlen(name)), kind, SH(i, F_ALIGN),
                                    type == SHT_NOBITS ? NULL : d + SH(i, F_OFFSET), size);
    }

    /* символы — с теми же номерами, что в файле: на них ссылаются перемещения */
    for (int j = 0; j < n_syms; j++) {
        unsigned char *s = symd + j * 16;
        unsigned info = s[12], shndx = rd16(s + 14);
        int bind = (info >> 4) == 0 ? BIND_LOCAL : (info >> 4) == 2 ? BIND_WEAK : BIND_GLOBAL;
        int type = (info & 15) <= 3 ? (int)(info & 15) : STY_NOTYPE;
        char *name = strtab + rd32(s);
        int sec;
        if (shndx == 0) sec = SYM_UNDEF;
        else if (shndx == SHN_ABS) sec = SYM_ABS;
        else if (shndx == SHN_COMMON) sec = SYM_COMMON;
        else if (shndx < (unsigned)shnum) sec = secmap[shndx] >= 0 ? secmap[shndx] : SYM_UNDEF;
        else error("%s: symbol %s: unsupported section index %u", path, name, shndx);
        if (type == STY_SECTION && shndx < (unsigned)shnum) name = shstr + SH(shndx, F_NAME);
        obj_add_sym(f, strndup2(name, strlen(name)), j == 0 ? BIND_LOCAL : bind, type, sec, rd32(s + 4), rd32(s + 8));
    }

    for (int i = 1; i < shnum; i++) {
        unsigned type = SH(i, F_TYPE), target = SH(i, F_INFO);
        if (type != SHT_REL && type != SHT_RELA) continue;
        if (target >= (unsigned)shnum || secmap[target] < 0) continue;
        if (type == SHT_RELA) error("%s: RELA relocations are not supported on i386", path);
        Section *sec = f->secs[secmap[target]];
        unsigned char *r = d + SH(i, F_OFFSET);
        int n = SH(i, F_SIZE) / 8;
        for (int j = 0; j < n; j++) {
            unsigned off = rd32(r + j * 8), rinfo = rd32(r + j * 8 + 4);
            int sym = rinfo >> 8;
            if (sym >= n_syms) error("%s: relocation refers to symbol %d out of range", path, sym);
            if (sec->kind == SEC_BSS || off + 4 > (unsigned)sec->size)
                error("%s: relocation outside of section %s", path, sec->name);
            obj_add_reloc(sec, off, rinfo & 255, sym);
        }
    }
    #undef SH
    free(secmap);
    free(dropped);
    free(d);
    return f;
}

/* ------------------------------------------------------------ компоновщик */

struct GSym {
    GSym *next;
    char *name;
    int   state;                /* 0 — не определён, 1 — COMMON, 2 — слабое, 3 — обычное определение */
    ObjFile *file;              /* где определение */
    int   idx;
    unsigned common_size, common_align;
    Section *common;            /* выделенное место для COMMON */
};

#define GBUCKETS 4096
static GSym *gtab[GBUCKETS];

static GSym *gsym_get(char *name, bool create) {
    unsigned h = 2166136261u;
    for (char *p = name; *p; p++) h = (h ^ (unsigned char)*p) * 16777619u;
    h %= GBUCKETS;
    for (GSym *g = gtab[h]; g; g = g->next)
        if (!strcmp(g->name, name)) return g;
    if (!create) return NULL;
    GSym *g = xcalloc(sizeof(GSym));
    g->name = name;
    g->next = gtab[h];
    gtab[h] = g;
    return g;
}

static const char *file_name(ObjFile *f) { return *f->name ? f->name : "C sources"; }

static void add_globals(ObjFile *f) {
    for (int i = 0; i < f->n_syms; i++) {
        LinkSym *s = &f->syms[i];
        if (s->bind == BIND_LOCAL) continue;
        GSym *g = gsym_get(s->name, true);
        s->global = g;
        if (s->sec == SYM_UNDEF) continue;
        if (s->sec == SYM_COMMON) {
            if (g->state == 0) { g->state = 1; g->file = f; g->idx = i; }
            if (g->state == 1) {
                if (s->size > g->common_size) g->common_size = s->size;
                if (s->value > g->common_align) g->common_align = s->value;
            }
            continue;
        }
        int st = s->bind == BIND_WEAK ? 2 : 3;
        if (g->state == 3 && st == 3)
            error("multiple definition of '%s' (%s and %s)", s->name, file_name(g->file), file_name(f));
        if (g->state >= st) continue;
        g->state = st;
        g->file = f;
        g->idx = i;
    }
}

/* куда указывает символ idx файла f: секция (или NULL — абсолютный) и смещение. false — некуда */
static bool sym_target(ObjFile *f, int idx, Section **sec, unsigned *value) {
    LinkSym *s = &f->syms[idx];
    *sec = NULL;
    *value = 0;
    if (s->bind != BIND_LOCAL) {
        GSym *g = s->global;
        if (g->state == 0) return s->bind == BIND_WEAK;         /* слабая ссылка без определения = 0 */
        if (g->state == 1) { *sec = g->common; return true; }
        f = g->file;
        s = &f->syms[g->idx];
    }
    if (s->sec >= 0) { *sec = f->secs[s->sec]; *value = s->value; return true; }
    if (s->sec == SYM_ABS) { *value = s->value; return true; }
    return false;
}

/* где ссылка: имя функции для своих секций (.text.имя), иначе имя секции; и файл, если это .o */
static const char *where(Section *s) {
    const char *n = strncmp(s->name, ".text.", 6) ? s->name : s->name + 6;
    if (!*s->file->name) return n;
    return format("%s (%s)", n, s->file->name);
}

static Section **work;
static int n_work, cap_work;

static void mark(Section *s) {
    if (!s || s->live) return;
    s->live = true;
    if (n_work == cap_work) {
        cap_work = cap_work ? cap_work * 2 : 256;
        Section **p = xcalloc(sizeof(Section *) * cap_work);
        if (work) memcpy(p, work, sizeof(Section *) * n_work);
        free(work);
        work = p;
    }
    work[n_work++] = s;
}

static void mark_from_relocs(void) {
    while (n_work) {
        Section *s = work[--n_work];
        for (int i = 0; i < s->n_rels; i++) {
            LinkReloc *r = &s->rels[i];
            if (r->type == R_386_NONE) continue;
            if (r->type != R_386_32 && r->type != R_386_PC32 && r->type != R_386_PLT32)
                error("unsupported relocation type %d in %s (compile with -fno-pic)", r->type, where(s));
            Section *t;
            unsigned v;
            if (!sym_target(s->file, r->sym, &t, &v))
                error("undefined reference to '%s' in %s", s->file->syms[r->sym].name, where(s));
            mark(t);
        }
    }
}

/* файловое смещение секции в готовой программе */
static unsigned file_off(Section *s, unsigned data_vaddr, unsigned data_off) {
    if (s->kind == SEC_TEXT || s->kind == SEC_RODATA) return s->addr - TEXT_BASE;
    return data_off + (s->addr - data_vaddr);
}

void link_program(ObjFile **files, int n_files, const char *out_path) {
    for (int i = 0; i < n_files; i++) add_globals(files[i]);

    /* COMMON: место в bss — в порядке первого появления, чтобы раскладка не зависела от хеша */
    ObjFile *common = obj_new("COMMON");
    for (int i = 0; i < n_files; i++)
        for (int j = 0; j < files[i]->n_syms; j++) {
            GSym *g = files[i]->syms[j].global;
            if (!g || g->state != 1 || g->common || g->file != files[i] || g->idx != j) continue;
            int s = obj_add_section(common, g->name, SEC_BSS, g->common_align, NULL, g->common_size);
            g->common = common->secs[s];
        }

    /* точка входа: заглушка зовёт __libc_start(argc, argv) */
    GSym *start = gsym_get("__libc_start", false);
    Section *start_sec;
    unsigned start_val;
    if (!start || !start->state) error("undefined reference to '__libc_start'");
    sym_target(start->file, start->idx, &start_sec, &start_val);
    mark(start_sec);
    mark_from_relocs();

    /* место под заголовки ELF: 52 + 2*32 = 116 байт, код с 0x80 */
    static const unsigned char stub_myos[] = { 0xFF, 0x74, 0x24, 0x08, 0xFF, 0x74, 0x24, 0x08 };  /* ядро: _start(argc, argv) */
    static const unsigned char stub_linux[] = { 0x8B, 0x04, 0x24, 0x8D, 0x4C, 0x24, 0x04, 0x51, 0x50 }; /* argc = [esp], argv = esp+4 */
    const unsigned char *stub = opt_linux ? stub_linux : stub_myos;
    int stub_len = opt_linux ? sizeof(stub_linux) : sizeof(stub_myos);
    unsigned entry = 0x80;
    unsigned pos = entry + stub_len + 5 + 2;                     /* + call __libc_start; jmp $ */

    ObjFile **all = xcalloc(sizeof(ObjFile *) * (n_files + 1));
    for (int i = 0; i < n_files; i++) all[i] = files[i];
    all[n_files] = common;
    int n_all = n_files + 1;

    static const int order[] = { SEC_TEXT, SEC_RODATA, SEC_DATA, SEC_BSS };
    unsigned data_vaddr = 0, data_len = 0, text_len = 0;
    for (int k = 0; k < 4; k++) {
        if (order[k] == SEC_DATA) {
            text_len = pos;
            data_vaddr = align_up(TEXT_BASE + pos, 4096);
            pos = 0;
        }
        if (order[k] == SEC_BSS) {
            if (!pos) pos = 4;                                  /* сегмент данных не бывает пустым */
            data_len = pos;
        }
        for (int i = 0; i < n_all; i++)
            for (int j = 0; j < all[i]->n_secs; j++) {
                Section *s = all[i]->secs[j];
                if (!s->live || s->kind != order[k]) continue;
                pos = align_up(pos, s->align);
                s->addr = (k < 2 ? TEXT_BASE : data_vaddr) + pos;
                pos += s->size;
            }
    }
    unsigned bss_end = pos;
    unsigned data_off = align_up(text_len, 4096);

    Buf out = {0};
    out.cap = data_off + data_len;
    out.len = out.cap;
    out.data = xcalloc(out.cap);
    unsigned char *o = out.data;
    memcpy(o + entry, stub, stub_len);
    o[entry + stub_len] = 0xE8;
    unsigned call_end = TEXT_BASE + entry + stub_len + 5;
    wr32(o + entry + stub_len + 1, (start_sec ? start_sec->addr : 0) + start_val - call_end);
    o[entry + stub_len + 5] = 0xEB;                             /* jmp $: __libc_start не возвращается */
    o[entry + stub_len + 6] = 0xFE;

    for (int i = 0; i < n_all; i++)
        for (int j = 0; j < all[i]->n_secs; j++) {
            Section *s = all[i]->secs[j];
            if (!s->live || s->kind == SEC_BSS) continue;
            unsigned char *p = o + file_off(s, data_vaddr, data_off);
            memcpy(p, s->data, s->size);
            for (int r = 0; r < s->n_rels; r++) {
                LinkReloc *rl = &s->rels[r];
                if (rl->type == R_386_NONE) continue;
                Section *t;
                unsigned v;
                sym_target(s->file, rl->sym, &t, &v);
                unsigned S = (t ? t->addr : 0) + v;
                unsigned A = rd32(p + rl->offset);
                unsigned P = s->addr + rl->offset;
                wr32(p + rl->offset, rl->type == R_386_32 ? S + A : S + A - P);
            }
        }

    /* заголовки ELF */
    Buf h = {0};
    buf_put(&h, "\x7f" "ELF\x01\x01\x01\0\0\0\0\0\0\0\0\0", 16);
    buf_u16(&h, 2);                                             /* e_type = EXEC */
    buf_u16(&h, 3);                                             /* e_machine = 386 */
    buf_u32(&h, 1);                                             /* e_version */
    buf_u32(&h, TEXT_BASE + entry);
    buf_u32(&h, 52);                                            /* e_phoff */
    buf_u32(&h, 0);                                             /* e_shoff */
    buf_u32(&h, 0);                                             /* e_flags */
    buf_u16(&h, 52);                                            /* e_ehsize */
    buf_u16(&h, 32);                                            /* e_phentsize */
    buf_u16(&h, 2);                                             /* e_phnum */
    buf_u32(&h, 0);                                             /* shentsize, shnum */
    buf_u16(&h, 0);                                             /* shstrndx */
    /* сегмент кода: R-X */
    buf_u32(&h, 1); buf_u32(&h, 0); buf_u32(&h, TEXT_BASE); buf_u32(&h, TEXT_BASE);
    buf_u32(&h, text_len); buf_u32(&h, text_len); buf_u32(&h, 5); buf_u32(&h, 0x1000);
    /* сегмент данных: RW- */
    buf_u32(&h, 1); buf_u32(&h, data_off); buf_u32(&h, data_vaddr); buf_u32(&h, data_vaddr);
    buf_u32(&h, data_len); buf_u32(&h, bss_end); buf_u32(&h, 6); buf_u32(&h, 0x1000);
    memcpy(o, h.data, h.len);
    write_file(out_path, &out);
}
