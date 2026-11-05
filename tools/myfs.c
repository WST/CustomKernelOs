/* myfs — работа с диском MyFS (disk.img) прямо с компьютера, без запуска MyOS.
 *
 *   myfs disk.img ls [path]          список файлов
 *   myfs disk.img mkdir <path>       создать каталог (вместе с родителями)
 *   myfs disk.img put <file> <path>  скопировать файл компьютера на диск
 *   myfs disk.img cat <path>         вывести (текстовый) файл с диска
 *
 * Используется ТОТ ЖЕ kernel/fs.c, что и в ядре: вместо драйвера ATA ему
 * подсовываются функции, читающие и пишущие сектора файла-образа. */
#include "fs.h"
#include "ata.h"
#include "task.h"
#include "string.h"

int  host_open_image(const char *path);
unsigned host_sectors(void);
int  host_read_sector(unsigned lba, void *buf);
int  host_write_sector(unsigned lba, const void *buf);
void host_close_image(void);
void *host_load_file(const char *path, unsigned *len);
void host_print(const char *s);
void host_error(const char *s);
int  snprintf(char *buf, unsigned long n, const char *fmt, ...);

/* ---- «драйвер диска» для fs.c ---- */
bool ata_init(void)                   { return true; }
u32  ata_sectors(void)                { return host_sectors(); }
const char *ata_model(void)           { return "disk image"; }
int  ata_read(u32 lba, void *buf)     { return host_read_sector(lba, buf); }
int  ata_write(u32 lba, const void *buf) { return host_write_sector(lba, buf); }
void mutex_lock(struct mutex *m)      { (void)m; }
void mutex_unlock(struct mutex *m)    { (void)m; }

static char out[512];
#define say(...) (snprintf(out, sizeof(out), __VA_ARGS__), host_print(out))
#define err(...) (snprintf(out, sizeof(out), __VA_ARGS__), host_error(out))

static int mkdir_p(const char *path) {
    char buf[128];
    u32 n = strlen(path), ino;
    if (n >= sizeof(buf)) return FS_ENAMETOOLONG;
    memcpy(buf, path, n + 1);
    for (u32 i = 1; i <= n; i++) {
        if (buf[i] != '/' && buf[i] != 0) continue;
        char c = buf[i];
        buf[i] = 0;
        if (fs_resolve(FS_ROOT, buf, &ino) == FS_ENOENT) {
            int r = fs_create(FS_ROOT, buf, FS_TYPE_DIR, NULL);
            if (r) return r;
        }
        buf[i] = c;
    }
    return FS_OK;
}

static int cmd_ls(const char *path) {
    u32 dir, pos = 0;
    struct fs_dirent de;
    struct fs_stat st;
    int r = fs_resolve(FS_ROOT, path, &dir);
    if (r) return r;
    while (fs_readdir(dir, &pos, &de) == 1) {
        if (fs_stat(de.ino, &st)) continue;
        if (st.type == FS_TYPE_DIR) say("   <DIR>  %s/\n", de.name);
        else                        say("%8u  %s\n", st.size, de.name);
    }
    return FS_OK;
}

static int cmd_put(const char *src, const char *dst) {
    unsigned len;
    unsigned char *data = host_load_file(src, &len);
    if (!data) { err("myfs: cannot read %s\n", src); return FS_EIO; }

    /* создать родительские каталоги */
    char parent[128];
    u32 n = strlen(dst);
    if (n >= sizeof(parent)) return FS_ENAMETOOLONG;
    memcpy(parent, dst, n + 1);
    while (n && parent[n - 1] != '/') n--;
    if (n > 1) { parent[n - 1] = 0; int r = mkdir_p(parent); if (r) return r; }

    u32 ino;
    int r = fs_resolve(FS_ROOT, dst, &ino);
    if (r == FS_ENOENT) r = fs_create(FS_ROOT, dst, FS_TYPE_FILE, &ino);
    if (r) return r;
    if ((r = fs_truncate(ino))) return r;
    r = fs_write(ino, 0, data, len);
    if (r < 0) return r;
    say("  %-24s -> %s (%u bytes)\n", src, dst, len);
    return FS_OK;
}

static int cmd_cat(const char *path) {
    u32 ino, off = 0;
    int r = fs_resolve(FS_ROOT, path, &ino);
    if (r) return r;
    for (;;) {
        int n = fs_read(ino, off, out, sizeof(out) - 1);
        if (n <= 0) break;
        out[n] = 0;
        host_print(out);
        off += n;
    }
    return FS_OK;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        err("usage: myfs <disk.img> ls [path] | mkdir <path> | put <file> <path> | cat <path>\n");
        return 2;
    }
    if (host_open_image(argv[1])) { err("myfs: cannot open %s\n", argv[1]); return 1; }
    bool formatted;
    int r = fs_init(&formatted);
    if (r) { err("myfs: %s\n", fs_strerror(r)); return 1; }
    if (formatted) say("myfs: %s had no file system - formatted\n", argv[1]);

    const char *cmd = argv[2];
    if (!strcmp(cmd, "ls"))                       r = cmd_ls(argc > 3 ? argv[3] : "/");
    else if (!strcmp(cmd, "mkdir") && argc > 3)   r = mkdir_p(argv[3]);
    else if (!strcmp(cmd, "put") && argc > 4)     r = cmd_put(argv[3], argv[4]);
    else if (!strcmp(cmd, "cat") && argc > 3)     r = cmd_cat(argv[3]);
    else { err("myfs: bad command\n"); r = FS_EINVAL; }

    host_close_image();
    if (r) { err("myfs: %s\n", fs_strerror(r)); return 1; }
    return 0;
}
