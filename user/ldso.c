/* Динамический загрузчик /lib/ld.so.
 * Ядро отображает его как ET_DYN от INTERP_BASE и передаёт стек
 * argc, argv, envp, auxv. Сначала загрузчик перемещает сам себя
 * (все функции до этого — static, адреса считаются от счётчика команд),
 * затем читает библиотеки DT_NEEDED, сразу связывает JMP_SLOT, GLOB_DAT,
 * R_386_32 и R_386_COPY и вызывает массивы инициализации.
 * Ленивого связывания нет: и компоновщик, и загрузчик работают с BIND_NOW. */
#define SYS_EXIT 0
#define SYS_WRITE 1
#define SYS_READ 2
#define SYS_OPEN 3
#define SYS_CLOSE 4
#define SYS_STAT 26
#define SYS_MMAP 69
#define SYS_MUNMAP 70

#define PROT_RWX 7
#define MAP_PRIVATE 2
#define MAP_ANON 0x20

#define ET_EXEC 2
#define ET_DYN 3
#define PT_LOAD 1
#define PT_DYNAMIC 2
#define DT_NULL 0
#define DT_NEEDED 1
#define DT_PLTRELSZ 2
#define DT_HASH 4
#define DT_STRTAB 5
#define DT_SYMTAB 6
#define DT_STRSZ 10
#define DT_SYMENT 11
#define DT_INIT 12
#define DT_FINI 13
#define DT_SONAME 14
#define DT_REL 17
#define DT_RELSZ 18
#define DT_JMPREL 23
#define DT_INIT_ARRAY 25
#define DT_FINI_ARRAY 26
#define DT_INIT_ARRAYSZ 27
#define DT_FINI_ARRAYSZ 28

#define R_386_32 1
#define R_386_COPY 5
#define R_386_GLOB_DAT 6
#define R_386_JMP_SLOT 7
#define R_386_RELATIVE 8

#define AT_PHDR 3
#define AT_PHNUM 5
#define AT_BASE 7
#define AT_ENTRY 9
#define AT_NULL 0

#define MAX_OBJ 16
#define MAX_NEED 8

static int bad(int r) { return (unsigned)r >= 0xfffff001u; }

