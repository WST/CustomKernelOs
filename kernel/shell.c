/* Командная оболочка MyOS: история команд, файловые команды, перенаправление вывода. */
#include "shell.h"
#include "vga.h"
#include "keyboard.h"
#include "timer.h"
#include "string.h"
#include "fs.h"
#include "pmm.h"
#include "paging.h"
#include "heap.h"
#include "task.h"
#include "demo.h"
#include "process.h"

#define LINE_MAX 200
#define HIST_MAX 16

static char history[HIST_MAX][LINE_MAX + 1];
static int  hist_count;
static u8   text_color = LIGHT_GREY;
static u32  cwd = FS_ROOT;               /* текущий каталог (inode) */

/* ======================================================== ввод строки */

static void prompt(void) {
    char path[128];
    vga_set_color(LIGHT_GREEN, BLACK); vga_puts("myos");
    if (fs_ready() && fs_getpath(cwd, path, sizeof(path)) == FS_OK) {
        vga_set_color(WHITE, BLACK);      vga_putc(':');
        vga_set_color(LIGHT_BLUE, BLACK); vga_puts(path);
    }
    vga_set_color(WHITE, BLACK);       vga_puts("> ");
    vga_set_color(text_color, BLACK);
}

static void replace_line(char *line, int *len, const char *src) {
    while (*len > 0) { vga_putc('\b'); (*len)--; }
    while (src[*len] && *len < LINE_MAX) { line[*len] = src[*len]; vga_putc(src[*len]); (*len)++; }
    line[*len] = 0;
}

static void readline(char *line, bool use_history) {
    int len = 0, hpos = hist_count;
    line[0] = 0;
    for (;;) {
        int c = kb_getchar();
        if (c == '\n') { vga_putc('\n'); line[len] = 0; return; }
        if (c == KEY_CTRL_C) { vga_puts("^C\n"); line[0] = 0; return; }
        if (c == '\b') { if (len > 0) { len--; vga_putc('\b'); } continue; }
        if (use_history && c == KEY_UP) {
            if (hpos > 0) replace_line(line, &len, history[--hpos]);
            continue;
        }
        if (use_history && c == KEY_DOWN) {
            if (hpos < hist_count - 1) replace_line(line, &len, history[++hpos]);
            else { hpos = hist_count; replace_line(line, &len, ""); }
            continue;
        }
        if (c < 32 || c >= 127) continue;
        if (len < LINE_MAX) { line[len++] = c; vga_putc(c); }
    }
}

static void add_history(const char *line) {
    if (!line[0]) return;
    if (hist_count && !strcmp(history[hist_count - 1], line)) return;
    if (hist_count == HIST_MAX) {
        memmove(history[0], history[1], sizeof(history[0]) * (HIST_MAX - 1));
        hist_count--;
    }
    memcpy(history[hist_count++], line, strlen(line) + 1);
}

/* разбить строку на слова (на месте); возвращает их количество */
static int split(char *s, char **argv, int max) {
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ') *s++ = 0;
        if (!*s) break;
        argv[n++] = s;
        while (*s && *s != ' ') s++;
    }
    return n;
}

static char *trim(char *s) {
    while (*s == ' ') s++;
    char *e = s + strlen(s);
    while (e > s && e[-1] == ' ') *--e = 0;
    return s;
}

static bool fs_err(const char *what, int r) {
    if (r >= 0) return false;
    vga_set_color(LIGHT_RED, BLACK);
    kprintf("%s: %s\n", what, fs_strerror(r));
    vga_set_color(text_color, BLACK);
    return true;
}

/* открыть существующий файл или создать новый */
static int open_file(const char *path, bool create, u32 *ino) {
    int r = fs_resolve(cwd, path, ino);
    if (r == FS_ENOENT && create) r = fs_create(cwd, path, FS_TYPE_FILE, ino);
    if (r) return r;
    struct fs_stat st;
    if ((r = fs_stat(*ino, &st))) return r;
    return st.type == FS_TYPE_DIR ? FS_EISDIR : FS_OK;
}

