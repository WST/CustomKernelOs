#include "module.h"

/* Учебный модуль: внешний вызов проверяет R_386_PC32, строка имени — R_386_32. */
extern int kmod_ping(int v);
int kmod_mark;

static int sample_init(void) {
    kmod_mark = kmod_ping(7);
    return kmod_mark == 7 ? 0 : -1;
}
static void sample_fini(void) { kmod_mark = 0; }

struct myos_module myos_module = { MOD_MAGIC, "sample", sample_init, sample_fini };
