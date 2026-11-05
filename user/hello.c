/* hello — первая программа для MyOS. Работает в кольце 3, в своей памяти. */
#include "myos.h"

int main(int argc, char **argv) {
    unsigned cs;
    __asm__ volatile ("mov %%cs, %0" : "=r"(cs));

    printf("Hello from user space!\n");
    printf("  pid %d, running in ring %u, code at %p, stack near %p\n",
           getpid(), cs & 3, (void *)main, (void *)&cs);
    printf("  argc = %d\n", argc);
    for (int i = 0; i < argc; i++) printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    return 0;
}