/* записать текст в файл: append=false — перезаписать */
static int write_text(const char *path, const char *text, bool append, bool newline) {
    u32 ino;
    int r = open_file(path, true, &ino);
    if (r) return r;
    struct fs_stat st;
    if (!append && (r = fs_truncate(ino))) return r;
    if ((r = fs_stat(ino, &st))) return r;
    u32 off = st.size;
    if ((r = fs_write(ino, off, text, strlen(text))) < 0) return r;
    off += r;
    if (newline && (r = fs_write(ino, off, "\n", 1)) < 0) return r;
    return FS_OK;
}

/* ======================================================== системные команды */

static void cmd_help(char *a);
static void cmd_clear(char *a) { (void)a; vga_clear(); }

/* echo текст | echo текст > файл | echo текст >> файл */
static void cmd_echo(char *a) {
    char *gt = a;
    while (*gt && *gt != '>') gt++;
    if (!*gt) { kprintf("%s\n", a); return; }

    bool append = gt[1] == '>';
    *gt = 0;
    char *file = trim(gt + (append ? 2 : 1));
    char *text = trim(a);
    if (!*file) { kprintf("echo: missing file name after '>'\n"); return; }
    fs_err(file, write_text(file, text, append, true));
}

static void cmd_about(char *a) {
    (void)a;
    kprintf("MyOS 0.5 - hobby operating system written from scratch.\n");
    kprintf("  x86 32-bit protected mode, own bootloader, processes in ring 3, MyFS.\n");
}

static void cmd_uptime(char *a) {
    (void)a;
    u32 t = timer_ticks();
    u32 s = t / TIMER_HZ;
    kprintf("Uptime: %u:%02u:%02u.%02u (%u ticks)\n",
            s / 3600, (s / 60) % 60, s % 60, t % TIMER_HZ, t);
}

static void cmd_color(char *a) {
    if (!*a) { kprintf("usage: color <0-15>\n"); return; }
    int c = atoi(a) & 15;
    text_color = c;
    vga_set_color(c, BLACK);
    kprintf("Text color set to %d.\n", c);
}

static void cmd_history(char *a) {
    (void)a;
    for (int i = 0; i < hist_count; i++) kprintf("%3d  %s\n", i + 1, history[i]);
}

static void cmd_sleep(char *a) {
    int ms = atoi(a);
    if (ms <= 0) { kprintf("usage: sleep <ms>\n"); return; }
    sleep_ms(ms);
    kprintf("Woke up after %d ms.\n", ms);
}

/* panic        — деление на ноль (#DE)
 * panic pf     — чтение по адресу NULL (#PF)
 * panic double — двойное освобождение памяти (проверка в kfree) */
static void cmd_panic(char *a) {
    if (!strcmp(a, "pf")) {
        volatile u32 *null = (volatile u32 *)0;
        kprintf("Reading from NULL...\n");
        (void)*null;
    } else if (!strcmp(a, "double")) {
        void *p = kmalloc(32);
        kfree(p);
        kfree(p);
    } else {
        /* делим на ноль инструкцией div -> исключение #DE (вектор 0).
         * На C это делать нельзя: компилятор вправе «оптимизировать» UB. */
        __asm__ volatile ("xor %%ecx, %%ecx; mov $1, %%eax; xor %%edx, %%edx; div %%ecx"
                          : : : "eax", "ecx", "edx");
    }
}

/* ======================================================== команды памяти */

static void print_size(u64 bytes) {
    if (bytes >= (1ull << 30) && bytes % (1ull << 30) == 0) kprintf("%u GB", (u32)(bytes >> 30));
    else if (bytes >= (1u << 20)) kprintf("%u MB", (u32)(bytes >> 20));
    else kprintf("%u KB", (u32)(bytes >> 10));
}