static void mem_copy(void *dst, const void *src, unsigned n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static const char *base_name(const char *p) {
    const char *s = p;
    while (*p) {
        if (*p == '/') s = p + 1;
        p++;
    }
    return s;
}

static unsigned rd32(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

static unsigned rd16(const unsigned char *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

static void wr(int fd, const char *s) {
    int n = 0;
    while (s[n]) n++;
    __syscall(SYS_WRITE, fd, (int)s, n);
}

static void die(const char *s) {
    wr(2, "ld.so: ");
    wr(2, s);
    wr(2, "\n");
    __syscall(SYS_EXIT, 127);
    for (;;) ;
}

static unsigned char *heap;
static unsigned heap_used, heap_cap;

static void *alloc(unsigned n) {
    unsigned char *p;
    n = (n + 3u) & ~3u;
    if (heap_used + n > heap_cap) {
        unsigned cap = heap_cap ? heap_cap * 2 : 65536;
        struct { unsigned addr, len, prot, flags, fd, off; } a;
        int r;
        while (cap < heap_used + n) cap *= 2;
        a.addr = 0;
        a.len = cap;
        a.prot = PROT_RWX;
        a.flags = MAP_PRIVATE | MAP_ANON;
        a.fd = 0xffffffffu;
        a.off = 0;
        r = __syscall(SYS_MMAP, (int)&a);
        if (bad(r)) die("out of memory");
        if (heap) mem_copy((void *)(unsigned)r, heap, heap_used);
        heap = (unsigned char *)(unsigned)r;
        heap_cap = cap;
    }
    p = heap + heap_used;
    heap_used += n;
    return p;
}

static char *dup_str(const char *s) {
    int n = 0;
    char *d;
    while (s[n]) n++;
    d = alloc((unsigned)n + 1);
    mem_copy(d, s, (unsigned)n + 1);
    return d;
}

typedef struct Obj {
    unsigned char *map;
    unsigned bias, span;
    unsigned symtab, strtab, hash, strsz, syment;
    unsigned rel, relsz, jmprel, pltrelsz;
    unsigned init, init_sz, fini, fini_sz;
    unsigned init_fn, fini_fn;
    char *path, *soname;
    char *needed[MAX_NEED];
    int nneeded, refs, pinned, kind;       /* kind: 1 exe, 2 lib, 3 self */
} Obj;

static Obj objs[MAX_OBJ];
static int nobj;
static Obj self_obj;
static char *libpath;                      /* LD_LIBRARY_PATH, если окружение есть */

static unsigned elf_hash(const char *name) {
    unsigned h = 0, g;
    while (*name) {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000u;
        if (g) h ^= g >> 24;
        h &= ~g;
    }
    return h;
}

static int is_elf(const unsigned char *b, unsigned sz) {
    return sz >= 52 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F' &&
           b[4] == 1 && b[5] == 1 && rd16(b + 18) == 3;
}

/* Теги-указатели храним уже с базой загрузки. */
static void read_dynamic(Obj *o, unsigned dyn) {
    unsigned p;
    o->syment = 16;
    for (p = dyn; rd32((unsigned char *)p) != DT_NULL; p += 8) {
        unsigned tag = rd32((unsigned char *)p);
        unsigned val = rd32((unsigned char *)(p + 4));
        unsigned ptr = val + o->bias;
        if (tag == DT_NEEDED) {
            if (o->nneeded >= MAX_NEED) die("too many DT_NEEDED");
            o->needed[o->nneeded++] = (char *)(o->strtab ? o->strtab + val : 0);
            /* STRTAB может встретиться позже: смещение запомним, поправим вторым проходом. */
            if (!o->strtab) o->needed[o->nneeded - 1] = (char *)(unsigned)val;
        } else if (tag == DT_SONAME) {
            o->soname = o->strtab ? (char *)(o->strtab + val) : (char *)(unsigned)val;
        } else if (tag == DT_STRTAB) o->strtab = ptr;
        else if (tag == DT_SYMTAB) o->symtab = ptr;
        else if (tag == DT_STRSZ) o->strsz = val;
        else if (tag == DT_SYMENT) o->syment = val;
        else if (tag == DT_HASH) o->hash = ptr;
        else if (tag == DT_REL) o->rel = ptr;
        else if (tag == DT_RELSZ) o->relsz = val;
        else if (tag == DT_JMPREL) o->jmprel = ptr;
        else if (tag == DT_PLTRELSZ) o->pltrelsz = val;
        else if (tag == DT_INIT) o->init_fn = ptr;
        else if (tag == DT_FINI) o->fini_fn = ptr;
        else if (tag == DT_INIT_ARRAY) o->init = ptr;
        else if (tag == DT_INIT_ARRAYSZ) o->init_sz = val;
        else if (tag == DT_FINI_ARRAY) o->fini = ptr;
        else if (tag == DT_FINI_ARRAYSZ) o->fini_sz = val;
    }
    if (o->strtab) {
        int i;
        for (i = 0; i < o->nneeded; i++)
            if ((unsigned)o->needed[i] < o->strsz)
                o->needed[i] = (char *)(o->strtab + (unsigned)o->needed[i]);
        if (o->soname && (unsigned)o->soname < o->strsz)
            o->soname = (char *)(o->strtab + (unsigned)o->soname);
    }
}

static unsigned dyn_addr(const unsigned char *eh, unsigned bias) {
    unsigned phoff = rd32(eh + 28);
    unsigned phnum = rd16(eh + 44);
    unsigned i;
    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = eh + phoff + i * 32;
        if (rd32(ph) == PT_DYNAMIC) return bias + rd32(ph + 8);
    }
    return 0;
}

static const char *sym_name(Obj *o, unsigned idx) {
    unsigned off = rd32((unsigned char *)(o->symtab + idx * o->syment));
    return (const char *)(o->strtab + off);
}

static int find_in(Obj *o, const char *name, unsigned *addr, unsigned *size) {
    unsigned nb, i, *bucket, *chain;
    if (!o->hash || !o->symtab || o->syment != 16) return 0;
    nb = rd32((unsigned char *)o->hash);
    if (!nb) return 0;
    bucket = (unsigned *)(o->hash + 8);
    chain = bucket + nb;
    for (i = bucket[elf_hash(name) % nb]; i; i = chain[i]) {
        unsigned char *s = (unsigned char *)(o->symtab + i * 16);
        unsigned shndx = rd16(s + 14);
        if (!shndx || !str_eq(sym_name(o, i), name)) continue;
        *size = rd32(s + 8);
        *addr = shndx == 0xfff1 ? rd32(s + 4) : rd32(s + 4) + o->bias;
        return 1;
    }
    return 0;
}

static int self_only;

static int resolve(Obj *from, unsigned idx, int copy, unsigned *addr, unsigned *size) {
    const char *name = sym_name(from, idx);
    int i;
    if (self_only) return find_in(&self_obj, name, addr, size);
    for (i = 0; i < nobj; i++) {
        if (copy && &objs[i] == from) continue;
        if (find_in(&objs[i], name, addr, size)) return 1;
    }
    if (!copy && find_in(&self_obj, name, addr, size)) return 1;
    return 0;
}

static void apply_one(Obj *o, unsigned rel, unsigned nbyte, int relative) {
    unsigned n = nbyte / 8, i;
    for (i = 0; i < n; i++) {
        unsigned char *r = (unsigned char *)(rel + i * 8);
        unsigned off = rd32(r);
        unsigned info = rd32(r + 4);
        unsigned type = info & 0xff;
        unsigned sym = info >> 8;
        unsigned *loc = (unsigned *)(o->bias + off);
        unsigned addr = 0, size = 0;
        int is_rel = type == R_386_RELATIVE;
        if (relative != is_rel) continue;
        if (type == R_386_RELATIVE) {
            *loc += o->bias;
            continue;
        }
        if (type != R_386_32 && type != R_386_COPY && type != R_386_GLOB_DAT && type != R_386_JMP_SLOT)
            die("unsupported relocation");
        if (!resolve(o, sym, type == R_386_COPY, &addr, &size)) {
            unsigned char *s = (unsigned char *)(o->symtab + sym * 16);
            if ((s[12] >> 4) == 2) {       /* STB_WEAK */
                if (type != R_386_COPY) *loc = 0;
                continue;
            }
            die(sym_name(o, sym));
        }
        if (type == R_386_COPY) {
            if (!size) size = rd32((unsigned char *)(o->symtab + sym * 16) + 8);
            mem_copy(loc, (void *)addr, size);
        } else if (type == R_386_32) *loc += addr;
        else *loc = addr;
    }
}

static void relocate(Obj *o) {
    apply_one(o, o->rel, o->relsz, 1);
    apply_one(o, o->jmprel, o->pltrelsz, 1);
    apply_one(o, o->rel, o->relsz, 0);
    apply_one(o, o->jmprel, o->pltrelsz, 0);
}

static void call_list(unsigned addr, unsigned sz) {
    unsigned n = sz / 4, i;
    for (i = 0; i < n; i++) {
        unsigned fn = ((unsigned *)addr)[i];
        if (fn) ((void (*)(void))fn)();
    }
}

static void call_init(Obj *o) {
    if (o->init_fn) ((void (*)(void))o->init_fn)();
    if (o->init && o->init_sz) call_list(o->init, o->init_sz);
}

static void call_fini(Obj *o) {
    if (o->fini && o->fini_sz) {
        unsigned n = o->fini_sz / 4;
        while (n--) {
            unsigned fn = ((unsigned *)o->fini)[n];
            if (fn) ((void (*)(void))fn)();
        }
    }
    if (o->fini_fn) ((void (*)(void))o->fini_fn)();
}

static int join(char *dst, int cap, const char *dir, const char *name) {
    int n = 0, m = 0, slash;
    while (dir[n]) n++;
    while (name[m]) m++;
    slash = n && dir[n - 1] != '/';
    if (n + slash + m + 1 > cap) return 0;
    mem_copy(dst, dir, (unsigned)n);
    if (slash) dst[n++] = '/';
    mem_copy(dst + n, name, (unsigned)m + 1);
    return 1;
}

static int exists(const char *path) {
    struct { char name[28]; unsigned type, size; } st;
    int r = __syscall(SYS_STAT, (int)path, (int)&st);
    return !bad(r) && st.type == 1 && st.size >= 52;
}

static int locate(const char *name, char *path, int cap) {
    const char *dirs[8];
    int nd = 0, i;
    if (name[0] == '/') {
        int n = 0;
        while (name[n]) n++;
        if (n + 1 > cap) return 0;
        mem_copy(path, name, (unsigned)n + 1);
        return exists(path);
    }
    if (libpath && libpath[0]) {
        char buf[128];
        int a = 0;
        while (libpath[a] && nd < 6) {
            int b = a;
            while (libpath[b] && libpath[b] != ':') b++;
            if (b > a && b - a < (int)sizeof(buf)) {
                mem_copy(buf, libpath + a, (unsigned)(b - a));
                buf[b - a] = 0;
                dirs[nd++] = dup_str(buf);
            }
            a = libpath[b] ? b + 1 : b;
        }
    }
    dirs[nd++] = "/lib";
    dirs[nd++] = "/usr/lib";
    for (i = 0; i < nd; i++)
        if (join(path, cap, dirs[i], name) && exists(path)) return 1;
    return 0;
}

static unsigned char *read_whole(const char *path, unsigned *sz) {
    struct { char name[28]; unsigned type, size; } st;
    unsigned char *buf;
    unsigned got = 0;
    int fd, n;
    if (bad(__syscall(SYS_STAT, (int)path, (int)&st)) || st.type != 1) return 0;
    if (st.size < 52 || st.size > 8u * 1024u * 1024u) return 0;
    fd = __syscall(SYS_OPEN, (int)path, 0);
    if (bad(fd)) return 0;
    buf = alloc(st.size);
    while (got < st.size) {
        n = __syscall(SYS_READ, fd, (int)(buf + got), (int)(st.size - got));
        if (bad(n) || n == 0) { __syscall(SYS_CLOSE, fd); return 0; }
        got += (unsigned)n;
    }
    __syscall(SYS_CLOSE, fd);
    *sz = st.size;
    return buf;
}

static unsigned map_span(const unsigned char *eh) {
    unsigned phoff = rd32(eh + 28), phnum = rd16(eh + 44), i, end = 0;
    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = eh + phoff + i * 32;
        unsigned v, mem;
        if (rd32(ph) != PT_LOAD) continue;
        v = rd32(ph + 8);
        mem = rd32(ph + 20);
        if (v + mem > end) end = v + mem;
    }
    return (end + 4095u) & ~4095u;
}

static void *map_anon(unsigned len) {
    struct { unsigned addr, len, prot, flags, fd, off; } a;
    int r;
    a.addr = 0;
    a.len = len;
    a.prot = PROT_RWX;
    a.flags = MAP_PRIVATE | MAP_ANON;
    a.fd = 0xffffffffu;
    a.off = 0;
    r = __syscall(SYS_MMAP, (int)&a);
    if (bad(r)) die("mmap failed");
    return (void *)(unsigned)r;
}

static Obj *find_loaded(const char *name) {
    const char *base = base_name(name);
    int i;
    if (self_obj.soname && (str_eq(name, self_obj.soname) || str_eq(base, self_obj.soname)))
        return &self_obj;
    if (self_obj.path && str_eq(name, self_obj.path)) return &self_obj;
    for (i = 0; i < nobj; i++) {
        if (objs[i].path && str_eq(objs[i].path, name)) return &objs[i];
        if (objs[i].soname && (str_eq(objs[i].soname, name) || str_eq(objs[i].soname, base)))
            return &objs[i];
    }
    return 0;
}

static void load_needed(const char *name);

static Obj *map_library(const char *path) {
    unsigned sz = 0, span, phoff, phnum, i, dyn;
    unsigned char *file = read_whole(path, &sz);
    Obj *o;
    if (!file || !is_elf(file, sz) || rd16(file + 16) != ET_DYN) die("not a shared library");
    if (rd16(file + 42) != 32 || rd16(file + 44) == 0) die("bad program headers");
    span = map_span(file);
    if (!span) die("library has no segments");
    if (nobj >= MAX_OBJ) die("too many libraries");
    o = &objs[nobj++];
    o->map = map_anon(span);
    o->span = span;
    o->bias = (unsigned)o->map;            /* наши .so собраны от нуля */
    o->path = dup_str(path);
    o->refs = 1;
    o->kind = 2;
    phoff = rd32(file + 28);
    phnum = rd16(file + 44);
    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = file + phoff + i * 32;
        unsigned off, va, filesz;
        if (rd32(ph) != PT_LOAD) continue;
        off = rd32(ph + 4);
        va = rd32(ph + 8);
        filesz = rd32(ph + 16);
        if (off + filesz > sz) die("segment past end of file");
        mem_copy(o->map + va, file + off, filesz);
    }
    dyn = dyn_addr(o->map, o->bias);
    if (!dyn) die("library has no dynamic section");
    read_dynamic(o, dyn);
    if (!o->symtab || !o->strtab || !o->hash) die("library has no symbol hash");
    return o;
}

