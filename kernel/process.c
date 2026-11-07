/* Пользовательские процессы: загрузка ELF и подготовка адресного пространства.
 *
 * Адресное пространство новой программы:
 *
 *   0x40000000  .text .rodata      (только чтение)    ┐ из ELF-файла
 *   0x4000x000  .data .bss         (чтение и запись)  ┘
 *   brk ...     куча программы, растёт через sbrk() до 0x80000000
 *   0x80000000  динамический загрузчик, если в ELF есть PT_INTERP
 *   0xA0000000  общая память shm
 *   0xB0000000  mmap (анонимные страницы и частные копии файлов)
 *   0xBFFF0000  стек программы, 64 КБ, растёт вниз
 *   0xBFFFFFFF  на вершине: строки аргументов, argv[], argc
 *               у программы с PT_INTERP ещё envp и auxv
 */
#include "process.h"
#include "ipc.h"
#include "task.h"
#include "paging.h"
#include "pmm.h"
#include "heap.h"
#include "fs.h"
#include "string.h"
#include "vga.h"
#include "file.h"

/* ---- формат ELF (только то, что нужно) ---- */
struct elf_header {
    u8  ident[16];
    u16 type, machine;
    u32 version, entry, phoff, shoff, flags;
    u16 ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} __attribute__((packed));

struct elf_phdr {
    u32 type, offset, vaddr, paddr, filesz, memsz, flags, align;
} __attribute__((packed));

#define PT_LOAD   1
#define PT_INTERP 3
#define PF_W      2
#define ET_EXEC   2
#define ET_DYN    3
#define EM_386    3

#define AT_NULL   0
#define AT_PHDR   3
#define AT_PHENT  4
#define AT_PHNUM  5
#define AT_PAGESZ 6
#define AT_BASE   7
#define AT_ENTRY  9

#define STACK_BOTTOM (USER_TOP - USER_STACK_PAGES * PAGE_SIZE)

static volatile u32 foreground;

const char *proc_strerror(int err) {
    switch (err) {
    case PROC_ENOENT:  return "no such file";
    case PROC_ENOEXEC: return "not a MyOS executable (ELF32, i386)";
    case PROC_ENOMEM:  return "out of memory";
    case PROC_EAGAIN:  return "too many tasks";
    case PROC_EISDIR:  return "is a directory";
    default:           return "error";
    }
}

/* скопировать байты в чужое адресное пространство — через физические адреса */
static bool copy_to_dir(u32 dir, u32 va, const void *src, u32 n) {
    const u8 *s = src;
    while (n) {
        u32 phys;
        if (!vmm_lookup(dir, va, &phys, NULL)) return false;
        u32 chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > n) chunk = n;
        memcpy((void *)phys, s, chunk);          /* RAM < 1 ГБ отображена 1:1 */
        va += chunk; s += chunk; n -= chunk;
    }
    return true;
}

/* отобразить новые обнулённые страницы в [start, end).
 * reuse: страница от соседнего сегмента того же файла — только дополнить права. */
static bool map_zero_pages(u32 dir, u32 start, u32 end, u32 flags, u32 *pages, bool reuse) {
    for (u32 va = start & ~(PAGE_SIZE - 1); va < end; va += PAGE_SIZE) {
        u32 old_flags, phys;
        if (vmm_lookup(dir, va, &phys, &old_flags)) {
            if (!reuse) return false;
            if ((flags & PG_WRITE) && !(old_flags & PG_WRITE))
                vmm_map_in(dir, va, phys & ~(PAGE_SIZE - 1), flags);
            continue;
        }
        u32 frame = pmm_alloc();
        if (!frame) return false;
        memset((void *)frame, 0, PAGE_SIZE);
        if (vmm_map_in(dir, va, frame, flags)) { pmm_free(frame); return false; }
        (*pages)++;
    }
    return true;
}