static void cmd_mem(char *a) {
    (void)a;
    task_reap();
    kprintf("BIOS memory map (E820):\n");
    for (u32 i = 0; i < e820_count(); i++) {
        const struct e820_entry *e = e820_get(i);
        u64 end = e->base + e->length - 1;
        if (e->type == E820_USABLE) vga_set_color(LIGHT_GREEN, BLACK);
        if (end >> 32) kprintf("  %x%08x - %x%08x  ",
                               (u32)(e->base >> 32), (u32)e->base, (u32)(end >> 32), (u32)end);
        else           kprintf("  %08x - %08x  ", (u32)e->base, (u32)end);
        print_size(e->length);
        kprintf("  %s\n", e820_type_name(e->type));
        vga_set_color(text_color, BLACK);
    }

    struct pmm_stats ps;
    pmm_stats(&ps);
    u32 free = ps.total_frames - ps.used_frames;
    kprintf("Physical: %u KB total, %u KB used, %u KB free (4 KB pages)\n",
            ps.total_frames * 4, ps.used_frames * 4, free * 4);
    kprintf("Paging:   identity map 0-%u MB, %u page table(s) of 4 KB\n",
            paging_identity_limit() >> 20, paging_table_count());

    struct heap_stats hs;
    heap_stats(&hs);
    kprintf("Heap:     %u KB mapped at %p, integrity: %s\n",
            hs.mapped / 1024, HEAP_START, heap_check() ? "BROKEN" : "ok");
    kprintf("          used %u B in %u block(s), free %u B in %u block(s)\n",
            hs.used, hs.used_blocks, hs.free, hs.free_blocks);
    kprintf("          kmalloc: %u calls, kfree: %u calls\n", hs.allocs, hs.frees);
}

static u32 rnd_state;
static u32 rnd(void) { rnd_state = rnd_state * 1103515245 + 12345; return rnd_state >> 8; }

/* heaptest [N] — случайные kmalloc/krealloc/kfree с проверкой содержимого */
static void cmd_heaptest(char *a) {
    #define SLOTS 64
    u32 rounds = *a ? strtou(a, NULL) : 2000;
    static u8  *ptr[SLOTS];
    static u32  len[SLOTS];
    static u8   tag[SLOTS];
    struct heap_stats before, peak, after;
    struct pmm_stats pm_before;
    heap_stats(&before);
    pmm_stats(&pm_before);
    memset(ptr, 0, sizeof(ptr));
    memset(&peak, 0, sizeof(peak));
    rnd_state = timer_ticks() | 1;

    kprintf("heaptest: %u random operations on %u slots...\n", rounds, SLOTS);
    for (u32 i = 0; i < rounds; i++) {
        u32 k = rnd() % SLOTS;
        if (ptr[k]) {                               /* проверить, что данные целы */
            for (u32 j = 0; j < len[k]; j++)
                if (ptr[k][j] != (u8)(tag[k] + j)) {
                    kprintf("FAIL: slot %u corrupted at byte %u\n", k, j);
                    return;
                }
        }
        u32 op = rnd() % 3;
        u32 size = (rnd() % 8 == 0) ? 1 + rnd() % 65536 : 1 + rnd() % 512;
        if (op == 0 && ptr[k]) {                    /* free */
            kfree(ptr[k]); ptr[k] = NULL;
            continue;
        }
        u8 *p = (op == 1 && ptr[k]) ? krealloc(ptr[k], size) : NULL;
        if (op == 1 && ptr[k]) {
            if (!p) { kprintf("FAIL: krealloc(%u) returned NULL\n", size); return; }
            u32 keep = len[k] < size ? len[k] : size;
            for (u32 j = 0; j < keep; j++)
                if (p[j] != (u8)(tag[k] + j)) { kprintf("FAIL: krealloc lost data\n"); return; }
        } else {
            kfree(ptr[k]);
            p = kmalloc(size);
            if (!p) { kprintf("FAIL: kmalloc(%u) returned NULL\n", size); return; }
        }
        ptr[k] = p; len[k] = size; tag[k] = rnd();
        for (u32 j = 0; j < size; j++) p[j] = (u8)(tag[k] + j);

        if (heap_check()) { kprintf("FAIL: heap integrity check %d\n", heap_check()); return; }
        struct heap_stats now;
        if ((i & 63) == 0) { heap_stats(&now); if (now.used > peak.used) peak = now; }
    }
    for (u32 k = 0; k < SLOTS; k++) { kfree(ptr[k]); ptr[k] = NULL; }
    heap_stats(&after);

    vga_set_color(LIGHT_GREEN, BLACK);
    kprintf("heaptest: PASSED");
    vga_set_color(text_color, BLACK);
    kprintf(" - peak %u KB in use, heap grew %u KB -> %u KB\n",
            peak.used / 1024, before.mapped / 1024, after.mapped / 1024);
    kprintf("          leaks: %s (used %u B before, %u B after), integrity: %s\n",
            after.used == before.used ? "none" : "YES", before.used, after.used,
            heap_check() ? "BROKEN" : "ok");
}