static void load_needed(const char *name) {
    char path[128];
    Obj *o;
    int i;
    if (!name || !name[0] || find_loaded(name)) return;
    if (!locate(name, path, (int)sizeof(path))) die(name);
    if (find_loaded(path)) return;
    o = map_library(path);
    for (i = 0; i < o->nneeded; i++) load_needed(o->needed[i]);
}

static void fill_mapped(Obj *o, unsigned char *eh, unsigned bias, int kind) {
    unsigned dyn;
    o->map = eh;
    o->bias = bias;
    o->kind = kind;
    o->pinned = 1;
    o->refs = 1;
    dyn = dyn_addr(eh, bias);
    if (!dyn) die(kind == 3 ? "ld.so has no dynamic section" : "executable has no dynamic section");
    read_dynamic(o, dyn);
    if (!o->symtab || !o->strtab || !o->hash) die("missing symbol hash");
}

static unsigned exe_bias(unsigned char *eh, unsigned header) {
    unsigned phoff = rd32(eh + 28), phnum = rd16(eh + 44), i;
    for (i = 0; i < phnum; i++) {
        const unsigned char *ph = eh + phoff + i * 32;
        if (rd32(ph) != PT_LOAD || rd32(ph + 4) != 0) continue;
        return header - rd32(ph + 8);
    }
    return 0;
}

