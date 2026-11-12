/* libdl.a без загрузчика: dlopen не находит библиотеку и dlerror это сообщает. */
#include <dlfcn.h>
#include <stdio.h>

int main(void) {
    void *handle = dlopen("libz.so", RTLD_NOW);
    if (handle) return 1;
    if (!dlerror()) return 2;
    if (dlerror()) return 3;
    if (dlsym(RTLD_DEFAULT, "crc32")) return 4;
    if (!dlerror()) return 5;
    if (dlclose(0)) return 6;
    puts("dl: ok");
    return 0;
}