/* peek <addr> [len] — hex-дамп памяти */
static void cmd_peek(char *a) {
    if (!*a) { kprintf("usage: peek <addr> [len]   e.g. peek 0xB8000 64\n"); return; }
    const char *rest;
    u32 addr = strtou(a, &rest);
    u32 n = *rest ? strtou(rest, NULL) : 64;
    if (n == 0 || n > 256) n = 64;

    u32 phys;
    if (!vmm_translate(addr, &phys)) {
        kprintf("%p: page not mapped (would cause a page fault)\n", addr);
        return;
    }
    kprintf("virtual %p -> physical %p\n", addr, phys);
    for (u32 row = 0; row < n; row += 16) {
        u32 base = addr + row;
        u32 cnt = n - row < 16 ? n - row : 16;
        if (!vmm_translate(base, &phys) || !vmm_translate(base + cnt - 1, &phys)) {
            kprintf("%p: <not mapped>\n", base);
            break;
        }
        const volatile u8 *p = (const volatile u8 *)base;
        kprintf("%p  ", base);
        for (u32 j = 0; j < 16; j++) {
            if (j < cnt) kprintf("%02x ", p[j]); else kprintf("   ");
            if (j == 7) vga_putc(' ');
        }
        vga_putc(' ');
        for (u32 j = 0; j < cnt; j++) vga_putc(p[j] >= 32 && p[j] < 127 ? p[j] : '.');
        vga_putc('\n');
    }
}

static void cmd_reboot(char *a) {
    (void)a;
    kprintf("Rebooting...\n");
    while (inb(0x64) & 2) ;     /* ждём, пока контроллер 8042 освободится */
    outb(0x64, 0xFE);           /* импульс на линию RESET процессора */
    for (;;) __asm__ volatile ("hlt");
}

static void cmd_halt(char *a) {
    (void)a;
    kprintf("System halted. It is now safe to turn off your computer.\n");
    /* ACPI-выключение в QEMU (порт 0x604); на реальном железе просто стоим */
    __asm__ volatile ("outw %0, %1" : : "a"((u16)0x2000), "Nd"((u16)0x604));
    for (;;) __asm__ volatile ("cli; hlt");
}

/* ======================================================== файловые команды */

static void cmd_pwd(char *a) {
    (void)a;
    char path[128];
    if (!fs_err("pwd", fs_getpath(cwd, path, sizeof(path)))) kprintf("%s\n", path);
}

static void cmd_cd(char *a) {
    u32 ino;
    struct fs_stat st;
    if (!*a) { cwd = FS_ROOT; return; }
    int r = fs_resolve(cwd, a, &ino);
    if (!r) r = fs_stat(ino, &st);
    if (!r && st.type != FS_TYPE_DIR) r = FS_ENOTDIR;
    if (!fs_err(a, r)) cwd = ino;
}

static void cmd_ls(char *a) {
    u32 dir;
    struct fs_stat st;
    const char *path = *a ? a : ".";
    int r = fs_resolve(cwd, path, &dir);
    if (!r) r = fs_stat(dir, &st);
    if (fs_err(path, r)) return;
    if (st.type == FS_TYPE_FILE) { kprintf("%6u  %s\n", st.size, path); return; }

    struct fs_dirent de;
    u32 pos = 0, files = 0, dirs = 0, bytes = 0;
    while (fs_readdir(dir, &pos, &de) == 1) {
        if (fs_stat(de.ino, &st)) continue;
        if (st.type == FS_TYPE_DIR) {
            dirs++;
            kprintf("   <DIR>  ");
            vga_set_color(LIGHT_BLUE, BLACK); kprintf("%s/\n", de.name);
            vga_set_color(text_color, BLACK);
        } else {
            files++;
            bytes += st.size;
            kprintf("%8u  %s\n", st.size, de.name);
        }
    }
    kprintf("%u dir(s), %u file(s), %u bytes\n", dirs, files, bytes);
}

