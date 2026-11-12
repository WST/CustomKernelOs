/* Замыкание лежит в странице, которую можно и писать, и исполнять.
 * На i386 отдельного бита NX нет, а PROT_EXEC ядро уже принимает.
 * Чужой closures.c завязан на dlmalloc, pthread и запрет W^X в Linux. */
#include <ffi.h>
#include <stddef.h>
#include <sys/mman.h>

void *ffi_closure_alloc(size_t size, void **code) {
    size_t len;
    char *p;
    if (!code || !size) return 0;
    len = (size + sizeof(size_t) + 4095u) & ~4095u;
    p = mmap(0, (unsigned)len, PROT_READ | PROT_WRITE | PROT_EXEC,
             MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;
    *(size_t *)p = len;
    *code = p + sizeof(size_t);
    return *code;
}

void ffi_closure_free(void *ptr) {
    char *p = (char *)ptr - sizeof(size_t);
    munmap(p, *(size_t *)p);
}

void *ffi_data_to_code_pointer(void *data) { return data; }

int ffi_tramp_is_present(void *ptr) {
    (void)ptr;
    return 0;
}
