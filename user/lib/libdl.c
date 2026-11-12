/* libdl: публичные dlopen/dlsym/dlclose/dlerror.
 * Настоящее состояние загрузчика живёт в ld.so. Разделяемая библиотека только
 * перенаправляет туда вызовы. Архив для -static загрузчика не содержит. */
#include "dlfcn.h"

#ifdef DL_STUB
static char stub_msg[80];
static int stub_set;

static void stub_note(const char *s) {
    int i = 0;
    while (s[i] && i < (int)sizeof(stub_msg) - 1) {
        stub_msg[i] = s[i];
        i++;
    }
    stub_msg[i] = 0;
    stub_set = 1;
}

void *dlopen(const char *path, int flags) {
    (void)path;
    (void)flags;
    stub_note("dynamic loader is not linked");
    return 0;
}

void *dlsym(void *handle, const char *name) {
    (void)handle;
    (void)name;
    stub_note("dynamic loader is not linked");
    return 0;
}

int dlclose(void *handle) {
    (void)handle;
    return 0;
}

char *dlerror(void) {
    if (!stub_set) return 0;
    stub_set = 0;
    return stub_msg;
}
#else
void *_dl_open(const char *path, int flags);
void *_dl_sym(void *handle, const char *name);
int _dl_close(void *handle);
char *_dl_error(void);

void *dlopen(const char *path, int flags) { return _dl_open(path, flags); }
void *dlsym(void *handle, const char *name) { return _dl_sym(handle, name); }
int dlclose(void *handle) { return _dl_close(handle); }
char *dlerror(void) { return _dl_error(); }
#endif