static const char *basename(const char *p) {
    const char *b = p;
    for (; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

struct image {
    u32 dir, entry, sp, brk, pages;
};

struct elfmap {
    u32 base, entry, phdr, phnum, end;
};

/* ET_DYN с адресами меньше базы грузится от неё; уже абсолютный образ — как есть */
static u32 elf_bias(struct elf_phdr *ph, u32 nph, u32 preferred) {
    u32 top = 0;
    for (u32 i = 0; i < nph; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        u32 end = ph[i].vaddr + ph[i].memsz;
        if (end > top) top = end;
    }
    return top <= preferred ? preferred : 0;
}

static int map_elf(u8 *file, u32 size, u32 dir, u32 bias, bool reuse, u32 *pages, struct elfmap *out) {
    struct elf_header *eh = (struct elf_header *)file;
    struct elf_phdr *ph = (struct elf_phdr *)(file + eh->phoff);
    u32 end_max = 0;
    for (u32 i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_LOAD || ph[i].memsz == 0) continue;
        u32 start = bias + ph[i].vaddr, end = start + ph[i].memsz;
        bool interp = start >= INTERP_BASE;
        if (end < start || start < USER_BASE || ph[i].filesz > ph[i].memsz ||
            ph[i].offset + ph[i].filesz > size ||
            (interp ? end > SHM_BASE : end > INTERP_BASE))
            return PROC_ENOEXEC;
        u32 flags = PG_USER | ((ph[i].flags & PF_W) ? PG_WRITE : 0);
        if (!map_zero_pages(dir, start, end, flags, pages, reuse)) return PROC_ENOMEM;
        if (!copy_to_dir(dir, start, file + ph[i].offset, ph[i].filesz)) return PROC_ENOMEM;
        if (end > end_max) end_max = end;
    }
    if (!end_max) return PROC_ENOEXEC;
    u32 entry = bias + eh->entry;
    if (entry < USER_BASE || entry >= SHM_BASE) return PROC_ENOEXEC;
    out->base = bias;
    out->entry = entry;
    out->phdr = bias + eh->phoff;
    out->phnum = eh->phnum;
    out->end = end_max;
    return 0;
}

/* Стек для динамического загрузчика: argc, argv, пустой envp, auxv. esp кратен 16. */
static int aux_stack(u32 dir, int argc, char **argv, struct elfmap *prog, u32 interp, u32 *sp_out) {
    u32 sp = USER_TOP, uargv[PROC_MAX_ARGS + 1];
    for (int i = argc - 1; i >= 0; i--) {
        u32 len = strlen(argv[i]) + 1;
        if (len > 256) len = 256;
        sp -= len;
        if (sp < STACK_BOTTOM + 1024) return PROC_ENOMEM;
        copy_to_dir(dir, sp, argv[i], len - 1);
        copy_to_dir(dir, sp + len - 1, "", 1);
        uargv[i] = sp;
    }
    uargv[argc] = 0;
    u32 vec[PROC_MAX_ARGS + 24];
    int n = 0;
    vec[n++] = (u32)argc;
    for (int i = 0; i <= argc; i++) vec[n++] = uargv[i];
    vec[n++] = 0;                                            /* envp */
    vec[n++] = AT_PHDR;   vec[n++] = prog->phdr;
    vec[n++] = AT_PHENT;  vec[n++] = sizeof(struct elf_phdr);
    vec[n++] = AT_PHNUM;  vec[n++] = prog->phnum;
    vec[n++] = AT_PAGESZ; vec[n++] = PAGE_SIZE;
    vec[n++] = AT_BASE;   vec[n++] = interp;
    vec[n++] = AT_ENTRY;  vec[n++] = prog->entry;
    vec[n++] = AT_NULL;   vec[n++] = 0;
    u32 bytes = (u32)n * 4;
    u32 dest = (sp - bytes) & ~15u;
    if (dest < STACK_BOTTOM) return PROC_ENOMEM;
    if (!copy_to_dir(dir, dest, vec, bytes)) return PROC_ENOMEM;
    *sp_out = dest;
    return 0;
}

/* Загрузить ELF в НОВОЕ адресное пространство и положить на стек argc/argv.
 * argv — строки в памяти ядра. */
static int load_image(const char *path, u32 cwd, int argc, char **argv, struct image *im) {
    u32 ino;
    struct fs_stat st;
    if (fs_resolve(cwd, path, &ino) || fs_stat(ino, &st)) return PROC_ENOENT;
    if (st.type == FS_TYPE_DIR) return PROC_EISDIR;
    if (st.size < sizeof(struct elf_header)) return PROC_ENOEXEC;

    u8 *file = kmalloc(st.size);
    if (!file) return PROC_ENOMEM;
    int err = PROC_ENOEXEC;
    u32 dir = 0, pages = 0;
    if (fs_read(ino, 0, file, st.size) != (int)st.size) goto fail;

    /* --- проверяем заголовок --- */
    struct elf_header *eh = (struct elf_header *)file;
    if (eh->ident[0] != 0x7F || eh->ident[1] != 'E' || eh->ident[2] != 'L' || eh->ident[3] != 'F' ||
        eh->ident[4] != 1 /* 32 бит */ || eh->ident[5] != 1 /* little endian */ ||
        (eh->type != ET_EXEC && eh->type != ET_DYN) || eh->machine != EM_386 ||
        eh->phentsize != sizeof(struct elf_phdr) || !eh->phnum ||
        eh->phoff + eh->phnum * sizeof(struct elf_phdr) > st.size)
        goto fail;

    err = PROC_ENOMEM;
    dir = vmm_create_dir();
    if (!dir) goto fail;

    struct elf_phdr *ph = (struct elf_phdr *)(file + eh->phoff);
    u32 bias = eh->type == ET_DYN ? elf_bias(ph, eh->phnum, USER_BASE) : 0;
    struct elfmap prog;
    err = map_elf(file, st.size, dir, bias, true, &pages, &prog);
    if (err) goto fail;
    u32 brk = (prog.end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (brk > INTERP_BASE) brk = INTERP_BASE;

    /* PT_INTERP: путь к загрузчику внутри файла, сам загрузчик — ET_DYN от INTERP_BASE */
    char interp[128];
    interp[0] = 0;
    for (u32 i = 0; i < eh->phnum; i++) {
        if (ph[i].type != PT_INTERP) continue;
        if (!ph[i].filesz || ph[i].filesz >= sizeof(interp) || ph[i].offset + ph[i].filesz > st.size) {
            err = PROC_ENOEXEC;
            goto fail;
        }
        bool nul = false;
        for (u32 k = 0; k < ph[i].filesz; k++) if (file[ph[i].offset + k] == 0) nul = true;
        if (!nul) { err = PROC_ENOEXEC; goto fail; }
        memcpy(interp, file + ph[i].offset, ph[i].filesz);
        break;
    }

    u32 entry = prog.entry;
    u32 sp = 0;
    if (!map_zero_pages(dir, STACK_BOTTOM, USER_TOP, PG_USER | PG_WRITE, &pages, false)) {
        err = PROC_ENOMEM;
        goto fail;
    }
    if (argc > PROC_MAX_ARGS) argc = PROC_MAX_ARGS;

    if (interp[0]) {
        u32 lino;
        struct fs_stat lst;
        if (fs_resolve(cwd, interp, &lino) || fs_stat(lino, &lst) || lst.type != FS_TYPE_FILE) {
            err = PROC_ENOENT;
            goto fail;
        }
        u8 *ld = kmalloc(lst.size);
        if (!ld) { err = PROC_ENOMEM; goto fail; }
        bool got = fs_read(lino, 0, ld, lst.size) == (int)lst.size && lst.size >= sizeof(struct elf_header);
        struct elf_header *leh = (struct elf_header *)ld;
        int lerr = PROC_ENOEXEC;
        struct elfmap lm;
        if (got && leh->ident[0] == 0x7F && leh->type == ET_DYN && leh->machine == EM_386 &&
            leh->phentsize == sizeof(struct elf_phdr) && leh->phnum &&
            leh->phoff + leh->phnum * sizeof(struct elf_phdr) <= lst.size) {
            u32 lbias = elf_bias((struct elf_phdr *)(ld + leh->phoff), leh->phnum, INTERP_BASE);
            lerr = map_elf(ld, lst.size, dir, lbias, false, &pages, &lm);
        }
        kfree(ld);
        if (lerr) { err = lerr; goto fail; }
        entry = lm.entry;
        u32 ph;
        if (!vmm_lookup(dir, prog.phdr, &ph, NULL)) { err = PROC_ENOEXEC; goto fail; }
        err = aux_stack(dir, argc, argv, &prog, lm.base, &sp);
        if (err) goto fail;
    } else {
        /* _start(argc, argv): адрес возврата, argc, argv. esp+4 кратен 16. */
        u32 uargv[PROC_MAX_ARGS + 1];
        sp = USER_TOP;
        for (int i = argc - 1; i >= 0; i--) {
            u32 len = strlen(argv[i]) + 1;
            if (len > 256) len = 256;
            sp -= len;
            copy_to_dir(dir, sp, argv[i], len - 1);
            copy_to_dir(dir, sp + len - 1, "", 1);
            uargv[i] = sp;
        }
        uargv[argc] = 0;
        u32 argv_addr = (sp - 4 * (argc + 1)) & ~15u;
        copy_to_dir(dir, argv_addr, uargv, 4 * (argc + 1));
        sp = argv_addr - 20;
        u32 frame[3] = { 0, (u32)argc, argv_addr };
        copy_to_dir(dir, sp, frame, sizeof(frame));
    }

    im->dir = dir;
    im->entry = entry;
    im->sp = sp;
    im->brk = brk;
    im->pages = pages;
    kfree(file);
    return 0;

fail:
    if (dir) vmm_destroy_dir(dir);
    kfree(file);
    return err;
}

int proc_spawn(const char *path, u32 cwd, int argc, char **argv, bool fg) {
    struct image im;
    int err = load_image(path, cwd, argc, argv, &im);
    if (err) return err;
    struct task *t = task_alloc_user(basename(path), im.dir, im.entry, im.sp);
    if (!t) { vmm_destroy_dir(im.dir); return PROC_ENOMEM; }
    t->brk_start = t->brk = im.brk;
    t->user_pages = im.pages;
    t->cwd = cwd;
    t->keep_zombie = fg;                                 /* оболочка заберёт код выхода */
    u32 id = task_start(t);                              /* при неудаче сама освободит всё */
    if (!id) return PROC_EAGAIN;
    return id;
}

/* ---------------------------------------------------------------- fork
 *
 * Копия текущего процесса. Регистры потомка — это копия кадра системного
 * вызова родителя (r), только eax = 0. Поэтому потомок «возвращается» из
 * того же самого fork(), но получает 0, а родитель — номер потомка. */
int proc_fork(struct regs *r) {
    struct task *parent = task_current();
    u32 pages;
    u32 dir = vmm_clone_user(parent->page_dir, &pages);
    if (!dir) return PROC_ENOMEM;

    struct task *t = task_alloc_user(parent->name, dir, 0, 0);
    if (!t) { vmm_destroy_dir(dir); return PROC_ENOMEM; }
    struct regs *cr = (struct regs *)t->esp;
    *cr = *r;                                            /* те же регистры... */
    cr->eax = 0;                                         /* ...но fork() вернёт 0 */

    t->brk_start  = parent->brk_start;
    t->brk        = parent->brk;
    t->user_pages = pages;
    t->uid = parent->uid;
    t->authenticated = parent->authenticated;
    t->cwd        = parent->cwd;
    t->parent     = parent->id;
    t->pgid       = parent->pgid;
    t->keep_zombie = true;                               /* родитель заберёт код в waitpid */
    fpu_copy_current(t);                                 /* и регистры FPU тоже копия */
    for (int i = 0; i < MAX_FDS; i++) t->fds[i] = file_dup(parent->fds[i]);

    u32 id = task_start(t);
    return id ? (int)id : PROC_EAGAIN;
}

/* ---------------------------------------------------------------- exec
 *
 * Заменить программу в ТЕКУЩЕМ процессе. Номер, родитель, открытые файлы и
 * текущий каталог остаются, а память — новая. Если новую программу загрузить
 * не удалось, процесс продолжает жить со старой и получает ошибку. */
int proc_exec(struct regs *r, const char *path, int argc, char **argv) {
    struct task *t = task_current();
    struct image im;
    int err = load_image(path, t->cwd, argc, argv, &im);
    if (err) return err;

    u32 old = t->page_dir;
    t->page_dir = im.dir;
    vmm_switch(im.dir);                                  /* уходим со старого каталога... */
    shm_release_all(t, old);                             /* общую память — отпустить, не освобождая */
    vmm_destroy_dir(old);                                /* ...и только теперь его освобождаем */
    t->brk_start = t->brk = im.brk;
    t->user_pages = im.pages;
    fpu_reset_current();                                 /* новой программе — чистый FPU */
    const char *b = basename(path);
    u32 n = strlen(b);
    if (n >= TASK_NAME) n = TASK_NAME - 1;
    memset(t->name, 0, TASK_NAME);
    memcpy(t->name, b, n);

    /* iret из системного вызова уйдёт в начало новой программы */
    u32 cs = r->cs, ss = r->ss, ds = r->ds;
    memset(r, 0, sizeof(*r));
    r->cs = cs; r->ss = ss;
    r->ds = r->es = r->fs = r->gs = ds;
    r->eip = im.entry;
    r->useresp = im.sp;
    r->eflags = 0x202;
    return 0;
}

/* ---------------------------------------------------------------- передний план */

void proc_set_foreground(u32 pgid) { foreground = pgid; }
u32  proc_foreground(void) { return foreground; }

/* Ctrl+C: убить всю группу переднего плана (например, весь конвейер) */
bool proc_ctrl_c(void) {
    if (!foreground || !console_isig()) return false;   /* программа просила отдать ^C ей */
    if (!task_kill_group(foreground)) return false;
    vga_puts("^C\n");
    return true;
}