static void tree_walk(u32 dir, int depth) {
    struct fs_dirent de;
    struct fs_stat st;
    u32 pos = 0;
    if (depth > 8) return;
    while (fs_readdir(dir, &pos, &de) == 1) {
        if (fs_stat(de.ino, &st)) continue;
        for (int i = 0; i < depth; i++) vga_puts("|   ");
        vga_puts("+-- ");
        if (st.type == FS_TYPE_DIR) {
            vga_set_color(LIGHT_BLUE, BLACK); kprintf("%s/\n", de.name);
            vga_set_color(text_color, BLACK);
            tree_walk(de.ino, depth + 1);
        } else {
            kprintf("%s (%u)\n", de.name, st.size);
        }
    }
}

static void cmd_tree(char *a) {
    u32 dir;
    const char *path = *a ? a : ".";
    if (fs_err(path, fs_resolve(cwd, path, &dir))) return;
    kprintf("%s\n", path);
    tree_walk(dir, 0);
}

static void cmd_mkdir(char *a) {
    char *argv[8];
    int n = split(a, argv, 8);
    if (!n) { kprintf("usage: mkdir <dir>...\n"); return; }
    for (int i = 0; i < n; i++) fs_err(argv[i], fs_create(cwd, argv[i], FS_TYPE_DIR, NULL));
}

static void cmd_touch(char *a) {
    char *argv[8];
    int n = split(a, argv, 8);
    if (!n) { kprintf("usage: touch <file>...\n"); return; }
    for (int i = 0; i < n; i++) {
        u32 ino;
        fs_err(argv[i], open_file(argv[i], true, &ino));
    }
}

static void cmd_cat(char *a) {
    char *argv[8];
    int n = split(a, argv, 8);
    if (!n) { kprintf("usage: cat <file>...\n"); return; }
    for (int i = 0; i < n; i++) {
        u32 ino;
        if (fs_err(argv[i], open_file(argv[i], false, &ino))) continue;
        static char buf[513];
        u32 off = 0;
        int r;
        bool last_nl = true;
        while ((r = fs_read(ino, off, buf, 512)) > 0) {
            for (int j = 0; j < r; j++) vga_putc(buf[j]);
            last_nl = buf[r - 1] == '\n';
            off += r;
        }
        fs_err(argv[i], r);
        if (!last_nl) vga_putc('\n');
    }
}

/* write <file> <text> — перезаписать файл одной строкой */
static void cmd_write(char *a) {
    char *text = a;
    while (*text && *text != ' ') text++;
    if (*text) *text++ = 0;
    if (!*a) { kprintf("usage: write <file> <text>\n"); return; }
    fs_err(a, write_text(a, trim(text), false, true));
}

/* edit <file> — простой построчный редактор */
static void cmd_edit(char *a) {
    a = trim(a);
    if (!*a) { kprintf("usage: edit <file>\n"); return; }
    u32 ino;
    int r = open_file(a, true, &ino);
    if (fs_err(a, r)) return;

    struct fs_stat st;
    fs_stat(ino, &st);
    vga_set_color(YELLOW, BLACK);
    kprintf("-- editing %s (%u bytes). Lines are appended to the file.\n", a, st.size);
    kprintf("-- '.' on its own line = save & exit, ':clear' = erase file first\n");
    vga_set_color(text_color, BLACK);

    static char line[LINE_MAX + 1];
    for (;;) {
        vga_set_color(DARK_GREY, BLACK); vga_puts("~ ");
        vga_set_color(text_color, BLACK);
        readline(line, false);
        if (!strcmp(line, ".")) break;
        if (!strcmp(line, ":clear")) { if (!fs_err(a, fs_truncate(ino))) kprintf("(file cleared)\n"); continue; }
        fs_stat(ino, &st);
        u32 len = strlen(line);
        line[len++] = '\n';
        if (fs_err(a, fs_write(ino, st.size, line, len))) break;
    }
    fs_stat(ino, &st);
    kprintf("Saved %s: %u bytes.\n", a, st.size);
}