static void take_env(unsigned *env) {
    while (*env) {
        char *s = (char *)*env++;
        const char *key = "LD_LIBRARY_PATH=";
        int i = 0;
        while (key[i] && s[i] == key[i]) i++;
        if (!key[i]) libpath = s + i;
    }
}

static unsigned *aux_walk(unsigned *sp, unsigned tag, unsigned *out) {
    unsigned argc = sp[0];
    unsigned *p = sp + 1 + argc + 1;
    while (*p) p++;
    p++;
    while (*p != AT_NULL) {
        if (*p == tag && out) *out = p[1];
        p += 2;
    }
    return sp + 1 + argc + 1;
}

static void jump_entry(unsigned *sp, unsigned entry) {
    unsigned char code[16];
    unsigned next;
    code[0] = 0xBC;
    code[1] = (unsigned char)(unsigned)sp;
    code[2] = (unsigned char)((unsigned)sp >> 8);
    code[3] = (unsigned char)((unsigned)sp >> 16);
    code[4] = (unsigned char)((unsigned)sp >> 24);
    code[5] = 0xE9;
    next = (unsigned)code + 10;
    code[6] = (unsigned char)(entry - next);
    code[7] = (unsigned char)((entry - next) >> 8);
    code[8] = (unsigned char)((entry - next) >> 16);
    code[9] = (unsigned char)((entry - next) >> 24);
    ((void (*)(void))code)();
    die("entry returned");
}

