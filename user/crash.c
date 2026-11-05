/* crash <how> — способы сломаться. Ядро должно пережить каждый из них. */
#include "myos.h"

static int depth(int n) {
    volatile char pad[1024];
    pad[0] = n;
    if (n > 1000000) return 0;       /* не случится: стек кончится раньше */
    return depth(n + 1) + pad[0];
}

int main(int argc, char **argv) {
    const char *how = argc > 1 ? argv[1] : "";
    if (!strcmp(how, "null")) {
        printf("reading address 0...\n");
        return *(volatile int *)0;
    }
    if (!strcmp(how, "kernel")) {
        printf("reading kernel memory at 0x10000...\n");
        return *(volatile int *)0x10000;
    }
    if (!strcmp(how, "code")) {
        printf("writing into my own code (read-only page)...\n");
        *(volatile char *)main = 0x90;
        return 0;
    }
    if (!strcmp(how, "cli")) {
        printf("executing privileged instruction 'cli'...\n");
        __asm__ volatile ("cli");
        return 0;
    }
    if (!strcmp(how, "io")) {
        printf("talking to hardware directly: outb to port 0x64...\n");
        __asm__ volatile ("outb %%al, $0x64" : : "a"(0xFE));
        return 0;
    }
    if (!strcmp(how, "stack")) {
        printf("infinite recursion...\n");
        return depth(0);
    }
    if (!strcmp(how, "div")) {
        volatile int z = 0;
        printf("dividing by zero...\n");
        int r;
        __asm__ volatile ("cltd; idivl %2" : "=a"(r) : "a"(1), "r"(z) : "edx");
        return r;
    }
    if (!strcmp(how, "syscall")) {
        printf("asking the kernel to print its own memory: write(1, 0x10000, 16)\n");
        int r = write(STDOUT, (const void *)0x10000, 16);
        printf("kernel answered %d (-14 = EFAULT: bad address). Nice try.\n", r);
        return 0;
    }
    if (!strcmp(how, "loop")) {
        printf("spinning forever, press Ctrl+C...\n");
        for (;;) ;
    }
    printf("usage: crash null|kernel|code|cli|io|stack|div|syscall|loop\n");
    return 1;
}