static void cmd_rm(char *a) {
    char *argv[8];
    int n = split(a, argv, 8);
    if (!n) { kprintf("usage: rm <file|empty dir>...\n"); return; }
    for (int i = 0; i < n; i++) fs_err(argv[i], fs_remove(cwd, argv[i]));
    /* если удалили каталог, в котором стояли, — возвращаемся в корень */
    struct fs_stat st;
    if (fs_stat(cwd, &st) || st.type != FS_TYPE_DIR) cwd = FS_ROOT;
}

static void cmd_mv(char *a) {
    char *argv[3];
    if (split(a, argv, 3) != 2) { kprintf("usage: mv <src> <dst>\n"); return; }
    fs_err("mv", fs_rename(cwd, argv[0], argv[1]));
}

static void cmd_cp(char *a) {
    char *argv[3];
    if (split(a, argv, 3) != 2) { kprintf("usage: cp <src> <dst>\n"); return; }
    u32 src, dst;
    if (fs_err(argv[0], open_file(argv[0], false, &src))) return;

    /* cp file dir -> dir/file */
    char target[128];
    u32 d;
    struct fs_stat st;
    if (!fs_resolve(cwd, argv[1], &d) && !fs_stat(d, &st) && st.type == FS_TYPE_DIR) {
        const char *base = argv[0] + strlen(argv[0]);
        while (base > argv[0] && base[-1] != '/') base--;
        u32 l1 = strlen(argv[1]), l2 = strlen(base);
        if (l1 + l2 + 2 > sizeof(target)) { kprintf("cp: path too long\n"); return; }
        memcpy(target, argv[1], l1);
        target[l1] = '/';
        memcpy(target + l1 + 1, base, l2 + 1);
    } else {
        memcpy(target, argv[1], strlen(argv[1]) + 1);
    }

    if (fs_err(target, open_file(target, true, &dst))) return;
    if (src == dst) { kprintf("cp: source and destination are the same\n"); return; }
    if (fs_err(target, fs_truncate(dst))) return;

    static char buf[512];
    u32 off = 0;
    int r;
    while ((r = fs_read(src, off, buf, sizeof(buf))) > 0) {
        int w = fs_write(dst, off, buf, r);
        if (fs_err(target, w)) return;
        off += r;
    }
    if (!fs_err(argv[0], r)) kprintf("Copied %u bytes.\n", off);
}

static void cmd_df(char *a) {
    (void)a;
    if (!fs_ready()) { kprintf("df: %s\n", fs_strerror(FS_ENODEV)); return; }
    struct fs_info fi;
    fs_info(&fi);
    u32 used = fi.total_blocks - fi.free_blocks;
    kprintf("MyFS: %u KB total, %u KB used, %u KB free (%u%% used)\n",
            fi.total_blocks / 2, used / 2, fi.free_blocks / 2,
            fi.total_blocks ? used * 100 / fi.total_blocks : 0);
    kprintf("      inodes: %u of %u free\n", fi.free_inodes, fi.total_inodes);
}

static void cmd_format(char *a) {
    if (strcmp(trim(a), "yes")) {
        kprintf("This erases ALL files on the disk. Run 'format yes' to confirm.\n");
        return;
    }
    if (!fs_err("format", fs_format())) {
        cwd = FS_ROOT;
        kprintf("Disk formatted.\n");
    }
}

/* ======================================================== команды задач */

static void cmd_ps(char *a) {
    (void)a;
    static struct task_info list[MAX_TASKS];
    int n = task_list(list, MAX_TASKS);
    vga_set_color(WHITE, BLACK);
    kprintf("  ID  NAME            RING  STATE      CPU%%   CPU TIME    MEM\n");
    vga_set_color(text_color, BLACK);
    for (int i = 0; i < n; i++) {
        struct task_info *t = &list[i];
        u32 ms = t->cpu_ticks * (1000 / TIMER_HZ);
        kprintf("%4u  ", t->id);
        if (t->user) vga_set_color(LIGHT_CYAN, BLACK);
        kprintf("%s", t->name);
        vga_set_color(text_color, BLACK);
        for (u32 k = strlen(t->name); k < 16; k++) vga_putc(' ');
        kprintf("%s     %s", t->user ? "3" : "0", task_state_name(t->state));
        for (u32 k = strlen(task_state_name(t->state)); k < 9; k++) vga_putc(' ');
        kprintf("%5u%%  %6u.%02us  ", t->cpu_percent, ms / 1000, (ms % 1000) / 10);
        if (t->mem_kb) kprintf("%4u KB\n", t->mem_kb);
        else kprintf("   boot\n");
    }
}