static void bootstrap(unsigned *sp) {
    unsigned base = 0, phdr = 0, entry = 0, i;
    unsigned char *exe;
    aux_walk(sp, AT_BASE, &base);
    if (!base || !is_elf((unsigned char *)base, 64)) die("bad AT_BASE");
    fill_mapped(&self_obj, (unsigned char *)base, base, 3);
    self_obj.path = "/lib/ld.so";
    self_only = 1;
    relocate(&self_obj);
    self_only = 0;

    aux_walk(sp, AT_PHDR, &phdr);
    aux_walk(sp, AT_ENTRY, &entry);
    take_env(aux_walk(sp, 0, 0));
    if (!phdr) die("no AT_PHDR");
    exe = (unsigned char *)(phdr - 52);
    if (!is_elf(exe, 64)) die("bad executable");
    fill_mapped(&objs[nobj++], exe, exe_bias(exe, (unsigned)exe), 1);
    for (i = 0; i < objs[0].nneeded; i++) load_needed(objs[0].needed[i]);
    for (i = 0; i < (unsigned)nobj; i++) relocate(&objs[i]);
    for (i = (unsigned)nobj; i-- > 1; ) call_init(&objs[i]);
    call_init(&objs[0]);
    if (!entry) die("no AT_ENTRY");
    jump_entry(sp, entry);
}

