/* Проверка copy-on-write, mmap и (если передали путь) запуска ET_DYN. */
#include "myos.h"

static int failed;

static void check(const char *name, int ok) {
    printf("%s: %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) failed++;
}

static int cow_ok(void) {
    static int marker = 42;
    int pid = fork();
    if (pid < 0) return 0;
    if (pid == 0) { marker = 7; exit(marker); }
    int st = -1;
    if (waitpid(pid, &st, 0) != pid) return 0;
    return st == 7 && marker == 42;
}

static int anon_ok(void) {
    unsigned char *p = mmap(0, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == (void *)-1) return 0;
    p[0] = 1;
    p[4096] = 2;
    if (p[0] != 1 || p[4096] != 2) return 0;
    if (mprotect(p, 8192, PROT_READ) != 0) return 0;
    if (mprotect(p, 8192, PROT_READ | PROT_WRITE) != 0) return 0;
    p[100] = 3;
    return p[100] == 3 && munmap(p, 8192) == 0;
}

static int file_ok(void) {
    int fd = open("/tmp/vmdata", O_WRITE);
    if (fd < 0) return 0;
    if (write(fd, "abcdef", 6) != 6) return 0;
    close(fd);
    fd = open("/tmp/vmdata", O_RDWR);
    if (fd < 0) return 0;
    char *p = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p == (void *)-1) return 0;
    if (memcmp(p, "abcdef", 6) != 0) return 0;
    p[0] = 'Z';
    char buf[8];
    memset(buf, 0, sizeof(buf));
    if (lseek(fd, 0, SEEK_SET) != 0) return 0;
    if (read(fd, buf, 6) != 6 || buf[0] != 'a' || p[0] != 'Z') return 0;
    close(fd);
    return munmap(p, 4096) == 0;
}

int main(int argc, char **argv) {
    if (argc > 1) {
        char *av[] = { argv[1], 0 };
        exec(argv[1], av);
        printf("exec failed\n");
        return 1;
    }
    check("cow", cow_ok());
    check("mmap", anon_ok());
    check("file", file_ok());
    printf("vmtest: %d failed\n", failed);
    return failed ? 1 : 0;
}