static void cmd_kill(char *a) {
    if (!*a) { kprintf("usage: kill <id> | kill all   (see 'ps')\n"); return; }
    if (!strcmp(a, "all")) {
        static struct task_info list[MAX_TASKS];
        int n = task_list(list, MAX_TASKS), killed = 0;
        for (int i = 0; i < n; i++) if (task_kill(list[i].id) == 0) killed++;
        kprintf("%d task(s) killed.\n", killed);
        return;
    }
    u32 id = strtou(a, NULL);
    int r = task_kill(id);
    if (r == -1) kprintf("kill: no task with id %u\n", id);
    else if (r == -2) kprintf("kill: task %u cannot be killed\n", id);
    else kprintf("Task %u killed.\n", id);
}

static void cmd_spawn(char *a) {
    char *arg = a;
    while (*arg && *arg != ' ') arg++;
    if (*arg) *arg++ = 0;
    arg = trim(arg);
    if (!*a) {
        kprintf("usage: spawn <demo> [arg]. Demos:\n");
        for (u32 i = 0; i < demo_count; i++) kprintf("  %s%s- %s\n", demos[i].name,
                                                      "         " + strlen(demos[i].name), demos[i].help);
        return;
    }
    int id = demo_spawn(a, arg);
    if (id == -100) kprintf("spawn: unknown demo '%s' (run 'spawn' for the list)\n", a);
    else if (id < 0) kprintf("spawn: cannot create task (out of memory or too many tasks)\n");
    else kprintf("Started '%s' as task %d.\n", a, id);
}

/* ======================================================== запуск программ */

/* argv[0] — путь к программе. Последний аргумент "&" — запустить в фоне. */
static void run_program(char **argv, int argc) {
    bool bg = argc > 1 && !strcmp(argv[argc - 1], "&");
    if (bg) argc--;
    int pid = proc_spawn(argv[0], cwd, argc, argv, !bg);
    if (pid < 0) {
        vga_set_color(LIGHT_RED, BLACK);
        kprintf("%s: %s\n", argv[0], proc_strerror(pid));
        vga_set_color(text_color, BLACK);
        return;
    }
    if (bg) { kprintf("[%d] %s started in background\n", pid, argv[0]); return; }

    /* передний план: Ctrl+C убьёт программу; ждём её завершения */
    proc_set_foreground(pid);
    int code = 0;
    task_waitpid(pid, &code);
    proc_set_foreground(0);
    task_reap();                                  /* сразу вернуть память программы */
    vga_set_color(text_color, BLACK);
    if (code == 137)      kprintf("[killed]\n");
    else if (code == 139) ;                       /* сообщение уже напечатано */
    else if (code)        kprintf("[exit code %d]\n", code);
}

static void cmd_run(char *a) {
    char *argv[PROC_MAX_ARGS + 1];
    int argc = split(a, argv, PROC_MAX_ARGS + 1);
    if (!argc) { kprintf("usage: run <program> [args] [&]   (programs live in /bin)\n"); return; }
    run_program(argv, argc);
}

/* команда не встроенная — ищем программу: путь с '/' как есть, иначе в /bin */
static bool try_program(char *cmd, char *args) {
    static char path[64];
    if (!fs_ready()) return false;
    u32 ino;
    bool has_slash = false;
    for (char *p = cmd; *p; p++) if (*p == '/') has_slash = true;
    if (has_slash) {
        if (strlen(cmd) >= sizeof(path)) return false;
        memcpy(path, cmd, strlen(cmd) + 1);
    } else {
        if (strlen(cmd) + 6 > sizeof(path)) return false;
        memcpy(path, "/bin/", 5);
        memcpy(path + 5, cmd, strlen(cmd) + 1);
    }
    if (fs_resolve(cwd, path, &ino)) return false;

    char *argv[PROC_MAX_ARGS + 1];
    argv[0] = path;
    int argc = 1 + split(args, argv + 1, PROC_MAX_ARGS);
    run_program(argv, argc);
    return true;
}