/* Заглушка компоновщика вызывает _start(esp). До перемещения нельзя
 * трогать чужие глобалы: эта функция и bootstrap пользуются только static. */
int _start(unsigned *sp) {
    bootstrap(sp);
    return 0;
}

void *dlopen(const char *path, int flags) {
    Obj *o;
    int i, from;
    (void)flags;
    if (!path) return &objs[0];
    o = find_loaded(path);
    if (o) { if (o != &self_obj) o->refs++; return o; }
    from = nobj;
    load_needed(path);
    for (i = from; i < nobj; i++) relocate(&objs[i]);
    for (i = nobj; i-- > from; ) call_init(&objs[i]);
    o = find_loaded(path);
    return o ? o : 0;
}

void *dlsym(void *handle, const char *name) {
    unsigned addr = 0, size = 0;
    int i;
    if (!name) return 0;
    if (!handle) {
        for (i = 0; i < nobj; i++)
            if (find_in(&objs[i], name, &addr, &size)) return (void *)addr;
        if (find_in(&self_obj, name, &addr, &size)) return (void *)addr;
        return 0;
    }
    if (find_in(handle, name, &addr, &size)) return (void *)addr;
    return 0;
}

int dlclose(void *handle) {
    Obj *o = handle;
    if (!o || o == &self_obj || o->pinned) return 0;
    if (o->refs > 0) o->refs--;
    if (o->refs) return 0;
    call_fini(o);
    if (o->map && o->span) __syscall(SYS_MUNMAP, (int)o->map, (int)o->span);
    o->map = 0;
    o->path = o->soname = 0;
    o->symtab = o->hash = o->strtab = 0;
    return 0;
}
