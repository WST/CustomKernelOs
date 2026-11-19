#ifndef MODULE_H
#define MODULE_H
#include "types.h"

/* Загружаемый модуль ядра: ELF32 ET_REL, релокации R_386_32 и R_386_PC32.
 * Точка входа — глобальный символ myos_module. */

#define MOD_MAGIC 0x4D594F53u
enum { MOD_LOAD = 1, MOD_UNLOAD = 2, MOD_STAT = 3, MOD_RESCAN = 4, MOD_COUNT = 5 };

struct mod_info {
    char name[16];
    u32 index, refs;
};

struct myos_module {
    u32 magic;
    const char *name;
    int (*init)(void);
    void (*fini)(void);
};

struct kexport {
    const char *name;
    void *addr;
};

int  module_ctl(int op, const char *path, int index, struct mod_info *out);
int  module_load_image(const void *image, u32 len, const char *name);
void *module_symbol(int id, const char *name);
void *module_current(void);
void module_hold(void *mod);
void module_release(void *mod);
void modules_autoload(void);
int  firmware_read(const char *path, void **out, u32 *len);

#define MYOS_MODULE(name)
#define MYOS_PCI_ID(vendor, device)

#endif