/* ======================================================== таблица команд */

struct command {
    const char *name;
    const char *help;
    void (*fn)(char *args);
};

static const struct command commands[] = {
    { "help",     "show this list",                cmd_help },
    { "clear",    "clear the screen",              cmd_clear },
    { "echo",     "print text (> file, >> file)",  cmd_echo },
    { "about",    "about MyOS",                    cmd_about },
    { "uptime",   "time since boot",               cmd_uptime },
    { "color",    "text color 0-15",               cmd_color },
    { "history",  "command history",               cmd_history },
    { "sleep",    "sleep N ms",                    cmd_sleep },
    { "ls",       "list directory",                cmd_ls },
    { "tree",     "directory tree",                cmd_tree },
    { "cd",       "change directory",              cmd_cd },
    { "pwd",      "current directory",             cmd_pwd },
    { "mkdir",    "create directories",            cmd_mkdir },
    { "touch",    "create empty files",            cmd_touch },
    { "cat",      "print files",                   cmd_cat },
    { "write",    "write <file> <text>",           cmd_write },
    { "edit",     "line editor (appends)",         cmd_edit },
    { "cp",       "copy a file",                   cmd_cp },
    { "mv",       "move / rename",                 cmd_mv },
    { "rm",       "remove file / empty dir",       cmd_rm },
    { "df",       "disk usage",                    cmd_df },
    { "format",   "erase disk (format yes)",       cmd_format },
    { "mem",      "memory map and usage",          cmd_mem },
    { "heaptest", "stress-test the heap [N]",      cmd_heaptest },
    { "peek",     "hex dump <addr> [len]",         cmd_peek },
    { "run",      "run <program> [args] [&]",      cmd_run },
    { "ps",       "list tasks",                    cmd_ps },
    { "spawn",    "start a demo task",             cmd_spawn },
    { "kill",     "kill <id> | kill all",         cmd_kill },
    { "panic",    "crash [pf|double]",             cmd_panic },
    { "reboot",   "restart the computer",          cmd_reboot },
    { "halt",     "power off",                     cmd_halt },
};
#define NCMDS (sizeof(commands) / sizeof(commands[0]))

static void cmd_help(char *a) {
    (void)a;
    kprintf("Available commands:\n");
    u32 half = (NCMDS + 1) / 2;
    for (u32 i = 0; i < half; i++) {
        for (u32 c = 0; c < 2; c++) {
            u32 k = i + c * half;
            if (k >= NCMDS) break;
            const char *n = commands[k].name, *h = commands[k].help;
            vga_set_color(WHITE, BLACK);
            kprintf(" %s", n);
            vga_set_color(text_color, BLACK);
            u32 col = 1 + strlen(n);
            for (; col < 10; col++) vga_putc(' ');
            kprintf("%s", h);
            col += strlen(h);
            if (c == 0) for (; col < 40; col++) vga_putc(' ');
        }
        vga_putc('\n');
    }
}

static void execute(char *line) {
    line = trim(line);
    if (!*line) return;
    char *args = line;
    while (*args && *args != ' ') args++;
    if (*args) *args++ = 0;
    args = trim(args);

    for (u32 i = 0; i < NCMDS; i++) {
        if (!strcmp(line, commands[i].name)) { commands[i].fn(args); return; }
    }
    if (try_program(line, args)) return;
    kprintf("Unknown command: %s (type 'help', programs: 'ls /bin')\n", line);
}

void shell_run(void) {
    static char line[LINE_MAX + 1];

    /* как init в Unix: запустить пользовательскую оболочку, если она есть */
    u32 ino;
    if (fs_ready() && fs_resolve(FS_ROOT, "/bin/sh", &ino) == FS_OK) {
        static char sh_path[] = "/bin/sh";
        char *argv[] = { sh_path };
        for (;;) {
            run_program(argv, 1);
            kprintf("Session ended. Starting login again.\n");
        }
    }

    for (;;) {
        prompt();
        readline(line, true);
        add_history(line);
        execute(line);
    }
}
