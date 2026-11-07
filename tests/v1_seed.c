/* Собрать маленький том версии 1. Дальше его расширяет `myfs grow`. */
#include "fs.h"
#include "block.h"
#include "ata.h"
#include "task.h"
#include "string.h"
int printf(const char *, ...);
int host_open_image(const char *);
unsigned host_sectors(void);
int host_read_sector(unsigned, void *);
int host_write_sector(unsigned, const void *);
void host_close_image(void);
void exit(int);
bool ata_init(void) { return true; }
u32 ata_sectors(void) { return host_sectors(); }
const char *ata_model(void) { return "v1 seed"; }
int ata_read(u32 l, void *b) { return host_read_sector(l, b); }
int ata_write(u32 l, const void *b) { return host_write_sector(l, b); }
void mutex_lock(struct mutex *m) { (void)m; }
void mutex_unlock(struct mutex *m) { (void)m; }
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
int main(int argc, char **argv) {
    bool formatted;
    u32 ino;
    char name[28], longer[29];
    CHECK(argc == 2 && host_open_image(argv[1]) == 0);
    CHECK(fs_init(&formatted) == 0);
    CHECK(fs_format_legacy() == 0);
    memset(name, 'n', 27);
    name[27] = 0;
    memset(longer, 'm', 28);
    longer[28] = 0;
    CHECK(fs_create(1, "keep", FS_TYPE_DIR, 0) == 0);
    CHECK(fs_create(1, "keep/note", FS_TYPE_FILE, &ino) == 0);
    CHECK(fs_write(ino, 0, "v1-ok", 6) == 6);
    CHECK(fs_create(1, name, FS_TYPE_FILE, &ino) == 0);
    CHECK(fs_write(ino, 0, "long", 4) == 4);
    CHECK(fs_create(1, longer, FS_TYPE_FILE, 0) == FS_ENAMETOOLONG);
    host_close_image();
    return 0;
}
